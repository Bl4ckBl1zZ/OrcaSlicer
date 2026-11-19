"""Slice analysis tools: slice_layers and analyze, cross-checked against slice_result.

Own instance: the 20 mm cube plus, for the overhang checks, the in-tree bridge model sliced with and
without supports.
"""

from __future__ import annotations

import pytest

from orca_mcp import McpClient, REPO_ROOT

DATA = REPO_ROOT / "tests" / "data"
HANDY = REPO_ROOT / "resources" / "handy_models"
CUBE = DATA / "20mm_cube.obj"

ALL_CHECKS = ["flow", "speeds", "cooling", "supports", "overhangs", "unsupported", "first_layer", "seams", "thin_features",
              "multi_material", "summary"]


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
def an_app(launch_app, out_dir):
    roots = ";".join(str(p) for p in (DATA, HANDY, out_dir))
    return launch_app("analysis", files=[str(CUBE)], extra_env={"ORCA_MCP_ROOTS": roots})


@pytest.fixture(scope="module")
def m(an_app) -> McpClient:
    return McpClient(an_app.url, an_app.token, era="modern", timeout=320).connect()


@pytest.fixture(scope="module")
def lg(an_app) -> McpClient:
    return McpClient(an_app.url, an_app.token, era="legacy", timeout=320).connect()


@pytest.fixture(params=["modern", "legacy"])
def c(request, m, lg) -> McpClient:
    return m if request.param == "modern" else lg


@pytest.fixture(scope="module")
def sliced(m):
    r = m.tool("slice", wait_s=200)
    assert r["status"] == "finished", r
    return r


def available(client) -> bool:
    names = {t["name"] for t in client.list_tools()}
    return "analyze" in names


def require_core(client):
    result = client.call("analyze", {"checks": ["summary"]})
    if result.is_error and result.error_code == "not_available":
        pytest.skip("analysis core not built in")


def test_tools_listed(c):
    names = {t["name"] for t in c.list_tools()}
    assert {"slice_layers", "analyze"} <= names


def test_analyze_before_slicing(launch_app, out_dir):
    app = launch_app("analysis_empty", files=[str(CUBE)])
    client = McpClient(app.url, app.token, era="modern").connect()
    err = client.tool_error("analyze", checks=["flow"])
    assert err.code in ("not_sliced", "not_available")
    err = client.tool_error("slice_layers")
    assert err.code in ("not_sliced", "not_available")
    app.stop()


def test_layers_table_paged(c, sliced):
    require_core(c)
    result = c.tool("slice_result")
    first = c.tool("slice_layers", limit=10)
    assert first["layer_count"] == result["summary"]["layers"]
    assert len(first["rows"]) == 10 and first["truncated"] is True and first["next"] == 11
    assert first["rows"][0]["index"] == 1
    assert abs(first["rows"][0]["z"] - result["summary"]["first_layer_height"]) < 0.01
    second = c.tool("slice_layers", **{"from": first["next"], "limit": 10})
    assert second["rows"][0]["index"] == 11
    sampled = c.tool("slice_layers", every=10, limit=200)
    assert [r["index"] for r in sampled["rows"]][:3] == [1, 11, 21]
    total_time = sum(r["time_s"] for r in c.tool("slice_layers", limit=200)["rows"])
    # Layer times exclude start/end G-code; they stay below the total and close to it for a small cube.
    assert 0.5 * result["summary"]["time_s"] < total_time <= result["summary"]["time_s"] + 1


def test_layers_columns(c, sliced):
    require_core(c)
    r = c.tool("slice_layers", layer=5, columns=["z", "roles"])
    row = r["rows"][0]
    assert set(row) == {"index", "z", "roles"}
    assert any(role["role"] in ("outer_wall", "inner_wall") for role in row["roles"])


