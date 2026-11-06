#!/bin/sh
# The CoreAudio timing runs of docs/audio.md, "CoreAudio", one condition a
# command. Run from the repository root on a quiet machine, one at a time:
#
#     sh tests/probe/coreaudio/run.sh CONDITION [BUILD_DIR] [OUT_DIR]
#
# BUILD_DIR holds audio_clockstats and audio_schedule (default build-mac),
# OUT_DIR gets the CSV files (default BUILD_DIR/coreaudio). caffeinate keeps the
# display and the system awake: on the test Mac the HDMI monitor's audio
# device went away when its display slept, and no device started while the
# system was in a dark wake.
#
# Conditions (minutes):
#   idle-spk idle-hdmi       10 min, silence, the default 480-frame IO buffer
#   load-spk load-hdmi        3 min, 8 spinning threads and a 60 Hz frame
#                             loop that spins 8 ms a frame
#   p128-spk p128-hdmi        3 min at a 128-frame IO buffer
#   p32-spk                   3 min at a 32-frame IO buffer
#   hog-spk                   3 min in hog mode (desc.exclusive)
#   stall-spk                 1 min, a 30 ms stall in the callback every 10 s
#   sched-spk sched-hdmi      about 25 s: audio_schedule at -40 dBFS (sound)
#   sched-spk-p128 sched-hdmi-p128 sched-spk-hog   the same, as named
set -eu
cond=${1:?condition}
bin=${2:-build-mac}
out=${3:-$bin/coreaudio}
mkdir -p "$out"
here=$(dirname "$0")
spk="Mac mini"
hdmi="M27Q"
cs() {  # name device seconds extra...
    name=$1; dev=$2; secs=$3; shift 3
    caffeinate -d -i "$bin/audio_clockstats" --device "$dev" --seconds "$secs" --csv "$out/$name" "$@" | tee "$out/$name.txt"
    uv run --no-project python "$here/analyze.py" "$out/${name}_ticks.csv" --label "$name"
}
sc() {  # name device extra...
    name=$1; dev=$2; shift 2
    caffeinate -d -i "$bin/audio_schedule" --device "$dev" --db -40 --csv "$out/$name.csv" "$@" | tee "$out/$name.txt"
}
case $cond in
    idle-spk)        cs idle-spk "$spk" 600 ;;
    idle-hdmi)       cs idle-hdmi "$hdmi" 600 ;;
    load-spk)        cs load-spk "$spk" 180 --load 8 --frame-work 8 ;;
    load-hdmi)       cs load-hdmi "$hdmi" 180 --load 8 --frame-work 8 ;;
    p128-spk)        cs p128-spk "$spk" 180 --period 128 ;;
    p128-hdmi)       cs p128-hdmi "$hdmi" 180 --period 128 ;;
    p32-spk)         cs p32-spk "$spk" 180 --period 32 ;;
    hog-spk)         cs hog-spk "$spk" 180 --exclusive --long ;;
    stall-spk)       cs stall-spk "$spk" 60 --stall-at 10 --stall 30 --stall-every 10 ;;
    sched-spk)       sc sched-spk "$spk" ;;
    sched-hdmi)      sc sched-hdmi "$hdmi" ;;
    sched-spk-p128)  sc sched-spk-p128 "$spk" --period 128 ;;
    sched-hdmi-p128) sc sched-hdmi-p128 "$hdmi" --period 128 ;;
    sched-spk-hog)   sc sched-spk-hog "$spk" --exclusive ;;
    *) echo "unknown condition: $cond" >&2; exit 2 ;;
esac
