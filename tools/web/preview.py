# Preview the generated HTML without needing to run anything on device

from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import time
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from build_web import get_jinja_env
from mock_data import MOCK

# Routes:
    # GET /             Jinja-rendered shell, rebuilt per request
    # GET /static/...   assets served straight off disk
    # GET /api/state    one JSON snapshot, same keys as state_t
    # GET /events       Server-Sent Events, one frame per mock tick


ROOT = Path(__file__).resolve().parents[2]
TEMPLATE_DIR = ROOT / "templates"
PAGES = {"/": "pages/dashboard.html"}   # URL path -> template
PORT = 8000
MAX_SAMPLES = 64

class PreviewHandler(SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "esp32-preview/1.0"

    def __init__(self, *args, **kwargs):
        # so /static/... resolves under the project root
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def log_message(self, fmt, *args):
        # An SSE connection logs a line per frame forever otherwise.
        if self.path.startswith("/events"):
            return
        super().log_message(fmt, *args)

    # ---- routing --------------------------------------------------------

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in PAGES:
            return self._page(path)
        if path == "/api/state":
            return self._json(MOCK.event())
        if path == "/events":
            return self._events()
        if path.startswith("/static/"):
            return super().do_GET()
        self.send_error(404)

    # ---- handlers -------------------------------------------------------

    def _page(self, path: str) -> None:
        # Fresh env per request, so template edits appear on refresh.
        env = get_jinja_env(str(TEMPLATE_DIR))
        html = env.get_template(PAGES[path]).render()
        self._body(200, html.encode("utf-8"), "text/html; charset=utf-8")

    def _json(self, payload: dict) -> None:
        self._body(
            200,
            json.dumps(payload).encode("utf-8"),
            "application/json",
        )

    def _body(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    def _events(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Accel-Buffering", "no")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        cursor = MOCK.cursor_latest(0)
        seq = MOCK.seq
        try:
            self._chunk(self._frame(MOCK.event(reset=True)))
            while True:
                nxt = MOCK.wait_tick(seq)
                if nxt is None:
                    self._chunk(self._frame(MOCK.event()))   # idle: state-only frame
                    continue
                seq = nxt
                while True:                                  # drain in frame-sized chunks
                    cursor, samples, dropped = MOCK.read_since(cursor, MAX_SAMPLES)
                    if samples or dropped:
                        self._chunk(self._frame(MOCK.event(samples, dropped)))
                    if len(samples) < MAX_SAMPLES:
                        break
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass    # client navigated away; normal
        finally:
            try:
                self._chunk(b"")    # terminating zero-length chunk
            except OSError:
                pass 
    @staticmethod
    def _frame(payload: dict) -> bytes:
        return f"data: {json.dumps(payload)}\n\n".encode("utf-8")

    def _chunk(self, payload: bytes) -> None:
        self.wfile.write(b"%X\r\n" % len(payload) + payload + b"\r\n")
        self.wfile.flush()
def main() -> None:
    MOCK.start()
    httpd = ThreadingHTTPServer(("", PORT), PreviewHandler)
    httpd.daemon_threads = True    # SSE threads never exit on their own
    print(f"Preview on http://localhost:{PORT}  (mock data, Ctrl-C to stop)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping")
    finally:
        httpd.shutdown()
        httpd.server_close()
        MOCK.stop()


if __name__ == "__main__":
    main()
