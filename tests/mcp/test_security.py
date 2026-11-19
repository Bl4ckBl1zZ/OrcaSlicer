"""Security: names that would break the G-code, paths that leave the allowed folders or name devices,
secrets in preset values, embedded G-code and scripts in projects, request bodies nested too deep.

The module has its own instance. Its data folder holds a user printer preset carrying a print host and
an API key, which the agent cannot write itself: the preset is saved through MCP by a first run, the
secrets are added to its file, and the instance the tests use is started on that folder.
"""

import http.client
import json
import os
import sys
from pathlib import Path

import pytest

from orca_mcp import McpClient, OrcaApp, fresh_data_dir

SECRET_PRINTER = "MCP Secret Printer"
SECRET_HOST = "192.0.2.77:7125"
SECRET_KEY = "mcp-test-secret-0123456789abcdef"
REDACTED = "<redacted>"

windows_only = pytest.mark.skipif(sys.platform != "win32", reason="Windows path rules")


@pytest.fixture(scope="module")
def sec_app(run_dir):
    data_dir = fresh_data_dir(run_dir, "security")
    first = OrcaApp(data_dir).start()
    try:
        c = McpClient(first.url, first.token, era="modern", timeout=300).connect()
        parent = c.tool("preset_diff", type="printer")["b"]
        c.tool("settings_set", scope="printer", values={"printable_height": 222})
        c.tool("preset_save", type="printer", name=SECRET_PRINTER)
    finally:
        first.stop()
    saved = sorted(data_dir.glob(f"user/*/machine/{SECRET_PRINTER}.json"))
    assert saved, f"preset_save wrote no {SECRET_PRINTER}.json under {data_dir / 'user'}"
    preset = json.loads(saved[0].read_text(encoding="utf-8"))
    preset.update({"print_host": SECRET_HOST, "printhost_apikey": SECRET_KEY})
    saved[0].write_text(json.dumps(preset, indent=4) + "\n", encoding="utf-8", newline="\n")

    app = OrcaApp(data_dir).start()
    app.secret_parent = parent
    yield app
    app.stop()


@pytest.fixture(scope="module")
def sc(sec_app) -> McpClient:
    return McpClient(sec_app.url, sec_app.token, era="modern", timeout=300).connect()


def workspace(c: McpClient) -> Path:
    roots = c.tool("app_status")["write_roots"]
    return Path(next(r for r in roots if r.endswith("/mcp/workspace")))


def add_cube(c: McpClient) -> str:
    return c.tool("object_add_primitive", shape="cube", size=[20, 20, 20])["created"][0]["id"]


def refused(c: McpClient, tool: str, reason: str | None = None, **arguments):
    failure = c.tool_error(tool, **arguments)
    assert failure.code == "path_denied", failure
    if reason is not None:
        assert (failure.details or {}).get("reason") == reason, failure
    return failure


# ---------------------------------------------------------------------------------------------
# Names: no control characters (they would end a G-code comment and start a command)
# ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("name", ["a\nb", "x\nM104 S300", "a\rb", "tab\there", "nul\x00", "del\x7f"])
def test_names_with_control_characters_refused(sc, name):
    sc.tool("project_new", on_unsaved="discard")
    oid = add_cube(sc)
    failure = sc.tool_error("object_update", targets=[oid], name=name)
    assert failure.code == "invalid_argument", failure
    assert failure.details["argument"] == "name"
    assert sc.tool_error("object_add_primitive", shape="cube", size=[10, 10, 10], name=name).code == "invalid_argument"
    assert sc.tool_error("preset_save", type="process", name=name).code == "invalid_argument"
    # A plain name still works, and the refused ones changed nothing.
    renamed = sc.tool("object_update", targets=[oid], name="plain name")
    assert renamed["changed"][0]["name"] == "plain name"


# ---------------------------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------------------------

@windows_only
@pytest.mark.parametrize("path", ["\\\\127.0.0.1\\share\\a.3mf", "//127.0.0.1/share/a.3mf", "\\\\?\\C:\\Temp\\a.3mf",
                                  "\\\\.\\C:\\Temp\\a.3mf"])
def test_unc_and_device_paths_refused(sc, path):
    refused(sc, "project_open", "unc", path=path)
    sc.tool("project_new", on_unsaved="discard")
    add_cube(sc)
    refused(sc, "export_file", "unc", kind="3mf", path=path)


@pytest.mark.parametrize("path", ["a.3mf", "mcp/workspace/a.3mf", "./a.3mf", "..\\a.3mf"] +
                         (["C:a.3mf", "\\a.3mf"] if sys.platform == "win32" else []))
def test_relative_paths_refused(sc, path):
    refused(sc, "project_open", "relative", path=path)
    sc.tool("project_new", on_unsaved="discard")
    add_cube(sc)
    refused(sc, "export_file", "relative", kind="3mf", path=path)