def test_layer_section(c, sliced):
    require_core(c)
    sec = c.tool("slice_layers", detail="section", z=10.0)
    assert abs(sec["z"] - 10.0) < 0.3
    geo = sec["geometry"]
    # A 20 mm cube: one island of about 400 mm2.
    assert geo["islands"] == 1
    assert 350 < geo["island_area_mm2"] < 410, geo
    assert geo["perimeter_count"] >= 1
    boxed = c.tool("slice_layers", detail="section", layer=sec["layer"], box={"min": [0, 0], "max": [1, 1]})
    assert boxed["geometry"]["island_area_mm2"] == 0


def test_layer_out_of_range(c, sliced):
    require_core(c)
    err = c.tool_error("slice_layers", detail="section", layer=100000)
    assert err.code == "invalid_argument"


def test_all_checks(c, sliced):
    require_core(c)
    r = c.tool("analyze", checks=ALL_CHECKS, max_items=3)
    for check in ALL_CHECKS:
        assert check in r, check
    assert set(r["verdicts"]) >= {"flow", "cooling", "first_layer"}
    flow = r["flow"]["filaments"][0]
    assert flow["filament"] == 1  # slots are 1-based, as everywhere
    assert flow["max_flow"] > 0 and flow["limit"] > 0
    assert flow["max_at"]["layer"] >= 1
    fl = r["first_layer"]["objects"][0]
    assert 350 < fl["contact_area_mm2"] < 410, fl
    assert fl.get("object_id", "").startswith("o")
    # The cube needs no support and has no bridges.
    assert r["supports"]["volume_mm3"] == 0
    assert r["unsupported"]["floating_islands"] == 0
    # The analysis core's totals agree with the slicer's own.
    summary = c.tool("slice_result")["summary"]
    assert abs(r["summary"]["time_s"] - summary["time_s"]) <= 1
    assert abs(r["summary"]["total_weight_g"] - summary["filament_g"]) <= 0.05
    assert r["summary"]["layer_count"] == summary["layers"]
    # Per-object masses are grams too (no more than the plate total).
    objects = [o for o in r["summary"].get("objects", []) if o.get("mass_g")]
    assert objects and all(o["mass_g"] <= summary["filament_g"] + 0.05 for o in objects), objects


def test_lists_are_capped(c, sliced):
    require_core(c)
    r = c.tool("analyze", checks=["seams"], max_items=1)
    obj = r["seams"]["objects"][0]
    if obj["count"] > 1:
        assert len(obj["samples"]) == 1
    # Lists the core does not limit itself are cut by the tool, with the full length next to them.
    rows = c.tool("analyze", checks=["supports"], max_items=1)["supports"]
    for key, value in rows.items():
        if isinstance(value, list):
            assert len(value) <= 1, key


def test_region_query(c, sliced):
    require_core(c)
    box = c.tool("slice_result")["summary"]["toolpath_box"]
    whole = c.tool("analyze", checks=["region"], params={"region": {"box": {"min": box["min"], "max": box["max"]}}})
    assert whole["region"]["length_mm"] > 0
    lower = c.tool("analyze", checks=["region"], params={"region": {"box": {"min": box["min"][:2] + [0], "max": box["max"][:2] + [5]}}})
    assert 0 < lower["region"]["length_mm"] < whole["region"]["length_mm"]
    err = c.tool_error("analyze", checks=["region"])
    assert err.code == "invalid_argument"


def test_unknown_check_rejected_by_schema(c):
    result = c.call("analyze", {"checks": ["strength"]})
    assert result.is_error and result.error_code == "invalid_argument"


def test_diff_after_change(m, sliced):
    require_core(m)
    m.tool("analyze", checks=["summary"])  # make sure the current result is known
    m.tool("settings_set", scope="process", values={"sparse_infill_density": "40%"})
    try:
        r = m.tool("slice", wait_s=200)
        assert r["status"] == "finished"
        d = m.tool("analyze", checks=["diff"], max_items=10)["diff"]
        assert d["available"] is True, d
        keys = [e["key"] for e in d["changes"]]
        assert any("sparse_infill" in k for k in keys), keys
        assert d["after"]["weight_g"] > d["before"]["weight_g"]
    finally:
        m.tool("settings_set", scope="process", values={"sparse_infill_density": "15%"})


