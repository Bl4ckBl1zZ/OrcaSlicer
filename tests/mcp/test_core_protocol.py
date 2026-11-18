"""Transport and protocol: both eras, header validation, authentication, Origin/Host, limits.

Uses the raw HTTP layer of the client on purpose: these are the things an SDK hides.
"""

import http.client
import json
import socket
import threading
import time

import pytest

from orca_mcp import (LEGACY_VERSION, META_CLIENT_CAPS, META_SERVER_INFO, META_VERSION, MODERN_VERSION, McpClient,
                      RpcError, parse_sse)

ALL_VERSIONS = ["2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26"]


def modern_headers(method, name=None, version=MODERN_VERSION):
    headers = {"MCP-Protocol-Version": version, "Mcp-Method": method}
    if name is not None:
        headers["Mcp-Name"] = name
    return headers


def modern_message(method, params=None, request_id=1, version=MODERN_VERSION):
    params = dict(params or {})
    params["_meta"] = {META_VERSION: version, META_CLIENT_CAPS: {}}
    return {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}


# ---------------------------------------------------------------------------------------------
# Modern era (2026-07-28)
# ---------------------------------------------------------------------------------------------

def test_discover(modern):
    result = modern.rpc("server/discover")
    assert result["resultType"] == "complete"
    assert result["supportedVersions"] == ALL_VERSIONS
    assert result["capabilities"] == {"tools": {}}
    assert result["ttlMs"] > 0 and result["cacheScope"] == "private"
    assert result["_meta"][META_SERVER_INFO]["name"] == "orcaslicer"
    assert 200 < len(result["instructions"]) < 3000


def test_modern_tools_list_is_cacheable_and_stable(modern):
    first = modern.rpc("tools/list")
    second = modern.rpc("tools/list")
    assert first["resultType"] == "complete" and first["ttlMs"] > 0 and first["cacheScope"] == "private"
    assert json.dumps(first["tools"]) == json.dumps(second["tools"]), "tools/list must be deterministic"


def test_modern_result_carries_result_type_and_server_info(modern):
    raw = modern.call("app_status").raw
    assert raw["resultType"] == "complete"
    assert raw["_meta"][META_SERVER_INFO]["name"] == "orcaslicer"


@pytest.mark.parametrize("drop, expected", [("MCP-Protocol-Version", "MCP-Protocol-Version"), ("Mcp-Method", "Mcp-Method")])
def test_modern_missing_header_is_header_mismatch(modern, drop, expected):
    headers = modern_headers("tools/list")
    del headers[drop]
    reply = modern.post(modern_message("tools/list"), headers)
    assert reply.status == 400
    assert reply.json()["error"]["code"] == -32020
    assert expected in reply.json()["error"]["message"]


def test_modern_header_must_match_body(modern):
    reply = modern.post(modern_message("tools/list"), modern_headers("tools/call"))
    assert reply.status == 400 and reply.json()["error"]["code"] == -32020

    message = modern_message("tools/call", {"name": "app_status", "arguments": {}})
    reply = modern.post(message, modern_headers("tools/call", "scene_get"))
    assert reply.status == 400 and reply.json()["error"]["code"] == -32020
    assert "Mcp-Name" in reply.json()["error"]["message"]

    reply = modern.post(message, modern_headers("tools/call"))  # Mcp-Name missing
    assert reply.status == 400 and reply.json()["error"]["code"] == -32020


def test_modern_mcp_name_base64_form(modern):
    message = modern_message("tools/call", {"name": "app_status", "arguments": {}})
    reply = modern.post(message, modern_headers("tools/call", "=?base64?YXBwX3N0YXR1cw==?="))
    assert reply.status == 200 and "result" in reply.json()


def test_modern_unsupported_version(modern):
    reply = modern.post(modern_message("tools/list", version="1999-01-01"), modern_headers("tools/list", version="1999-01-01"))
    assert reply.status == 400
    error = reply.json()["error"]
    assert error["code"] == -32022
    assert error["data"]["supported"] == ALL_VERSIONS and error["data"]["requested"] == "1999-01-01"


