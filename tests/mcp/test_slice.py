"""Slice group: plate_validate, slice, slice_status, slice_cancel, slice_result, export_file (G-code kinds).

These tests run against an instance of their own (slicing changes plate state and the window's tab),
with the sample folders added to the file policy's roots. Scenes are set up with the real tools
(model_import, object_transform, plate_manage, settings_set).
"""

from __future__ import annotations

import re
import time
from pathlib import Path

import pytest

from orca_mcp import McpClient, REPO_ROOT

DATA = REPO_ROOT / "tests" / "data"
HANDY = REPO_ROOT / "resources" / "handy_models"
CUBE = DATA / "20mm_cube.obj"
BENCHY = HANDY / "3DBenchy.drc"


# ------------------------------------------------------------------------------------------------
# Fixtures: one instance for this module, both protocol eras.
# ------------------------------------------------------------------------------------------------

# Scene set-ups through the real tools (they used to be a debug fixture).
def load_model(client, path) -> dict:
    imported = client.tool("model_import", paths=[str(path)])["imported"]
    return {"objects": [o["id"] for o in imported]}


def plate_center(client):
    """Centre of the printable area in plate coordinates (from the printer's printable_area)."""
    area = client.tool("settings_get", scope="printer", keys=["printable_area"])["values"]["printable_area"]
    pts = [tuple(float(v) for v in (p if isinstance(p, list) else p.split("x"))) for p in (area if isinstance(area, list) else area.split(","))]
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return ((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2)


def move_to_plate(client, obj, plate, offset=(0, 0)):
    """Put the object on `plate` at the bed centre + `offset` (mm); one undo step."""
    cx, cy = plate_center(client)
    client.tool("object_transform", targets=[obj], move_to={"plate": plate, "center": [cx + offset[0], cy + offset[1]]})


def add_plate(client) -> int:
    return client.tool("plate_manage", action="add")["added"][0]


@pytest.fixture(scope="module")
def slice_app(launch_app, out_dir):
    roots = ";".join(str(p) for p in (DATA, HANDY, out_dir))
    return launch_app("slice", files=[str(CUBE)], extra_env={"ORCA_MCP_ROOTS": roots})


@pytest.fixture(scope="module")
def m(slice_app) -> McpClient:
    return McpClient(slice_app.url, slice_app.token, era="modern", timeout=320).connect()


@pytest.fixture(scope="module")
def lg(slice_app) -> McpClient:
    return McpClient(slice_app.url, slice_app.token, era="legacy", timeout=320).connect()


@pytest.fixture(params=["modern", "legacy"])
def c(request, m, lg) -> McpClient:
    return m if request.param == "modern" else lg


def objects(client):
    scene = client.tool("scene_get")
    found = []
    for plate in scene["plates"]:
        for obj in plate["objects"]:
            found.append((plate["plate"], obj))
    return found


def object_id(client, name_part: str) -> str:
    for _, obj in objects(client):
        if name_part.lower() in obj["name"].lower():
            return obj["id"]
    raise AssertionError(f"no object named like {name_part}")


def ensure_sliced(client, plate=1):
    result = client.tool("slice", plate=plate, wait_s=200)
    assert result["status"] == "finished", result
    return result


def parse_hms(text: str) -> float:
    """'1h 2m 3s' / '2m 3s' / '1d 2h 3m 4s' -> seconds."""
    total = 0.0
    for value, unit in re.findall(r"(\d+(?:\.\d+)?)\s*([dhms])", text):
        total += float(value) * {"d": 86400, "h": 3600, "m": 60, "s": 1}[unit]
    return total


# ------------------------------------------------------------------------------------------------
# Listing
# ------------------------------------------------------------------------------------------------

def test_tools_listed(c):
    names = {t["name"] for t in c.list_tools()}
    for name in ("plate_validate", "slice", "slice_status", "slice_cancel", "slice_result", "export_file"):
        assert name in names
    annotations = {t["name"]: t.get("annotations", {}) for t in c.list_tools()}
    assert annotations["slice_result"].get("readOnlyHint") is True
    assert annotations["slice_cancel"].get("destructiveHint") is True


# ------------------------------------------------------------------------------------------------
# Before slicing
# ------------------------------------------------------------------------------------------------

def test_validate_ok(c):
    v = c.tool("plate_validate")
    assert v["ok"] is True, v
    assert v["errors"] == []
    assert v["printable_instances"] == 1
    assert v["state"]["state"] in ("not_sliced", "sliced", "invalidated")


def test_result_before_slicing_is_not_sliced(m):
    # Make sure the plate has no result: change a setting, which invalidates any earlier slice.
    m.tool("settings_set", scope="process", values={"layer_height": 0.2})
    status = m.tool("slice_status")
    if status["plates"][0]["state"] == "sliced":
        pytest.skip("plate still sliced (setting unchanged)")
    err = m.tool_error("slice_result")
    assert err.code == "not_sliced"
    assert "slice" in err.hint
    assert err.details["state"] in ("not_sliced", "invalidated")


def test_slice_cancel_when_idle(c):
    err = c.tool_error("slice_cancel")
    assert err.code == "not_running"


def test_slice_status_shape(c):
    s = c.tool("slice_status")
    assert s["status"] in ("idle", "finished", "failed", "cancelled")
    assert len(s["plates"]) >= 1
    assert {"plate", "id", "state"} <= set(s["plates"][0])


# ------------------------------------------------------------------------------------------------
# Slicing
# ------------------------------------------------------------------------------------------------

def test_slice_with_progress(m):
    m.tool("settings_set", scope="process", values={"layer_height": 0.16})  # invalidates
    r = m.call("slice", {"wait_s": 200}, progress=True)
    assert not r.is_error, r.data
    assert r.data["status"] == "finished", r.data
    assert r.streamed and len(r.progress) >= 1, r.progress
    percents = [p["progress"] for p in r.progress]
    assert percents == sorted(percents), percents
    assert any(p.get("message") for p in r.progress)
    plate = r.data["result"]["plates"][0]
    assert plate["state"] == "sliced"
    assert plate["outcome"] == "finished"
    assert plate["time_s"] > 0 and plate["filament_g"] > 0
    # 20 mm at 0.16 mm layers, first layer 0.2 -> 125 layers
    assert 120 <= plate["layers"] <= 130, plate


def test_progress_streams_on_long_slice(m):
    loaded = load_model(m, str(BENCHY))
    # Fine layers: a slice of several seconds, so that several progress notifications (at most one
    # per second) are streamed.
    m.tool("settings_set", scope="process", values={"layer_height": 0.08})
    try:
        r = m.call("slice", {"wait_s": 250}, progress=True)
        assert not r.is_error and r.data["status"] == "finished", r.data
        percents = [p["progress"] for p in r.progress]
        assert len(percents) >= 2, r.progress
        assert percents == sorted(percents) and percents[-1] > percents[0], percents
        texts = {p.get("message", "") for p in r.progress}
        assert len(texts) >= 2 and all(t.startswith("Slicing plate 1") for t in texts if t), texts
    finally:
        m.tool("settings_set", scope="process", values={"layer_height": 0.2})
        m.tool("delete", targets=[loaded["objects"][0]])


def test_slice_again_is_reused(c):
    ensure_sliced(c)
    r = c.tool("slice", wait_s=10)
    assert r["status"] == "finished"
    assert r.get("reused") is True
    assert any("valid result" in n for n in r.get("notes", []))


def test_slice_result_sections(c):
    ensure_sliced(c)
    r = c.tool("slice_result", sections=["summary", "filaments", "features", "objects", "warnings"])
    s = r["summary"]
    assert s["time_s"] > 0 and s["filament_g"] > 0 and s["layers"] > 0
    assert abs(s["height_mm"] - 20.0) < 0.05
    assert r["filaments"][0]["filament"] == 1
    assert abs(sum(f["weight_g"] for f in r["filaments"]) - s["filament_g"]) < 0.05
    # Layer heights are z steps (bridge extrusions are thicker, but do not make a layer thicker).
    assert s["min_layer_height"] <= s["layer_height"] <= s["max_layer_height"] < 2 * s["layer_height"], s
    keys = {f["key"] for f in r["features"]}
    assert {"outer_wall", "inner_wall"} <= keys, keys
    assert abs(sum(f["percent"] for f in r["features"]) - 100) < 25
    # Grams, like the totals (the per-object masses come in mg): a part of the plate's filament.
    assert r["objects"] and 0 < r["objects"][0]["weight_g"] <= s["filament_g"] + 0.05, r["objects"]
    box = s["toolpath_box"]
    assert box["max"][2] <= 20.05
    assert isinstance(r["warnings"], list)


def test_slice_result_invalid_section(c):
    result = c.call("slice_result", {"sections": ["bogus"]})
    assert result.is_error and result.error_code == "invalid_argument"


def test_stale_result_after_change(m):
    ensure_sliced(m)
    m.tool("settings_set", scope="process", values={"layer_height": 0.24})
    s = m.tool("slice_status")
    assert s["plates"][0]["state"] == "invalidated", s["plates"][0]
    err = m.tool_error("slice_result")
    assert err.code == "not_sliced" and err.details["state"] == "invalidated"
    assert "again" in err.hint
    r = ensure_sliced(m)
    assert r.get("reused") is not True
    assert m.tool("slice_result")["summary"]["layers"] < 100


def test_wait_s_zero_returns_running_then_status_long_polls(m):
    m.tool("settings_set", scope="process", values={"layer_height": 0.12})
    r = m.tool("slice", wait_s=0)
    assert r["status"] in ("running", "finished"), r
    if r["status"] == "running":
        assert r["job"].startswith("job")
        assert "slice_status" in r.get("hint", "")
        busy = m.tool_error("slice_result")
        assert busy.code in ("busy_slicing", "not_sliced")
        again = m.tool_error("slice")
        assert again.code == "busy_slicing"
    t0 = time.monotonic()
    st = m.call("slice_status", {"wait_s": 200}, progress=True)
    assert not st.is_error
    assert st.data["status"] == "finished", st.data
    assert time.monotonic() - t0 < 200
    assert st.data["plates"][0]["state"] == "sliced"


def test_job_status_sees_slice_job(m):
    jobs = m.tool("job_status", kind="slice")
    assert jobs["kind"] == "slice"
    assert jobs["status"] in ("finished", "running", "cancelled", "failed")


# ------------------------------------------------------------------------------------------------
# Negative paths that need scene set-ups
# ------------------------------------------------------------------------------------------------

def test_object_partly_off_the_plate(m):
    cube = object_id(m, "20mm_cube")
    # Straddle the right edge of the plate: half in, half out.
    move_to_plate(m, cube, 1, [125, 0])
    try:
        v = m.tool("plate_validate")
        assert v["ok"] is False
        outside = [e for e in v["errors"] if e["kind"] == "outside_plate"]
        assert outside, v
        assert outside[0]["object"]["id"] == cube
        assert outside[0]["instance"].startswith("i") and outside[0]["box"]
        assert "fix" in outside[0]
        err = m.tool_error("slice")
        assert err.code == "validation_failed", err.payload
        assert err.details["errors"][0]["kind"] == "outside_plate"
        assert err.details["errors"][0]["fix"]
    finally:
        m.tool("history", action="undo")
    assert m.tool("plate_validate")["ok"] is True


def test_invalid_settings_combination(m):
    # A layer height above the nozzle diameter is refused by the slicer's validation. Set on the object:
    # on the process preset the GUI's own check clamps it to the printer's limit first (answered yes).
    oid = objects(m)[0][1]["id"]
    m.tool("settings_set", scope={"object": oid}, values={"layer_height": 0.6})
    try:
        v = m.tool("plate_validate")
        assert v["ok"] is False, v
        e = v["errors"][0]
        assert e["message"]
        assert e.get("key") or e["kind"] in ("layer_height_limit", "print", "setting"), e
        err = m.tool_error("slice")
        assert err.code == "validation_failed"
        assert err.details["errors"]
        assert m.tool("slice_status")["plates"][0]["state"] in ("invalid", "not_sliced", "invalidated", "failed")
    finally:
        # Undo the override (a reset would be one more undo step that a later undo could revert).
        m.tool("history", action="undo")
    assert m.tool("plate_validate")["ok"] is True


def test_two_plates_and_nothing_to_slice(m):
    plate2 = add_plate(m)
    try:
        err = m.tool_error("slice", plate=plate2)
        assert err.code == "nothing_to_slice"
        assert err.details["state"] == "empty"
        # Second model on the second plate, then slice all.
        loaded = load_model(m, str(DATA / "pyramid.obj"))
        pyramid = loaded["objects"][0]
        move_to_plate(m, pyramid, plate2)
        on = {p: o["id"] for p, o in objects(m)}
        assert on.get(plate2) == pyramid, on
        r = m.tool("slice", plate="all", wait_s=250)
        assert r["status"] == "finished", r
        plates = {p["plate"]: p for p in r["result"]["plates"]}
        assert set(plates) == {1, plate2}
        assert plates[1]["state"] == "sliced" and plates[plate2]["state"] == "sliced"
        assert plates[1]["filament_g"] != plates[plate2]["filament_g"]
        r1 = m.tool("slice_result", plate=1)
        r2 = m.tool("slice_result", plate=plate2)
        assert r1["summary"]["height_mm"] != r2["summary"]["height_mm"]
        # Toolpath boxes are in each plate's own frame: both near the bed centre.
        for r in (r1, r2):
            box = r["summary"]["toolpath_box"]
            assert 0 < box["min"][0] < 300 and 0 < box["min"][1] < 300, box
        # Slicing all again re-uses both.
        assert m.tool("slice", plate="all", wait_s=30).get("reused") is True
    finally:
        # Remove the second plate (and the pyramid with it).
        for _ in range(4):
            m.tool("history", action="undo")
        scene = m.tool("scene_get", detail="summary")
        if len(scene["plates"]) > 1:
            m.tool("plate_manage", action="delete", plate=len(scene["plates"]))


def test_cancel_mid_slice(m):
    loaded = load_model(m, str(BENCHY))
    m.tool("settings_set", scope="process", values={"layer_height": 0.08})  # a slice of several seconds
    try:
        r = m.tool("slice", wait_s=0)
        assert r["status"] == "running", r
        time.sleep(0.5)
        cancelled = m.tool("slice_cancel")
        assert cancelled["status"] == "cancelled"
        st = m.tool("slice_status", wait_s=30)
        assert st["status"] == "cancelled", st
        assert st["plates"][0]["state"] in ("cancelled", "not_sliced", "invalidated"), st["plates"][0]
        assert m.tool_error("slice_result").code == "not_sliced"
    finally:
        m.tool("settings_set", scope="process", values={"layer_height": 0.2})
        m.tool("delete", targets=[loaded["objects"][0]])


def test_scene_change_mid_slice(m):
    loaded = load_model(m, str(BENCHY))
    m.tool("settings_set", scope="process", values={"layer_height": 0.08})  # a slice of several seconds
    try:
        r = m.tool("slice", wait_s=0)
        assert r["status"] == "running", r
        time.sleep(0.5)
        # The user (or another call) changes a setting while slicing.
        m.tool("settings_set", scope="process", values={"wall_loops": 3})
        st = m.tool("slice_status", wait_s=60)
        assert st["status"] in ("cancelled", "finished"), st
        if st["status"] == "cancelled":
            assert st["result"]["plates"][0].get("cancel_reason") == "scene_changed_or_user", st
        r2 = m.tool("slice", wait_s=250)
        assert r2["status"] == "finished"
    finally:
        m.tool("settings_set", scope="process", values={"layer_height": 0.2})
        m.tool("settings_set", scope="process", values={"wall_loops": 2})
        m.tool("delete", targets=[loaded["objects"][0]])


# ------------------------------------------------------------------------------------------------
# Export
# ------------------------------------------------------------------------------------------------

def test_export_gcode_header_matches_result(m, out_dir):
    ensure_sliced(m)
    result = m.tool("slice_result", sections=["summary", "filaments"])
    path = out_dir / "slice_cube.gcode"
    if path.exists():
        path.unlink()
    e = m.tool("export_file", kind="gcode", path=str(path))
    assert e["ok"] and Path(e["path"]).exists()
    assert e["bytes"] == path.stat().st_size > 1000
    header = e["header"]
    text = path.read_text(encoding="utf-8", errors="replace")
    # Ground truth: the slicer's own comments in the file.
    m_time = re.search(r"; estimated printing time \(normal mode\) = (.+)", text) or re.search(r"total estimated time: ([^;\n]+)", text)
    assert m_time, "no time comment in the G-code"
    assert abs(parse_hms(m_time.group(1)) - result["summary"]["time_s"]) <= 1.5
    m_g = re.search(r"; total filament used \[g\] = ([\d.]+)", text) or re.search(r"; filament used \[g\] = ([\d.]+)", text)
    assert m_g, "no filament weight comment"
    assert abs(float(m_g.group(1)) - result["summary"]["filament_g"]) <= 0.02 + 0.01 * float(m_g.group(1))
    m_layers = re.search(r"; total layer(?:s count| number)[:=]\s*(\d+)", text)
    if m_layers:
        assert int(m_layers.group(1)) == result["summary"]["layers"]
    assert header, "header comments not parsed"
    # Refuses to overwrite unless asked.
    assert m.tool_error("export_file", kind="gcode", path=str(path)).code == "exists"
    assert m.tool("export_file", kind="gcode", path=str(path), overwrite=True)["ok"]


def test_export_outside_roots_denied(c, tmp_path_factory):
    ensure_sliced(c)
    outside = Path("C:/Windows/Temp/mcp_slice_denied.gcode") if Path("C:/Windows").exists() else Path("/proc/denied.gcode")
    err = c.tool_error("export_file", kind="gcode", path=str(outside))
    assert err.code == "path_denied"


def test_export_wrong_extension(c, out_dir):
    ensure_sliced(c)
    err = c.tool_error("export_file", kind="gcode", path=str(out_dir / "x.txt"))
    assert err.code in ("path_denied", "invalid_argument")


def test_export_all_needs_3mf(c, out_dir):
    err = c.tool_error("export_file", kind="gcode", path=str(out_dir / "all.gcode"), plate="all")
    assert err.code == "invalid_argument"


def test_export_gcode_3mf(m, out_dir):
    ensure_sliced(m)
    path = out_dir / "slice_cube.gcode.3mf"
    e = m.tool("export_file", kind="gcode_3mf", path=str(path), overwrite=True)
    assert e["ok"] and path.exists() and e["bytes"] > 1000
    import zipfile
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
    assert any(n.endswith(".gcode") for n in names), names
    # The plain .3mf suffix is refused: sliced projects are recognised by .gcode.3mf.
    assert m.tool_error("export_file", kind="gcode_3mf", path=str(out_dir / "x.3mf")).code == "path_denied"


def test_export_not_sliced(m, out_dir):
    m.tool("settings_set", scope="process", values={"layer_height": 0.28})
    try:
        err = m.tool_error("export_file", kind="gcode", path=str(out_dir / "never.gcode"))
        assert err.code == "not_sliced"
    finally:
        m.tool("settings_set", scope="process", values={"layer_height": 0.2})


# ------------------------------------------------------------------------------------------------
# End to end: load -> slice with progress -> results and checks -> pictures -> change -> re-slice
# -> diff -> export -> the file says what the tools said.
# ------------------------------------------------------------------------------------------------

# ------------------------------------------------------------------------------------------------
# Prime tower: moving it invalidates the result at once; the stored (clamped) position is explained.
# ------------------------------------------------------------------------------------------------

@pytest.fixture(scope="module")
def tower(launch_app) -> McpClient:
    """Its own instance: two cubes on two filaments, so plate 1 gets a prime tower."""
    app = launch_app("slice_tower", files=[str(CUBE)])
    client = McpClient(app.url, app.token, era="modern", timeout=320).connect()
    cube = client.tool("scene_get")["plates"][0]["objects"][0]["id"]
    client.tool("filament_edit", action="add", color="#FF0000")
    copy = client.tool("object_duplicate", targets=[cube], **{"as": "objects"})["created"][0]["id"]
    client.tool("object_update", targets=[copy], filament=2)
    client.tool("settings_set", scope="process", values={"enable_prime_tower": True})
    return client


def first_number(value) -> float:
    return float(value[0] if isinstance(value, list) else str(value).split(",")[0])


def tower_xy(client):
    v = client.tool("settings_get", scope="project", keys=["wipe_tower_x", "wipe_tower_y"])["values"]
    return first_number(v["wipe_tower_x"]), first_number(v["wipe_tower_y"])


def close2(a, b, tol=0.01):
    return len(a) == len(b) and all(abs(p - q) <= tol for p, q in zip(a, b))


def test_prime_tower_footprint_in_scene(tower):
    pt = tower.tool("scene_get")["plates"][0]["prime_tower"]
    assert pt is not None and pt["source"] in ("estimate", "sliced"), pt
    ensure_sliced(tower)
    pt = tower.tool("scene_get")["plates"][0]["prime_tower"]
    assert pt["source"] == "sliced", pt
    x, y = tower_xy(tower)
    assert close2(pt["position"], [x, y]), (pt, x, y)
    assert pt["size"][0] > 0 and pt["size"][1] > 0 and pt["height"] > 0
    box = pt["box"]
    assert close2([box["max"][0] - box["min"][0], box["max"][1] - box["min"][1]], pt["size"])
    # The footprint (brim included) contains the tower's front-left corner.
    assert box["min"][0] <= x + 0.01 and box["min"][1] <= y + 0.01 and box["max"][0] > x and box["max"][1] > y, (box, x, y)


def test_prime_tower_move_then_slice_at_once_is_not_reused(tower):
    ensure_sliced(tower)
    x, y = tower_xy(tower)
    # Writing the value it already has changes nothing: no invalidation, the result stays. (The value
    # read back is rounded to 4 decimals, so the first write may store it as given; the second is a no-op.)
    tower.tool("settings_set", scope="project", values={"wipe_tower_x": [x]})
    ensure_sliced(tower)
    same = tower.tool("settings_set", scope="project", values={"wipe_tower_x": [x]})
    assert "slice_invalidated" not in same, same
    assert tower.tool("slice", wait_s=30).get("reused") is True
    # Move the tower and slice right away (no pause for the application's 0.5 s apply timer).
    nx = 40.0 if x > 100 else 160.0
    moved = tower.tool("settings_set", scope="project", values={"wipe_tower_x": [nx], "wipe_tower_y": [150.0]})
    assert moved.get("slice_invalidated") is True, moved
    r = tower.tool("slice", wait_s=200)
    assert r["status"] == "finished" and r.get("reused") is not True, r
    pt = tower.tool("scene_get")["plates"][0]["prime_tower"]
    assert pt["source"] == "sliced" and close2(pt["position"], list(tower_xy(tower))), pt
    # A status read right after a move reports it too.
    tower.tool("settings_set", scope="project", values={"wipe_tower_x": [x], "wipe_tower_y": [y]})
    assert tower.tool("slice_status")["plates"][0]["state"] == "invalidated"
    assert tower.tool_error("slice_result").code == "not_sliced"


def test_prime_tower_clamped_position_is_explained(tower):
    r = tower.tool("settings_set", scope="project", values={"wipe_tower_y": [1.5]})
    entry = next(e for e in r["changed"] if e["key"] == "wipe_tower_y")
    assert entry["requested"] != entry["new"], entry
    assert abs(first_number(entry["new"]) - 1.5) > 0.5, entry
    assert any("wipe_tower_y was stored as" in n and "front-left corner" in n for n in r.get("notes", [])), r
    # A value kept as given carries no `requested`.
    ok = tower.tool("settings_set", scope="project", values={"wipe_tower_y": [150.0]})
    assert all("requested" not in e for e in ok["changed"]), ok


def test_end_to_end_scenario(launch_app, out_dir):
    roots = ";".join(str(p) for p in (DATA, HANDY, out_dir))
    app = launch_app("slice_e2e", files=[str(BENCHY)], extra_env={"ORCA_MCP_ROOTS": roots})
    client = McpClient(app.url, app.token, era="modern", timeout=320).connect()
    shots = out_dir / "e2e"
    shots.mkdir(exist_ok=True)
    try:
        assert client.tool("plate_validate")["ok"] is True
        r = client.call("slice", {"wait_s": 250}, progress=True)
        assert not r.is_error and r.data["status"] == "finished", r.data
        assert r.progress, "no progress streamed"
        first = client.tool("slice_result", sections=["summary", "features", "filaments", "objects", "warnings"])
        names = {t["name"] for t in client.list_tools()}
        if "analyze" in names:
            checks = client.tool("analyze", checks=["flow", "cooling", "overhangs", "supports", "first_layer", "seams"])
            assert set(checks["verdicts"]) >= {"flow", "cooling", "overhangs"}
            assert checks["overhangs"]["bridge_length_mm"] > 0  # Benchy's cabin roof and windows
        tool = next(t for t in client.list_tools() if t["name"] == "screenshot")
        if "preview" in tool["inputSchema"]["properties"]["view"]["enum"]:
            for name, args in {
                "iso_feature": {"camera": {"preset": "iso", "target": "objects"}},
                "front_speed": {"camera": {"preset": "front", "target": "objects"}, "preview": {"color_by": "speed"}},
                "top_section": {"camera": {"preset": "top", "target": "objects", "projection": "orthographic"},
                                "preview": {"z_range": {"min": 9.9, "max": 10.1}, "section": True}},
            }.items():
                shot = client.call("screenshot", {"view": "preview", **args})
                assert not shot.is_error, shot.data
                (shots / f"{name}.png").write_bytes(shot.images[0][1])
        # Change something, re-slice, ask what changed.
        client.tool("settings_set", scope="process", values={"sparse_infill_density": "35%"})
        assert client.tool("slice_status")["plates"][0]["state"] == "invalidated"
        assert client.tool("slice", wait_s=250)["status"] == "finished"
        second = client.tool("slice_result", sections=["summary", "filaments"])
        assert second["summary"]["filament_g"] > first["summary"]["filament_g"]
        if "analyze" in names:
            diff = client.tool("analyze", checks=["diff"])["diff"]
            assert diff["available"] and diff["after"]["weight_g"] > diff["before"]["weight_g"], diff
        # Export and compare the file with the tools.
        path = out_dir / "e2e_benchy.gcode"
        e = client.tool("export_file", kind="gcode", path=str(path), overwrite=True)
        text = path.read_text(encoding="utf-8", errors="replace")
        m_time = re.search(r"; estimated printing time \(normal mode\) = (.+)", text)
        m_g = re.search(r"; total filament used \[g\] = ([\d.]+)", text) or re.search(r"; filament used \[g\] = ([\d.]+)", text)
        assert m_time and m_g
        assert abs(parse_hms(m_time.group(1)) - second["summary"]["time_s"]) <= 1.5
        assert abs(float(m_g.group(1)) - second["summary"]["filament_g"]) <= 0.05
        (out_dir / "e2e_numbers.json").write_text(__import__("json").dumps({
            "tool_time_s": second["summary"]["time_s"], "file_time": m_time.group(1).strip(),
            "tool_filament_g": second["summary"]["filament_g"], "file_filament_g": float(m_g.group(1)),
            "layers": second["summary"]["layers"], "bytes": e["bytes"],
            "first_filament_g": first["summary"]["filament_g"]}, indent=1), encoding="utf-8")
        e3 = client.tool("export_file", kind="gcode_3mf", path=str(out_dir / "e2e_benchy.gcode.3mf"), overwrite=True)
        assert e3["bytes"] > e["bytes"] / 10
    finally:
        app.stop()
