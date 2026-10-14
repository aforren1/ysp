#!/bin/sh
# Opens the sound demo (av.html) COUNT times in each browser on each audio
# path and counts the opens with no sound (docs/web.md, "How to check the
# sound at open"). Git Bash, from the repo root, with node, npm and uv on
# PATH and a site built by examples/web/build.sh:
#   sh tests/probe/web_audio/run.sh SITE [COUNT] [--headed] [open.mjs options...]
# BROWSERS (default "firefox chrome") and PATHS (default "fallback worklet")
# choose the runs; the servers use PORT (default 8871) and PORT + 1. Each
# open starts a new browser and takes about 20 s. The RESULT lines go to
# build/web_audio_probe/<browser>_<path>.txt (appended), then summary.py
# prints the silent opens and the counts.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
site=$(cd "${1:?usage: run.sh SITE [COUNT] [options]}" && pwd)
shift
count=10
case "${1:-}" in ''|*[!0-9]*) ;; *) count=$1; shift ;; esac
work="$root/build/web_audio_probe"
mkdir -p "$work"
cp "$here/open.mjs" "$work/"
if [ ! -d "$work/node_modules/puppeteer-core" ]; then
    (cd "$work" && npm install --no-save --silent puppeteer-core) || exit 1
fi
# The worklet build needs an isolated page (headers); the fallback build
# runs on a page that is not (no service worker).
port0=${PORT:-8871}
uv run --no-project python "$here/serve.py" "$site" "$port0" --coi &
s1=$!
uv run --no-project python "$here/serve.py" "$site" $((port0 + 1)) --strip-sw &
s2=$!
trap 'kill $s1 $s2 2>/dev/null' EXIT
sleep 2
for b in ${BROWSERS:-firefox chrome}; do
    for p in ${PATHS:-fallback worklet}; do
        port=$((port0 + 1)); [ "$p" = worklet ] && port=$port0
        i=0
        while [ "$i" -lt "$count" ]; do
            (cd "$work" && timeout 150 node open.mjs "$b" "http://127.0.0.1:$port/av.html" "$@") \
                | grep '^RESULT' >> "$work/${b}_$p.txt" || echo 'RESULT {"exception":"driver timeout"}' >> "$work/${b}_$p.txt"
            i=$((i + 1))
        done
        uv run --no-project python "$here/summary.py" "$work/${b}_$p.txt"
    done
done
