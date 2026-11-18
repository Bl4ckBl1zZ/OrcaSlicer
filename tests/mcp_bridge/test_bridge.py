#!/usr/bin/env python3
"""Scenario tests: run the real orca-mcp-bridge binary over stdio against mock_server.py.

    python tests/mcp_bridge/test_bridge.py --bridge <path to orca-mcp-bridge[.exe]> [-v] [-k pattern]
or set ORCA_MCP_BRIDGE_BIN. Standard library only (Python 3.8+).
"""
import base64
import faulthandler
import hashlib
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.dont_write_bytecode = True  # keep the source tree clean
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mock_server import MODERN, MockServer, image_bytes  # noqa: E402

BRIDGE = os.environ.get("ORCA_MCP_BRIDGE_BIN", "")
CLIENT_INFO = {"name": "bridge-test", "version": "1"}


def modern_params(extra=None, **meta):
    m = {"io.modelcontextprotocol/protocolVersion": MODERN,
         "io.modelcontextprotocol/clientCapabilities": {},
         "io.modelcontextprotocol/clientInfo": CLIENT_INFO}
    m.update(meta)
    p = {"_meta": m}
    p.update(extra or {})
    return p


def clean_env(**kw):
    env = {k: v for k, v in os.environ.items() if not k.startswith("ORCA_MCP")}
    env.update({k: str(v) for k, v in kw.items()})
    return env


class Bridge:
    """The bridge as a subprocess. Every stdout line must be one JSON-RPC object."""

    def __init__(self, args=(), env=None):
        self.proc = subprocess.Popen([BRIDGE, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, env=env if env is not None else clean_env())
        self.lines = queue.Queue()
        self.bad_lines = []
        self.stderr = []
        self.t_out = threading.Thread(target=self._read_out, daemon=True)
        self.t_err = threading.Thread(target=self._read_err, daemon=True)
        self.t_out.start()
        self.t_err.start()
        self.next_id = 1
        self.unclaimed = []

    def _read_out(self):
        for raw in self.proc.stdout:
            assert raw.endswith(b"\n")
            line = raw[:-1]
            try:
                if b"\r" in line:
                    raise ValueError("CR in stdout line")
                obj = json.loads(line.decode("utf-8"))
                if not isinstance(obj, dict) or obj.get("jsonrpc") != "2.0":
                    raise ValueError("not a JSON-RPC object")
                self.lines.put(obj)
            except Exception as e:  # noqa: BLE001
                self.bad_lines.append((raw[:200], str(e)))
        self.lines.put(None)

    def _read_err(self):
        for raw in self.proc.stderr:
            self.stderr.append(raw.decode("utf-8", "replace"))

    def send(self, obj):
        data = (json.dumps(obj) + "\n").encode("utf-8") if not isinstance(obj, (bytes, str)) else \
            (obj.encode("utf-8") if isinstance(obj, str) else obj)
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def request(self, method, params=None, rid=None):
        if rid is None:
            rid = self.next_id
            self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        return rid

    def recv(self, timeout=10.0, pred=None):
        """Next message matching pred (others are kept for later)."""
        for i, m in enumerate(self.unclaimed):
            if pred is None or pred(m):
                return self.unclaimed.pop(i)
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                raise AssertionError("timed out waiting for a message; stderr:\n" + "".join(self.stderr[-30:]))
            try:
                m = self.lines.get(timeout=left)
            except queue.Empty:
                continue
            if m is None:
                raise AssertionError("bridge stdout closed; stderr:\n" + "".join(self.stderr[-30:]))
            if pred is None or pred(m):
                return m
            self.unclaimed.append(m)

    def response(self, rid, timeout=10.0):
        return self.recv(timeout, lambda m: m.get("id") == rid and "method" not in m)

    def call(self, method, params=None, timeout=10.0):
        return self.response(self.request(method, params), timeout)

    def no_message(self, seconds, pred=lambda m: True):
        end = time.time() + seconds
        while time.time() < end:
            try:
                m = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                return True
            if m is None:
                return True
            if pred(m):
                raise AssertionError("unexpected message: %s" % json.dumps(m)[:300])
            self.unclaimed.append(m)
        return True

    def close(self, timeout=5.0):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            return self.proc.wait(timeout)
        finally:
            if self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait()
            self.t_out.join(2)
            self.t_err.join(2)
            for f in (self.proc.stdout, self.proc.stderr):
                try:
                    f.close()
                except OSError:
                    pass


class BridgeTestCase(unittest.TestCase):
    era = "dual"
    server_cfg = {}

    def setUp(self):
        faulthandler.dump_traceback_later(90, exit=True)  # a hung scenario fails loudly
        self.tmp = tempfile.mkdtemp(prefix="orca-mcp-bridge-test-")
        self.datadir = os.path.join(self.tmp, "data")
        self.endpoint = os.path.join(self.datadir, "mcp", "endpoint.json")
        self.server = MockServer(era=self.era, **self.server_cfg).start()
        self.server.write_endpoint_file(self.endpoint)
        self.bridges = []

    def tearDown(self):
        for b in self.bridges:
            if b.proc.poll() is None:
                b.close()
            self.assertEqual(b.bad_lines, [], "bridge wrote something to stdout that is not a JSON-RPC message")
        self.server.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)
        faulthandler.cancel_dump_traceback_later()

    def bridge(self, *args, env=None, datadir=True):
        e = clean_env(**(env or {}))
        a = list(args)
        if datadir and "--url" not in a and "--endpoint-file" not in a and "--datadir" not in a:
            a = ["--datadir", self.datadir] + a
        b = Bridge(a, e)
        self.bridges.append(b)
        return b

    def posts(self, method=None):
        with self.server.state.lock:
            return [r for r in self.server.state.requests
                    if r["verb"] == "POST" and (method is None or r["method"] == method)]

    def legacy_handshake(self, b, version="2025-11-25"):
        r = b.call("initialize", {"protocolVersion": version, "capabilities": {}, "clientInfo": CLIENT_INFO})
        self.assertIn("result", r, r)
        b.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return r