@windows_only
@pytest.mark.parametrize("name", ["CON.3mf", "con.3mf", "NUL.3mf", "nul.backup.3mf", "COM1.3mf", "LPT9.3mf", "AUX .3mf", "PRN.3mf"])
def test_reserved_device_names_refused(sc, name):
    ws = workspace(sc)
    refused(sc, "project_open", "device_name", path=str(ws / name))
    sc.tool("project_new", on_unsaved="discard")
    add_cube(sc)
    refused(sc, "export_file", "device_name", kind="3mf", path=str(ws / name))
    # In a folder name as well.
    refused(sc, "export_file", "device_name", kind="3mf", path=str(ws / "con" / "a.3mf"))


@windows_only
@pytest.mark.parametrize("suffix", [":evil", "::$DATA", ":evil:$DATA"])
def test_alternate_data_streams_refused(sc, suffix):
    ws = workspace(sc)
    path = str(ws / "a.3mf") + suffix
    refused(sc, "project_open", "stream", path=path)
    sc.tool("project_new", on_unsaved="discard")
    add_cube(sc)
    refused(sc, "export_file", "stream", kind="3mf", path=path)
    assert not any(p.name.startswith("a.3mf") for p in ws.iterdir())


@windows_only
@pytest.mark.parametrize("name", ["a.3mf.", "a.3mf ", "a.3mf. .", "folder./a.3mf"])
def test_trailing_dot_or_space_refused(sc, name):
    ws = workspace(sc)
    sc.tool("project_new", on_unsaved="discard")
    add_cube(sc)
    refused(sc, "export_file", "trailing_dot_or_space", kind="3mf", path=str(ws) + "/" + name)


def test_dot_dot_cannot_leave_the_workspace(sc):
    ws = workspace(sc)
    sc.tool("project_new", on_unsaved="discard")
    add_cube(sc)
    # One level up: the data folder, which is not a write root.
    escaped = str(ws) + os.sep + ".." + os.sep + "escape.3mf"
    refused(sc, "export_file", kind="3mf", path=escaped)
    assert not (ws.parent / "escape.3mf").exists()
    # Far past the drive root: lands in a system folder, outside every root.
    far = str(ws) + (os.sep + "..") * (len(ws.parts) + 3) + os.sep + "Windows" + os.sep + "escape.3mf"
    refused(sc, "export_file", kind="3mf", path=far)
    refused(sc, "project_open", path=far)
    # debug_paint_3mf goes through the same checks.
    refused(sc, "debug_paint_3mf", path=escaped)


def remove_link(path: Path):
    """Removes a link (to a folder: a symbolic link or a junction), never what it points to."""
    if not os.path.lexists(path):
        return
    if sys.platform == "win32" and os.path.isdir(path):
        os.rmdir(path)
    else:
        os.unlink(path)


def test_link_out_of_the_workspace_refused(sc, run_dir):
    """A symbolic link or junction inside the workspace must not lead the agent out of it."""
    ws = workspace(sc)
    outside = Path(run_dir) / "security_outside"
    outside.mkdir(exist_ok=True)
    link = ws / "link_out"
    remove_link(link)
    try:
        os.symlink(outside, link, target_is_directory=True)
    except OSError:
        if sys.platform != "win32":
            pytest.skip("cannot create a symbolic link here")
        import _winapi
        try:
            _winapi.CreateJunction(str(outside), str(link))
        except OSError:
            pytest.skip("cannot create a symbolic link or junction here")
    try:
        sc.tool("project_new", on_unsaved="discard")
        add_cube(sc)
        refused(sc, "export_file", kind="3mf", path=str(link / "through_link.3mf"))
        assert not (outside / "through_link.3mf").exists()
        # A file link inside the workspace is not written through either.
        target = outside / "victim.3mf"
        target.write_bytes(b"not yours")
        file_link = ws / "file_link.3mf"
        remove_link(file_link)
        try:
            os.symlink(target, file_link)
        except OSError:
            return  # file links need a privilege on Windows; the folder case above is the main one
        try:
            refused(sc, "export_file", kind="3mf", path=str(file_link), overwrite=True)
            assert target.read_bytes() == b"not yours"
        finally:
            remove_link(file_link)
    finally:
        remove_link(link)


def test_hard_link_not_written_through(sc, run_dir):
    """A hard link in the workspace to a file elsewhere: writing over it would change that file."""
    ws = workspace(sc)
    target = Path(run_dir) / "security_hard_link_target.3mf"
    target.write_bytes(b"not yours")
    link = ws / "hard_link.3mf"
    link.unlink(missing_ok=True)
    try:
        os.link(target, link)
    except OSError:
        pytest.skip("cannot create a hard link here")
    try:
        sc.tool("project_new", on_unsaved="discard")
        add_cube(sc)
        refused(sc, "export_file", "hard_link", kind="3mf", path=str(link), overwrite=True)
        refused(sc, "project_save", "hard_link", path=str(link), overwrite=True)
        assert target.read_bytes() == b"not yours"
    finally:
        link.unlink(missing_ok=True)


