#!/usr/bin/env python3
"""Integration check of orca-mcp-bridge against the real MCP server inside a running OrcaSlicer.

Start OrcaSlicer (ideally a test instance: --datadir <temp dir>, ORCA_MCP_ENABLE=1,
ORCA_MCP_PORT=<free port>), enable its MCP server, then run:

    python tests/mcp_bridge/integration_test.py --bridge <path to orca-mcp-bridge> [--datadir DIR]
        [--endpoint-file FILE] [--call TOOL '{"arg": 1}'] [--era modern|legacy|both] [--no-raw]

The URL and token come from the endpoint file the app writes (<data dir>/mcp/endpoint.json; the
default data directory is used when neither --datadir nor --endpoint-file is given). The token is
never printed. Exit status 0 when every check passed. Read-only: the only tool called is the one
named with --call.

Checks:
  raw HTTP (server conformance, skipped with --no-raw): missing token -> 401, Origin -> 403,
    wrong Host -> 403, GET -> 405, modern server/discover with mirrored headers -> 200;
  through the bridge, modern era: server/discover, tools/list, optional tools/call;
  through the bridge, legacy era: initialize, notifications/initialized, tools/list, ping;
  the bridge exits 0 when stdin closes.
"""
import argparse
import http.client
import json
import os
import sys
import time
import urllib.parse

sys.dont_write_bytecode = True  # keep the source tree clean
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_bridge  # noqa: E402
from test_bridge import Bridge, clean_env, modern_params  # noqa: E402

MODERN = "2026-07-28"
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok))
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name, (" - " + detail) if detail else ""))
    return ok


def default_endpoint_file():
    if sys.platform == "win32":
        base = os.path.join(os.environ.get("APPDATA", ""), "OrcaSlicer")
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support/OrcaSlicer")
    else:
        base = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "OrcaSlicer")
    return os.path.join(base, "mcp", "endpoint.json")


def raw_post(url, body, headers):
    u = urllib.parse.urlsplit(url)
    conn = http.client.HTTPConnection(u.hostname, u.port, timeout=30)
    try:
        conn.putrequest("POST", u.path or "/", skip_host=True, skip_accept_encoding=True)
        for k, v in headers.items():
            conn.putheader(k, v)
        data = json.dumps(body).encode()
        conn.putheader("Content-Length", str(len(data)))
        conn.endheaders(data)
        r = conn.getresponse()
        return r.status, r.getheader("Content-Type"), r.read()
    finally:
        conn.close()


def raw_get(url, headers):
    u = urllib.parse.urlsplit(url)
    conn = http.client.HTTPConnection(u.hostname, u.port, timeout=30)
    try:
        conn.putrequest("GET", u.path or "/", skip_host=True, skip_accept_encoding=True)
        for k, v in headers.items():
            conn.putheader(k, v)
        conn.endheaders()
        r = conn.getresponse()
        r.read()
        return r.status
    finally:
        conn.close()


def raw_checks(url, token):
    u = urllib.parse.urlsplit(url)
    host = "%s:%d" % (u.hostname, u.port)
    body = {"jsonrpc": "2.0", "id": 1, "method": "server/discover", "params": modern_params()}
    base = {"Host": host, "Content-Type": "application/json", "Accept": "application/json, text/event-stream",
            "MCP-Protocol-Version": MODERN, "Mcp-Method": "server/discover"}
    auth = dict(base, Authorization="Bearer " + token)
    st, ctype, data = raw_post(url, body, auth)
    ok = st == 200
    if ok:
        res = json.loads(data).get("result", {}) if (ctype or "").startswith("application/json") else {}
        check("raw: server/discover -> 200", True, "supportedVersions %s" % res.get("supportedVersions"))
    else:
        check("raw: server/discover -> 200", False, "HTTP %d %s" % (st, data[:200]))
    st, _, _ = raw_post(url, body, base)
    check("raw: missing token -> 401", st == 401, "HTTP %d" % st)
    st, _, _ = raw_post(url, body, dict(base, Authorization="Bearer wrong-" + "x" * 8))
    check("raw: wrong token -> 401", st == 401, "HTTP %d" % st)
    st, _, _ = raw_post(url, body, dict(auth, Origin="https://evil.example"))
    check("raw: Origin header -> 403", st == 403, "HTTP %d" % st)
    st, _, _ = raw_post(url, body, dict(auth, Host="evil.example:%d" % u.port))
    check("raw: foreign Host -> 403", st == 403, "HTTP %d" % st)
    st = raw_get(url, {"Host": host, "Authorization": "Bearer " + token, "Accept": "text/event-stream"})
    check("raw: GET -> 405", st == 405, "HTTP %d" % st)
    bad = dict(auth, **{"Mcp-Method": "tools/list"})
    st, _, data = raw_post(url, body, bad)
    code = None
    try:
        code = json.loads(data)["error"]["code"]
    except Exception:  # noqa: BLE001
        pass
    check("raw: Mcp-Method mismatch -> 400 / -32020", st == 400 and code == -32020, "HTTP %d code %s" % (st, code))


