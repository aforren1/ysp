"""Static server for the filly web probe, with COOP and COEP (a cross-origin
isolated page gets the finer performance.now()), and two POST paths that a
page uses to report without a driver: /log (one line) and /result (JSON,
appended to OUTDIR/results.jsonl).

    uv run --no-project python tests/probe/filly/web/serve.py SITE OUTDIR [PORT]
"""

import http.server
import pathlib
import sys

site = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
port = int(sys.argv[3]) if len(sys.argv) > 3 else 8766
out.mkdir(parents=True, exist_ok=True)


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".wasm": "application/wasm", ".js": "text/javascript"}

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(site), **kwargs)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode("utf-8", "replace")
        name = "results.jsonl" if self.path == "/result" else "log.txt"
        with open(out / name, "a", encoding="utf-8", newline="\n") as f:
            f.write(body.rstrip("\n") + "\n")
        self.send_response(204)
        self.end_headers()

    def log_message(self, *args):
        pass


http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
