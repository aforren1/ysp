"""Reference for ysp/json.h's canonical numbers (v0.2.0).

Python's repr(float) is David Gay's correctly rounded shortest conversion
(the digits that read back to the same double, the nearest of them, ties to
even), so it is a reference that shares no code with the header's Ryu.

    uv run --no-project python tests/adapt/json_dtoa_ref.py tables
        the C tables of the header, checked exactly
    uv run --no-project python tests/adapt/json_dtoa_ref.py digest N
        the FNV-1a digests of the canonical text of the first N doubles and
        the first N/2 number texts of json_test.c's streams (the test
        prints its own with YJS_TEST_DOUBLES=N)
    uv run --no-project python tests/adapt/json_dtoa_ref.py edges
        the expected text of json_test.c's edge cases
    uv run --no-project python tests/adapt/json_dtoa_ref.py dump N FILE
        each double of the stream as hex bits and its canonical text
"""
import struct
import sys

M64 = (1 << 64) - 1
BITS = 125          # bits of each multiplier
STEP = 26           # 5^0 .. 5^25 fit in 64 bits


# ---------------------------------------------------------------- spelling

PLAIN_LO, PLAIN_HI = -18, 20


def spell(neg, digits, e):
    """digits: no leading or trailing zero; e: the exponent of the first."""
    if not digits:
        return "0"
    k = len(digits)
    s = "-" if neg else ""
    if PLAIN_LO <= e <= PLAIN_HI:
        if e >= k - 1:
            return s + digits + "0" * (e - k + 1)
        if e >= 0:
            return s + digits[: e + 1] + "." + digits[e + 1:]
        return s + "0." + "0" * (-e - 1) + digits
    return s + digits[0] + ("." + digits[1:] if k > 1 else "") + "e" + ("+" if e > 0 else "-") + str(abs(e))


def canon_text(t):
    """The canonical spelling of JSON number text: its exact value."""
    neg = t.startswith("-")
    if neg:
        t = t[1:]
    mant, _, ex = t.lower().partition("e")
    ex = int(ex) if ex else 0
    ip, _, fp = mant.partition(".")
    c = ip + fp
    a = len(c) - len(c.lstrip("0"))
    d = c.strip("0")
    if not d:
        return "0"
    e = len(ip) - 1 - a + ex
    if abs(e) > 999999999:
        return None                     # the grammar refuses it
    return spell(neg, d, e)


def canon_double(x):
    if x != x or x in (float("inf"), float("-inf")):
        raise ValueError(x)
    if x == 0:
        return "0"
    return canon_text(repr(x))


# ------------------------------------------------------------------ tables

def bitlen5(e):
    return (5 ** e).bit_length()


def split(i):
    s = bitlen5(i) - BITS
    return (5 ** i) >> s if s >= 0 else (5 ** i) << -s


def inv(q):
    return (1 << (bitlen5(q) - 1 + BITS)) // (5 ** q) + 1


def mulshr(m, v, s):
    lo, hi = v & M64, v >> 64
    return (((m * lo) >> s) + ((m * hi) << (64 - s))) & ((1 << 128) - 1)


def ranges():
    maxi = maxq = 0
    for ex in range(0, 2047):
        e2 = (1 if ex == 0 else ex) - 1077
        if e2 >= 0:
            maxq = max(maxq, ((e2 * 78913) >> 18) - (e2 > 3))
        else:
            q = ((-e2 * 732923) >> 20) - (-e2 > 1)
            maxi = max(maxi, -e2 - q)
    return maxi, maxq


