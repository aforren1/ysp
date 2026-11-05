#!/bin/sh
# serial_macos.sh - the serial and device timing set on macOS, against the
# echo board (firmware/ysp_echo, version 2). One command builds every tool,
# checks the board and runs, in this order:
#
#   1. board_check   functional checks, no timing (stops the run on a failure)
#   2. raw_posix     round trip and stream: the termios floor (block, select,
#                    kqueue) and ysp/serial.h in C; two runs, the second in
#                    the reverse mode order
#   3. serial_bench  MATLAB: the ysp_serial MEX binding and serialport (and
#                    IOPort if it is on the MATLAB path); two runs, the
#                    second in the reverse API order
#   4. pulse_bench   ysp/serial.h pulses against the raw ioctl() and write()
#                    floor: 7 modes, widths 1, 2, 5 and 10 ms, 200 pulses a
#                    cell; two runs, the second reversed
#   5. device_pulse  ysp/device.h host-timed pulses (TriggerBox byte, LINES
#                    DTR and RTS), widths 1 and 5 ms, 200 pulses a cell; two
#                    runs, the second reversed
#   6. pulse_stats   summaries of 4 and 5, per run and pooled
#
# Usage, from the repository root, on a quiet machine (CLAUDE.md: run nothing
# else while measuring):
#
#   sh tests/compare/serial_macos.sh [quick] [port=/dev/cu.usbmodemNNN] [out=DIR] [skip=matlab,...]
#
# quick runs every step with small counts, for a functional check; its
# numbers are not results. Without port=, the one /dev/cu.usbmodem* is used.
# skip= takes step names (raw, matlab, pulse, device). Everything goes to
# out= (default build/serial-macos/<date-time>/): conditions.txt (machine,
# board, versions, the load before and after each step), log.txt, and each
# step's folder.
#
# MATLAB: /Applications/MATLAB_R2026b.app unless MATLAB= names the binary.
# With only the Command Line Tools, MATLAB's mex refuses the compiler (no
# Xcode license record); the script then writes a copy of the options file
# without that check and builds with it (bindings/mex/README.md, Build).
#
# Public domain / MIT-0, as the rest of ysp.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
QUICK=0
PORT=
OUT=
SKIP=
for a in "$@"; do
    case $a in
        quick) QUICK=1 ;;
        port=*) PORT=${a#port=} ;;
        out=*) OUT=${a#out=} ;;
        skip=*) SKIP=${a#skip=} ;;
        *) echo "serial_macos: unknown argument $a" >&2; exit 2 ;;
    esac
done
MATLAB=${MATLAB:-/Applications/MATLAB_R2026b.app/bin/matlab}

if [ -z "$PORT" ]; then
    set -- /dev/cu.usbmodem*
    if [ "$#" -ne 1 ] || [ ! -e "$1" ]; then
        echo "serial_macos: give port=: found $* " >&2
        exit 2
    fi
    PORT=$1
fi
if [ -z "$OUT" ]; then
    OUT=$ROOT/build/serial-macos/$(date +%Y%m%d-%H%M%S)$( [ $QUICK = 1 ] && echo -quick || true )
fi
mkdir -p "$OUT"
BIN=$ROOT/build/serial-macos/bin
mkdir -p "$BIN"
LOG=$OUT/log.txt
COND=$OUT/conditions.txt

skipped() { case ",$SKIP," in *",$1,"*) return 0 ;; esac; return 1; }
say() { echo "serial_macos: $*" | tee -a "$LOG"; }
load_note() {
    {
        echo "load $1: $(date +%H:%M:%S) loadavg $(sysctl -n vm.loadavg)"
        ps -A -o %cpu=,comm= | sort -rn | head -5 | sed 's/^/    /'
    } >> "$COND"
}