def bridge_args(a):
    out = []
    if a.endpoint_file:
        out += ["--endpoint-file", a.endpoint_file]
    elif a.datadir:
        out += ["--datadir", a.datadir]
    return out


def modern_checks(a):
    b = Bridge(bridge_args(a), clean_env())
    try:
        r = b.call("server/discover", modern_params(), timeout=30)
        if not check("bridge/modern: server/discover", "result" in r, json.dumps(r.get("error"))[:300] if "error" in r else
                     "versions %s" % r["result"].get("supportedVersions")):
            return
        r = b.call("tools/list", modern_params(), timeout=30)
        tools = [t["name"] for t in r.get("result", {}).get("tools", [])]
        check("bridge/modern: tools/list", bool(tools), "%d tools: %s" % (len(tools), ", ".join(tools[:50])))
        if a.call:
            name, args = a.call[0], json.loads(a.call[1]) if len(a.call) > 1 else {}
            t0 = time.time()
            r = b.call("tools/call", modern_params({"name": name, "arguments": args}, progressToken="it"), timeout=600)
            res = r.get("result") or {}
            kinds = [c.get("type") for c in res.get("content", [])]
            check("bridge/modern: tools/call %s" % name, "result" in r and not res.get("isError"),
                  "%.1f s, content %s%s" % (time.time() - t0, kinds,
                                            "" if "result" in r else " error " + json.dumps(r.get("error"))[:300]))
    finally:
        code = b.close()
        check("bridge/modern: exit 0 on EOF", code == 0, "exit %s" % code)
        if b.bad_lines:
            check("bridge/modern: stdout carried only JSON-RPC", False, str(b.bad_lines[:2]))


def legacy_checks(a):
    b = Bridge(bridge_args(a), clean_env())
    try:
        r = b.call("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                  "clientInfo": {"name": "integration", "version": "1"}}, timeout=30)
        if not check("bridge/legacy: initialize", "result" in r,
                     ("protocolVersion %s" % r["result"].get("protocolVersion")) if "result" in r else json.dumps(r)[:300]):
            return
        b.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        r = b.call("tools/list", timeout=30)
        check("bridge/legacy: tools/list", "result" in r, "%d tools" % len(r.get("result", {}).get("tools", [])))
        r = b.call("ping", timeout=30)
        check("bridge/legacy: ping", "result" in r)
    finally:
        code = b.close()
        check("bridge/legacy: exit 0 on EOF", code == 0, "exit %s" % code)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bridge", default=os.environ.get("ORCA_MCP_BRIDGE_BIN"))
    ap.add_argument("--datadir")
    ap.add_argument("--endpoint-file")
    ap.add_argument("--era", choices=["modern", "legacy", "both"], default="both")
    ap.add_argument("--call", nargs="+", metavar=("TOOL", "ARGS_JSON"))
    ap.add_argument("--no-raw", action="store_true")
    a = ap.parse_args()
    if not a.bridge or not os.path.exists(a.bridge):
        sys.exit("pass --bridge <path to orca-mcp-bridge> or set ORCA_MCP_BRIDGE_BIN")
    test_bridge.BRIDGE = os.path.abspath(a.bridge)

    ep_path = a.endpoint_file or (os.path.join(a.datadir, "mcp", "endpoint.json") if a.datadir else default_endpoint_file())
    if not os.path.exists(ep_path):
        sys.exit("no endpoint file at %s: is OrcaSlicer running with its MCP server enabled?" % ep_path)
    with open(ep_path, encoding="utf-8") as f:
        ep = json.load(f)
    url = ep.get("url") or "http://127.0.0.1:%d/mcp" % ep["port"]
    print("endpoint file %s: url %s, pid %s, protocolVersions %s, token %s" %
          (ep_path, url, ep.get("pid"), ep.get("protocolVersions"), "present" if ep.get("token") else "MISSING"))

    if not a.no_raw:
        raw_checks(url, ep.get("token", ""))
    if a.era in ("modern", "both"):
        modern_checks(a)
    if a.era in ("legacy", "both"):
        legacy_checks(a)
    failed = [n for n, ok in RESULTS if not ok]
    print("\n%d checks, %d failed" % (len(RESULTS), len(failed)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
