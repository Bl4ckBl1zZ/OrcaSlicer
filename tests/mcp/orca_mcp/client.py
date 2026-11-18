"""A small MCP client for the OrcaSlicer server tests: Streamable HTTP, both protocol eras.

Standard library only. It deliberately exposes the wire (headers, status codes, raw bodies) so the
tests can also send what a well-behaved SDK never would.

    modern  2026-07-28: stateless; every request carries _meta and the mirrored HTTP headers.
    legacy  2025-11-25 (or older): initialize handshake first, then plain JSON-RPC.
"""

from __future__ import annotations

import base64
import http.client
import itertools
import json
from dataclasses import dataclass, field
from urllib.parse import urlparse

MODERN_VERSION = "2026-07-28"
LEGACY_VERSION = "2025-11-25"

META_VERSION = "io.modelcontextprotocol/protocolVersion"
META_CLIENT_INFO = "io.modelcontextprotocol/clientInfo"
META_CLIENT_CAPS = "io.modelcontextprotocol/clientCapabilities"
META_SERVER_INFO = "io.modelcontextprotocol/serverInfo"

CLIENT_INFO = {"name": "orca-mcp-tests", "version": "1.0"}


class RpcError(Exception):
    """A JSON-RPC error response (or an HTTP error without one)."""

    def __init__(self, status: int, error: dict | None, body: str = ""):
        self.status = status
        self.code = (error or {}).get("code")
        self.message = (error or {}).get("message", "")
        self.data = (error or {}).get("data")
        self.body = body
        super().__init__(f"HTTP {status}, JSON-RPC {self.code}: {self.message or body[:200]}")


class ToolFailure(Exception):
    """A tool result with isError: the {error: {code, message, hint, details}} body."""

    def __init__(self, tool: str, payload: dict):
        error = payload.get("error", {}) if isinstance(payload, dict) else {}
        self.tool = tool
        self.payload = payload
        self.code = error.get("code")
        self.message = error.get("message", "")
        self.hint = error.get("hint", "")
        self.details = error.get("details")
        super().__init__(f"{tool} failed: {self.code}: {self.message}")


@dataclass
class HttpReply:
    status: int
    headers: dict
    body: bytes

    @property
    def text(self) -> str:
        return self.body.decode("utf-8", "replace")

    def json(self):
        return json.loads(self.body)

    @property
    def content_type(self) -> str:
        return self.headers.get("content-type", "")


@dataclass
class ToolResult:
    data: dict | list | None          # the JSON document of the text block
    is_error: bool
    images: list = field(default_factory=list)      # [(mime_type, bytes)]
    progress: list = field(default_factory=list)    # notifications/progress params, in order
    raw: dict | None = None                         # the whole JSON-RPC response
    streamed: bool = False

    @property
    def error_code(self):
        return (self.data or {}).get("error", {}).get("code") if self.is_error else None


def parse_sse(text: str) -> list:
    """The JSON messages of a text/event-stream body, in order (comments are skipped)."""
    messages = []
    for block in text.replace("\r\n", "\n").split("\n\n"):
        data = [line[5:].lstrip() for line in block.split("\n") if line.startswith("data:")]
        if data:
            messages.append(json.loads("\n".join(data)))
    return messages


