"""Printer tools: printer_list, printer_status, print_send, printer_control (DESIGN.md D13).

No real printer is ever involved:
  - Bambu Lab printers are simulated in-process by the test-only fake printer agent
    (src/slic3r/Mcp/testing/FakePrinters.cpp; build option SLIC3R_MCP_TEST_PRINTERS, activated by
    ORCA_MCP_FAKE_PRINTERS=<fixture>). It records every job and command into a dump directory.
  - OctoPrint and Moonraker hosts are Python servers on 127.0.0.1 (orca_mcp/fake_hosts.py).
  - Every instance has its own data directory; no login, no plug-in.

The confirmation window that guards every send is answered the way a person answers it: by
clicking its buttons through the operating system (orca_mcp/confirm_ui.py). No MCP tool can.

Skipped unless the application under test was built with SLIC3R_MCP_TEST_PRINTERS (app_status
reports printers.fake_printers) and the platform is Windows (the click harness is Win32).
"""

from __future__ import annotations

import json
import sys
import time
import zipfile
from pathlib import Path

import pytest

from orca_mcp import McpClient, OrcaApp, fresh_data_dir
from orca_mcp.app import seed_data_dir
from orca_mcp import confirm_ui
from orca_mcp.client import ToolFailure
from orca_mcp.fake_hosts import FakeMoonraker, FakeOctoPrint

pytestmark = pytest.mark.skipif(sys.platform != "win32", reason="the confirmation click harness is Win32")

A1_PRESET = "Bambu Lab A1 0.4 nozzle"
BAMBU_MODEL = {"vendor": "BBL", "model": "Bambu Lab A1", "nozzle_diameter": "0.4"}
CONFIRM_TIMEOUT_S = 8

ACCESS_CODES = {"FAKEA1STUDIO0001": "31415926", "FAKEA1DESK000002": "27182818",
                "FAKEA1GARAGE0003": "16180339", "FAKEP1SOFFICE004": "14142135"}
SECRETS = list(ACCESS_CODES) + list(ACCESS_CODES.values()) + ["192.0.2.", "fake-octo-key-777", "fake-moon-key-888",
                                                                "fake-prusa-key-999"]


def white_black_ams(third=None):
    return [{"id": 0, "trays": [{"type": "PLA", "color": "FFFFFFFF", "remain": 80, "idx": "GFA00"},
                                {"type": "PLA", "color": "000000FF", "remain": 60, "idx": "GFA00"},
                                third,
                                {"type": "PETG", "color": "FF0000FF", "remain": 100, "idx": "GFG00"}]}]


def fixture_doc(dump_dir: Path) -> dict:
    return {
        "dump_dir": str(dump_dir),
        "progress_step": 0,      # a running print stays running
        "finish_hold_s": 2,
        "printers": [
            {"dev_id": "FAKEA1STUDIO0001", "name": "A1 Studio", "dev_type": "N2S", "ip": "192.0.2.21",
             "access_code": ACCESS_CODES["FAKEA1STUDIO0001"], "ams": white_black_ams(),
             "external": {"type": "PLA", "color": "FFFFFFFF"}},
            {"dev_id": "FAKEA1DESK000002", "name": "A1 Desk", "dev_type": "N2S", "ip": "192.0.2.22",
             "access_code": ACCESS_CODES["FAKEA1DESK000002"], "ams": white_black_ams()},
            {"dev_id": "FAKEA1GARAGE0003", "name": "A1 Garage", "dev_type": "N2S", "ip": "192.0.2.23",
             "access_code": ACCESS_CODES["FAKEA1GARAGE0003"], "state": "RUNNING", "progress": 40,
             "remaining_min": 30, "layer": [20, 50], "subtask_name": "Busy job",
             "nozzle_temp": [219, 220], "bed_temp": [64, 65], "ams": white_black_ams(),
             "hms": [{"attr": 0x05000200, "code": 0x00020001}]},
            {"dev_id": "FAKEP1SOFFICE004", "name": "P1S Office", "dev_type": "C12", "ip": "192.0.2.24",
             "access_code": ACCESS_CODES["FAKEP1SOFFICE004"], "ams": white_black_ams()},
        ],
    }