def tables():
    maxi, maxq = ranges()
    for e in range(0, 3529):
        assert ((e * 1217359) >> 19) + 1 == bitlen5(e)
    corr, icorr = [], []
    for i in range(maxi + 1):
        b, o = divmod(i, STEP)
        a = split(i) if o == 0 else mulshr(5 ** o, split(b * STEP), bitlen5(i) - bitlen5(b * STEP))
        c = split(i) - a
        assert 0 <= c <= 3 and split(i) < 1 << 128
        corr.append(c)
    for q in range(maxq + 1):
        b = (q + STEP - 1) // STEP
        o = b * STEP - q
        if o == 0:
            a = inv(q)
        else:
            a = mulshr(5 ** o, inv(b * STEP) - 1, bitlen5(b * STEP) - bitlen5(q)) + 1
        c = inv(q) - a
        assert 0 <= c <= 3 and inv(q) < 1 << 128
        icorr.append(c)

    def pack(cs):
        out = []
        for k in range(0, len(cs), 4):
            v = 0
            for j, c in enumerate(cs[k:k + 4]):
                v |= c << (2 * j)
            out.append(v)
        return out

    def words(v):
        return "{ 0x%016xu, 0x%016xu }" % (v & M64, v >> 64)

    nb = maxi // STEP + 1
    ni = (maxq + STEP - 1) // STEP + 1
    print("/* tests/adapt/json_dtoa_ref.py tables: i <= %d, q <= %d */" % (maxi, maxq))
    print("static const uint64_t yjs__p5[%d] = {" % STEP)
    for k in range(0, STEP, 4):
        print("    " + ", ".join("%du" % (5 ** j) for j in range(k, min(k + 4, STEP))) + ",")
    print("};")
    print("static const uint64_t yjs__p5_split[%d][2] = {" % nb)
    for b in range(nb):
        print("    " + words(split(b * STEP)) + ",")
    print("};")
    print("static const uint64_t yjs__p5_inv[%d][2] = {" % ni)
    for b in range(ni):
        print("    " + words(inv(b * STEP) - 1) + ",")
    print("};")
    for name, cs in (("yjs__p5_corr", corr), ("yjs__p5_inv_corr", icorr)):
        p = pack(cs)
        print("static const uint8_t %s[%d] = {" % (name, len(p)))
        for k in range(0, len(p), 16):
            print("    " + ", ".join("%d" % v for v in p[k:k + 16]) + ",")
        print("};")
    # a checksum of every multiplier, for json_test.c
    h = 0xcbf29ce484222325
    for i in range(maxi + 1):
        for w in (split(i) & M64, split(i) >> 64):
            h = ((h ^ w) * 0x100000001b3) & M64
    for q in range(maxq + 1):
        for w in (inv(q) & M64, inv(q) >> 64):
            h = ((h ^ w) * 0x100000001b3) & M64
    print("/* multiplier checksum 0x%016x */" % h)


# ------------------------------------------------------------------ stream

