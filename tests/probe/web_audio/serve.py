"""Serves a built web site (examples/web/build.sh) for the sound probe.

    serve.py SITE PORT [--coi] [--strip-sw]

--coi sends COOP and COEP: the page is cross-origin isolated and loads the
AudioWorklet build with no service-worker reload. --strip-sw removes the
coi-serviceworker tag: the page is not isolated and loads the
ScriptProcessorNode build. Every HTML page gets probe.js (beside this
script) as its first script."""
import functools
import http.server
import os
import sys

site, port = sys.argv[1], int(sys.argv[2])
coi = "--coi" in sys.argv
strip = "--strip-sw" in sys.argv
here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "probe.js"), "rb") as f:
    PROBE = f.read()


class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        if coi:
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def send_body(self, body, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/probe.js":
            return self.send_body(PROBE, "text/javascript")
        fp = os.path.join(site, path.lstrip("/"))
        if path.endswith(".html") and os.path.isfile(fp):
            with open(fp, "rb") as f:
                body = f.read()
            if strip:
                body = body.replace(b'<script src="coi-serviceworker.js"></script>', b"")
            body = body.replace(b"<head>", b'<head>\n<script src="probe.js"></script>', 1)
            return self.send_body(body, "text/html; charset=utf-8")
        super().do_GET()

    def log_message(self, *a):
        pass


http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(H, directory=site)).serve_forever()