# ---------------------------------------------------------------------------------------------
# Instances
# ---------------------------------------------------------------------------------------------

@pytest.fixture(scope="module")
def hosts():
    octo = FakeOctoPrint(api_key="fake-octo-key-777").start()
    moon = FakeMoonraker(api_key="fake-moon-key-888").start()
    yield octo, moon
    octo.stop()
    moon.stop()


@pytest.fixture(scope="module")
def printers_app(run_dir, hosts):
    """An instance with both printer switches on, four fake Bambu printers and two host printers."""
    octo, moon = hosts
    data_dir = fresh_data_dir(run_dir, "printers")
    dump_dir = run_dir / "printers_dump"
    if dump_dir.exists():
        for f in dump_dir.iterdir():
            f.unlink()
    fixture = run_dir / "printers_fixture.json"
    fixture.write_text(json.dumps(fixture_doc(dump_dir), indent=2), encoding="utf-8")
    env = {"ORCA_MCP_FAKE_PRINTERS": str(fixture), "ORCA_MCP_CONFIRM_TIMEOUT_S": str(CONFIRM_TIMEOUT_S)}
    prefs = {"mcp_printer_read": "true", "mcp_printer_send": "true"}

    # The data directory starts with the Bambu Lab A1 as its only printer model.
    seed_data_dir(data_dir, prefs)
    conf_path = data_dir / "OrcaSlicer.conf"
    conf = json.loads(conf_path.read_text(encoding="utf-8"))
    conf["models"] = [BAMBU_MODEL]
    conf_path.write_text(json.dumps(conf, indent=4) + "\n", encoding="utf-8", newline="\n")

    # First start: create the two host printer presets (A1 based), then give them their hosts.
    first = OrcaApp(data_dir, extra_env=env, app_config=prefs).start()
    try:
        c = McpClient(first.url, first.token, era="modern", timeout=300).connect()
        c.tool("preset_select", type="printer", name=A1_PRESET, on_unsaved="discard")
        c.tool("preset_save", type="printer", name="A1 Octo")
        c.tool("preset_save", type="printer", name="A1 Moon")
        c.tool("preset_save", type="printer", name="A1 Prusa")
        c.tool("preset_select", type="printer", name=A1_PRESET, on_unsaved="discard")
    finally:
        if not first.close_window():
            first.stop()
    # A1 Octo uploads plain G-code; A1 Moon keeps the A1's "Support 3MF as G-code" (use_3mf).
    for name, host, key, kind, use_3mf in (("A1 Octo", octo, "fake-octo-key-777", "octoprint", "0"),
                                           ("A1 Moon", moon, "fake-moon-key-888", "moonraker", "1"),
                                           # A host type whose state the server cannot read; nothing listens.
                                           ("A1 Prusa", None, "fake-prusa-key-999", "prusalink", "0")):
        files = sorted(data_dir.glob(f"user/*/machine/{name}.json"))
        assert files, f"preset_save wrote no {name}.json"
        preset = json.loads(files[0].read_text(encoding="utf-8"))
        preset.update({"print_host": f"127.0.0.1:{host.port if host else 9}", "printhost_apikey": key, "host_type": kind,
                       "use_3mf": use_3mf})
        files[0].write_text(json.dumps(preset, indent=4) + "\n", encoding="utf-8", newline="\n")

    app = OrcaApp(data_dir, extra_env=env, app_config=prefs).start()
    app.dump_dir = dump_dir
    yield app
    app.stop()


@pytest.fixture(scope="module")
def pc(printers_app) -> McpClient:
    c = McpClient(printers_app.url, printers_app.token, era="modern", timeout=300).connect()
    status = c.tool("app_status")
    if not status.get("printers", {}).get("fake_printers"):
        pytest.skip("the application was built without SLIC3R_MCP_TEST_PRINTERS")
    c.tool("preset_select", type="printer", name=A1_PRESET, on_unsaved="discard")
    return c


@pytest.fixture(scope="module")
def pid(printers_app) -> int:
    return printers_app.process.pid