T_START=$(date +%s)
{
    echo "date: $(date '+%Y-%m-%d %H:%M:%S %Z')"
    [ $QUICK = 1 ] && echo "mode: quick (functional; the numbers are not results)"
    echo "machine: $(sysctl -n hw.model), $(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) cores"
    echo "OS: macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $(uname -m)"
    echo "power: $(pmset -g batt | head -1)"
    echo "compiler: $(cc --version | head -1)"
    echo "tree: $(git -C "$ROOT" rev-parse --short HEAD)$(git -C "$ROOT" diff --quiet HEAD -- include tests bindings || echo ' (with local changes)')"
    echo "headers: $(for h in serial device rt; do head -1 "$ROOT/include/ysp/$h.h" | sed 's|^/\* \(ysp/[a-z]*\.h - v[0-9.]*\).*|\1|'; done | tr '\n' ' ')"
    echo "port: $PORT"
    echo "board (ioreg):"
    ioreg -p IOUSB -l -w 0 -r -n "USB Serial" | grep -E '"(USB Product Name|idVendor|idProduct|USB Serial Number|bcdDevice|Device Speed|locationID)"' | sed 's/^[ |]*/    /'
    echo "driver: $(ioreg -r -c IOSerialBSDClient -w 0 -t | grep -o 'AppleUSB[A-Za-z]*Data' | sort -u | tr '\n' ' ')"
} > "$COND"
load_note start
if [ "$(sysctl -n vm.loadavg | awk '{print ($2 > 2.0)}')" = 1 ]; then
    say "WARNING: 1-minute load above 2: the machine is not quiet"
fi

# --- build -------------------------------------------------------------------
CFLAGS="-O2 -Wall -Wextra -I$ROOT/include"
say "building the C tools in $BIN"
cc $CFLAGS -o "$BIN/board_check" "$ROOT/tests/compare/serial_pulse/board_check.c" -lpthread
cc $CFLAGS -o "$BIN/raw_posix" "$ROOT/tests/compare/serial_bench/raw_posix.c" -lpthread -lm
cc $CFLAGS -o "$BIN/pulse_bench" "$ROOT/tests/compare/serial_pulse/pulse_bench.c" -lpthread
cc $CFLAGS -o "$BIN/device_pulse" "$ROOT/tests/compare/serial_pulse/device_pulse.c" -lpthread

if ! skipped matlab; then
    if [ ! -x "$MATLAB" ]; then
        say "no MATLAB at $MATLAB: skipping step 3"
        SKIP="$SKIP,matlab"
    else
        if [ -z "${YSP_MEXOPTS:-}" ]; then
            XML=$(dirname "$MATLAB")/maca64/mexopts/clang_maca64.xml
            if [ -f "$XML" ] && ! defaults read com.apple.dt.Xcode IDEXcodeVersionForAgreedToGMLicense >/dev/null 2>&1; then
                sed '/<XCODE_AGREED_VERSION>/,/<\/XCODE_AGREED_VERSION>/d' "$XML" > "$BIN/clang_maca64_clt.xml"
                YSP_MEXOPTS=$BIN/clang_maca64_clt.xml
                export YSP_MEXOPTS
            fi
        fi
        say "building ysp_serial.mexmaca64${YSP_MEXOPTS:+ with $YSP_MEXOPTS}"
        (cd "$ROOT/bindings/mex" && "$MATLAB" -batch "build('serial')") >> "$LOG" 2>&1
        echo "MATLAB: $("$MATLAB" -batch "disp(version)" 2>/dev/null | tail -1)" >> "$COND"
    fi
fi

# --- 1. functional checks ----------------------------------------------------
say "1. board_check"
if ! "$BIN/board_check" "$PORT" > "$OUT/board_check.txt" 2>&1; then
    cat "$OUT/board_check.txt" | tee -a "$LOG"
    say "board_check failed: no timed run"
    exit 1
fi
tail -1 "$OUT/board_check.txt" | tee -a "$LOG"

# --- 2. the round trip floor and ysp/serial.h in C -----------------------------
if ! skipped raw; then
    N=1000; FL=
    [ $QUICK = 1 ] && N=100 && FL=rt
    say "2. raw_posix run 1"
    load_note raw1
    "$BIN/raw_posix" "$PORT" "$OUT/raw1" $N ${FL:-fwd} > /dev/null
    say "2. raw_posix run 2 (reversed)"
    load_note raw2
    "$BIN/raw_posix" "$PORT" "$OUT/raw2" $N ${FL}rev > /dev/null