def test_bridge_with_and_without_supports(m):
    require_core(m)
    loaded = load_model(m, str(DATA / "bridge.obj"))
    obj = loaded["objects"][0]
    scene = m.tool("scene_get")
    cube = [o["id"] for p in scene["plates"] for o in p["objects"] if "cube" in o["name"].lower()]
    try:
        move_to_plate(m, cube[0], 1, [60, 60])
        m.tool("settings_set", scope="process", values={"enable_support": 0})
        assert m.tool("slice", wait_s=200)["status"] == "finished"
        without = m.tool("analyze", checks=["overhangs", "supports", "unsupported"])
        assert without["supports"]["volume_mm3"] == 0
        assert without["overhangs"]["bridge_length_mm"] > 0, without["overhangs"]
        assert without["overhangs"]["longest_bridge_mm"] > 5
        m.tool("settings_set", scope="process", values={"enable_support": 1, "support_type": "normal(auto)"})
        assert m.tool("slice", wait_s=200)["status"] == "finished"
        with_support = m.tool("analyze", checks=["supports"])
        sup = with_support["supports"]
        assert sup["volume_mm3"] > 0, sup
        assert sup["layers_with_support"] > 0 and sup["first_layer"] >= 1
        assert sup["box"] is not None
    finally:
        m.tool("settings_set", scope="process", values={"enable_support": 0})
        m.tool("delete", targets=[obj])
        move_to_plate(m, cube[0], 1, [0, 0])


def test_two_filaments(m, out_dir):
    require_core(m)
    added = m.tool("filament_edit", action="add")
    assert len(added["filaments"]) >= 2
    loaded = load_model(m, str(DATA / "pyramid.obj"))
    pyramid = loaded["objects"][0]
    scene = m.tool("scene_get")
    cube = [o["id"] for p in scene["plates"] for o in p["objects"] if "cube" in o["name"].lower()][0]
    try:
        move_to_plate(m, pyramid, 1, [40, 0])
        m.tool("object_update", targets=[pyramid], filament=2)
        r = m.tool("slice", wait_s=250)
        assert r["status"] == "finished", r
        res = m.tool("slice_result", sections=["summary", "filaments"])
        slots = {f["filament"]: f for f in res["filaments"]}
        assert {1, 2} <= set(slots), res["filaments"]
        assert slots[2]["model_mm3"] > 0 and slots[1]["model_mm3"] > 0
        assert res["summary"]["filament_changes"] > 0
        mm = m.tool("analyze", checks=["multi_material"], max_items=5)["multi_material"]
        assert mm["tool_changes"] > 0
        assert mm["filaments"] and len(mm["filaments"]) >= 2
        assert mm["transitions"][0]["layer"] >= 1
        names = {t["name"] for t in m.list_tools()}
        if "screenshot" in names:
            shot = m.call("screenshot", {"view": "preview", "preview": {"color_by": "filament"},
                                         "camera": {"preset": "iso", "target": "objects"}})
            if not shot.is_error:
                (out_dir / "preview").mkdir(exist_ok=True)
                (out_dir / "preview" / "two_filaments.png").write_bytes(shot.images[0][1])
                (out_dir / "preview" / "two_filaments.json").write_text(__import__("json").dumps(shot.data, indent=1), encoding="utf-8")
                keys = {i["key"] for i in shot.data["legend"]["items"]}
                assert len(keys) >= 2, shot.data["legend"]
                colours = {i["color"] for i in shot.data["legend"]["items"]}
                assert len(colours) >= 2, shot.data["legend"]
    finally:
        m.tool("delete", targets=[pyramid])