@pytest.fixture(scope="module")
def ids(pc) -> dict:
    """{printer name: id}, after every fake printer has been discovered."""
    deadline = time.monotonic() + 30
    while True:
        listing = pc.tool("printer_list", probe_hosts=False)
        found = {p["name"]: p["id"] for p in listing["printers"]}
        if {"A1 Studio", "A1 Desk", "A1 Garage", "P1S Office", "A1 Octo", "A1 Moon", "A1 Prusa"} <= set(found):
            return found
        if time.monotonic() > deadline:
            pytest.fail(f"printers not discovered: {listing}")
        time.sleep(1)


@pytest.fixture(scope="module")
def sliced(pc) -> dict:
    """Plate 1: two cubes, filament 1 white and filament 2 black, sliced for the A1."""
    pc.tool("project_new", on_unsaved="discard")
    filaments = pc.tool("filament_list")["filaments"]
    if len(filaments) < 2:
        pc.tool("filament_edit", action="add", color="#000000")
    pc.tool("filament_edit", action="set_color", filament=1, color="#FFFFFF")
    pc.tool("filament_edit", action="set_color", filament=2, color="#000000")
    a = pc.tool("object_add_primitive", shape="cube", size=[20, 20, 10])["created"][0]["id"]
    b = pc.tool("object_add_primitive", shape="cube", size=[20, 20, 10])["created"][0]["id"]
    pc.tool("object_update", targets=[b], filament=2)
    pc.tool("object_arrange")
    result = pc.tool("slice", wait_s=240)
    assert result["status"] == "finished", result
    return {"objects": [a, b]}


def ensure_sliced(pc):
    state = pc.tool("slice_status")["plates"][0]
    if state.get("state") != "sliced":
        assert pc.tool("slice", wait_s=240)["status"] == "finished"


def live(pc, printer_id: str) -> dict:
    status = pc.tool("printer_status", printer=printer_id, wait_s=20)
    assert status["live"] is True, status
    return status


def dump_files(app, pattern: str) -> list[Path]:
    return sorted(app.dump_dir.glob(pattern)) if app.dump_dir.exists() else []


def jobs_dumped(app) -> list[dict]:
    out = []
    for f in dump_files(app, "[0-9][0-9][0-9]_*.json"):
        if f.name.endswith("_command.json"):
            continue
        doc = json.loads(f.read_text(encoding="utf-8"))
        if not doc.get("verify_job"):
            doc["_file"] = f
            out.append(doc)
    return out


def commands_dumped(app) -> list[dict]:
    return [json.loads(f.read_text(encoding="utf-8")) for f in dump_files(app, "[0-9][0-9][0-9]_command.json")]


def wait_job(pc, job: str, timeout: float = 60) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        status = pc.tool("job_status", job=job, wait_s=10)
        if status["status"] != "running" or time.monotonic() > deadline:
            return status


def send(pc, **arguments) -> dict:
    """print_send that returns at once with the running job."""
    result = pc.tool("print_send", wait_s=0, **arguments)
    assert result["status"] == "running", result
    return result


def no_secrets(document) -> None:
    text = json.dumps(document)
    for secret in SECRETS:
        assert secret not in text, f"{secret!r} leaked: {text[:400]}"


def audit_entries(pc, tool: str | None = None) -> list[dict]:
    return [e for e in pc.tool("history")["journal"]
            if "printer" in (e.get("scope") or {}) and (tool is None or e["tool"] == tool)]


# ---------------------------------------------------------------------------------------------
# Switches off (the shared instance: defaults) and read-only
# ---------------------------------------------------------------------------------------------

def test_switches_off_by_default(modern):
    status = modern.tool("app_status")["printers"]
    assert status["read_allowed"] is False and status["send_allowed"] is False
    for tool, args in (("printer_list", {}), ("printer_status", {"printer": "x"}),
                       ("print_send", {"printer": "x"}), ("printer_control", {"printer": "x", "action": "pause"})):
        failure = modern.tool_error(tool, **args)
        assert failure.code == "printer_access_disabled", failure
        assert "Preferences" in failure.hint and "no tool can" in failure.hint
    # Every refusal is in the journal.
    refused = [e for e in modern.tool("history")["journal"] if e["key"] == "refused"]
    assert {e["tool"] for e in refused} >= {"printer_list", "printer_status", "print_send", "printer_control"}


