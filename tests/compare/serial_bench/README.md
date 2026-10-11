# Serial bench: MATLAB serialport, Psychtoolbox IOPort and ysp_serial

This folder compares three ways to use a serial port from MATLAB:

- `serialport`: built into MATLAB since R2019b. No toolbox is necessary.
- `IOPort`: the serial driver of Psychtoolbox-3.
- `ysp_serial`: the MEX binding of [ysp/serial.h](../../../include/ysp/serial.h),
  in [bindings/mex/](../../../bindings/mex/).

One entry script, `serial_bench.m`, runs each API that it finds with the same
port, the same settings and the same sample counts. It skips an API that it
cannot load and tells you why. The measured numbers and their conditions are in
[docs/serial.md](../../../docs/serial.md#compared-with-matlab-serialport-and-ioport).

## Parts and the hardware that each part needs

| Part | Measures | Needs |
|---|---|---|
| `clock` | Cost of two back-to-back reads of the API's clock | Nothing |
| `noport` | The cheapest call into the API, port enumeration, open of a port that does not exist | Nothing |
| `calls` | Open, close, write of 1, 8, 64 and 512 bytes, read with nothing waiting, bytes-available query | Any serial port |
| `calls` (read with data waiting) | Read call when the bytes are already in the driver | A loopback or the echo board |
| `rt` | Round trip of a 13-byte command, with a blocking read and with a poll loop | A loopback or the echo board |
| `rt` (one-way split) | Host to board and board to host delays, and the API receive stamp against the board's send stamp | The echo board |
| `stream` | 1000 lines per second for 10 s: missed lines, lines that arrive together, latency, CPU use; a blocking read per line, a poll loop, and the asynchronous mode of each API | The echo board |

The two device options:

- **(a) Loopback.** A USB-serial adapter with its TX pin connected to its RX
  pin. On an FTDI FT232R TTL cable, connect TXD (orange) to RXD (yellow). On an
  RS-232 adapter with a DB9 plug, connect pin 2 to pin 3. The adapter sends
  each byte back to the host. This shows the adapter, its driver and its
  latency timer. It cannot split the round trip, because nothing on the far
  side has a clock.
- **(b) Echo board.** A Teensy 4.0 or 4.1, or a Raspberry Pi Pico, on its own
  USB port, with [firmware/ysp_echo/](../../../firmware/ysp_echo/ysp_echo.ino).
  The board stamps each command when it reads it and stamps each answer
  before it sends it. A clock fit on the host maps the board's stamps to the
  host clock, so the round trip splits into its two one-way delays. A native
  USB board has no bridge chip, so it has no FTDI latency timer.

Option (b) is better: it gives every number that option (a) gives except the
FTDI latency timer, plus the one-way split, the receive-stamp quality and the
stream test. Use option (a) only to show the latency timer.

## Before you start

1. Build the MEX files from this tree. In MATLAB, run
   `run('<repo>/bindings/mex/build.m')`.
2. Get a Psychtoolbox `IOPort` that loads. Psychtoolbox 3.0.20 and later
   need a paid license key on Windows. Their `IOPort.mexw64` links
   `LexActivator.dll`, the license manager. Without that DLL, MATLAB reports
   "Invalid MEX-file ... The specified module could not be found".
   Psychtoolbox 3.0.19.16 is the last release without a license check. To use
   it without an installation, download two files into one folder. No
   administrator rights are necessary:

   ```
   https://raw.githubusercontent.com/Psychtoolbox-3/Psychtoolbox-3/3.0.19.16/Psychtoolbox/PsychBasic/MatlabWindowsFilesR2007a/IOPort.mexw64
   https://raw.githubusercontent.com/Psychtoolbox-3/Psychtoolbox-3/3.0.19.16/Psychtoolbox/PsychBasic/MatlabWindowsFilesR2007a/GetSecs.mexw64
   ```

   Then give that folder as `ioport_dir`. The script puts it first on the
   path, so it takes precedence over an installed Psychtoolbox.
3. Connect the computer to AC power. Close other programs. Timing on battery
   power or on a busy machine is not comparable.

## Run it

Put this folder on the MATLAB path, or make it the current folder. Each
command writes its results to a new folder and prints the folder name.

No hardware (under 1 minute, measured on the test laptop):

```matlab
serial_bench('ioport_dir', 'C:\tmp\psy-work\serialbench\ptb-3.0.19.16')
```

A serial port with nothing attached, for the call costs:

```matlab
serial_bench('port', 'COM3', 'ioport_dir', '...')
```

Option (a), the loopback (estimated: 5 minutes at 16 ms, 3 minutes at 1 ms):

```matlab
serial_bench('port', 'COM3', 'device', 'loopback', 'ioport_dir', '...', ...
             'label', 'FT232R loopback, latency timer 16 ms')
```

Option (b), the echo board (estimated: 4 minutes):

```matlab
serial_bench('port', 'COM5', 'device', 'echo', 'ioport_dir', '...', ...
             'label', 'Teensy 4.0 echo')
```

To run fewer APIs or parts, give `'apis', {'ysp', 'ioport'}` or
`'parts', {'rt'}`. `help serial_bench` lists every option.

### Flash the echo board

The compiled images are in `C:\tmp\psy-work\serialbench\fw-*`. To compile
again, use the `arduino-cli` setup in `C:\tmp\psy-work\devices\arduino\`:

```
arduino-cli --config-file C:\tmp\psy-work\devices\arduino\arduino-cli.yaml compile --fqbn teensy:avr:teensy40 firmware\ysp_echo
```

Use `teensy:avr:teensy41` for a Teensy 4.1 and `rp2040:rp2040:rpipico` for
a Pico.

- **Teensy 4.0 or 4.1.** Start the Teensy Loader, `teensy.exe` in
  `C:\tmp\psy-work\devices\arduino\data\packages\teensy\tools\teensy-tools\1.62.0\`.
  Select **File > Open HEX File** and open `ysp_echo.ino.hex` from
  `fw-teensy_avr_teensy40` (or `fw-teensy_avr_teensy41`). Connect the board
  and push its button. The loader writes the image and restarts the board.
- **Pico.** Hold the BOOTSEL button and connect the board. A drive named
  `RPI-RP2` appears. Copy `ysp_echo.ino.uf2` to that drive. The board restarts.

To check the board, send `i` and a newline. The board answers
`I ysp-line 1 teensy echo` (or `rp2040`):

```matlab
h = ysp_serial('open', 'COM5');
ysp_serial('write', h, uint8(sprintf('i\n')));
char(ysp_serial('read', h, 64, 0.5))      % 0.5 s timeout
ysp_serial('close', h);
```

### Change the FTDI latency timer on Windows

An FTDI chip holds received bytes until its USB packet is full (62 data
bytes, FTDI application note AN232B-04) or until the latency timer expires.
The default is 16 ms. To set 1 ms:

1. Open Device Manager. Expand **Ports (COM & LPT)**.
2. Open **USB Serial Port (COMn)**. Select **Port Settings**, then
   **Advanced**.
3. Set **Latency Timer (msec)** to 1. Select **OK**.
4. Disconnect the adapter and connect it again. The driver reads the value
   only when the device starts.

The script reads the value from the registry and writes it to
`conditions.txt`. Run option (a) once at 16 ms and once at 1 ms. The LOW
LATENCY section of [ysp/serial.h](../../../include/ysp/serial.h) gives the
same procedure, and the Linux and macOS ones.

## Read the results

Each run writes these files:

| File | Contents |
|---|---|
| `summary.txt`, `summary.csv` | One row for each API and metric: n, median, p99, max, min, mean |
| `<api>_samples.csv` | Every sample of every metric |
| `<api>_rt_<block or poll>.csv` | Raw stamps of each round trip: host before and after the write call, after the read call, the API stamp, the board stamps |
| `<api>_stream_<mode>.csv` | Raw stamps of each stream line |
| `conditions.txt` | Date, MATLAB, CPU, power, port, FTDI latency timer, API versions, all options |

All times are in microseconds. A note that says "count" or "percent" marks a
metric that is not a time. p99 is the sample at rank ceil(0.99 n), so for
fewer than 100 samples it is the maximum.

Each API is timed with its own clock: `tic`/`toc` for serialport, `GetSecs`
for IOPort, `ysp_serial('now')` for ysp. All three return seconds; the bench
scales them to microseconds. The `clock` part gives the cost of each clock.
Subtract it from a call cost if the call is near that size. The results of
2026-10-11 used the binding's earlier `now_us`, which counted whole
microseconds; the C path under it is the same.

Metrics of the `rt` part, with `block` or `poll` before the name:

| Metric | Meaning |
|---|---|
| `rtt` | Start of the write call to the return of the read call that completed the answer |
| `write_call` | Duration of the write call |
| `write_ret_to_read_ret` | Return of the write call to return of the read call |
| `stamp_minus_return` | The API's own receive stamp minus the host time after the read (IOPort only) |
| `host_to_board`, `write_ret_to_board` | Write call start, or return, to the board reading the command (echo board) |
| `board_to_host` | Board send stamp to the return of the read call (echo board) |
| `stamp_minus_board_send` | The API receive stamp minus the board send stamp (IOPort, echo board) |
| `fit_unc`, `fit_spread`, `fit_ppm` | Quality of the clock fit; see below |

Metrics of the `stream` part, with `line`, `poll` or `async` before the name:
`lines_missed`, `lines_duplicated`, `bad_lines` (counts), `lines_per_read`,
`coalesced_pct` (lines that arrived in one read with another line),
`cpu_pct` (MATLAB process CPU time divided by wall time, percent of one core),
`lat_host` (host time after the read minus the board send stamp) and
`lat_stamp` (the API's own stamp minus the board send stamp). For IOPort,
`lat_stamp` in `async` mode is the background reader's stamp of the first
byte of each line. For serialport, `lat_stamp` in `async` mode is the
callback's `evt.AbsTime`, converted to the `tic` clock. For ysp, `lat_stamp`
in `async` mode is the reader thread's stamp of the read that brought the
line's newline. The `async` mode of IOPort and ysp also gives
`async_write_call` and `async_cmd_write_call`: a write while the reader
thread waits for input.

### How the one-way split works, and its limit

Each echo exchange gives four times: host before the write (t0), host after
the read (t2), board read of the command (rx) and board send of the answer
(tx). In each 200 ms of host time, the script keeps the exchange with the
smallest path time `(t2 - t0) - (tx - rx)`. It assumes that the two one-way
delays of that exchange are equal, and fits a line from board time to host
time through the kept exchanges. ysp/rt.h's BRACKET fit and Psychtoolbox's
DataPixx clock sync use the same selection.

The variation of each one-way delay from exchange to exchange is measured.
The constant part is not: it is known to plus or minus half the path time of
the best exchanges. The script reports this bound as `fit_unc` and in each
note. Only a shared physical signal, such as an oscilloscope on a pin that
the board toggles when it reads a command, can remove it. The same limit
applies to the stream latencies.

## The tuned IOPort and the raw Win32 floor

The API list has two IOPort entries. `ioport` uses IOPort's defaults.
`ioport_tuned` adds `BlockingBackgroundRead=1 PollLatency=0
HardwareBufferSizes=32768,32768`, so a blocking `Read` waits in the kernel
instead of in a 1 ms sleep loop. `sb_ioport.m` says what each option does,
with line references into Psychtoolbox's source.

`raw_win32.c` is a small C program without ysp code and without MATLAB. It
does the same round trip and the same stream with `CreateFile`, `WriteFile`
and `ReadFile` in five ways (a non-overlapped handle; the same with IOPort's
transmit mask; IOPort's driver calls without its sleep loop; overlapped;
`WaitCommEvent`) and prints the same metrics. It shows the floor of this port
and driver: what no API can beat. Build and run it on Windows:

```
gcc -O2 -Wall -o raw_win32.exe raw_win32.c
raw_win32.exe COM5 C:\tmp\raw-out 1000
```

A fourth argument `rev` runs the modes in reverse order, `rt` skips the
streams. Run it twice, once with `rev`: the first mode of a run measured
40 to 80 us slower than the same calls later in the run.

## Limits

- The API order is fixed in a run, and the machine state drifts. Run the
  bench twice, the second time with the API order reversed (`'apis'`).
- serialport has no non-blocking read. Its poll is a `NumBytesAvailable`
  query and then a read of that many bytes.
- IOPort writes use `blocking = 0` in the round trips. The `calls` part also
  times the default, `blocking = 1`, which waits until the driver reports an
  empty transmit queue.
- The write payload is the letter `z` without a newline, so the echo board
  ignores it. A loopback returns it, and the script discards it between
  writes, outside the timed calls.
- A 13-byte command at 115200 baud spends 1.1 ms on the wire in each
  direction of a loopback. Give a higher `'baud'` (for example 921600) to
  make it smaller. A native USB board ignores the baud rate.