def test_modern_missing_meta_is_invalid_params(modern):
    message = {"jsonrpc": "2.0", "id": 5, "method": "tools/list", "params": {}}
    reply = modern.post(message, modern_headers("tools/list"))
    assert reply.status == 400 and reply.json()["error"]["code"] == -32602

    message = {"jsonrpc": "2.0", "id": 6, "method": "tools/list", "params": {"_meta": {META_VERSION: MODERN_VERSION}}}
    reply = modern.post(message, modern_headers("tools/list"))
    assert reply.status == 400 and reply.json()["error"]["code"] == -32602
    assert "clientCapabilities" in reply.json()["error"]["message"]


def test_modern_unknown_method_is_404(modern):
    reply = modern.post(modern_message("resources/list"), modern_headers("resources/list"))
    assert reply.status == 404 and reply.json()["error"]["code"] == -32601
    # The handshake of older revisions is not a modern method.
    reply = modern.post(modern_message("ping"), modern_headers("ping"))
    assert reply.status == 404 and reply.json()["error"]["code"] == -32601


def test_modern_unknown_tool_is_protocol_error(modern):
    with pytest.raises(RpcError) as info:
        modern.call("no_such_tool")
    assert info.value.code == -32602 and "no_such_tool" in info.value.message


def test_subscriptions_listen_acknowledges_and_stays_open(app):
    client = McpClient(app.url, app.token, era="modern")
    message, headers = client.build("subscriptions/listen", {"notifications": {"toolsListChanged": True}})
    lines = [f"POST {client.path} HTTP/1.1", f"Host: 127.0.0.1:{client.port}", f"Authorization: Bearer {client.token}",
             "Content-Type: application/json", "Accept: application/json, text/event-stream"]
    body = json.dumps(message, separators=(",", ":")).encode()
    lines += [f"{key}: {value}" for key, value in headers.items()]
    lines += [f"Content-Length: {len(body)}", "", ""]
    request = "\r\n".join(lines)
    with socket.create_connection((client.host, client.port), timeout=10) as sock:
        sock.sendall(request.encode() + body)
        received = b""
        while b"acknowledged" not in received:
            received += sock.recv(4096)
        text = received.decode()
        assert text.startswith("HTTP/1.1 200")
        assert "content-type: text/event-stream" in text.lower()
        assert f'"io.modelcontextprotocol/subscriptionId":{message["id"]}' in text
        assert '"notifications":{}' in text, "no notification type is honoured: the tool list never changes"
        # Nothing more arrives; the stream stays open.
        sock.settimeout(1.0)
        with pytest.raises((socket.timeout, TimeoutError)):
            sock.recv(4096)
    # The server survives the abrupt close.
    assert client.rpc("server/discover")["resultType"] == "complete"


# ---------------------------------------------------------------------------------------------
# Legacy era (initialize handshake)
# ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("requested, negotiated", [("2025-11-25", "2025-11-25"), ("2025-06-18", "2025-06-18"),
                                                    ("2025-03-26", "2025-03-26"), ("2024-11-05", "2025-11-25"),
                                                    ("2030-01-01", "2025-11-25")])
def test_initialize_negotiates_version(app, requested, negotiated):
    client = McpClient(app.url, app.token, era="legacy")
    result = client.rpc("initialize", {"protocolVersion": requested, "capabilities": {}, "clientInfo": {"name": "t", "version": "0"}})
    assert result["protocolVersion"] == negotiated
    assert result["capabilities"]["tools"] == {"listChanged": False}
    assert result["serverInfo"]["name"] == "orcaslicer" and result["serverInfo"]["version"]
    assert "scene_get" in result["instructions"]
    assert "resultType" not in result


