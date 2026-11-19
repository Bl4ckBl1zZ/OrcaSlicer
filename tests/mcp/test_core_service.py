"""The server as an installation has it: off by default, switched on by preferences, one port.

These tests launch instances of their own without the ORCA_MCP_* overrides the rest of the suite
uses, so they take a few seconds each.
"""

import json
import socket
import sys
import time

import pytest

from orca_mcp import McpClient, OrcaApp, fresh_data_dir


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def is_listening(port: int) -> bool:
    with socket.socket() as sock:
        sock.settimeout(0.5)
        return sock.connect_ex(("127.0.0.1", port)) == 0


def test_off_by_default(run_dir):
    """A fresh installation listens on nothing and writes no endpoint file."""
    app = OrcaApp(fresh_data_dir(run_dir, "service_default"), env_control=False)
    app.launch()
    try:
        app.wait_until_up()
        assert not app.endpoint_file.exists()
        assert not (app.data_dir / "mcp" / "token").exists(), "no token is created until the server is used"
        assert not is_listening(13720), "the default port must not be open"
    finally:
        app.stop()


def test_enabled_by_preferences(run_dir):
    """mcp_enabled / mcp_port in the application configuration start the server on that port."""
    port = free_port()
    app = OrcaApp(fresh_data_dir(run_dir, "service_enabled"), env_control=False,
                  app_config={"mcp_enabled": "true", "mcp_port": str(port)})
    app.launch()
    second = None
    try:
        app.wait_for_endpoint(120)
        endpoint = app.endpoint
        assert endpoint["schema"] == 1 and endpoint["port"] == port and endpoint["url"] == f"http://127.0.0.1:{port}/mcp"
        assert endpoint["app"] == "OrcaSlicer" and endpoint["appVersion"] and "2026-07-28" in endpoint["protocolVersions"]
        # The token was generated and stored; it is 32 random bytes in URL-safe base64.
        token = (app.data_dir / "mcp" / "token").read_text(encoding="utf-8").strip()
        assert token == endpoint["token"] and len(token) == 43

        client = McpClient(app.url, token).connect()
        names = [tool["name"] for tool in client.list_tools()]
        assert "app_status" in names
        assert not [name for name in names if name.startswith("debug_")], "debug tools are not part of a normal run"
        assert client.tool("app_status")["server"]["debug_tools"] is False

        # A second instance configured for the same port does not start its server (and does not pick another port).
        second = OrcaApp(fresh_data_dir(run_dir, "service_port_taken"), env_control=False,
                         app_config={"mcp_enabled": "true", "mcp_port": str(port)})
        second.launch()
        second.wait_until_up()
        assert not second.endpoint_file.exists()
        assert "not available" in second.log_text() or "MCP: server not started" in second.log_text()
        assert client.tool("app_status")["app"], "the first instance is unaffected"

        if sys.platform == "win32":
            # Closing the application removes the endpoint file and frees the port.
            assert app.close_window(), "the application should exit when its window is closed"
            assert not app.endpoint_file.exists()
            time.sleep(0.5)
            assert not is_listening(port)
    finally:
        app.stop()
        if second is not None:
            second.stop()


def test_token_is_kept_between_runs(run_dir):
    port = free_port()
    data_dir = fresh_data_dir(run_dir, "service_token")
    tokens = []
    for _ in range(2):
        app = OrcaApp(data_dir, env_control=False, app_config={"mcp_enabled": "true", "mcp_port": str(port)})
        app.launch()
        try:
            app.wait_for_endpoint(120)
            tokens.append(json.loads(app.endpoint_file.read_text(encoding="utf-8"))["token"])
        finally:
            app.stop()
        time.sleep(1.0)
    assert tokens[0] == tokens[1]
