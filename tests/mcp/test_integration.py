"""Integration of the merged tool groups (edit, settings, paint, slice / analysis / preview), in an
instance of its own:

- the catalogue is consistent (no overlapping tools, one name per concept, the instructions name only
  tools that exist);
- an end-to-end job across every group: import -> modifier -> per-object settings -> paint -> slice ->
  analyse -> preview picture -> G-code export, each step checked with numbers;
- the gaps the group branches could not test alone: bodies and paint on a rotated, scaled instance,
  effective values through every scope (and paint_apply's warnings that read them), the overlay
  options of the screenshot schema.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from orca_mcp import McpClient, REPO_ROOT
from test_paint_effects import t_shape

DATA = REPO_ROOT / "tests" / "data"
CUBE = DATA / "20mm_cube.obj"

# The catalogue after integration (debug tools aside).
TOOLS = {
    "app_status", "history", "job_status", "job_cancel", "scene_get", "mesh_query", "mesh_edit", "screenshot",
    "project_new", "project_open", "project_save", "model_import", "export_file",
    "object_add_primitive", "object_update", "object_transform", "object_duplicate", "object_arrange", "object_orient",
    "object_cut", "object_split", "object_merge", "delete", "plate_manage",
    "settings_search", "settings_get", "settings_set", "preset_list", "preset_select", "preset_save", "preset_delete",
    "preset_rename", "preset_diff", "filament_list", "filament_edit", "modifier_add", "modifier_update", "layer_range",
    "paint_apply", "paint_get", "plate_layer_events",
    "plate_validate", "slice", "slice_status", "slice_cancel", "slice_result", "slice_layers", "analyze",
    "printer_list", "printer_status", "print_send", "printer_control",
}
REMOVED = {"slice_export", "modifier_delete"}


@pytest.fixture(scope="module")
def int_app(launch_app, out_dir):
    roots = ";".join(str(p) for p in (DATA, out_dir))
    return launch_app("integration", files=[str(CUBE)], extra_env={"ORCA_MCP_ROOTS": roots})


@pytest.fixture(scope="module")
def m(int_app) -> McpClient:
    return McpClient(int_app.url, int_app.token, era="modern", timeout=320).connect()


@pytest.fixture(scope="module")
def lg(int_app) -> McpClient:
    return McpClient(int_app.url, int_app.token, era="legacy", timeout=320).connect()


@pytest.fixture(params=["modern", "legacy"])
def c(request, m, lg) -> McpClient:
    return m if request.param == "modern" else lg


def all_objects(client):
    return [o for p in client.tool("scene_get")["plates"] for o in p["objects"]]


def approx(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def size_of(box):
    return [box["max"][i] - box["min"][i] for i in range(3)]


def center_of(box):
    return [(box["max"][i] + box["min"][i]) / 2 for i in range(len(box["min"]))]


def picture(client, out_dir, name, **arguments):
    result = client.call("screenshot", arguments)
    assert not result.is_error, result.data
    assert len(result.images) == 1
    (out_dir / f"{name}.png").write_bytes(result.images[0][1])
    return result.data


# ------------------------------------------------------------------------------------------------
# Catalogue
# ------------------------------------------------------------------------------------------------

def test_catalogue_is_consistent(c):
    tools = [t for t in c.list_tools() if not t["name"].startswith("debug_")]
    names = {t["name"] for t in tools}
    assert names == TOOLS, (sorted(names - TOOLS), sorted(TOOLS - names))
    assert not names & REMOVED
    for t in tools:
        props = t["inputSchema"].get("properties", {})
        # Filament slots are always `filament` (1-based); `slot` was the old spelling of some tools.
        assert "slot" not in props, t["name"]
        assert t.get("description") and t.get("title") or t.get("annotations", {}).get("title"), t["name"]


def test_instructions_name_only_existing_tools(c):
    text = c.instructions or ""
    assert 1000 < len(text) < 4096
    names = {t["name"] for t in c.list_tools()}
    prefixes = ("app_", "scene_", "mesh_", "project_", "model_", "export_", "object_", "plate_", "settings_", "preset_",
                "filament_", "modifier_", "layer_", "paint_", "slice", "job_")
    # Words that look like tool names ("object_*" is a family, not a name).
    mentioned = {w for w in re.findall(r"\b[a-z][a-z_]*[a-z]\b", text) if w.startswith(prefixes) and "_" in w}
    assert len(mentioned) > 10, mentioned
    unknown = sorted(w for w in mentioned if w not in names)
    assert not unknown, unknown
    for removed in REMOVED:
        assert removed not in text


def test_gcode_kinds_are_export_file(c):
    schema = next(t for t in c.list_tools() if t["name"] == "export_file")["inputSchema"]
    assert {"gcode", "gcode_3mf"} <= set(schema["properties"]["kind"]["enum"])


# ------------------------------------------------------------------------------------------------
# Gap: rotated and scaled instance
# ------------------------------------------------------------------------------------------------

def test_bodies_and_paint_on_a_rotated_scaled_instance(m, out_dir):
    oid = m.tool("model_import", paths=[str(CUBE)])["imported"][0]["id"]
    try:
        m.tool("object_transform", targets=[oid], set_rotation=[0, 0, 30], set_scale=[1.5, 1, 1])
        part = m.tool("paint_get", targets=[oid])["parts"][0]
        lo, hi = part["box"]["min"], part["box"]["max"]
        cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
        # The rotated, stretched cube: 30 x 20 rotated by 30 deg = 35.98 x 32.32 on the bed.
        assert approx(size_of(part["box"])[:2], [30 * 0.866 + 20 * 0.5, 30 * 0.5 + 20 * 0.866], 0.05), part["box"]
        stats = m.tool("mesh_query", kind="stats", targets=[oid])
        assert stats

        # A modifier placed in the plate frame stays bed-aligned with the size asked for.
        mod = m.tool("modifier_add", object=oid, type="modifier", shape={"primitive": "cube", "size": [6, 6, 6]},
                     place={"center": [cx, cy, 10], "frame": "plate"}, settings={"sparse_infill_density": "100%"})
        assert approx(mod["placement"]["center_plate"], [cx, cy, 10], 0.01), mod["placement"]
        assert approx(size_of(mod["placement"]["bbox_plate"]), [6, 6, 6], 0.05), mod["placement"]
        # An absolute move of the body later, still in the plate frame.
        upd = m.tool("modifier_update", target=mod["volume"]["id"], place={"center": [cx + 3, cy, 12], "frame": "plate"})
        assert approx(upd["placement"]["center_plate"], [cx + 3, cy, 12], 0.01), upd["placement"]
        # A relative step with object_transform on the body id.
        m.tool("object_transform", targets=[mod["volume"]["id"]], translate=[0, 2, 0])
        after = m.tool("scene_get", objects=[oid], detail="full")
        body = [v for p in after["plates"] for o in p["objects"] if o["id"] == oid for v in o["volumes"] if v["id"] == mod["volume"]["id"]][0]
        assert body["type"] == "modifier"

        # Paint in the plate frame lands on the rotated part: the top face only.
        # (The 0.02 mm band also takes a sliver of the side walls: 0.01 mm x 100 mm of perimeter.)
        r = m.tool("paint_apply", targets=[oid], channel="support", state="enforce",
                   selector={"z_range": {"min": 19.99, "max": 20.01}})
        assert r["changed"] and r["area_mm2"]["after"]["enforce"] == pytest.approx(30 * 20, abs=4), r["area_mm2"]
        picture(m, out_dir, "integration_rotated", editor={"overlay": "support_paint"}, camera={"preset": "iso", "target": [oid]})
    finally:
        m.tool("delete", targets=[oid])


# ------------------------------------------------------------------------------------------------
# Gap: effective values through every scope
# ------------------------------------------------------------------------------------------------

def test_effective_value_through_every_scope(m):
    oid = m.tool("model_import", paths=[str(CUBE)])["imported"][0]["id"]
    vid = [o for o in all_objects(m) if o["id"] == oid][0]["volumes"][0]["id"]
    key = "sparse_infill_density"

    def eff(scope):
        # source is "<scope>" or "<scope>:<id>"; the scope kind is what the chain is about.
        e = m.tool("settings_get", scope=scope, keys=[key], effective=True)["values"][key]
        return {"value": e["value"], "source": e["source"].split(":")[0]}

    before = m.tool("settings_get", scope="process", keys=[key])["values"][key]
    try:
        m.tool("settings_set", scope="process", values={key: "17%"})
        assert eff({"volume": vid}) == {"value": "17%", "source": "process"}
        m.tool("settings_set", scope={"object": oid}, values={key: "27%"})
        assert eff({"volume": vid}) == {"value": "27%", "source": "object"}
        m.tool("settings_set", scope={"volume": vid}, values={key: "37%"})
        assert eff({"volume": vid}) == {"value": "37%", "source": "volume"}
        assert eff({"object": oid})["value"] == "27%"
        m.tool("layer_range", action="add", object=oid, z_min=0, z_max=5, values={key: "47%"})
        lr = eff({"layer_range": {"object": oid, "z_min": 0, "z_max": 5}})
        assert lr["value"] == "47%" and lr["source"].startswith("layer_range"), lr
        m.tool("layer_range", action="delete", object=oid, z_min=0, z_max=5)
        m.tool("settings_set", scope={"volume": vid}, reset=[key])
        assert eff({"volume": vid}) == {"value": "27%", "source": "object"}
        m.tool("settings_set", scope={"object": oid}, reset=[key])
        assert eff({"volume": vid}) == {"value": "17%", "source": "process"}

        # A per-plate key: the plate's value is in force for the object.
        m.tool("settings_set", scope={"plate": 1}, values={"print_sequence": "by object"})
        seq = m.tool("settings_get", scope={"object": oid}, keys=["print_sequence"], effective=True)["values"]["print_sequence"]
        assert seq == {"value": "by object", "source": "plate:1"}, seq
        m.tool("settings_set", scope={"plate": 1}, reset=["print_sequence"])

        # paint_apply reads the same chain: supports off in the process, on for the object -> no warning.
        m.tool("settings_set", scope="process", values={"enable_support": False})
        r = m.tool("paint_apply", targets=[oid], channel="support", state="enforce", selector={"z_range": {"min": 19.9, "max": 20.1}})
        assert any("enable_support" in n for n in r.get("notes", [])), r.get("notes")
        m.tool("settings_set", scope={"object": oid}, values={"enable_support": True, "support_type": "normal(auto)"})
        r = m.tool("paint_apply", targets=[oid], channel="support", state="enforce", selector={"z_range": {"min": 0.0, "max": 0.1}})
        assert not any("enable_support" in n for n in r.get("notes", [])), r.get("notes")
    finally:
        m.tool("settings_set", scope="process", values={key: before, "enable_support": False})
        m.tool("delete", targets=[oid])


# ------------------------------------------------------------------------------------------------
# Gap: overlay options in the screenshot schema
# ------------------------------------------------------------------------------------------------

def test_overlay_options_in_screenshot_schema(c, out_dir):
    schema = next(t for t in c.list_tools() if t["name"] == "screenshot")["inputSchema"]
    editor = schema["properties"]["editor"]["properties"]
    assert "overhang" in editor["overlay"]["enum"]
    assert editor["overhang_threshold_deg"]["maximum"] == 90
    shot = picture(c, out_dir, f"integration_overhang_60_{c.era}", editor={"overlay": "overhang", "overhang_threshold_deg": 60},
                   camera={"preset": "bottom"})
    assert shot["legend"]["overlay"]["threshold_deg"] == 60
    assert c.tool_error("screenshot", editor={"overlay": "overhang", "overhang_threshold_deg": 120}).code == "invalid_argument"


# ------------------------------------------------------------------------------------------------
# End to end across all groups
# ------------------------------------------------------------------------------------------------

def test_end_to_end_all_groups(m, int_app, out_dir):
    shots = out_dir / "integration"
    shots.mkdir(exist_ok=True)
    model = out_dir / "integration_t.obj"
    t_shape(model)
    # Start from an empty project.
    m.tool("project_new", on_unsaved="discard")
    assert all_objects(m) == []

    # 1. Import (edit group).
    imported = m.tool("model_import", paths=[str(model)])["imported"]
    oid = imported[0]["id"]
    part = m.tool("paint_get", targets=[oid])["parts"][0]
    lo, hi = part["box"]["min"], part["box"]["max"]
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    assert approx(size_of(part["box"]), [40, 10, 25], 0.01)
    over = m.tool("mesh_query", kind="overhangs", targets=[oid], threshold_deg=45)
    assert over

    # 2. A modifier with dense infill in the column (settings group).
    mod = m.tool("modifier_add", object=oid, type="modifier", shape={"primitive": "cube", "size": [12, 12, 8]},
                 place={"center": [cx, cy, 8], "frame": "plate"}, name="dense", settings={"sparse_infill_density": "100%"})
    assert approx(mod["placement"]["center_plate"], [cx, cy, 8], 0.01)

    # 3. Per-object settings: supports only where enforced.
    m.tool("settings_set", scope={"object": oid}, values={"enable_support": True, "support_type": "normal(manual)",
                                                          "sparse_infill_density": "10%"})

    # 4. Paint: enforce support under the left arm only (paint group).
    r = m.tool("paint_apply", targets=[oid], channel="support", state="enforce",
               selector={"and": [{"box": {"min": [lo[0] - 1, lo[1] - 1, 1], "max": [cx - 2, hi[1] + 1, 30]}},
                                 {"overhang": {"threshold_deg": 45}}]})
    assert r["changed"] and r["area_mm2"]["after"]["enforce"] == pytest.approx(15 * 10, rel=0.02), r["area_mm2"]
    assert not any("enable_support" in n for n in r.get("notes", [])), r.get("notes")
    picture(m, shots, "1_editor_support_paint", editor={"overlay": "support_paint"}, camera={"preset": "iso", "target": [oid]})
    picture(m, shots, "2_editor_overhang", editor={"overlay": "overhang", "overhang_threshold_deg": 45}, camera={"preset": "bottom"})

    # 5. Validate and slice (slice group).
    assert m.tool("plate_validate")["ok"] is True
    s = m.tool("slice", wait_s=280)
    assert s["status"] == "finished", s
    result = m.tool("slice_result", sections=["summary"])
    assert result["summary"]["layers"] > 50

    # 6. Analyse: support only on the left; the modifier's infill is denser than the infill above it.
    a = m.tool("analyze", checks=["supports", "overhangs", "flow", "summary"])
    sup = a["supports"]
    assert sup["volume_mm3"] > 0 and sup["box"] is not None, sup
    assert center_of(sup["box"])[0] < cx - 5, (sup["box"], cx)
    for check in ("supports", "overhangs", "flow"):
        message = a[check]["verdict"]["message"]
        assert not re.search(r"\d\.\d{5,}", message), message  # human-readable numbers
    inside = m.tool("analyze", checks=["region"], params={"region": {"box": {"min": [cx - 4, cy - 4, 5], "max": [cx + 4, cy + 4, 11]}}})
    above = m.tool("analyze", checks=["region"], params={"region": {"box": {"min": [cx - 4, cy - 4, 13], "max": [cx + 4, cy + 4, 19]}}})
    assert inside["region"]["length_mm"] > 1.5 * above["region"]["length_mm"], (inside["region"], above["region"])
    layers = m.tool("slice_layers", limit=5)
    assert len(layers["rows"]) == 5 and layers["layer_count"] == result["summary"]["layers"]

    # 7. Preview pictures (preview view of the slice group, camera of the screenshot tool).
    shot = picture(m, shots, "3_preview_feature", view="preview", camera={"preset": "iso"})
    assert shot["legend"]
    picture(m, shots, "4_preview_support_front", view="preview", camera={"preset": "front", "projection": "orthographic"})

    # 8. Export the G-code (edit group's export_file, slice group's writer).
    path = out_dir / "integration_t.gcode"
    e = m.tool("export_file", kind="gcode", path=str(path), overwrite=True)
    assert Path(e["path"]).exists() and e["bytes"] == path.stat().st_size and e["files"][0]["bytes"] == e["bytes"]
    assert e["header"]
    text = path.read_text(encoding="utf-8", errors="replace")
    assert ";TYPE:Support" in text or "; FEATURE: Support" in text
    e3 = m.tool("export_file", kind="gcode_3mf", path=str(out_dir / "integration_t.gcode.3mf"), overwrite=True)
    assert e3["bytes"] > 1000

    # 9. Every change is one named undo step.
    steps = m.tool("history", action="list")
    text_steps = str(steps)
    assert "MCP: " in text_steps

    # 10. An edit invalidates the slice; export then refuses.
    # (A rename does not invalidate a slice, in the GUI either; a move does.)
    m.tool("object_transform", targets=[oid], translate=[5, 0, 0])
    assert m.tool_error("export_file", kind="gcode", path=str(out_dir / "stale.gcode"), overwrite=True).code == "not_sliced"