def test_no_tool_can_switch_access_on(modern):
    """Nothing in the tool surface writes application preferences; attempts leave the switches off."""
    for tool in modern.list_tools():
        props = json.dumps(tool.get("inputSchema", {}))
        assert "mcp_printer" not in props, tool["name"]
    for scope in ("printer", "process", "filament"):
        with pytest.raises(ToolFailure):
            modern.tool("settings_set", scope=scope, values={"mcp_printer_send": True})
        with pytest.raises(ToolFailure):
            modern.tool("settings_set", scope=scope, values={"mcp_printer_read": "true"})
    status = modern.tool("app_status")["printers"]
    assert status["read_allowed"] is False and status["send_allowed"] is False
    assert modern.tool_error("printer_list").code == "printer_access_disabled"


def test_read_only_switch(launch_app):
    app = launch_app("printers_read_only", app_config={"mcp_printer_read": "true"})
    c = McpClient(app.url, app.token, era="modern").connect()
    listing = c.tool("printer_list")
    assert listing["printers"] == [] and "note" in listing
    assert c.tool_error("print_send", printer="anything").code == "printer_access_disabled"
    assert c.tool_error("printer_control", printer="anything", action="stop").code == "printer_access_disabled"


# ---------------------------------------------------------------------------------------------
# printer_list / printer_status
# ---------------------------------------------------------------------------------------------

def test_list(pc, ids, hosts):
    listing = pc.tool("printer_list")
    no_secrets(listing)
    by_name = {p["name"]: p for p in listing["printers"]}
    assert listing["selected_printer_preset"] == A1_PRESET
    for name in ("A1 Studio", "A1 Desk", "A1 Garage", "P1S Office"):
        p = by_name[name]
        assert p["kind"] == "bambu" and p["connection"] == "lan" and p["id"].startswith("prn_")
        assert p["online"] is True
    assert by_name["A1 Studio"]["model"] == "Bambu Lab A1"
    assert by_name["A1 Studio"]["compatibility"]["compatible"] is True
    assert by_name["P1S Office"]["compatibility"]["compatible"] is False
    assert by_name["P1S Office"]["compatibility"]["reasons"]
    octo = by_name["A1 Octo"]
    assert octo["kind"] == "octoprint" and octo["connection"] == "host" and octo["id"].startswith("host_")
    assert octo["online"] is True and octo["state"] == "idle"
    assert by_name["A1 Moon"]["kind"] == "moonraker" and by_name["A1 Moon"]["state"] == "idle"
    # Ids are stable.
    again = {p["name"]: p["id"] for p in pc.tool("printer_list", probe_hosts=False)["printers"]}
    assert again == {n: p["id"] for n, p in by_name.items()}


def test_status_bambu_idle_with_trays(pc, ids):
    status = live(pc, ids["A1 Studio"])
    no_secrets(status)
    assert status["state"] == "idle" and status["online"] is True
    trays = {t["tray"]: t for t in status["trays"]}
    assert trays["A1"]["type"] == "PLA" and trays["A1"]["color"].upper() == "#FFFFFF" and trays["A1"]["empty"] is False
    assert trays["A2"]["color"].upper() == "#000000"
    assert trays["A3"]["empty"] is True
    assert trays["A4"]["type"] == "PETG"
    assert "ext" in trays
    assert status["nozzle"]
    assert "temperatures" in status


def test_status_bambu_printing(pc, ids):
    status = live(pc, ids["A1 Garage"])
    assert status["state"] == "printing"
    job = status["job"]
    assert job["name"] == "Busy job" and job["percent"] == 40 and job["layer"] == [20, 50]
    assert job["remaining_s"] == 30 * 60
    assert status["errors"], status  # the fixture's HMS code