@windows_only
def test_case_variant_of_the_workspace_accepted(sc):
    """NTFS compares names without case: the workspace written in other case is the same folder."""
    ws = workspace(sc)
    variant = Path(str(ws).upper()) / "case_variant.3mf"
    sc.tool("project_new", on_unsaved="discard")
    add_cube(sc)
    sc.tool("export_file", kind="3mf", path=str(variant), overwrite=True)
    assert (ws / "case_variant.3mf").exists()
    result = sc.call("project_open", {"path": str(variant).lower(), "on_unsaved": "discard"})
    assert not result.is_error or result.error_code == "untrusted_gcode", result.data


# ---------------------------------------------------------------------------------------------
# Secrets
# ---------------------------------------------------------------------------------------------

def assert_no_secret(document):
    text = json.dumps(document)
    assert SECRET_KEY not in text and SECRET_HOST not in text, text[:2000]


def test_settings_get_redacts_host_and_key(sc):
    sc.tool("preset_select", type="printer", name=SECRET_PRINTER, on_unsaved="discard")
    keys = ["print_host", "printhost_apikey", "printhost_user", "printhost_password"]
    stored = sc.tool("settings_get", scope="printer", keys=keys)
    assert_no_secret(stored)
    assert stored["values"]["print_host"] == REDACTED
    assert stored["values"]["printhost_apikey"] == REDACTED
    effective = sc.tool("settings_get", scope="printer", keys=keys, effective=True)
    assert_no_secret(effective)
    assert effective["values"]["printhost_apikey"]["value"] == REDACTED
    everything = sc.tool("settings_get", scope="printer")
    assert_no_secret(everything)


def test_preset_diff_redacts_host_and_key(sc, sec_app):
    diff = sc.tool("preset_diff", type="printer", a=SECRET_PRINTER, b=sec_app.secret_parent, limit=1000)
    assert_no_secret(diff)
    by_key = {d["key"]: d for d in diff["differences"]}
    assert by_key["print_host"]["a"] == REDACTED
    assert by_key["printhost_apikey"]["a"] == REDACTED
    assert by_key["printhost_apikey"]["b"] in ("", None)
    assert_no_secret(sc.tool("preset_diff", type="printer", a=sec_app.secret_parent, b=SECRET_PRINTER))


@pytest.mark.parametrize("key, value", [("print_host", "10.0.0.1"), ("printhost_apikey", "x"), ("host_type", "octoprint"),
                                        ("flashforge_serial_number", "x"), ("post_process", "C:/evil.bat")])
def test_secret_and_script_keys_not_writable(sc, key, value):
    scope = "process" if key == "post_process" else "printer"
    failure = sc.tool_error("settings_set", scope=scope, values={key: value})
    assert failure.code == "forbidden_key", failure


# ---------------------------------------------------------------------------------------------
# Projects: embedded G-code needs the user's permission
# ---------------------------------------------------------------------------------------------

def test_trusting_embedded_gcode_needs_the_preference(sc, sec_app):
    ws = workspace(sc)
    sc.tool("preset_select", type="printer", name=sec_app.secret_parent, on_unsaved="discard")
    sc.tool("project_new", on_unsaved="discard")
    add_cube(sc)
    project = ws / "trust.3mf"
    sc.tool("export_file", kind="3mf", path=str(project), overwrite=True)
    failure = sc.tool_error("project_open", path=str(project), trust_embedded_gcode=True, on_unsaved="discard")
    assert failure.code == "forbidden_key", failure
    assert "mcp_allow_gcode_edit" in failure.hint
    # Refused before anything happened: the current project is still there.
    assert sc.tool("scene_get")["object_count"] == 1
    # Without trust a project on system presets opens; had the application asked about its G-code,
    # the load would have been undone.
    result = sc.call("project_open", {"path": str(project), "on_unsaved": "discard"})
    if result.is_error:
        assert result.error_code == "untrusted_gcode", result.data
        assert sc.tool("scene_get")["object_count"] == 0
    else:
        assert len(result.data["changed"]["objects"]) == 1


# ---------------------------------------------------------------------------------------------
# Protocol: bodies nested too deep are refused before they are parsed
# ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("body", ["[" * 10000, "[" * 10000 + "]" * 10000, '{"a":' * 5000 + "1" + "}" * 5000,
                                  '{"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": ' + "[" * 100 + "]" * 100 + "}"])
def test_deeply_nested_body_refused(sc, body):
    message, headers = sc.build("server/discover")
    connection = http.client.HTTPConnection(sc.host, sc.port, timeout=30)
    try:
        connection.request("POST", sc.path, body=body.encode("utf-8"),
                           headers={"Authorization": f"Bearer {sc.token}", "Content-Type": "application/json", **headers})
        response = connection.getresponse()
        status = response.status
        reply = json.loads(response.read())
    finally:
        connection.close()
    assert status == 400
    assert reply["error"]["code"] == -32700
    assert "64" in reply["error"]["message"]
    # The server is still there.
    assert "version" in sc.tool("app_status")
