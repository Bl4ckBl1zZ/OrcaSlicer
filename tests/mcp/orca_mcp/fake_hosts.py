"""Fake OctoPrint and Moonraker servers for the MCP printer tests.

They listen on 127.0.0.1 only, on a free port, and keep everything they receive in memory so a test can
check what the application uploaded (file name, G-code bytes, form fields, API key). Nothing here talks
to a real printer.

    with FakeOctoPrint(api_key="k") as octo:
        ...                      # configure a physical printer with octo.host / octo.port
        octo.uploads[-1].data    # the G-code the app sent

State the tests can set: `state` ("idle", "printing", "paused", "error", "offline"), `progress`,
`job_name`, temperatures. "offline" makes every request fail with 503.
"""

from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass
from email.parser import BytesParser
from email.policy import HTTP
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


@dataclass
class Upload:
    path: str
    filename: str
    data: bytes
    fields: dict
    headers: dict


@dataclass
class HostState:
    state: str = "idle"          # idle | printing | paused | error | offline
    progress: float = 0.0        # 0..100
    job_name: str = ""
    nozzle: tuple = (24.0, 0.0)  # actual, target
    bed: tuple = (23.0, 0.0)
    time_left_s: int = 0
    layer: tuple = (0, 0)        # current, total
    error_text: str = ""