def test_status_hosts(pc, ids, hosts):
    octo, moon = hosts
    octo.state.state, octo.state.progress, octo.state.job_name, octo.state.time_left_s = "printing", 42.5, "part.gcode", 600
    octo.state.nozzle, octo.state.bed = (210.0, 210.0), (60.0, 60.0)
    try:
        status = pc.tool("printer_status", printer=ids["A1 Octo"])
        no_secrets(status)
        assert status["state"] == "printing" and status["job"]["percent"] == 42 and status["job"]["remaining_s"] == 600
        assert status["temperatures"]["nozzle"]["current"] == 210.0
    finally:
        octo.state.state, octo.state.progress, octo.state.job_name = "idle", 0.0, ""
    moon.state.layer = (3, 120)
    status = pc.tool("printer_status", printer="A1 Moon")  # by name
    assert status["state"] == "idle" and status["id"] == ids["A1 Moon"]


def test_status_unknown_printer(pc, ids):
    failure = pc.tool_error("printer_status", printer="No such printer")
    assert failure.code == "printer_not_found"
    no_secrets(failure.payload)


# ---------------------------------------------------------------------------------------------
# print_send refusals (nothing is asked, nothing is sent)
# ---------------------------------------------------------------------------------------------

def test_refuse_not_sliced(pc, ids, sliced):
    pc.tool("plate_manage", action="add")
    empty = len(pc.tool("scene_get", detail="summary")["plates"])
    try:
        failure = pc.tool_error("print_send", printer=ids["A1 Studio"], plate=empty)
        assert failure.code == "not_sliced", failure
    finally:
        pc.tool("plate_manage", action="delete", plate=empty)


def test_refuse_stale_slice(pc, ids, sliced):
    ensure_sliced(pc)
    pc.tool("object_transform", targets=[sliced["objects"][0]], translate=[1, 0, 0])
    failure = pc.tool_error("print_send", printer=ids["A1 Studio"])
    assert failure.code == "slice_stale", failure
    pc.tool("history", action="undo")
    ensure_sliced(pc)


def test_refuse_busy_and_incompatible(pc, ids, sliced):
    ensure_sliced(pc)
    live(pc, ids["A1 Garage"])
    assert pc.tool_error("print_send", printer=ids["A1 Garage"]).code == "printer_busy"
    live(pc, ids["P1S Office"])
    failure = pc.tool_error("print_send", printer=ids["P1S Office"])
    assert failure.code == "printer_incompatible" and failure.details["reasons"]


def test_refuse_status_unknown(pc, ids, sliced):
    ensure_sliced(pc)
    live(pc, ids["A1 Garage"])  # A1 Studio is no longer the connected printer
    time.sleep(16)
    assert pc.tool_error("print_send", printer=ids["A1 Studio"]).code == "printer_status_unknown"


def test_refuse_tray_problems(pc, ids, sliced, printers_app):
    ensure_sliced(pc)
    live(pc, ids["A1 Studio"])
    before = len(jobs_dumped(printers_app))
    f = pc.tool_error("print_send", printer=ids["A1 Studio"], ams_mapping=[{"filament": 1, "tray": "A3"}, {"filament": 2, "tray": "A2"}])
    assert f.code == "tray_empty", f
    f = pc.tool_error("print_send", printer=ids["A1 Studio"], ams_mapping=[{"filament": 1, "tray": "A4"}, {"filament": 2, "tray": "A2"}])
    assert f.code == "filament_type_mismatch", f
    f = pc.tool_error("print_send", printer=ids["A1 Studio"], ams_mapping=[{"filament": 1, "tray": "A1"}])
    assert f.code == "incomplete_mapping", f
    f = pc.tool_error("print_send", printer=ids["A1 Studio"], ams_mapping=[{"filament": 1, "tray": "Z9"}, {"filament": 2, "tray": "A2"}])
    assert f.code == "tray_not_found", f
    assert len(jobs_dumped(printers_app)) == before
    assert not confirm_ui.find(printers_app.process.pid)
    reasons = [e["after"].get("reason") for e in audit_entries(pc, "print_send") if e["key"] == "send_refused"]
    assert {"tray_empty", "filament_type_mismatch"} <= set(reasons)


def test_refuse_host_offline_and_busy(pc, ids, sliced, hosts):
    octo, _ = hosts
    ensure_sliced(pc)
    octo.state.state = "offline"
    try:
        assert pc.tool_error("print_send", printer=ids["A1 Octo"]).code == "printer_offline"
    finally:
        octo.state.state = "idle"
    octo.state.state = "printing"
    try:
        assert pc.tool_error("print_send", printer=ids["A1 Octo"]).code == "printer_busy"
    finally:
        octo.state.state = "idle"
    assert not octo.uploads