def hget(headers, name):
    for k, v in headers.items():
        if k.lower() == name.lower():
            return v
    return None


# =============================================================================================
class ModernEra(BridgeTestCase):
    def test_discover_list_call_and_mirrored_headers(self):
        b = self.bridge()
        r = b.call("server/discover", modern_params())
        self.assertIn(MODERN, r["result"]["supportedVersions"])
        r = b.call("tools/list", modern_params())
        names = [t["name"] for t in r["result"]["tools"]]
        self.assertIn("echo", names)
        r = b.call("tools/call", modern_params({"name": "echo", "arguments": {"text": "hi ✓"}}))
        self.assertEqual(r["result"]["content"][0]["text"], "hi ✓")
        h = self.posts("tools/call")[-1]["headers"]
        self.assertEqual(hget(h, "MCP-Protocol-Version"), MODERN)
        self.assertEqual(hget(h, "Mcp-Method"), "tools/call")
        self.assertEqual(hget(h, "Mcp-Name"), "echo")
        self.assertEqual(hget(h, "Authorization"), "Bearer test-token")
        self.assertEqual(hget(h, "Host"), "127.0.0.1:%d" % self.server.port)
        self.assertIsNone(hget(h, "Origin"))
        self.assertIsNone(hget(h, "Mcp-Session-Id"))
        self.assertIn("text/event-stream", hget(h, "Accept"))
        self.assertEqual(b.close(), 0)

    def test_name_needing_base64_header(self):
        b = self.bridge()
        r = b.call("tools/call", modern_params({"name": "ünknown tool", "arguments": {}}))
        self.assertEqual(r["error"]["code"], -32602)  # reached the tool dispatcher: header matched
        self.assertTrue(hget(self.posts("tools/call")[-1]["headers"], "Mcp-Name").startswith("=?base64?"))

    def test_image_result_multi_megabyte_json_and_sse(self):
        b = self.bridge()
        for size, token in ((6, None), (9, "img")):
            extra = {"name": "image", "arguments": {"size_mb": size}}
            p = modern_params(extra, progressToken=token) if token else modern_params(extra)
            rid = b.request("tools/call", p)
            r = b.response(rid, timeout=60)
            content = r["result"]["content"]
            data = base64.b64decode(content[1]["data"])
            self.assertEqual(len(data), size * 1024 * 1024)
            self.assertEqual(hashlib.sha256(data).hexdigest(), content[0]["text"])
            if token:
                self.assertEqual(b.recv(5, lambda m: m.get("method") == "notifications/progress")["params"]["progressToken"], "img")

    def test_progress_stream_precedes_result(self):
        b = self.bridge()
        rid = b.request("tools/call", modern_params({"name": "slow", "arguments": {"seconds": 1.0}}, progressToken="p1"))
        seen = []
        while True:
            m = b.recv(10, lambda m: m.get("id") == rid or m.get("method") == "notifications/progress")
            if "method" in m:
                seen.append(m["params"]["progress"])
            else:
                break
        self.assertGreaterEqual(len(seen), 3)
        self.assertEqual(seen, sorted(seen))
        self.assertEqual(m["result"]["content"][0]["text"], "slept 1")

    def test_sse_grammar_comments_ids_crlf_multiline_custom_events(self):
        for chunked in (True, False):
            self.server.cfg["chunked"] = chunked
            b = self.bridge()
            rid = b.request("tools/call", modern_params({"name": "sse_multi", "arguments": {}}))
            msg = b.recv(5, lambda m: m.get("method") == "notifications/message")
            self.assertEqual(msg["params"]["data"], "multi\nline")
            prog = b.recv(5, lambda m: m.get("method") == "notifications/progress")
            self.assertEqual(prog["params"]["progress"], 1)
            r = b.response(rid)
            self.assertEqual(r["result"]["content"][0]["text"], "done ✓ ünïcode")
            b.no_message(0.3, lambda m: "not" in json.dumps(m))
            b.close()

    def test_cancellation_closes_stream_and_is_not_forwarded(self):
        b = self.bridge()
        rid = b.request("tools/call", modern_params({"name": "slow", "arguments": {"seconds": 5}}, progressToken="c"))
        b.recv(5, lambda m: m.get("method") == "notifications/progress")
        b.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": rid, "reason": "test"}})
        deadline = time.time() + 5
        while json.dumps(rid) not in self.server.state.disconnects and time.time() < deadline:
            time.sleep(0.05)
        self.assertIn(json.dumps(rid), self.server.state.disconnects, "server did not see the stream close")
        b.no_message(1.0, lambda m: m.get("id") == rid)
        self.assertEqual(self.posts("notifications/cancelled"), [])
        # still usable afterwards
        self.assertIn("result", b.call("tools/call", modern_params({"name": "echo", "arguments": {"text": "x"}})))

    def test_concurrent_requests_complete_out_of_order(self):
        b = self.bridge()
        durations = [1.6, 0.2, 1.2, 0.4, 0.8, 1.0, 0.6, 1.4]
        t0 = time.time()
        ids = {}
        for d in durations:
            ids[b.request("tools/call", modern_params({"name": "slow", "arguments": {"seconds": d}}))] = d
        order = []
        while len(order) < len(ids):
            m = b.recv(10, lambda m: m.get("id") in ids)
            self.assertEqual(m["result"]["content"][0]["text"], "slept %g" % ids[m["id"]])
            order.append(ids[m["id"]])
        elapsed = time.time() - t0
        self.assertLess(elapsed, sum(durations) * 0.6, "requests were not concurrent")
        self.assertNotEqual(order, durations)

    def test_unsupported_version_and_unknown_method_are_forwarded(self):
        b = self.bridge()
        p = modern_params()
        p["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2099-01-01"
        r = b.call("tools/list", p)
        self.assertEqual(r["error"]["code"], -32022)
        self.assertIn(MODERN, r["error"]["data"]["supported"])
        r = b.call("nope/nothing", modern_params())
        self.assertEqual(r["error"]["code"], -32601)

    def test_x_mcp_header_mirroring_and_invalid_tool_hidden(self):
        b = self.bridge()
        r = b.call("tools/list", modern_params())
        names = [t["name"] for t in r["result"]["tools"]]
        self.assertIn("region", names)
        self.assertNotIn("bad_annotation", names)
        r = b.call("tools/call", modern_params({"name": "region", "arguments": {"region": "us-west1", "opts": {"level": 3}}}))
        self.assertEqual(r["result"]["content"][0]["text"], "region=us-west1")
        h = self.posts("tools/call")[-1]["headers"]
        self.assertEqual(hget(h, "Mcp-Param-Region"), "us-west1")
        self.assertEqual(hget(h, "Mcp-Param-Level"), "3")
        r = b.call("tools/call", modern_params({"name": "region", "arguments": {"region": " Zürich "}}))
        self.assertIn("result", r, r)
        self.assertTrue(hget(self.posts("tools/call")[-1]["headers"], "Mcp-Param-Region").startswith("=?base64?"))

    def test_header_mismatch_triggers_tools_list_refresh(self):
        b = self.bridge()  # never listed tools: first call goes without Mcp-Param-Region
        r = b.call("tools/call", modern_params({"name": "region", "arguments": {"region": "eu"}}))
        self.assertIn("result", r, r)
        self.assertGreaterEqual(len(self.posts("tools/list")), 1)

    def test_subscription_listen_and_cancel(self):
        b = self.bridge("--request-timeout", "1")  # listen streams are exempt from the timeout
        rid = b.request("subscriptions/listen", modern_params({"notifications": {"toolsListChanged": True}}))
        ack = b.recv(5, lambda m: m.get("method") == "notifications/subscriptions/acknowledged")
        self.assertEqual(ack["params"]["_meta"]["io.modelcontextprotocol/subscriptionId"], rid)
        b.recv(5, lambda m: m.get("method") == "notifications/tools/list_changed")
        b.no_message(1.5, lambda m: m.get("id") == rid)
        b.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": rid}})
        deadline = time.time() + 5
        while json.dumps(rid) not in self.server.state.disconnects and time.time() < deadline:
            time.sleep(0.05)
        self.assertIn(json.dumps(rid), self.server.state.disconnects)

    def test_tool_error_result_passes_through(self):
        b = self.bridge()
        r = b.call("tools/call", modern_params({"name": "fail", "arguments": {}}))
        self.assertTrue(r["result"]["isError"])

    def test_server_drops_connection(self):
        b = self.bridge()
        r = b.call("tools/call", modern_params({"name": "drop", "arguments": {}}))
        self.assertEqual(r["error"]["code"], -31000)
        self.assertEqual(r["error"]["data"]["source"], "orca-mcp-bridge")
        rid = b.request("tools/call", modern_params({"name": "drop", "arguments": {"sse": True}}, progressToken=1))
        r = b.response(rid)
        self.assertIn(r["error"]["code"], (-31000, -31005))

    def test_busy_429_is_retried(self):
        self.server.state.busy_remaining = 3
        b = self.bridge()
        r = b.call("tools/call", modern_params({"name": "echo", "arguments": {"text": "ok"}}))
        self.assertEqual(r["result"]["content"][0]["text"], "ok")

    def test_request_timeout(self):
        b = self.bridge("--request-timeout", "0.5")
        r = b.call("tools/call", modern_params({"name": "slow", "arguments": {"seconds": 3}}, progressToken=9), timeout=5)
        self.assertEqual(r["error"]["code"], -31003)

    def test_check_mode(self):
        p = subprocess.run([BRIDGE, "--datadir", self.datadir, "--check"], capture_output=True, env=clean_env(), timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr.decode())
        self.assertEqual(p.stdout, b"")
        self.assertNotIn(b"test-token", p.stderr)


class LegacyEra(BridgeTestCase):
    server_cfg = {"sessions": True, "require_version_header": True}

    def test_handshake_list_call_and_session(self):
        b = self.bridge()
        r = self.legacy_handshake(b, "2025-06-18")
        self.assertEqual(r["result"]["protocolVersion"], "2025-06-18")
        r = b.call("tools/list")
        self.assertIn("echo", [t["name"] for t in r["result"]["tools"]])
        r = b.call("tools/call", {"name": "echo", "arguments": {"text": "legacy"}})
        self.assertEqual(r["result"]["content"][0]["text"], "legacy")
        self.assertIn("result", b.call("ping"))
        init_h = self.posts("initialize")[-1]["headers"]
        self.assertIsNone(hget(init_h, "Mcp-Session-Id"))
        call_h = self.posts("tools/call")[-1]["headers"]
        self.assertEqual(hget(call_h, "MCP-Protocol-Version"), "2025-06-18")
        sid = hget(call_h, "Mcp-Session-Id")
        self.assertTrue(sid)
        self.assertEqual(hget(self.posts("notifications/initialized")[-1]["headers"], "Mcp-Session-Id"), sid)
        self.assertEqual(b.close(), 0)
        time.sleep(0.2)
        self.assertIn(sid, self.server.state.deleted_sessions, "session not terminated with DELETE on exit")

    def test_progress_and_cancellation_forwarded(self):
        b = self.bridge()
        self.legacy_handshake(b)
        rid = b.request("tools/call", {"name": "slow", "arguments": {"seconds": 5}, "_meta": {"progressToken": "lp"}})
        b.recv(5, lambda m: m.get("method") == "notifications/progress")
        b.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": rid}})
        deadline = time.time() + 5
        while json.dumps(rid) not in self.server.state.cancelled and time.time() < deadline:
            time.sleep(0.05)
        self.assertIn(json.dumps(rid), self.server.state.cancelled, "legacy cancel not forwarded")
        b.no_message(1.0, lambda m: m.get("id") == rid)

    def test_image_and_concurrency(self):
        b = self.bridge()
        self.legacy_handshake(b)
        ids = [b.request("tools/call", {"name": "slow", "arguments": {"seconds": 0.5}}) for _ in range(4)]
        img = b.request("tools/call", {"name": "image", "arguments": {"size_mb": 5}})
        r = b.response(img, 60)
        self.assertEqual(hashlib.sha256(base64.b64decode(r["result"]["content"][1]["data"])).hexdigest(),
                         r["result"]["content"][0]["text"])
        for i in ids:
            self.assertIn("result", b.response(i))

    def test_session_expiry_reinitializes(self):
        b = self.bridge()
        self.legacy_handshake(b)
        b.call("ping")  # the initialized notification has certainly been delivered
        with self.server.state.lock:
            self.server.state.sessions.clear()  # server "restarted": old session unknown -> 404
        ids = [b.request("tools/call", {"name": "echo", "arguments": {"text": "again%d" % i}}) for i in range(3)]
        for i, rid in enumerate(ids):
            self.assertEqual(b.response(rid)["result"]["content"][0]["text"], "again%d" % i)
        self.assertEqual(len(self.posts("initialize")), 2)  # one shared re-initialize


class LegacyResumption(BridgeTestCase):
    server_cfg = {"legacy_resume": True}

    def test_sse_resume_with_last_event_id(self):
        b = self.bridge()
        self.legacy_handshake(b)
        rid = b.request("tools/call", {"name": "slow", "arguments": {"seconds": 1.0}, "_meta": {"progressToken": "r"}})
        progress = []
        while True:
            m = b.recv(10, lambda m: m.get("id") == rid or m.get("method") == "notifications/progress")
            if "method" in m:
                progress.append(m["params"]["progress"])
            else:
                break
        self.assertEqual(m["result"]["content"][0]["text"], "slept 1")
        self.assertEqual(progress, sorted(set(progress)), "events lost or duplicated across the resume")
        self.assertEqual(len(progress), 5)
        gets = [r for r in self.server.state.requests if r["verb"] == "GET" and hget(r["headers"], "Last-Event-ID")]
        self.assertTrue(gets, "no GET with Last-Event-ID")


class LegacyGetStream(BridgeTestCase):
    server_cfg = {"legacy_get_stream": True}

    def test_get_stream_messages_forwarded(self):
        b = self.bridge()
        self.legacy_handshake(b)
        b.recv(5, lambda m: m.get("method") == "notifications/tools/list_changed")


class EraMismatch(BridgeTestCase):
    def test_legacy_client_modern_only_server(self):
        self.server.cfg["era"] = "modern"
        b = self.bridge()
        r = b.call("initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": CLIENT_INFO})
        self.assertIn("error", r)
        self.assertIn("legacy initialize", r["error"]["message"])

    def test_modern_probe_legacy_only_server_then_fallback(self):
        self.server.cfg["era"] = "legacy"
        b = self.bridge()
        r = b.call("server/discover", modern_params())
        self.assertIn("error", r)
        self.assertNotIn(r["error"]["code"], (-32020, -32021, -32022))  # not a recognised modern error
        r = self.legacy_handshake(b)
        self.assertEqual(r["result"]["protocolVersion"], "2025-11-25")
        self.assertIn("result", b.call("tools/list"))