class _Handler(BaseHTTPRequestHandler):
    server_version = "FakePrinterHost/1.0"

    def log_message(self, fmt, *args):  # keep test output quiet
        self.server.owner.log.append(f"{self.command} {self.path}")

    # -- helpers ---------------------------------------------------------------------------------
    def _json(self, code: int, body) -> None:
        raw = json.dumps(body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _authorised(self) -> bool:
        owner = self.server.owner
        if owner.api_key is None:
            return True
        key = self.headers.get("X-Api-Key") or ""
        if not key:
            auth = self.headers.get("Authorization") or ""
            if auth.lower().startswith("bearer "):
                key = auth[7:]
        if key != owner.api_key:
            self._json(401 if not key else 403, {"error": "Invalid API key"})
            return False
        return True

    def _body(self) -> bytes:
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def _multipart(self, body: bytes):
        """Return (fields, file) where file is an Upload-like (filename, data) tuple or None."""
        ctype = self.headers.get("Content-Type", "")
        msg = BytesParser(policy=HTTP).parsebytes(b"Content-Type: " + ctype.encode("latin-1") + b"\r\n\r\n" + body)
        fields, file_item = {}, None
        if not msg.is_multipart():
            return fields, None
        for part in msg.iter_parts():
            name = part.get_param("name", header="content-disposition")
            filename = part.get_filename()
            payload = part.get_payload(decode=True) or b""
            if filename is not None:
                file_item = (filename, payload)
            elif name:
                fields[name] = payload.decode("utf-8", "replace")
        return fields, file_item

    def _dispatch(self):
        owner = self.server.owner
        owner.requests.append((self.command, self.path, dict(self.headers)))
        if owner.state.state == "offline":
            self._json(503, {"error": "offline"})
            return
        if not self._authorised():
            return
        owner.route(self)

    do_GET = _dispatch
    do_POST = _dispatch
    do_DELETE = _dispatch


class _FakeHost:
    kind = "fake"

    def __init__(self, api_key: str | None = "fake-api-key-123", port: int = 0):
        self.api_key = api_key
        self.state = HostState()
        self.uploads: list[Upload] = []
        self.commands: list[tuple[str, dict]] = []
        self.requests: list[tuple] = []
        self.log: list[str] = []
        self._server = ThreadingHTTPServer(("127.0.0.1", port), _Handler)
        self._server.owner = self
        self._server.daemon_threads = True
        self._thread: threading.Thread | None = None

    @property
    def host(self) -> str:
        return "127.0.0.1"

    @property
    def port(self) -> int:
        return self._server.server_address[1]

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    def start(self):
        self._thread = threading.Thread(target=self._server.serve_forever, name=f"{self.kind}-{self.port}", daemon=True)
        self._thread.start()
        return self

    def stop(self):
        self._server.shutdown()
        self._server.server_close()

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    def wait_for_upload(self, timeout: float = 30.0) -> Upload:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.uploads:
                return self.uploads[-1]
            time.sleep(0.1)
        raise TimeoutError(f"{self.kind}: no upload within {timeout} s; requests: {self.log[-10:]}")

    def route(self, h: _Handler):  # pragma: no cover - overridden
        h._json(404, {"error": "not found"})


class FakeOctoPrint(_FakeHost):
    """The OctoPrint REST API subset OrcaSlicer's OctoPrint host and the MCP status probe use."""

    kind = "octoprint"

    def _state_text(self) -> str:
        return {"idle": "Operational", "printing": "Printing", "paused": "Paused", "error": "Error"}.get(self.state.state, "Operational")

    def route(self, h: _Handler):
        url = urlparse(h.path)
        s = self.state
        if h.command == "GET" and url.path == "/api/version":
            h._json(200, {"api": "0.1", "server": "1.10.2", "text": "OctoPrint 1.10.2"})
        elif h.command == "GET" and url.path == "/api/connection":
            h._json(200, {"current": {"state": self._state_text(), "port": "/dev/ttyACM0", "baudrate": 115200}})
        elif h.command == "GET" and url.path == "/api/printer":
            flags = {"operational": s.state != "error", "printing": s.state == "printing", "paused": s.state == "paused",
                     "error": s.state == "error", "ready": s.state == "idle", "cancelling": False, "pausing": False}
            h._json(200, {"temperature": {"tool0": {"actual": s.nozzle[0], "target": s.nozzle[1]},
                                          "bed": {"actual": s.bed[0], "target": s.bed[1]}},
                          "state": {"text": s.error_text or self._state_text(), "flags": flags}})
        elif h.command == "GET" and url.path == "/api/job":
            h._json(200, {"job": {"file": {"name": s.job_name or None}},
                          "progress": {"completion": s.progress if s.state in ("printing", "paused") else None,
                                       "printTimeLeft": s.time_left_s if s.state in ("printing", "paused") else None},
                          "state": self._state_text()})
        elif h.command == "POST" and url.path == "/api/job":
            body = json.loads(h._body() or b"{}")
            self.commands.append(("job", body))
            cmd = body.get("command")
            if cmd == "pause":
                action = body.get("action", "toggle")
                if action == "resume" or (action == "toggle" and s.state == "paused"):
                    s.state = "printing"
                else:
                    s.state = "paused"
            elif cmd == "cancel":
                s.state = "idle"
                s.progress = 0.0
            h.send_response(204)
            h.end_headers()
        elif h.command == "POST" and url.path.startswith("/api/files/"):
            body = h._body()
            fields, item = h._multipart(body)
            if item is None:
                h._json(400, {"error": "no file"})
                return
            location = url.path[len("/api/files/"):]
            filename, data = item
            name = filename
            if fields.get("path"):
                name = fields["path"].rstrip("/") + "/" + name
            self.uploads.append(Upload(path=location, filename=name, data=data,
                                       fields=fields, headers=dict(h.headers)))
            if str(fields.get("print", "")).lower() == "true":
                s.state, s.job_name, s.progress = "printing", filename, 0.0
            h._json(201, {"done": True, "files": {"local": {"name": filename, "origin": "local"}}})
        else:
            h._json(404, {"error": f"not found: {h.command} {url.path}"})


class FakeMoonraker(_FakeHost):
    """The Moonraker API subset (plus its OctoPrint-compatible version endpoint) used by the tests."""

    kind = "moonraker"

    def _print_state(self) -> str:
        return {"idle": "standby", "printing": "printing", "paused": "paused", "error": "error"}.get(self.state.state, "standby")

    def route(self, h: _Handler):
        url = urlparse(h.path)
        s = self.state
        if h.command == "GET" and url.path == "/api/version":
            h._json(200, {"server": "1.1.0", "api": "0.1", "text": "OctoPrint (Moonraker v0.9.3)"})
        elif h.command == "GET" and url.path == "/server/info":
            h._json(200, {"result": {"klippy_connected": True, "klippy_state": "ready" if s.state != "error" else "error",
                                     "moonraker_version": "v0.9.3"}})
        elif h.command == "GET" and url.path == "/printer/info":
            h._json(200, {"result": {"state": "ready" if s.state != "error" else "error", "state_message": s.error_text or "Printer is ready",
                                     "hostname": "fake-voron", "software_version": "v0.12.0"}})
        elif h.command == "GET" and url.path == "/printer/objects/query":
            total = s.layer[1]
            h._json(200, {"result": {"status": {
                "print_stats": {"state": self._print_state(), "filename": s.job_name, "message": s.error_text,
                                "info": {"current_layer": s.layer[0] if total else None, "total_layer": total or None}},
                "display_status": {"progress": s.progress / 100.0},
                "virtual_sdcard": {"progress": s.progress / 100.0, "is_active": s.state == "printing"},
                "extruder": {"temperature": s.nozzle[0], "target": s.nozzle[1]},
                "heater_bed": {"temperature": s.bed[0], "target": s.bed[1]},
            }, "eventtime": time.time()}})
        elif h.command == "POST" and url.path in ("/server/files/upload", "/api/files/local"):
            body = h._body()
            fields, item = h._multipart(body)
            if item is None:
                h._json(400, {"error": "no file"})
                return
            filename, data = item
            name = filename
            if fields.get("path"):
                name = fields["path"].rstrip("/") + "/" + name
            self.uploads.append(Upload(path=url.path, filename=name, data=data,
                                       fields=fields, headers=dict(h.headers)))
            if str(fields.get("print", "")).lower() == "true":
                s.state, s.job_name, s.progress = "printing", filename, 0.0
            if url.path == "/api/files/local":
                h._json(201, {"done": True, "files": {"local": {"name": filename, "origin": "local"}}})
            else:
                h._json(201, {"item": {"path": name, "root": fields.get("root", "gcodes")}, "action": "create_file",
                              "print_started": s.state == "printing", "print_queued": False})
        elif h.command == "POST" and url.path in ("/printer/print/pause", "/printer/print/resume", "/printer/print/cancel"):
            action = url.path.rsplit("/", 1)[1]
            self.commands.append((action, {}))
            s.state = {"pause": "paused", "resume": "printing", "cancel": "idle"}[action]
            h._json(200, {"result": "ok"})
        else:
            h._json(404, {"error": f"not found: {h.command} {url.path}"})