fi

# --- 3. MATLAB -----------------------------------------------------------------
if ! skipped matlab; then
    Q=
    [ $QUICK = 1 ] && Q=", 'n_clock', 1000, 'n_floor', 1000, 'n_enum', 10, 'n_open_absent', 5, 'n_open', 3, 'n_calls', 100, 'n_blocking', 20, 'n_read_waiting', 20, 'n_rt', 50, 'n_probe', 30, 'stream_s', 2"
    for run in 1 2; do
        if [ $run = 1 ]; then APIS="{'serialport', 'ioport', 'ioport_tuned', 'ysp'}"; else APIS="{'ysp', 'ioport_tuned', 'ioport', 'serialport'}"; fi
        say "3. serial_bench run $run"
        load_note matlab$run
        (cd "$ROOT/tests/compare/serial_bench" && "$MATLAB" -batch \
            "serial_bench('port', '$PORT', 'device', 'echo', 'apis', $APIS, 'out', '$OUT/matlab$run', 'label', 'Teensy 4.0 echo, macOS, run $run'$Q)") \
            > "$OUT/matlab$run.txt" 2>&1 || say "serial_bench run $run failed: see $OUT/matlab$run.txt"
    done
fi

# --- 4. ysp/serial.h pulses ----------------------------------------------------
MODES=ysp_pulse,ysp_pulse_async,ysp_dtr,ysp_rts,raw_byte,raw_dtr,raw_rts
if ! skipped pulse; then
    N=200; W=1,2,5,10
    [ $QUICK = 1 ] && N=20 && W=5
    say "4. pulse_bench run 1"
    load_note pulse1
    "$BIN/pulse_bench" "$PORT" "$OUT/pulse1" $N modes=$MODES widths=$W 2>> "$LOG"
    say "4. pulse_bench run 2 (reversed)"
    load_note pulse2
    "$BIN/pulse_bench" "$PORT" "$OUT/pulse2" $N rev modes=$MODES widths=$W 2>> "$LOG"
fi

# --- 5. ysp/device.h pulses ----------------------------------------------------
if ! skipped device; then
    N=200; W=1,5
    [ $QUICK = 1 ] && N=20 && W=5
    say "5. device_pulse run 1"
    load_note device1
    "$BIN/device_pulse" "$PORT" "$OUT/device1" $N widths=$W 2>> "$LOG"
    say "5. device_pulse run 2 (reversed)"
    load_note device2
    "$BIN/device_pulse" "$PORT" "$OUT/device2" $N rev widths=$W 2>> "$LOG"
fi
load_note end

# --- 6. summaries --------------------------------------------------------------
PY="uv run --no-project python"
for d in pulse1 pulse2 device1 device2; do
    [ -d "$OUT/$d" ] && $PY "$ROOT/tests/compare/serial_pulse/pulse_stats.py" "$OUT/$d" > /dev/null
done
if [ -d "$OUT/pulse1" ] && [ -d "$OUT/pulse2" ]; then
    mkdir -p "$OUT/pulse12"
    $PY "$ROOT/tests/compare/serial_pulse/pulse_stats.py" "$OUT/pulse12" "$OUT/pulse1" "$OUT/pulse2" > /dev/null
fi
if [ -d "$OUT/device1" ] && [ -d "$OUT/device2" ]; then
    mkdir -p "$OUT/device12"
    $PY "$ROOT/tests/compare/serial_pulse/pulse_stats.py" "$OUT/device12" "$OUT/device1" "$OUT/device2" > /dev/null
fi
T_END=$(date +%s)
echo "duration: $(( (T_END - T_START) / 60 )) min $(( (T_END - T_START) % 60 )) s" >> "$COND"
say "done in $(( (T_END - T_START) / 60 )) min $(( (T_END - T_START) % 60 )) s: $OUT"