# =============================================================================================
class Discovery(BridgeTestCase):
    def test_missing_endpoint_file(self):
        os.remove(self.endpoint)
        b = self.bridge()
        r = b.call("server/discover", modern_params())
        self.assertEqual(r["error"]["code"], -31002)
        self.assertEqual(r["error"]["data"]["reason"], "no_endpoint_file")
        self.assertIn("Start OrcaSlicer", r["error"]["message"])

    def test_env_datadir_and_endpoint_file_option(self):
        b = self.bridge(env={"ORCA_MCP_DATADIR": self.datadir}, datadir=False)
        self.assertIn("result", b.call("server/discover", modern_params()))
        b2 = self.bridge("--endpoint-file", self.endpoint)
        self.assertIn("result", b2.call("server/discover", modern_params()))

    def test_explicit_url_and_token_file(self):
        tf = os.path.join(self.tmp, "token.txt")
        with open(tf, "w") as f:
            f.write("test-token\n")
        b = self.bridge("--url", self.server.url, "--token-file", tf)
        self.assertIn("result", b.call("server/discover", modern_params()))
        b2 = self.bridge(env={"ORCA_MCP_URL": self.server.url, "ORCA_MCP_TOKEN": "test-token"}, datadir=False)
        self.assertIn("result", b2.call("server/discover", modern_params()))

    def test_bad_token_then_rotation(self):
        self.server.write_endpoint_file(self.endpoint, token="wrong")
        b = self.bridge()
        r = b.call("server/discover", modern_params())
        self.assertEqual(r["error"]["code"], -31001)
        self.assertEqual(r["error"]["data"]["reason"], "token_rejected")
        self.assertNotIn("wrong", json.dumps(r))
        self.assertNotIn("wrong", "".join(b.stderr))
        self.server.write_endpoint_file(self.endpoint)  # the app wrote the right token
        self.assertIn("result", b.call("server/discover", modern_params()))

    def test_explicit_wrong_token(self):
        b = self.bridge("--url", self.server.url, env={"ORCA_MCP_TOKEN": "nope"})
        r = b.call("server/discover", modern_params())
        self.assertEqual(r["error"]["code"], -31001)
        self.assertIn("ORCA_MCP_TOKEN", r["error"]["message"])

    def test_server_down_then_up_and_port_change(self):
        port = self.server.port
        self.server.stop()
        b = self.bridge()
        r = b.call("server/discover", modern_params(), timeout=15)
        self.assertEqual(r["error"]["code"], -31000)
        self.assertIn(r["error"]["data"]["reason"], ("server_not_listening", "connection_refused"))
        self.server.start(port)  # app back, same port
        self.assertIn("result", b.call("server/discover", modern_params()))
        # restart on another port: the endpoint file changes, the bridge follows
        self.server.stop()
        self.server.start(0)
        self.server.write_endpoint_file(self.endpoint)
        self.assertIn("result", b.call("server/discover", modern_params(), timeout=15))

    def test_port_changed_without_file_mtime_change_detected_on_refusal(self):
        b = self.bridge()
        self.assertIn("result", b.call("server/discover", modern_params()))
        old = self.server.port
        self.server.stop()
        self.server.start(0)
        st = os.stat(self.endpoint)
        self.server.write_endpoint_file(self.endpoint)
        os.utime(self.endpoint, ns=(st.st_atime_ns, st.st_mtime_ns))  # same mtime: stamp may not change
        self.assertNotEqual(old, self.server.port)
        self.assertIn("result", b.call("server/discover", modern_params(), timeout=15))

    def test_stale_endpoint_file_pid(self):
        p = subprocess.Popen([sys.executable, "-c", "pass"])
        p.wait()
        port = self.server.port
        self.server.stop()
        self.server.port = port
        self.server.write_endpoint_file(self.endpoint, pid=p.pid)
        b = self.bridge()
        r = b.call("server/discover", modern_params(), timeout=15)
        self.assertEqual(r["error"]["data"]["reason"], "app_not_running")
        self.assertIn("has exited", r["error"]["message"])

    def test_connect_wait_bridges_app_start(self):
        port = self.server.port
        self.server.stop()
        b = self.bridge("--connect-wait", "10")
        rid = b.request("server/discover", modern_params())
        time.sleep(1.0)
        self.server.start(port)
        self.assertIn("result", b.response(rid, 15))

    def test_non_loopback_refused(self):
        p = subprocess.run([BRIDGE, "--url", "http://192.0.2.10:13720/mcp"], capture_output=True, env=clean_env(),
                           timeout=10, input=b"")
        self.assertEqual(p.returncode, 2)
        self.assertEqual(p.stdout, b"")
        self.server.write_endpoint_file(self.endpoint, url="http://10.1.2.3:13720/mcp")
        b = self.bridge()
        r = b.call("server/discover", modern_params())
        self.assertEqual(r["error"]["data"]["reason"], "non_loopback")

    def test_localhost_url(self):
        b = self.bridge("--url", "http://localhost:%d/mcp" % self.server.port, env={"ORCA_MCP_TOKEN": "test-token"})
        self.assertIn("result", b.call("server/discover", modern_params(), timeout=15))
        self.assertEqual(hget(self.posts()[-1]["headers"], "Host"), "localhost:%d" % self.server.port)