# ---------------------------------------------------------------------------------------------
# The confirmation: decline, timeout, immunity, withdrawal, queue
# ---------------------------------------------------------------------------------------------

def test_decline(pc, ids, sliced, printers_app, pid):
    ensure_sliced(pc)
    live(pc, ids["A1 Studio"])
    before = len(jobs_dumped(printers_app))
    job = send(pc, printer=ids["A1 Studio"])["job"]
    shown = confirm_ui.answer(pid, "Cancel")
    text = "\n".join(shown)
    assert "A1 Studio" in text and "Plate 1" in text and "A1" in text and "A2" in text
    result = wait_job(pc, job)
    assert result["status"] == "failed" and result["result"]["outcome"] == "declined"
    assert result["result"]["error"]["code"] == "declined"
    assert len(jobs_dumped(printers_app)) == before
    keys = [e["key"] for e in audit_entries(pc, "print_send")]
    assert "send_requested" in keys and "send_answer" in keys


def test_close_button_declines(pc, ids, sliced, pid):
    ensure_sliced(pc)
    live(pc, ids["A1 Studio"])
    job = send(pc, printer=ids["A1 Studio"])["job"]
    confirm_ui.close(confirm_ui.wait(pid))
    assert confirm_ui.wait_gone(pid)
    assert wait_job(pc, job)["result"]["outcome"] == "declined"


def test_timeout(pc, ids, sliced, printers_app, pid):
    ensure_sliced(pc)
    live(pc, ids["A1 Studio"])
    before = len(jobs_dumped(printers_app))
    job = send(pc, printer=ids["A1 Studio"])["job"]
    confirm_ui.wait(pid)
    result = pc.tool("job_status", job=job, wait_s=CONFIRM_TIMEOUT_S + 10)
    assert result["status"] == "failed" and result["result"]["outcome"] == "timed_out", result
    assert not confirm_ui.find(pid)
    assert len(jobs_dumped(printers_app)) == before


def test_mcp_cannot_answer_the_confirmation(pc, ids, sliced, printers_app, pid):
    """Dialog-answering paths of the MCP server do not reach the confirmation window."""
    ensure_sliced(pc)
    live(pc, ids["A1 Studio"])
    before = len(jobs_dumped(printers_app))
    job = send(pc, printer=ids["A1 Studio"])["job"]
    hwnd = confirm_ui.wait(pid)
    # The test-only dialog closer, a dialog-raising debug call with its own answering policy,
    # ordinary calls: the window stays, the job keeps waiting.
    pc.tool("debug_window", action="close_dialogs")
    try:
        pc.call("debug_dialog", {})
    except Exception:
        pass
    pc.tool("app_status")
    pc.tool("scene_get")
    assert confirm_ui.find(pid) == hwnd
    status = pc.tool("job_status", job=job)
    assert status["status"] == "running" and "confirm" in status["text"]
    # Withdrawing does not close it either; the user's answer then changes nothing.
    pc.tool("job_cancel", job=job)
    assert confirm_ui.find(pid) == hwnd
    confirm_ui.click(hwnd, "Send")
    assert confirm_ui.wait_gone(pid)
    result = wait_job(pc, job)
    assert result["status"] == "cancelled" and result["result"]["outcome"] == "cancelled", result
    time.sleep(2)
    assert len(jobs_dumped(printers_app)) == before


def test_send_button_waits_a_moment(pc, ids, sliced, pid):
    ensure_sliced(pc)
    live(pc, ids["A1 Studio"])
    job = send(pc, printer=ids["A1 Studio"])["job"]
    hwnd = confirm_ui.wait(pid)
    # Send is disabled for the first second (no accidental Enter / double click), Cancel is not.
    assert confirm_ui.is_enabled(hwnd, "Send") is False
    assert confirm_ui.is_enabled(hwnd, "Cancel") is True
    deadline = time.monotonic() + 5
    while not confirm_ui.is_enabled(hwnd, "Send"):
        assert time.monotonic() < deadline, "Send never became enabled"
        time.sleep(0.1)
    confirm_ui.click(hwnd, "Cancel")
    assert wait_job(pc, job)["result"]["outcome"] == "declined"