def test_initialize_issues_no_session(app):
    client = McpClient(app.url, app.token, era="legacy")
    message, headers = client.build("initialize", {"protocolVersion": LEGACY_VERSION, "capabilities": {}, "clientInfo": {"name": "t", "version": "0"}})
    reply = client.post(message, headers)
    assert reply.status == 200 and "mcp-session-id" not in reply.headers
    # A session id sent by a client is ignored.
    client.legacy_version = LEGACY_VERSION
    message, headers = client.build("tools/list")
    reply = client.post(message, {**headers, "Mcp-Session-Id": "whatever"})
    assert reply.status == 200 and "tools" in reply.json()["result"]


def test_legacy_ping_and_notifications(legacy):
    assert legacy.rpc("ping") == {}
    message, headers = legacy.build("notifications/initialized", notification=True)
    reply = legacy.post(message, headers)
    assert reply.status == 202 and reply.body == b""
    message, headers = legacy.build("notifications/cancelled", {"requestId": 424242}, notification=True)
    assert legacy.post(message, headers).status == 202


def test_legacy_without_version_header_is_accepted(app):
    # Clients older than 2025-06-18 send no MCP-Protocol-Version header.
    client = McpClient(app.url, app.token, era="legacy")
    assert "tools" in client.rpc("tools/list")


def test_legacy_results_have_no_modern_fields(legacy):
    result = legacy.rpc("tools/list")
    assert "resultType" not in result and "ttlMs" not in result
    assert "resultType" not in legacy.call("app_status").raw


def test_legacy_unknown_method(legacy):
    with pytest.raises(RpcError) as info:
        legacy.rpc("resources/list")
    assert info.value.code == -32601 and info.value.status == 200


def test_legacy_bad_version_header(legacy):
    message, headers = legacy.build("tools/list")
    reply = legacy.post(message, {**headers, "MCP-Protocol-Version": "1999-01-01"})
    assert reply.status == 400 and reply.json()["error"]["code"] == -32022


def test_legacy_cancel_notification_cancels_a_waiting_call(app):
    client = McpClient(app.url, app.token, era="legacy").connect()
    message, headers = client.build("tools/call", {"name": "debug_job", "arguments": {"duration_s": 20, "wait_s": 20}})
    outcome = {}

    def waiter():
        outcome["reply"] = client.post(message, headers, timeout=30)

    thread = threading.Thread(target=waiter)
    started = time.monotonic()
    thread.start()
    time.sleep(1.0)
    cancel, cancel_headers = client.build("notifications/cancelled", {"requestId": message["id"]}, notification=True)
    assert client.post(cancel, cancel_headers).status == 202
    thread.join(15)
    assert not thread.is_alive()
    assert time.monotonic() - started < 10, "the cancelled call should return promptly"
    body = json.loads(outcome["reply"].json()["result"]["content"][0]["text"])
    assert body["error"]["code"] == "cancelled"
    # The job itself keeps running: cancelling a wait is not cancelling the work.
    status = client.tool("job_status", kind="debug")
    assert status["status"] == "running"
    client.tool("job_cancel", job=status["job"])


# ---------------------------------------------------------------------------------------------
# Both eras
# ---------------------------------------------------------------------------------------------

def test_tools_have_schema_and_annotations(client):
    tools = client.list_tools()
    assert len(tools) >= 5
    for tool in tools:
        assert tool["inputSchema"]["type"] == "object"
        assert len(tool["description"]) > 20
        annotations = tool["annotations"]
        assert annotations["openWorldHint"] is False
        assert isinstance(annotations["readOnlyHint"], bool) and isinstance(annotations["destructiveHint"], bool)
    by_name = {tool["name"]: tool for tool in tools}
    assert by_name["scene_get"]["annotations"]["readOnlyHint"] is True
    assert by_name["object_update"]["annotations"]["readOnlyHint"] is False
    # Shared fragments are embedded where referenced; mutating tools accept if_rev.
    assert "Id" in by_name["object_update"]["inputSchema"]["$defs"]
    assert "if_rev" in by_name["object_update"]["inputSchema"]["properties"]
    assert "if_rev" not in by_name["scene_get"]["inputSchema"]["properties"]


def test_tool_result_shape(client):
    result = client.call("app_status")
    assert result.is_error is False and result.raw["content"][0]["type"] == "text"
    assert result.data["app"]


