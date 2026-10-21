"""The async reader on a Linux pseudo-terminal: no hardware, so these run in
cibuildwheel's Linux test step. The test writes to the master side and the
Port reads the slave. Linux only: macOS and Windows have no pty that
ysp/serial.h has been checked on."""
import math
import os
import sys
import threading
import time

import pytest

import ysp.serial as yser

pytestmark = pytest.mark.skipif(not sys.platform.startswith("linux"),
                                reason="pty pair: Linux only")


@pytest.fixture
def pair():
    master, slave = os.openpty()
    port = yser.Port(os.ttyname(slave))
    yield port, master
    port.close()
    os.close(slave)
    os.close(master)


def settle(port, lines, timeout=2.0):
    """Wait until the reader has cut `lines` records."""
    end = time.monotonic() + timeout
    while port.async_stats["lines"] < lines and time.monotonic() < end:
        time.sleep(0.005)


def test_lifecycle(pair):
    port, master = pair
    port.async_start()
    with pytest.raises(yser.Error, match="already running"):
        port.async_start()
    for call in (lambda: port.read(1, 0), port.available, port.interrupt,
                 lambda: port.purge(rx=True)):
        with pytest.raises(yser.Error, match="owns the input"):
            call()
    st = port.async_stop()
    assert st["running"] is False and st["error"] is None
    assert port.async_stop() is None
    with pytest.raises(yser.Error, match="not running"):
        port.async_read()
    os.write(master, b"q")
    assert port.read(1, 1.0) == b"q"            # the reader role is back
    port.async_start()
    port.close()                                # stops the reader
    assert port.async_stats["running"] is False
    with pytest.raises(yser.Closed):
        port.async_wait(0)


def test_lines_are_stamped_on_arrival(pair):
    port, master = pair
    port.async_start(terminator=b"\n")
    t0 = yser.now()
    os.write(master, b"a\nbb\nccc\npartial")
    settle(port, 3)
    t1 = yser.now()
    recs = port.async_read(first=True)
    assert [r[0] for r in recs] == [b"a\n", b"bb\n", b"ccc\n"]
    assert all(t0 <= r[2] <= r[1] <= t1 for r in recs)
    assert port.async_read() == []
    os.write(master, b"!\n")
    assert port.async_wait(2.0) == 1
    assert port.async_read() == [(b"partial!\n", pytest.approx(yser.now(), abs=1.0))]
    st = port.async_stop()
    assert st["lines"] == 4 and st["bytes"] == 18 and st["overflows"] == 0


def test_terminator_as_int_and_overlong(pair):
    port, master = pair
    port.async_start(terminator=ord(";"), max_len=4)
    os.write(master, b"abcdefgh;xy;")
    settle(port, 2)
    assert [r[0] for r in port.async_read()] == [b"abcd", b"xy;"]
    assert port.async_stats["overlong"] == 1


def test_fixed_records(pair):
    port, master = pair
    port.async_start(record_size=4)
    os.write(master, b"0123456789")
    settle(port, 2)
    assert [r[0] for r in port.async_read()] == [b"0123", b"4567"]
    os.write(master, b"ab")
    settle(port, 3)
    assert [r[0] for r in port.async_read(max=5)] == [b"89ab"]


def test_one_record_per_read(pair):
    port, master = pair
    port.async_start(terminator=None)
    os.write(master, b"xyz")
    assert port.async_wait(2.0) >= 1
    assert b"".join(r[0] for r in port.async_read()) == b"xyz"


def test_overflow_is_counted_not_blocking(pair):
    port, master = pair
    port.async_start(capacity=2)
    os.write(master, b"1\n2\n3\n4\n5\n")
    settle(port, 5)
    st = port.async_stats
    assert st["lines"] == 5 and st["overflows"] == 3 and st["max_used"] == 2
    assert [r[0] for r in port.async_read(max=1)] == [b"1\n"]
    assert [r[0] for r in port.async_read()] == [b"2\n"]
    os.write(master, b"6\n")                   # room again
    assert port.async_wait(2.0) == 1
    assert port.async_read()[0][0] == b"6\n"


def test_wait_times_out_and_is_interrupted(pair):
    port, master = pair
    port.async_start()
    t = yser.now()
    assert port.async_wait(0.05) == 0
    assert 0.045 <= yser.now() - t < 1.0
    caught = []

    def waiter():
        try:
            port.async_wait(math.inf)
        except yser.Interrupted as e:
            caught.append(e)

    th = threading.Thread(target=waiter)
    th.start()
    time.sleep(0.1)
    port.async_stop()
    th.join(2.0)
    assert not th.is_alive() and caught


def test_close_from_another_thread_ends_a_wait(pair):
    port, master = pair
    port.async_start()
    caught = []

    def waiter():
        try:
            port.async_wait(math.inf)
        except yser.Interrupted as e:
            caught.append(e)

    th = threading.Thread(target=waiter)
    th.start()
    time.sleep(0.1)
    port.close()
    th.join(2.0)
    assert not th.is_alive() and caught


def test_write_while_the_reader_runs(pair):
    port, master = pair
    port.async_start()
    t = yser.now()
    assert port.write(b"hello") == 5
    assert yser.now() - t < 0.05
    assert os.read(master, 16) == b"hello"
    os.write(master, b"back\n")
    assert port.async_wait(2.0) == 1
    assert port.async_read()[0][0] == b"back\n"


def test_reader_error_is_raised_after_the_records():
    master, slave = os.openpty()
    port = yser.Port(os.ttyname(slave))
    os.close(slave)
    try:
        port.async_start()
        os.write(master, b"last\n")
        settle(port, 1)
        os.close(master)                        # hang up: the reader's read fails
        master = -1
        end = time.monotonic() + 2.0
        while port.async_stats["running"] and time.monotonic() < end:
            time.sleep(0.005)
        st = port.async_stats
        assert not st["running"] and st["error"]
        assert port.async_read()[0][0] == b"last\n"
        with pytest.raises(yser.Disconnected):
            port.async_read()
        with pytest.raises(yser.Disconnected):
            port.async_wait(1.0)
        assert port.async_stop()["error"] == st["error"]
    finally:
        port.close()
        if master >= 0:
            os.close(master)