def test_queue_and_rate_limit(pc, ids, sliced, printers_app, pid):
    """One window at a time; at most three requests wait behind it."""
    ensure_sliced(pc)
    live(pc, ids["A1 Studio"])
    jobs = [send(pc, printer=ids["A1 Studio"])["job"] for _ in range(4)]
    confirm_ui.wait(pid)
    assert pc.tool_error("print_send", printer=ids["A1 Studio"], wait_s=0).code == "rate_limited"
    second = pc.tool("job_status", job=jobs[1])
    assert second["status"] == "running" and "earlier" in second["text"]
    for _ in jobs:
        confirm_ui.answer(pid, "Cancel")
    for job in jobs:
        assert wait_job(pc, job)["result"]["outcome"] == "declined"
    assert pc.tool("app_status")["printers"]["sends_in_flight"] == 0


# ---------------------------------------------------------------------------------------------
# Sending
# ---------------------------------------------------------------------------------------------

def test_colour_mismatch_is_a_warning(pc, ids, sliced, pid):
    ensure_sliced(pc)
    live(pc, ids["A1 Studio"])
    job = send(pc, printer=ids["A1 Studio"], ams_mapping=[{"filament": 1, "tray": "A2"}, {"filament": 2, "tray": "A1"}])["job"]
    shown = "\n".join(confirm_ui.answer(pid, "Cancel"))
    assert "colour differs" in shown
    result = wait_job(pc, job)["result"]
    assert result["outcome"] == "declined" and len(result["warnings"]) == 2
    assert all(m["color_matches"] is False for m in result["ams_mapping"])


def test_send_bambu_auto_mapping(pc, ids, sliced, printers_app, pid):
    ensure_sliced(pc)
    live(pc, ids["A1 Desk"])
    before = len(jobs_dumped(printers_app))
    job = send(pc, printer=ids["A1 Desk"], options={"bed_leveling": "off", "timelapse": True})["job"]
    shown = "\n".join(confirm_ui.answer(pid, "Send"))
    assert "A1 Desk" in shown
    result = wait_job(pc, job, timeout=90)
    assert result["status"] == "finished" and result["result"]["outcome"] == "sent", result
    mapping = {m["filament"]: m["tray"] for m in result["result"]["ams_mapping"]}
    assert mapping == {1: "A1", 2: "A2"}
    no_secrets(result)

    sent = jobs_dumped(printers_app)[before:]
    assert len(sent) == 1, sent
    params = sent[0]
    assert params["dev_id"] == "FAKEA1DESK000002"
    assert json.loads(params["ams_mapping"]) == [0, 1]
    assert params["task_bed_leveling"] is False and params["auto_bed_leveling"] == 0
    assert params["task_record_timelapse"] is True
    assert params["task_use_ams"] is True
    assert params["plate_index"] == 1
    info = json.loads(params["ams_mapping_info"])
    assert [i["ams"] for i in info] == [0, 1]
    with zipfile.ZipFile(printers_app.dump_dir / params["dump_3mf"]) as z:
        names = z.namelist()
        assert "Metadata/plate_1.gcode" in names, names
    # The printer now prints it.
    status = live(pc, ids["A1 Desk"])
    assert status["state"] in ("printing", "preparing")
    # Audit: request, question, answer, result.
    keys = [e["key"] for e in audit_entries(pc) if e["scope"]["printer"] == ids["A1 Desk"]]
    assert {"send_requested", "send_asked", "send_answer", "send_result"} <= set(keys)


def test_send_host_octoprint(pc, ids, sliced, hosts, pid, printers_app):
    octo, _ = hosts
    ensure_sliced(pc)
    octo.uploads.clear()
    job = send(pc, printer=ids["A1 Octo"])["job"]
    confirm_ui.answer(pid, "Send")
    result = wait_job(pc, job)
    assert result["status"] == "finished" and result["result"]["outcome"] == "sent", result
    upload = octo.uploads[-1]
    assert upload.filename.endswith("_plate_1.gcode")
    assert str(upload.fields.get("print")).lower() == "true"
    assert upload.headers.get("X-Api-Key") == "fake-octo-key-777"
    path = Path(printers_app.data_dir) / "mcp" / "workspace" / "printers_octo.gcode"
    exported = pc.tool("export_file", kind="gcode", path=str(path), overwrite=True)
    assert upload.data == Path(exported["path"]).read_bytes()
    assert octo.state.state == "printing"
    octo.state.state = "idle"