def test_image_content(client):
    result = client.call("debug_echo", {"image": True, "payload": {"a": 1}})
    assert result.data["payload"] == {"a": 1}
    assert len(result.images) == 1
    mime, data = result.images[0]
    assert mime == "image/png" and data[:8] == b"\x89PNG\r\n\x1a\n"


def test_progress_stream(client):
    result = client.call("debug_job", {"duration_s": 2.5, "wait_s": 10}, progress=True)
    assert result.streamed, "a call with a progressToken is answered on an event stream"
    assert result.data["status"] == "finished"
    assert len(result.progress) >= 2
    percents = [p["progress"] for p in result.progress]
    assert percents == sorted(percents) and all(p["total"] == 100 for p in result.progress)


def test_without_progress_token_the_answer_is_plain_json(client):
    message, headers = client.build("tools/call", {"name": "debug_job", "arguments": {"duration_s": 0.3}})
    reply = client.post(message, headers)
    assert reply.content_type.startswith("application/json")


def test_malformed_requests(client):
    reply = client.http("POST", b"{not json")
    assert reply.status == 400 and reply.json()["error"]["code"] == -32700
    reply = client.http("POST", json.dumps([{"jsonrpc": "2.0", "id": 1, "method": "ping"}]))
    assert reply.status == 400 and reply.json()["error"]["code"] == -32600
    reply = client.http("POST", json.dumps({"id": 1, "method": "ping"}))
    assert reply.status == 400 and reply.json()["error"]["code"] == -32600


def test_client_responses_are_accepted_and_ignored(client):
    reply = client.http("POST", json.dumps({"jsonrpc": "2.0", "id": 99, "result": {}}))
    assert reply.status == 202


# ---------------------------------------------------------------------------------------------
# Security: authentication, Origin, Host, method, content type, limits
# ---------------------------------------------------------------------------------------------

def test_bearer_token_required(app, modern):
    message, headers = modern.build("tools/list")
    reply = modern.post(message, {**headers, "Authorization": None})
    assert reply.status == 401 and reply.headers.get("www-authenticate") == "Bearer" and reply.body == b""
    for wrong in ("Bearer wrong-token", "Bearer ", "Basic " + app.token, app.token, "Bearer " + app.token + "x", "Bearer " + app.token[:-1]):
        reply = modern.post(message, {**headers, "Authorization": wrong})
        assert reply.status == 401, wrong
    # The scheme is case-insensitive, the token is not.
    assert modern.post(message, {**headers, "Authorization": "bearer " + app.token}).status == 200


def test_origin_is_rejected(modern):
    message, headers = modern.build("tools/list")
    for origin in ("https://evil.example", "http://localhost:3000", "null", f"http://127.0.0.1:{modern.port}"):
        reply = modern.post(message, {**headers, "Origin": origin})
        assert reply.status == 403, origin
        assert "access-control-allow-origin" not in reply.headers


def test_origin_is_checked_before_the_token(modern):
    message, headers = modern.build("tools/list")
    reply = modern.post(message, {**headers, "Origin": "https://evil.example", "Authorization": None})
    assert reply.status == 403


def test_host_header_must_be_loopback(modern):
    message, headers = modern.build("tools/list")
    for host in ("evil.example", f"evil.example:{modern.port}", "127.0.0.1:1", f"rebind.attacker.test:{modern.port}"):
        assert modern.post(message, {**headers, "Host": host}).status == 403, host
    for host in (f"127.0.0.1:{modern.port}", f"localhost:{modern.port}", f"LOCALHOST:{modern.port}"):
        assert modern.post(message, {**headers, "Host": host}).status == 200, host


def test_only_post_on_the_endpoint(modern):
    for method in ("GET", "DELETE", "PUT"):
        reply = modern.http(method)
        assert reply.status == 405 and reply.headers.get("allow") == "POST", method
    assert modern.http("POST", "{}", path="/other").status == 404
    assert modern.http("GET", path="/").status == 404


