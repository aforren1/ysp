"""Runs the filly web probe page in browsers, one run at a time, headed on the
X display, and appends each page's result to OUTDIR/results.jsonl (the
server writes it; serve.py must be running on PORT).

    PLAYWRIGHT_BROWSERS_PATH=... uv run --no-project --with playwright \
        python tests/probe/filly/web/drive.py OUTDIR PORT chromium|firefox-system ARGS...

Each ARGS is one run's program arguments, for example "--mode filly --frames 3600".
chromium is Playwright's build, a new browser per run. firefox-system is
/usr/bin/firefox with a new profile per run; the page reports through the
server, and the browser is ended when the result arrives.
"""

import json
import pathlib
import subprocess
import sys
import tempfile
import time
import urllib.parse

out = pathlib.Path(sys.argv[1]).resolve()
port = int(sys.argv[2])
browser = sys.argv[3]
runs = sys.argv[4:]
results = out / "results.jsonl"


def count():
    return len(results.read_text().splitlines()) if results.exists() else 0


def url(args):
    return f"http://127.0.0.1:{port}/index.html?args={urllib.parse.quote(args)}"


def wait_result(before, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if count() > before:
            return json.loads(results.read_text().splitlines()[-1])
        time.sleep(0.5)
    return None


if browser == "chromium":
    from playwright.sync_api import sync_playwright

    with sync_playwright() as p:
        for args in runs:
            before = count()
            b = p.chromium.launch(headless=False)
            page = b.new_page(viewport={"width": 1100, "height": 900})
            page.on("console", lambda m: None)
            page.goto(url(args))
            r = wait_result(before, 300)
            print(b.version, args, json.dumps(r), flush=True)
            b.close()
            time.sleep(3)
elif browser == "firefox-system":
    for args in runs:
        before = count()
        with tempfile.TemporaryDirectory() as profile:
            # Without the prefs the first run shows its welcome pages over the canvas.
            (pathlib.Path(profile) / "user.js").write_text(
                'user_pref("browser.shell.checkDefaultBrowser", false);\n'
                'user_pref("browser.aboutwelcome.enabled", false);\n'
                'user_pref("datareporting.policy.dataSubmissionEnabled", false);\n'
                'user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);\n'
                'user_pref("browser.startup.homepage_override.mstone", "ignore");\n', newline="\n")
            proc = subprocess.Popen(["/usr/bin/firefox", "--no-remote", "--new-instance", "--profile", profile,
                                     "--width", "1100", "--height", "900", url(args)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            r = wait_result(before, 300)
            print("firefox-system", args, json.dumps(r), flush=True)
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        time.sleep(3)
else:
    sys.exit("browser: chromium or firefox-system")