def test_send_host_moonraker_upload_only(pc, ids, sliced, hosts, pid):
    _, moon = hosts
    ensure_sliced(pc)
    moon.uploads.clear()
    job = send(pc, printer=ids["A1 Moon"], options={"start_print": False, "file_name": "from_agent"})["job"]
    confirm_ui.answer(pid, "Send")
    result = wait_job(pc, job)
    assert result["result"]["outcome"] == "sent", result
    upload = moon.uploads[-1]
    assert upload.filename == "from_agent.gcode.3mf"
    assert str(upload.fields.get("print", "false")).lower() != "true"
    import io
    with zipfile.ZipFile(io.BytesIO(upload.data)) as z:
        gcode = z.read("Metadata/plate_1.gcode")
    assert b"G1" in gcode
    assert moon.state.state == "idle"


# ---------------------------------------------------------------------------------------------
# printer_control
# ---------------------------------------------------------------------------------------------

def test_control_bambu(pc, ids, printers_app, pid):
    garage = ids["A1 Garage"]
    live(pc, garage)
    assert pc.tool_error("printer_control", printer=garage, action="resume").code == "invalid_state"
    assert pc.tool("printer_control", printer=garage, action="pause")["ok"] is True
    deadline = time.monotonic() + 10
    while pc.tool("printer_status", printer=garage)["state"] != "paused":
        assert time.monotonic() < deadline
        time.sleep(0.5)
    assert pc.tool("printer_control", printer=garage, action="resume")["ok"] is True
    time.sleep(2)
    # stop needs the user: decline first, then confirm.
    job = pc.tool("printer_control", printer=garage, action="stop", wait_s=0)["job"]
    shown = "\n".join(confirm_ui.answer(pid, "Cancel"))
    assert "STOP" in shown
    assert wait_job(pc, job)["result"]["outcome"] == "declined"
    assert pc.tool("printer_status", printer=garage)["state"] == "printing"
    job = pc.tool("printer_control", printer=garage, action="stop", wait_s=0)["job"]
    confirm_ui.answer(pid, "Stop print")
    assert wait_job(pc, job)["result"]["outcome"] == "stopped"
    cmds = [(c["command"], c["accepted"]) for c in commands_dumped(printers_app) if c["dev_id"] == "FAKEA1GARAGE0003"]
    assert ("pause", True) in cmds and ("resume", True) in cmds and ("stop", True) in cmds
    assert [c for c, _ in cmds].count("stop") == 1


def test_control_host(pc, ids, hosts, pid):
    octo, _ = hosts
    octo.state.state = "printing"
    try:
        assert pc.tool("printer_control", printer=ids["A1 Octo"], action="pause")["ok"] is True
        assert octo.state.state == "paused"
        assert pc.tool("printer_control", printer=ids["A1 Octo"], action="resume")["ok"] is True
        assert octo.state.state == "printing"
        job = pc.tool("printer_control", printer=ids["A1 Octo"], action="stop", wait_s=0)["job"]
        confirm_ui.answer(pid, "Stop print")
        assert wait_job(pc, job)["result"]["outcome"] == "stopped"
        assert octo.state.state == "idle"
    finally:
        octo.state.state = "idle"


def test_send_host_with_unknown_state_asks_with_a_warning(pc, ids, sliced, pid):
    """A host type without status support is not refused and does not fail: the user decides, warned."""
    ensure_sliced(pc)
    job = send(pc, printer=ids["A1 Prusa"])["job"]
    shown = "\n".join(confirm_ui.answer(pid, "Cancel"))
    assert "cannot read the state" in shown
    result = wait_job(pc, job)["result"]
    assert result["outcome"] == "declined"
    assert any("cannot read the state" in w for w in result["warnings"])
    no_secrets(result)