def test_preflight_is_not_answered(modern):
    reply = modern.http("OPTIONS", headers={"Origin": "https://evil.example", "Access-Control-Request-Method": "POST"}, auth=False)
    assert reply.status == 403
    assert not any(name.startswith("access-control-") for name in reply.headers)


def test_content_type_must_be_json(modern):
    message, headers = modern.build("tools/list")
    for content_type in ("text/plain", "application/x-www-form-urlencoded", "multipart/form-data"):
        assert modern.post(message, {**headers, "Content-Type": content_type}).status == 415, content_type
    assert modern.post(message, {**headers, "Content-Type": "application/json; charset=utf-8"}).status == 200


def test_oversized_body_is_rejected(modern):
    message, headers = modern.build("tools/call", {"name": "debug_echo", "arguments": {"payload": "x" * (9 * 1024 * 1024)}})
    try:
        reply = modern.post(message, headers, timeout=30)
    except (ConnectionError, OSError):
        return  # the server closed the connection while the body was still being sent
    assert reply.status == 413


def test_large_body_under_the_limit_is_accepted(modern):
    # A 1 MB request is read and run; echoing it back is what exceeds the result limit.
    result = modern.call("debug_echo", {"payload": "y" * (1024 * 1024)})
    assert result.is_error and result.error_code == "too_large"


def test_rate_limit(app):
    client = McpClient(app.url, app.token, era="modern")
    message, headers = client.build("server/discover")
    statuses = []
    connection = http.client.HTTPConnection(client.host, client.port, timeout=10)
    started = time.monotonic()
    try:
        body = json.dumps(message)
        all_headers = {"Authorization": f"Bearer {client.token}", "Content-Type": "application/json", **headers}
        for _ in range(700):
            connection.request("POST", client.path, body=body, headers=all_headers)
            response = connection.getresponse()
            response.read()
            statuses.append(response.status)
    finally:
        connection.close()
    elapsed = time.monotonic() - started
    time.sleep(1.2)  # let the bucket refill for the following tests
    if elapsed > 2.5:
        pytest.skip(f"this machine sent 700 requests in {elapsed:.1f} s: too slow to exceed the limit")
    assert statuses.count(429) > 0, "a burst above 200 requests per second must be throttled"
    assert statuses[0] == 200
    time.sleep(1.2)  # let the bucket refill for the following tests
    assert client.rpc("server/discover")["resultType"] == "complete"


def test_keep_alive_connection_serves_several_requests(app):
    client = McpClient(app.url, app.token, era="modern")
    connection = http.client.HTTPConnection(client.host, client.port, timeout=10)
    try:
        for _ in range(3):
            message, headers = client.build("tools/list")
            connection.request("POST", client.path, body=json.dumps(message),
                               headers={"Authorization": f"Bearer {client.token}", "Content-Type": "application/json", **headers})
            response = connection.getresponse()
            assert response.status == 200 and "tools" in json.loads(response.read())["result"]
    finally:
        connection.close()


def test_closing_the_connection_cancels_the_wait(app, modern):
    """Streamable HTTP: the client cancels by closing the response stream."""
    client = McpClient(app.url, app.token, era="modern")
    message, headers = client.build("tools/call", {"name": "debug_job", "arguments": {"duration_s": 30, "wait_s": 30}, "_meta": {"progressToken": "t1"}})
    connection = http.client.HTTPConnection(client.host, client.port, timeout=10)
    connection.request("POST", client.path, body=json.dumps(message),
                       headers={"Authorization": f"Bearer {client.token}", "Content-Type": "application/json",
                                "Accept": "application/json, text/event-stream", **headers})
    response = connection.getresponse()
    assert response.status == 200
    time.sleep(1.0)
    connection.close()
    time.sleep(0.8)
    # The wait ended; the job itself is still there and can be cancelled explicitly.
    status = modern.tool("job_status", kind="debug")
    assert status["status"] == "running"
    assert modern.tool("job_cancel", job=status["job"])["job"] == status["job"]
    final = modern.tool("job_status", job=status["job"], wait_s=5)
    assert final["status"] == "cancelled"