# =============================================================================================
class StdioHandling(BridgeTestCase):
    def test_malformed_lines(self):
        b = self.bridge()
        b.send(b"this is not json\n")
        self.assertEqual(b.recv()["error"]["code"], -32700)
        b.send(b"[1,2,3]\n")
        self.assertEqual(b.recv()["error"]["code"], -32600)
        b.send(b'{"jsonrpc":"2.0","id":7}\n')
        r = b.recv()
        self.assertEqual((r["id"], r["error"]["code"]), (7, -32600))
        b.send(b"\xff\xfe{}\n")
        self.assertEqual(b.recv()["error"]["code"], -32700)
        b.send(b"\n   \n\r\n")  # blank lines are ignored
        # CRLF framing and a message split across writes
        msg = json.dumps({"jsonrpc": "2.0", "id": "s", "method": "server/discover", "params": modern_params()}).encode()
        b.send(msg[:10])
        time.sleep(0.1)
        b.send(msg[10:] + b"\r\n")
        self.assertIn("result", b.response("s"))
        self.assertEqual(b.close(), 0)

    def test_large_request_line(self):
        b = self.bridge()
        text = "x" * (3 * 1024 * 1024)
        r = b.call("tools/call", modern_params({"name": "echo", "arguments": {"text": text}}), timeout=30)
        self.assertEqual(len(r["result"]["content"][0]["text"]), len(text))

    def test_string_and_integer_ids_are_distinct(self):
        b = self.bridge()
        b.request("tools/call", modern_params({"name": "slow", "arguments": {"seconds": 0.5}}), rid=5)
        b.request("tools/call", modern_params({"name": "echo", "arguments": {"text": "s"}}), rid="5")
        r1 = b.recv(5, lambda m: m.get("id") == "5")
        self.assertEqual(r1["result"]["content"][0]["text"], "s")
        r2 = b.recv(5, lambda m: m.get("id") == 5)
        self.assertEqual(r2["result"]["content"][0]["text"], "slept 0.5")

    def test_eof_exits_promptly_and_cancels_inflight(self):
        b = self.bridge()
        rid = b.request("tools/call", modern_params({"name": "slow", "arguments": {"seconds": 10}}, progressToken="e"))
        b.recv(5, lambda m: m.get("method") == "notifications/progress")
        t0 = time.time()
        self.assertEqual(b.close(), 0)
        self.assertLess(time.time() - t0, 3)
        deadline = time.time() + 5
        while json.dumps(rid) not in self.server.state.disconnects and time.time() < deadline:
            time.sleep(0.05)
        self.assertIn(json.dumps(rid), self.server.state.disconnects)

    def test_eof_grace_lets_requests_finish(self):
        b = self.bridge("--eof-grace", "5")
        rid = b.request("tools/call", modern_params({"name": "slow", "arguments": {"seconds": 0.6}}))
        b.proc.stdin.close()
        self.assertIn("result", b.response(rid))
        self.assertEqual(b.proc.wait(5), 0)

    def test_stdout_closed_exits(self):
        r, w = os.pipe()
        proc = subprocess.Popen([BRIDGE, "--datadir", self.datadir], stdin=subprocess.PIPE, stdout=w,
                                stderr=subprocess.DEVNULL, env=clean_env())
        os.close(w)
        os.close(r)  # nobody reads the bridge's stdout any more
        try:
            line = (json.dumps({"jsonrpc": "2.0", "id": 1, "method": "server/discover", "params": modern_params()}) + "\n").encode()
            for _ in range(3):
                try:
                    proc.stdin.write(line)
                    proc.stdin.flush()
                except OSError:
                    break
                time.sleep(0.2)
            self.assertEqual(proc.wait(10), 3)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            try:
                proc.stdin.close()
            except OSError:
                pass

    def test_token_never_logged(self):
        b = self.bridge("-v")
        b.call("server/discover", modern_params())
        b.call("tools/call", modern_params({"name": "echo", "arguments": {"text": "x"}}))
        b.close()
        self.assertNotIn("test-token", "".join(b.stderr))
        log = os.path.join(self.tmp, "bridge.log")
        b2 = self.bridge("-v", "--log-file", log)
        b2.call("server/discover", modern_params())
        b2.close()
        with open(log, encoding="utf-8") as f:
            text = f.read()
        self.assertIn("server/discover", text)
        self.assertNotIn("test-token", text)


def main():
    global BRIDGE
    args = sys.argv[1:]
    if "--bridge" in args:
        i = args.index("--bridge")
        BRIDGE = args[i + 1]
        del args[i:i + 2]
    if not BRIDGE or not os.path.exists(BRIDGE):
        sys.exit("pass --bridge <path to orca-mcp-bridge> or set ORCA_MCP_BRIDGE_BIN")
    BRIDGE = os.path.abspath(BRIDGE)
    unittest.main(argv=[sys.argv[0]] + args)


if __name__ == "__main__":
    main()