class Rng:
    def __init__(self):
        self.s = 0x5953504A534F4E31     # "YSPJSON1"

    def next(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & M64
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        return z ^ (z >> 31)


def from_bits(b):
    return struct.unpack("<d", struct.pack("<Q", b))[0]


def to_bits(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def decimal_born(d, e):
    x = float("%de%d" % (d, e))
    if x == float("inf") or (x == 0 and d != 0):
        return 1.0
    return x


def stream(n):
    """json_test.c's doubles: eight classes in turn."""
    r = Rng()
    for i in range(n):
        c = i % 8
        if c in (0, 1):
            b = r.next()
            if (b >> 52) & 0x7FF == 0x7FF:
                b ^= 1 << 62
        elif c == 2:
            b = r.next() & 0x800FFFFFFFFFFFFF
        elif c in (3, 4):
            k = 1 + r.next() % 17
            d = r.next() % (10 ** k)
            e = r.next() % 650 - 340
            x = decimal_born(d, e)
            b = to_bits(x)
            if c == 4:
                mag = b & ~(1 << 63)
                step = 1 if r.next() & 1 else -1
                if mag == 0:
                    step = 1
                if (mag + step) >> 52 == 0x7FF:
                    step = -1
                b = (b & (1 << 63)) | (mag + step)
            if r.next() & 1:
                b ^= 1 << 63
        elif c == 5:
            p = r.next() % 2098 - 1074
            mag = (p + 1023) << 52 if p >= -1022 else 1 << (p + 1074)
            delta = r.next() % 3 - 1
            if 1 <= mag + delta <= 0x7FEFFFFFFFFFFFFF:
                mag += delta
            b = mag | ((r.next() & 1) << 63)
        elif c == 6:
            m = r.next() >> (11 + r.next() % 53)
            b = to_bits(float(m)) | ((r.next() & 1) << 63)
        else:
            k = 15 + r.next() % 3
            d = 10 ** (k - 1) + r.next() % (9 * 10 ** (k - 1))
            e = r.next() % 650 - 340
            b = to_bits(decimal_born(d, e)) | ((r.next() & 1) << 63)
        yield b


def text_stream(n):
    """json_test.c's number texts: zeros in runs, exponents with leading zeros."""
    r = Rng()
    r.s = 0x5953504A534F4E32            # "YSPJSON2"

    def digit():
        z = r.next()
        return "0" if z % 3 == 0 else str((z >> 8) % 10)

    for _ in range(n):
        t = []
        if r.next() & 1:
            t.append("-")
        n1 = r.next() % 25
        if n1 == 0:
            t.append("0")
        else:
            t.append(str(1 + r.next() % 9))
            t.extend(digit() for _ in range(n1 - 1))
        if r.next() & 1:
            t.append(".")
            t.extend(digit() for _ in range(1 + r.next() % 25))
        if r.next() & 1:
            t.append("e" if r.next() & 1 else "E")
            t.append(("", "+", "-")[r.next() % 3])
            t.append("0" * (r.next() % 3))
            t.append(str(r.next() % 1000000000 if r.next() % 8 == 0 else r.next() % 400))
        yield "".join(t)


def fnv(texts):
    h = 0xcbf29ce484222325
    for t in texts:
        for ch in t.encode() + b"\n":
            h = ((h ^ ch) * 0x100000001b3) & M64
    return h


def digest(n):
    return fnv(canon_double(from_bits(b)) for b in stream(n))


def text_digest(n):
    return fnv(canon_text(t) or "refused" for t in text_stream(n))


EDGES = [
    0.0, -0.0, 5e-324, -5e-324, 1e-323, 2.225073858507201e-308, 2.2250738585072014e-308,
    1.7976931348623157e308, -1.7976931348623157e308, 1.0, -1.0, 0.1, 0.2, 0.3, 0.1 + 0.2, 1.0 / 3.0,
    2.0 / 3.0, 100.0, 1.5, 9007199254740991.0, 9007199254740992.0, 9007199254740994.0,
    9223372036854775808.0, 18446744073709551616.0, 1e15, 1e16, 1e17, 1e20, 1e21, 1e22, 1e23,
    1e-6, 1e-7, 1e-18, 1.5e-18, 9.99e-19, 1e-19, 123456789012345678901.0, 1e300, 1e-300,
    2.98023223876953125e-8, 5.764607523034235e39, 1.152921504606847e40, 2.305843009213694e40,
    -2.109808898695963e16, 4.940656e-318, 1.18575755e-316, 2.989102097996e-312, 9.0608011534336e15,
    4.708356024711512e18, 9.409340012568248e18, 1.2345678, 1.9156918820264798e-56,
    6.6564021122018745e264, 4.91e-6, 5.547e-6, 1.7800590868057611e-307, 2.8480945388892178e-306,
    2.446494580089078e-296, 4.9824154337099686e-307, 0.000001, 123456.789, 0.5, 0.25, 0.125,
    2.0 ** -1022 * 3, 2.0 ** 1023, 2.0 ** 52, 2.0 ** 53 + 2, 0.0015, 1500.0, 1e-9,
]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "tables":
        tables()
    elif cmd == "digest":
        n = int(sys.argv[2])
        print("doubles 0x%016x" % digest(n))
        print("texts   0x%016x" % text_digest(n // 2))
    elif cmd == "edges":
        for x in EDGES:
            print('    { 0x%016xu, "%s" },' % (to_bits(x), canon_double(x)))
    elif cmd == "dump":
        n = int(sys.argv[2])
        with open(sys.argv[3], "w", newline="\n") as f:
            for b in stream(n):
                f.write("%016x %s\n" % (b, canon_double(from_bits(b))))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