class McpClient:
    def __init__(self, url: str, token: str | None, era: str = "modern", timeout: float = 60.0):
        assert era in ("modern", "legacy")
        self.url = url
        self.token = token
        self.era = era
        self.timeout = timeout
        parsed = urlparse(url)
        self.host = parsed.hostname
        self.port = parsed.port
        self.path = parsed.path or "/mcp"
        self._ids = itertools.count(1)
        self.legacy_version: str | None = None  # negotiated by initialize()
        self.server_info: dict | None = None
        self.instructions: str | None = None

    # -- wire ------------------------------------------------------------------------------------
    def http(self, method: str = "POST", body: bytes | str | None = None, headers: dict | None = None,
             path: str | None = None, auth: bool = True, timeout: float | None = None) -> HttpReply:
        """One HTTP request with full control over method, headers and body."""
        if isinstance(body, str):
            body = body.encode("utf-8")
        final = {}
        if auth and self.token is not None:
            final["Authorization"] = f"Bearer {self.token}"
        if body is not None:
            final.setdefault("Content-Type", "application/json")
        final["Accept"] = "application/json, text/event-stream"
        for key, value in (headers or {}).items():
            if value is None:
                final.pop(key, None)
            else:
                final[key] = value
        connection = http.client.HTTPConnection(self.host, self.port, timeout=timeout or self.timeout)
        try:
            connection.request(method, path or self.path, body=body, headers=final)
            response = connection.getresponse()
            data = response.read()
            return HttpReply(response.status, {k.lower(): v for k, v in response.getheaders()}, data)
        finally:
            connection.close()

    def build(self, method: str, params: dict | None = None, notification: bool = False):
        """(message, headers) for a request in this client's era."""
        params = dict(params or {})
        headers = {}
        if self.era == "modern":
            meta = dict(params.get("_meta") or {})
            meta.setdefault(META_VERSION, MODERN_VERSION)
            meta.setdefault(META_CLIENT_INFO, CLIENT_INFO)
            meta.setdefault(META_CLIENT_CAPS, {})
            params["_meta"] = meta
            headers["MCP-Protocol-Version"] = MODERN_VERSION
            headers["Mcp-Method"] = method
            if method == "tools/call":
                name = params.get("name", "")
                try:
                    name.encode("ascii")
                    headers["Mcp-Name"] = name
                except UnicodeEncodeError:
                    headers["Mcp-Name"] = "=?base64?" + base64.b64encode(name.encode("utf-8")).decode("ascii") + "?="
        elif self.legacy_version and method != "initialize":
            headers["MCP-Protocol-Version"] = self.legacy_version
        message = {"jsonrpc": "2.0", "method": method, "params": params}
        if not notification:
            message["id"] = next(self._ids)
        return message, headers

    def post(self, message: dict, headers: dict | None = None, timeout: float | None = None) -> HttpReply:
        return self.http("POST", json.dumps(message), headers, timeout=timeout)

    def rpc(self, method: str, params: dict | None = None, timeout: float | None = None) -> dict:
        """Send a request; return its result or raise RpcError."""
        message, headers = self.build(method, params)
        reply = self.post(message, headers, timeout)
        return self._result(reply, message["id"])

    @staticmethod
    def _result(reply: HttpReply, request_id) -> dict:
        if "text/event-stream" in reply.content_type:
            messages = parse_sse(reply.text)
            responses = [m for m in messages if "id" in m and ("result" in m or "error" in m)]
            if not responses:
                raise RpcError(reply.status, None, reply.text)
            body = responses[-1]
        else:
            try:
                body = reply.json()
            except ValueError:
                raise RpcError(reply.status, None, reply.text) from None
        if "error" in body:
            raise RpcError(reply.status, body["error"], reply.text)
        if reply.status != 200:
            raise RpcError(reply.status, None, reply.text)
        assert body.get("id") == request_id, f"response id {body.get('id')!r} != request id {request_id!r}"
        return body["result"]

    # -- session ---------------------------------------------------------------------------------
    def connect(self) -> "McpClient":
        """What a client does first: legacy handshake, or modern discovery."""
        if self.era == "legacy":
            result = self.rpc("initialize", {"protocolVersion": LEGACY_VERSION, "capabilities": {}, "clientInfo": CLIENT_INFO})
            self.legacy_version = result["protocolVersion"]
            self.server_info = result.get("serverInfo")
            self.instructions = result.get("instructions")
            message, headers = self.build("notifications/initialized", notification=True)
            reply = self.post(message, headers)
            assert reply.status == 202, reply.status
        else:
            result = self.rpc("server/discover")
            self.server_info = result.get("_meta", {}).get(META_SERVER_INFO)
            self.instructions = result.get("instructions")
        return self

    def list_tools(self) -> list:
        return self.rpc("tools/list")["tools"]

    # -- tools -----------------------------------------------------------------------------------
    def call(self, name: str, arguments: dict | None = None, progress: bool = False, timeout: float | None = None) -> ToolResult:
        """tools/call; returns the tool result whether or not it is an error. Raises RpcError for protocol errors."""
        params = {"name": name, "arguments": arguments or {}}
        if progress:
            params["_meta"] = {"progressToken": f"p{next(self._ids)}"}
        message, headers = self.build("tools/call", params)
        reply = self.post(message, headers, timeout)
        streamed = "text/event-stream" in reply.content_type
        notifications = []
        if streamed:
            for item in parse_sse(reply.text):
                if item.get("method") == "notifications/progress":
                    notifications.append(item["params"])
        result = self._result(reply, message["id"])
        data, images = None, []
        for block in result.get("content", []):
            if block["type"] == "text" and data is None:
                data = json.loads(block["text"])
            elif block["type"] == "image":
                images.append((block["mimeType"], base64.b64decode(block["data"])))
        return ToolResult(data=data, is_error=bool(result.get("isError")), images=images, progress=notifications,
                          raw=result, streamed=streamed)

    def tool(self, tool_name: str, /, **arguments):
        """tools/call that must succeed: returns the result document, raises ToolFailure on isError."""
        result = self.call(tool_name, arguments)
        if result.is_error:
            raise ToolFailure(tool_name, result.data)
        return result.data

    def tool_error(self, tool_name: str, /, **arguments) -> ToolFailure:
        """tools/call that must fail with isError: returns the failure."""
        result = self.call(tool_name, arguments)
        assert result.is_error, f"{tool_name} unexpectedly succeeded: {result.data}"
        return ToolFailure(tool_name, result.data)
