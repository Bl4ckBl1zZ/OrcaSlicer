#!/usr/bin/env python3
"""Mock Streamable-HTTP MCP server for testing orca-mcp-bridge. Standard library only.

Speaks both protocol eras the OrcaSlicer server is designed to speak:

* modern (2026-07-28): stateless; every request carries _meta protocolVersion and
  clientCapabilities; MCP-Protocol-Version / Mcp-Method / Mcp-Name / Mcp-Param-* headers must
  mirror the body (HeaderMismatch -32020 otherwise); unsupported version -32022; unknown method
  HTTP 404 -32601; server/discover; closing a request's SSE stream is cancellation.
* legacy (2025-03-26 .. 2025-11-25): initialize / notifications/initialized, MCP-Protocol-Version
  on later requests, optional Mcp-Session-Id, notifications/cancelled, optional GET stream,
  optional SSE resumption with Last-Event-ID.

Security checks like the real server: bearer token (401), Origin present (403), Host must be
127.0.0.1:<port> or localhost:<port> (403), POST + application/json.

Used in-process by test_bridge.py (MockServer class) or standalone:
    python mock_server.py --port 13720 --token secret --endpoint-file /tmp/x/mcp/endpoint.json
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODERN = "2026-07-28"
LEGACY_VERSIONS = ["2025-11-25", "2025-06-18", "2025-03-26"]
META_VERSION = "io.modelcontextprotocol/protocolVersion"
META_CAPS = "io.modelcontextprotocol/clientCapabilities"

TOOLS = [
    {"name": "echo", "description": "Echo text back.",
     "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}},
    {"name": "image", "description": "Return an image of size_mb megabytes (raw bytes).",
     "inputSchema": {"type": "object", "properties": {"size_mb": {"type": "number"}}}},
    {"name": "slow", "description": "Wait `seconds`; streams progress when a progressToken is given.",
     "inputSchema": {"type": "object", "properties": {"seconds": {"type": "number"}}}},
    {"name": "sse_multi", "description": "Answers with an SSE stream exercising the SSE grammar.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "fail", "description": "Returns isError.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "drop", "description": "Closes the connection without answering.",
     "inputSchema": {"type": "object", "properties": {"sse": {"type": "boolean"}}}},
    {"name": "region", "description": "Tool whose `region` argument is mirrored into Mcp-Param-Region.",
     "inputSchema": {"type": "object", "properties": {
         "region": {"type": "string", "x-mcp-header": "Region"},
         "opts": {"type": "object", "properties": {"level": {"type": "integer", "x-mcp-header": "Level"}}}}}},
    {"name": "bad_annotation", "description": "Invalid x-mcp-header (on an array item): must be hidden.",
     "inputSchema": {"type": "object", "properties": {
         "list": {"type": "array", "items": {"type": "string", "x-mcp-header": "Item"}}}}},
]


def b64_sentinel_decode(v):
    if v is not None and v.startswith("=?base64?") and v.endswith("?="):
        return base64.b64decode(v[9:-2]).decode("utf-8")
    return v


def image_bytes(size_mb):
    n = int(size_mb * 1024 * 1024)
    block = hashlib.sha256(b"orca").digest() * 64  # 2 KiB pattern
    data = (block * (n // len(block) + 1))[:n]
    return data


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []          # dicts: method, headers, era, verb
        self.cancelled = set()      # request ids (json) cancelled by notification
        self.disconnects = set()    # request ids whose stream the client closed
        self.sessions = set()
        self.deleted_sessions = set()
        self.busy_remaining = 0     # answer the next N POSTs with 429
        self.resume_streams = {}    # stream id -> list of (event id, data)
        self.listen_streams = 0

    def record(self, **kw):
        with self.lock:
            self.requests.append(kw)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MockMcp/1.0"

    def log_message(self, fmt, *args):  # quiet
        if self.server.cfg.get("verbose"):
            sys.stderr.write("mock: " + (fmt % args) + "\n")

    # ------------------------------------------------------------------ helpers
    @property
    def cfg(self):
        return self.server.cfg

    @property
    def st(self):
        return self.server.state

    def hdr(self, name):
        return self.headers.get(name)

    def send_json(self, status, obj, extra_headers=None):
        body = json.dumps(obj).encode("utf-8") if obj is not None else b""
        self.send_response(status)
        if obj is not None:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for k, v in (extra_headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)
        self.close_connection = True

    def rpc_error(self, status, rid, code, message, data=None):
        err = {"code": code, "message": message}
        if data is not None:
            err["data"] = data
        msg = {"jsonrpc": "2.0", "error": err}
        if rid is not None:
            msg["id"] = rid
        self.send_json(status, msg)

    def start_sse(self, chunked=True, extra_headers=None):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Accel-Buffering", "no")
        self.send_header("Connection", "close")
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        for k, v in (extra_headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self._chunked = chunked
        self.close_connection = True

    def sse_raw(self, text):
        data = text.encode("utf-8")
        if self._chunked:
            self.wfile.write(b"%x\r\n" % len(data) + data + b"\r\n")
        else:
            self.wfile.write(data)
        self.wfile.flush()

    def sse_event(self, obj, event_id=None):
        text = ""
        if event_id is not None:
            text += "id: %s\n" % event_id
        text += "data: " + json.dumps(obj) + "\n\n"
        self.sse_raw(text)

    def sse_end(self):
        if self._chunked:
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()

    def check_security(self):
        auth = self.hdr("Authorization")
        if self.cfg["token"] and auth != "Bearer " + self.cfg["token"]:
            self.send_json(401, None, {"WWW-Authenticate": "Bearer"})
            return False
        if self.hdr("Origin") is not None:
            self.rpc_error(403, None, -32600, "Origin not allowed")
            return False
        port = self.server.server_address[1]
        if self.hdr("Host") not in ("127.0.0.1:%d" % port, "localhost:%d" % port):
            self.rpc_error(403, None, -32600, "Host not allowed: %s" % self.hdr("Host"))
            return False
        return True

    # ------------------------------------------------------------------ verbs
    def do_GET(self):
        self.st.record(verb="GET", headers=dict(self.headers), method=None, era=None)
        if not self.check_security():
            return
        last = self.hdr("Last-Event-ID")
        if last and self.cfg.get("legacy_resume"):
            stream_id = last.split(":")[0]
            with self.st.lock:
                events = self.st.resume_streams.pop(stream_id, None)
            if events is None:
                self.send_json(404, None)
                return
            self.start_sse()
            for eid, obj in events:
                if eid <= last:
                    continue
                self.sse_event(obj, eid)
            self.sse_end()
            return
        if self.cfg.get("legacy_get_stream") and self.cfg["era"] != "modern":
            with self.st.lock:
                self.st.listen_streams += 1
            self.start_sse()
            try:
                self.sse_event({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
                for _ in range(100):
                    time.sleep(0.05)
                    self.sse_raw(": keep-alive\n\n")
                self.sse_end()
            except OSError:
                pass
            return
        self.send_json(405, None, {"Allow": "POST"})

    def do_DELETE(self):
        self.st.record(verb="DELETE", headers=dict(self.headers), method=None, era=None)
        if not self.check_security():
            return
        sid = self.hdr("Mcp-Session-Id")
        if self.cfg.get("sessions") and sid in self.st.sessions:
            with self.st.lock:
                self.st.sessions.discard(sid)
                self.st.deleted_sessions.add(sid)
            self.send_json(200, None)
            return
        self.send_json(405, None)

    def do_POST(self):
        length = int(self.hdr("Content-Length") or 0)
        raw = self.rfile.read(length)
        try:
            msg = json.loads(raw.decode("utf-8"))
        except Exception:
            msg = None
        method = msg.get("method") if isinstance(msg, dict) else None
        params = (msg.get("params") or {}) if isinstance(msg, dict) else {}
        meta = params.get("_meta") or {} if isinstance(params, dict) else {}
        era = "modern" if META_VERSION in meta else "legacy"
        self.st.record(verb="POST", headers=dict(self.headers), method=method, era=era, body=msg)
        if not self.check_security():
            return
        with self.st.lock:
            if self.st.busy_remaining > 0:
                self.st.busy_remaining -= 1
                busy = True
            else:
                busy = False
        if busy:
            self.send_json(429, None, {"Retry-After": "0"})
            return
        if (self.hdr("Content-Type") or "").split(";")[0].strip() != "application/json":
            self.rpc_error(415, None, -32600, "Content-Type must be application/json")
            return
        accept = self.hdr("Accept") or ""
        if "application/json" not in accept or "text/event-stream" not in accept:
            self.rpc_error(406, None, -32600, "Accept must list application/json and text/event-stream")
            return
        if not isinstance(msg, dict):
            self.rpc_error(400, None, -32700, "Parse error")
            return
        rid = msg.get("id")
        is_request = "id" in msg and method is not None
        if method is None:
            # a client response to a server request (legacy)
            self.send_json(202, None)
            return
        if era == "modern":
            self.handle_modern(msg, rid, method, params, meta, is_request)
        else:
            self.handle_legacy(msg, rid, method, params, is_request)

    # ------------------------------------------------------------------ modern
    def handle_modern(self, msg, rid, method, params, meta, is_request):
        if self.cfg["era"] == "legacy":
            # A legacy-only server that does not know server/discover etc. before initialize.
            self.rpc_error(400, None, -32000, "Bad Request: Server not initialized")
            return
        version = meta.get(META_VERSION)
        hv = self.hdr("MCP-Protocol-Version")
        if hv is None or hv != version:
            self.rpc_error(400, rid, -32020, "Header mismatch: MCP-Protocol-Version %r vs body %r" % (hv, version))
            return
        if self.hdr("Mcp-Method") != method:
            self.rpc_error(400, rid, -32020, "Header mismatch: Mcp-Method %r vs body %r" % (self.hdr("Mcp-Method"), method))
            return
        if self.hdr("Mcp-Session-Id") is not None:
            self.rpc_error(400, rid, -32600, "modern requests must not carry Mcp-Session-Id")
            return
        if method in ("tools/call", "prompts/get", "resources/read"):
            want = params.get("uri" if method == "resources/read" else "name")
            got = b64_sentinel_decode(self.hdr("Mcp-Name"))
            if got != want:
                self.rpc_error(400, rid, -32020, "Header mismatch: Mcp-Name %r vs body %r" % (got, want))
                return
        if not is_request:
            self.send_json(202, None)
            return
        if version != MODERN:
            self.rpc_error(400, rid, -32022, "Unsupported protocol version",
                           {"supported": [MODERN] + (LEGACY_VERSIONS if self.cfg["era"] == "dual" else []),
                            "requested": version})
            return
        if META_CAPS not in meta:
            self.rpc_error(400, rid, -32602, "Invalid params: missing clientCapabilities")
            return
        if method == "server/discover":
            self.send_json(200, {"jsonrpc": "2.0", "id": rid, "result": {
                "resultType": "complete",
                "supportedVersions": [MODERN] + (LEGACY_VERSIONS if self.cfg["era"] == "dual" else []),
                "capabilities": {"tools": {}}, "instructions": "mock", "ttlMs": 3600000, "cacheScope": "private",
                "_meta": {"io.modelcontextprotocol/serverInfo": {"name": "mock", "version": "1"}}}})
            return
        if method == "tools/list":
            self.send_json(200, {"jsonrpc": "2.0", "id": rid, "result": {
                "resultType": "complete", "tools": TOOLS, "ttlMs": 3600000, "cacheScope": "private"}})
            return
        if method == "subscriptions/listen":
            self.start_sse()
            try:
                self.sse_event({"jsonrpc": "2.0", "method": "notifications/subscriptions/acknowledged",
                                "params": {"_meta": {"io.modelcontextprotocol/subscriptionId": rid},
                                           "notifications": {"toolsListChanged": True}}})
                for i in range(600):
                    time.sleep(0.05)
                    self.sse_raw(":\n\n")
                    if i == 4:
                        self.sse_event({"jsonrpc": "2.0", "method": "notifications/tools/list_changed",
                                        "params": {"_meta": {"io.modelcontextprotocol/subscriptionId": rid}}})
                self.sse_end()
            except OSError:
                with self.st.lock:
                    self.st.disconnects.add(json.dumps(rid))
            return
        if method == "tools/call":
            # x-mcp-header validation (Region, opts.Level)
            if params.get("name") == "region":
                args = params.get("arguments") or {}
                for hname, val in (("Mcp-Param-Region", args.get("region")),
                                   ("Mcp-Param-Level", (args.get("opts") or {}).get("level"))):
                    got = b64_sentinel_decode(self.hdr(hname))
                    exp = None if val is None else (str(val).lower() if isinstance(val, bool) else str(val))
                    if got != exp:
                        self.rpc_error(400, rid, -32020, "Header mismatch: %s %r vs body %r" % (hname, got, exp))
                        return
            self.call_tool(rid, params, modern=True)
            return
        self.rpc_error(404, rid, -32601, "Method not found: %s" % method)

    # ------------------------------------------------------------------ legacy
    def handle_legacy(self, msg, rid, method, params, is_request):
        if self.cfg["era"] == "modern":
            # Modern-only server: the legacy request lacks the required headers / _meta.
            if self.hdr("MCP-Protocol-Version") is None:
                self.rpc_error(400, rid, -32020, "Header mismatch: missing MCP-Protocol-Version")
            else:
                self.rpc_error(400, rid, -32602, "Invalid params: missing _meta protocolVersion")
            return
        if method == "initialize":
            requested = params.get("protocolVersion")
            negotiated = requested if requested in LEGACY_VERSIONS else LEGACY_VERSIONS[0]
            headers = {}
            if self.cfg.get("sessions"):
                sid = uuid.uuid4().hex
                with self.st.lock:
                    self.st.sessions.add(sid)
                headers["Mcp-Session-Id"] = sid
            self.server.negotiated = negotiated
            body = {"jsonrpc": "2.0", "id": rid, "result": {
                "protocolVersion": negotiated, "capabilities": {"tools": {}},
                "serverInfo": {"name": "mock", "version": "1"}, "instructions": "mock"}}
            data = json.dumps(body).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            for k, v in headers.items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(data)
            return
        # every later request: version header and session
        hv = self.hdr("MCP-Protocol-Version")
        if hv is not None and hv not in LEGACY_VERSIONS + [MODERN]:
            self.rpc_error(400, rid, -32600, "Unsupported MCP-Protocol-Version %s" % hv)
            return
        if self.cfg.get("require_version_header") and hv != getattr(self.server, "negotiated", None):
            self.rpc_error(400, rid, -32600, "MCP-Protocol-Version %r does not match the negotiated version" % hv)
            return
        if self.cfg.get("sessions"):
            sid = self.hdr("Mcp-Session-Id")
            if not sid:
                self.rpc_error(400, rid, -32600, "Bad Request: missing session id")
                return
            if sid not in self.st.sessions:
                self.send_json(404, None)
                return
        if not is_request:
            if method == "notifications/cancelled":
                with self.st.lock:
                    self.st.cancelled.add(json.dumps((params or {}).get("requestId")))
            self.send_json(202, None)
            return
        if method == "ping":
            self.send_json(200, {"jsonrpc": "2.0", "id": rid, "result": {}})
            return
        if method == "tools/list":
            self.send_json(200, {"jsonrpc": "2.0", "id": rid, "result": {"tools": TOOLS}})
            return
        if method == "tools/call":
            self.call_tool(rid, params, modern=False)
            return
        self.rpc_error(200, rid, -32601, "Method not found: %s" % method)

    # ------------------------------------------------------------------ tools
    def result(self, rid, content, modern, is_error=False):
        r = {"content": content}
        if is_error:
            r["isError"] = True
        if modern:
            r["resultType"] = "complete"
        return {"jsonrpc": "2.0", "id": rid, "result": r}

    def call_tool(self, rid, params, modern):
        name = params.get("name")
        args = params.get("arguments") or {}
        token = (params.get("_meta") or {}).get("progressToken")
        if name == "echo":
            self.send_json(200, self.result(rid, [{"type": "text", "text": args.get("text", "")}], modern))
        elif name == "image":
            data = image_bytes(float(args.get("size_mb", 1)))
            content = [{"type": "text", "text": hashlib.sha256(data).hexdigest()},
                       {"type": "image", "mimeType": "image/png", "data": base64.b64encode(data).decode()}]
            if token is not None:
                self.start_sse()
                self.sse_event({"jsonrpc": "2.0", "method": "notifications/progress",
                                "params": {"progressToken": token, "progress": 50, "total": 100}})
                self.sse_event(self.result(rid, content, modern))
                self.sse_end()
            else:
                self.send_json(200, self.result(rid, content, modern))
        elif name == "slow":
            seconds = float(args.get("seconds", 1))
            if token is not None:
                self.slow_sse(rid, token, seconds, modern)
            else:
                end = time.time() + seconds
                while time.time() < end:
                    time.sleep(0.02)
                    with self.st.lock:
                        if json.dumps(rid) in self.st.cancelled:
                            return  # legacy cancel: no response
                self.send_json(200, self.result(rid, [{"type": "text", "text": "slept %g" % seconds}], modern))
        elif name == "sse_multi":
            self.start_sse(chunked=bool(self.cfg.get("chunked", True)))
            pretty = json.dumps({"jsonrpc": "2.0", "method": "notifications/message",
                                 "params": {"level": "info", "data": "multi\nline"}}, indent=2)
            text = ": a comment line\r\n"
            text += "retry: 1500\r\n"
            text += "id: e1\r\n"
            text += "".join("data: %s\r\n" % ln for ln in pretty.split("\n"))
            text += "\r\n"
            text += "event: message\nid: e2\ndata:" + json.dumps({"jsonrpc": "2.0", "method": "notifications/progress",
                                                                    "params": {"progressToken": "t", "progress": 1}}) + "\n\n"
            text += "event: custom-ignored\ndata: {\"not\": \"forwarded\"}\n\n"
            text += "data\n\n"  # field with no colon: empty data -> ignored
            # final response split across two writes in the middle of a line
            final = "data: " + json.dumps(self.result(rid, [{"type": "text", "text": "done ✓ ünïcode"}], modern)) + "\n\n"
            self.sse_raw(text + final[:20])
            time.sleep(0.05)
            self.sse_raw(final[20:])
            self.sse_end()
        elif name == "fail":
            self.send_json(200, self.result(rid, [{"type": "text", "text": "boom"}], modern, is_error=True))
        elif name == "drop":
            if args.get("sse"):
                self.start_sse()
                self.sse_event({"jsonrpc": "2.0", "method": "notifications/progress",
                                "params": {"progressToken": token or 0, "progress": 1}})
            self.connection.shutdown(socket.SHUT_RDWR)
            self.close_connection = True
        elif name == "region":
            self.send_json(200, self.result(rid, [{"type": "text", "text": "region=%s" % args.get("region")}], modern))
        else:
            if modern:
                self.rpc_error(200, rid, -32602, "Unknown tool: %s" % name)
            else:
                self.rpc_error(200, rid, -32602, "Unknown tool: %s" % name)

    def slow_sse(self, rid, token, seconds, modern):
        steps = max(1, int(seconds / 0.2))
        resume = self.cfg.get("legacy_resume") and not modern
        stream_id = uuid.uuid4().hex[:8]
        events = []
        for i in range(steps):
            events.append(("%s:%04d" % (stream_id, i + 1), {"jsonrpc": "2.0", "method": "notifications/progress",
                                                            "params": {"progressToken": token, "progress": int(100 * (i + 1) / steps),
                                                                       "total": 100, "message": "step %d" % (i + 1)}}))
        events.append(("%s:%04d" % (stream_id, steps + 1),
                       self.result(rid, [{"type": "text", "text": "slept %g" % seconds}], modern)))
        self.start_sse()
        try:
            if resume:
                # 2025-11-25 polling: priming event, a couple of events, then close the
                # connection (retry 100 ms) and let the client resume with Last-Event-ID.
                self.sse_raw("id: %s:0000\ndata:\n\n" % stream_id)
                cut = min(2, len(events) - 1)
                for eid, obj in events[:cut]:
                    self.sse_event(obj, eid)
                with self.st.lock:
                    self.st.resume_streams[stream_id] = events
                self.sse_raw("retry: 100\n\n")
                self.sse_end()
                return
            for i, (eid, obj) in enumerate(events[:-1]):
                for _ in range(4):
                    time.sleep(0.05)
                    self.sse_raw(": keep-alive\n\n")
                    with self.st.lock:
                        if json.dumps(rid) in self.st.cancelled:
                            return
                self.sse_event(obj, eid)
            self.sse_event(events[-1][1], events[-1][0])
            self.sse_end()
        except OSError:
            with self.st.lock:
                self.st.disconnects.add(json.dumps(rid))


class MockServer:
    """In-process server; start()/stop()/restart() keep the same port unless told otherwise."""

    def __init__(self, token="test-token", era="dual", port=0, **cfg):
        self.cfg = dict(token=token, era=era, **cfg)
        self.state = State()
        self.port = port
        self.httpd = None
        self.thread = None

    def start(self, port=None):
        if port is not None:
            self.port = port
        self.httpd = ThreadingHTTPServer(("127.0.0.1", self.port), Handler)
        self.httpd.daemon_threads = True
        self.httpd.cfg = self.cfg
        self.httpd.state = self.state
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()
        return self

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
            self.httpd = None

    @property
    def url(self):
        return "http://127.0.0.1:%d/mcp" % self.port

    def write_endpoint_file(self, path, token=None, pid=None, url=None):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        doc = {"schema": 1, "url": url or self.url, "token": token if token is not None else self.cfg["token"],
               "pid": pid if pid is not None else os.getpid(),
               "protocolVersions": [MODERN] + LEGACY_VERSIONS, "app": "OrcaSlicer", "appVersion": "mock"}
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(doc, f)
        os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=13720)
    ap.add_argument("--token", default="test-token")
    ap.add_argument("--era", choices=["dual", "modern", "legacy"], default="dual")
    ap.add_argument("--sessions", action="store_true")
    ap.add_argument("--endpoint-file")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    srv = MockServer(token=a.token, era=a.era, port=a.port, sessions=a.sessions, verbose=a.verbose).start()
    if a.endpoint_file:
        srv.write_endpoint_file(a.endpoint_file)
    print("mock MCP server on %s (era %s)" % (srv.url, a.era), file=sys.stderr)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        if a.endpoint_file and os.path.exists(a.endpoint_file):
            os.remove(a.endpoint_file)
        srv.stop()


if __name__ == "__main__":
    main()
