"""Paint group: paint_apply, paint_get and their GUI side (undo, gizmo, 3MF), against a 20 mm cube.

Runs in an instance of its own (the paint tests change the scene more than an undo can tidy up
cheaply), with both protocol eras. Areas are checked against analytic values; precise painting at
0.1 mm is expected within 1 %.
"""

from __future__ import annotations

import math

import pytest

from orca_mcp import McpClient, REPO_ROOT

CUBE = REPO_ROOT / "tests" / "data" / "20mm_cube.obj"
CHANNELS = ["support", "seam", "color", "fuzzy_skin"]


# ---------------------------------------------------------------------------------------------
# fixtures
# ---------------------------------------------------------------------------------------------

@pytest.fixture(scope="module")
def cube_app(launch_app):
    return launch_app("paint_cube", files=[str(CUBE)])


@pytest.fixture(scope="module")
def eras(cube_app):
    return {era: McpClient(cube_app.url, cube_app.token, era=era).connect() for era in ("modern", "legacy")}


def the_object(c) -> dict:
    scene = c.tool("scene_get")
    objs = [o for p in scene["plates"] for o in p["objects"]] + scene.get("off_plate", [])
    assert len(objs) == 1
    return objs[0]


def clear_all(c, oid):
    for ch in CHANNELS:
        c.tool("paint_apply", targets=[oid], channel=ch, action="clear")


@pytest.fixture(params=["modern", "legacy"])
def pc(request, eras):
    """(client, object id) on a cube with no paint; paint is cleared again afterwards."""
    c = eras[request.param]
    oid = the_object(c)["id"]
    clear_all(c, oid)
    yield c, oid
    c.call("debug_paint_gizmo", {"action": "close"})
    clear_all(c, oid)


def undo_through(c, step):
    """Undo until the undo step named `step` has been undone (paint clears after it add steps of their own)."""
    names = c.tool("history")["undo"]
    assert step in names, (step, names[:5])
    c.tool("history", action="undo", steps=names.index(step) + 1)


def box_of(c, oid):
    part = c.tool("paint_get", targets=[oid])["parts"][0]
    return part["box"]["min"], part["box"]["max"], part


def area(result, state, part=0):
    return result["parts"][part]["after"].get(state, {}).get("area_mm2", 0.0)


def close(value, expected, rel=0.015, abs_=0.05):
    return abs(value - expected) <= max(abs_, rel * abs(expected))


def paint(c, oid, selector, state="enforce", channel="support", **kw):
    return c.tool("paint_apply", targets=[oid], channel=channel, state=state, selector=selector, resolution_mm=kw.pop("resolution_mm", 0.1), **kw)


# ---------------------------------------------------------------------------------------------
# registration and read-only
# ---------------------------------------------------------------------------------------------

def test_tools_are_listed_with_annotations(eras):
    for c in eras.values():
        tools = {t["name"]: t for t in c.list_tools()}
        for name in ("paint_apply", "paint_get", "plate_layer_events"):
            assert name in tools, name
        assert tools["paint_get"]["annotations"]["readOnlyHint"] is True
        assert tools["paint_apply"]["annotations"].get("readOnlyHint") in (False, None)
        schema = tools["paint_apply"]["inputSchema"]
        assert "Selector" in schema["$defs"] and "Seed" in schema["$defs"]


def test_paint_get_reports_part_geometry(pc):
    c, oid = pc
    lo, hi, part = box_of(c, oid)
    assert [round(h - l, 3) for l, h in zip(lo, hi)] == [20.0, 20.0, 20.0]
    assert part["facets"] == 12
    assert close(part["surface_mm2"], 2400.0, rel=1e-4)
    assert part["box_object"] is not None
    assert part["channels"] == {}  # nothing painted -> no channel listed
    one = c.tool("paint_get", targets=[oid], channel="seam")["parts"][0]
    assert one["channels"]["seam"]["states"] == {}


# ---------------------------------------------------------------------------------------------
# selectors against analytic areas
# ---------------------------------------------------------------------------------------------

def test_all_and_idempotence(pc):
    c, oid = pc
    r = paint(c, oid, {"all": True})
    assert r["changed"] is True and r["undo"].startswith("MCP: paint support enforce")
    assert close(area(r, "enforce"), 2400.0) and r["parts"][0]["after"]["enforce"]["fraction"] == pytest.approx(1.0, abs=1e-6)
    assert r["slice_invalidated"] is True
    steps = c.tool("history")["undo"]
    again = paint(c, oid, {"all": True})
    assert again["changed"] is False and "undo" not in again
    assert c.tool("history")["undo"] == steps, "an idempotent repeat adds no undo step"


@pytest.mark.parametrize("name,selector_fn,expected", [
    # horizontal band 5.3..12.7 above the bottom: 4 sides x 20 x 7.4
    ("z_range", lambda lo, hi, cx, cy: {"z_range": {"min": lo[2] + 5.3, "max": lo[2] + 12.7}}, 592.0),
    # y <= 7.3 from the front: front face + 4 strips of 7.3
    ("box", lambda lo, hi, cx, cy: {"box": {"min": [lo[0] - 1, lo[1] - 1, lo[2] - 1], "max": [hi[0] + 1, lo[1] + 7.3, hi[2] + 1]}}, 984.0),
    # ball of radius 4 on the top face centre: a disc
    ("sphere", lambda lo, hi, cx, cy: {"sphere": {"center": [cx, cy, hi[2]], "radius": 4}}, math.pi * 16),
    # vertical cylinder r 3 through the cube: discs on top and bottom
    ("cylinder", lambda lo, hi, cx, cy: {"cylinder": {"base": [cx, cy, lo[2] - 1], "axis": [0, 0, 1], "radius": 3, "height": 22}}, 2 * math.pi * 9),
    # half the cube
    ("halfspace", lambda lo, hi, cx, cy: {"halfspace": {"point": [cx, cy, 0], "normal": [1, 0, 0]}}, 1200.0),
    # capsule along the top: stadium 2*r*L + pi r^2
    ("capsule", lambda lo, hi, cx, cy: {"capsule": {"a": [cx - 5, cy, hi[2]], "b": [cx + 5, cy, hi[2]], "radius": 2}}, 40 + math.pi * 4),
    # slab between planes x+y = 3 and 9 (normal not unit: planes as given) measured from the min corner
    ("slab", lambda lo, hi, cx, cy: {"slab": {"normal": [0, 0, 1], "min": lo[2] + 2, "max": lo[2] + 4}}, 4 * 20 * 2),
    # oriented box: a 4 x 4 x 40 column rotated 45 deg about z through the centre -> two squares (top, bottom)
    ("oriented_box", lambda lo, hi, cx, cy: {"box": {"center": [cx, cy, (lo[2] + hi[2]) / 2], "size": [4, 4, 40], "rotation_deg": [0, 0, 45]}}, 32.0),
    # box AND NOT sphere: the top 0.5 mm minus the disc
    ("and_not", lambda lo, hi, cx, cy: {"and": [{"box": {"min": [lo[0] - 1, lo[1] - 1, hi[2] - 0.5], "max": [hi[0] + 1, hi[1] + 1, hi[2] + 1]}},
                                                {"not": {"sphere": {"center": [cx, cy, hi[2]], "radius": 4}}}]}, 440 - math.pi * 16),
    # two discs, OR
    ("or", lambda lo, hi, cx, cy: {"or": [{"sphere": {"center": [cx, cy, hi[2]], "radius": 4}}, {"sphere": {"center": [cx, cy, lo[2]], "radius": 4}}]}, 2 * math.pi * 16),
    # whole-facet leaves
    ("normal_up", lambda lo, hi, cx, cy: {"normal": {"direction": [0, 0, 1], "max_angle_deg": 1}}, 400.0),
    ("overhang", lambda lo, hi, cx, cy: {"overhang": {"threshold_deg": 45}}, 400.0),
    ("facets", lambda lo, hi, cx, cy: {"facets": [0, 1]}, 400.0),
    ("connected", lambda lo, hi, cx, cy: {"connected": {"seed": {"point": [cx, cy, hi[2] + 3]}, "angle_deg": 30}}, 400.0),
])
def test_selector_areas(pc, name, selector_fn, expected):
    c, oid = pc
    lo, hi, _ = box_of(c, oid)
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    r = paint(c, oid, selector_fn(lo, hi, cx, cy))
    got = area(r, "enforce")
    assert close(got, expected), f"{name}: {got} vs {expected}"
    # paint_get agrees with what paint_apply returned
    g = c.tool("paint_get", targets=[oid], channel="support")["parts"][0]["channels"]["support"]["states"]
    assert g["enforce"]["area_mm2"] == pytest.approx(got, abs=1e-3)


def test_facets_mode_paints_whole_triangles(pc):
    c, oid = pc
    lo, hi, _ = box_of(c, oid)
    sel = {"box": {"min": [lo[0] - 1, lo[1] - 1, lo[2] - 1], "max": [hi[0] + 1, lo[1] + 7.3, hi[2] + 1]}}
    r = paint(c, oid, sel, mode="facets")
    # whole triangles whose centroid is inside: the front face's 2 triangles and some side ones
    assert area(r, "enforce") % 200 == pytest.approx(0, abs=1e-6)
    assert r["triangles_added"] == 0


def test_stroke_and_seeds(pc):
    c, oid = pc
    lo, hi, _ = box_of(c, oid)
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    r = paint(c, oid, {"stroke": {"path": [[cx - 5, cy, hi[2] + 0.5], [cx + 5, cy, hi[2] + 0.5]], "radius": 2}})
    assert close(area(r, "enforce"), 40 + math.pi * 4, rel=0.015)
    assert [s["type"] for s in r["seeds"]] == ["point", "point"]
    assert all(s["snap_distance_mm"] == pytest.approx(0.5, abs=1e-3) for s in r["seeds"])
    clear_all(c, oid)

    # ray seed from above, facet seed, both -> top face by smart fill
    r = paint(c, oid, {"connected": {"seed": {"ray": {"origin": [cx + 3, cy + 2, hi[2] + 50], "direction": [0, 0, -1]}}}})
    assert close(area(r, "enforce"), 400.0)
    assert r["seeds"][0]["normal"] == [0, 0, 1] and r["seeds"][0]["point"][2] == pytest.approx(hi[2], abs=1e-3)
    clear_all(c, oid)
    r = paint(c, oid, {"connected": {"seed": {"facet": 0}}})
    assert close(area(r, "enforce"), 400.0)


def top_camera(cx, cy, z_eye=100.0, half_h=20.0, width=400, height=300):
    """Orthographic camera looking straight down, as screenshot returns it (row-major matrices)."""
    aspect = width / height
    half_w = half_h * aspect
    near, far = 1.0, 300.0
    view = [[1, 0, 0, -cx], [0, 1, 0, -cy], [0, 0, 1, -z_eye], [0, 0, 0, 1]]
    proj = [[1 / half_w, 0, 0, 0], [0, 1 / half_h, 0, 0], [0, 0, -2 / (far - near), -(far + near) / (far - near)], [0, 0, 0, 1]]
    return {"projection": "orthographic", "view_matrix": view, "projection_matrix": proj, "width": width, "height": height}, half_w, half_h


def test_pixel_seed(pc):
    c, oid = pc
    lo, hi, _ = box_of(c, oid)
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    camera, half_w, half_h = top_camera(cx, cy)
    # the pixel over the point (cx + 4, cy - 3) on the top face
    u = (4 / half_w) * 200 + 200
    v = (1 - (-3 / half_h)) * 150
    r = paint(c, oid, {"connected": {"seed": {"pixel": [u, v], "camera": camera}}})
    seed = r["seeds"][0]
    assert seed["type"] == "pixel" and seed["normal"] == [0, 0, 1]
    assert seed["point"][0] == pytest.approx(cx + 4, abs=1e-3) and seed["point"][1] == pytest.approx(cy - 3, abs=1e-3)
    assert close(area(r, "enforce"), 400.0)
    # a pixel beside the cube misses
    err = c.tool_error("paint_apply", targets=[oid], channel="support", state="enforce",
                       selector={"connected": {"seed": {"pixel": [5, 5], "camera": camera}}})
    assert err.code == "seed_missed" and "pixel" in err.hint
    # a camera without matrices
    err = c.tool_error("paint_apply", targets=[oid], channel="support", state="enforce",
                       selector={"connected": {"seed": {"pixel": [5, 5], "camera": {"width": 10}}}})
    assert err.code == "invalid_argument" and "view_matrix" in err.message


# ---------------------------------------------------------------------------------------------
# operations and channels
# ---------------------------------------------------------------------------------------------

def test_operations(pc):
    c, oid = pc
    lo, hi, _ = box_of(c, oid)
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    top = {"normal": {"direction": [0, 0, 1], "max_angle_deg": 1}}
    paint(c, oid, {"all": True}, state="block")
    # erase a disc
    r = c.tool("paint_apply", targets=[oid], channel="support", action="erase", resolution_mm=0.1,
               selector={"sphere": {"center": [cx, cy, hi[2]], "radius": 4}})
    assert close(area(r, "block"), 2400 - math.pi * 16)
    assert r["parts"][0]["delta_mm2"]["block"] == pytest.approx(-math.pi * 16, rel=0.01)
    # only_where: turn the top's blockers into enforcers; the erased disc stays unpainted
    r = paint(c, oid, top, state="enforce", only_where="block")
    assert close(area(r, "enforce"), 400 - math.pi * 16)
    # bucket fill: what is enforce, connected to a top seed -> block again
    r = paint(c, oid, {"same_state": {"seed": {"point": [cx + 8, cy + 8, hi[2]]}}}, state="block")
    assert area(r, "enforce") == 0 and close(area(r, "block"), 2400 - math.pi * 16)
    # replace: clear the channel, then paint
    r = paint(c, oid, top, state="enforce", action="replace")
    assert close(area(r, "enforce"), 400) and area(r, "block") == 0
    # clear
    r = c.tool("paint_apply", targets=[oid], channel="support", action="clear")
    assert r["parts"][0]["after"] == {} and r["changed"] is True
    # dry run changes nothing
    r = paint(c, oid, top, dry_run=True)
    assert r["dry_run"] is True and r["would_change"] is True and r["changed"] is False
    assert c.tool("paint_get", targets=[oid], channel="support")["parts"][0]["channels"]["support"]["states"] == {}


@pytest.mark.parametrize("channel,state,name", [("seam", "enforce", "enforce"), ("seam", "block", "block"), ("fuzzy_skin", "fuzzy", "fuzzy")])
def test_other_channels(pc, channel, state, name):
    c, oid = pc
    lo, hi, _ = box_of(c, oid)
    r = paint(c, oid, {"z_range": {"min": lo[2] + 5, "max": lo[2] + 10}}, state=state, channel=channel)
    assert close(area(r, name), 400.0)
    got = c.tool("paint_get", targets=[oid])["parts"][0]["channels"]
    assert list(got) == [channel] and close(got[channel]["states"][name]["area_mm2"], 400.0)
    scene_obj = the_object(c)
    assert scene_obj["paint"][channel] is True


def test_color_needs_the_filament_slot(pc):
    c, oid = pc
    err = c.tool_error("paint_apply", targets=[oid], channel="color", state={"filament": 2}, selector={"all": True})
    assert err.code == "invalid_argument" and "filament_edit" in err.hint and err.details["filament_count"] == 1
    # filament 1 exists: allowed
    r = c.tool("paint_apply", targets=[oid], channel="color", state={"filament": 1}, selector={"facets": [0]})
    assert close(area(r, "filament_1"), 200.0)


# ---------------------------------------------------------------------------------------------
# validation
# ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("selector,fragment", [
    ({"box": {"min": [0, 0, 0], "max": [1, 1, 1]}, "sphere": {"center": [0, 0, 0], "radius": 1}}, "exactly one key"),
    ({"bx": {"min": [0, 0, 0]}}, "bx"),
    ({"box": {"min": [0, 0], "max": [1, 1, 1]}}, "selector.box.min"),
    ({"box": {"min": [2, 0, 0], "max": [1, 1, 1]}}, "min <= max"),
    ({"and": [{"all": True}, {"sphere": {"center": [0, 0, 0], "radius": -1}}]}, "radius"),
    ({"or": [{"same_state": {"seed": {"facet": 0}}}, {"all": True}]}, "bucket fill"),
    ({"facets": [99]}, "out of range"),
    ({"sphere": {"center": [0, 0, 0], "radius": 1, "frame": "mesh"}}, "frame"),
])
def test_bad_selectors(pc, selector, fragment):
    c, oid = pc
    err = c.tool_error("paint_apply", targets=[oid], channel="support", state="enforce", selector=selector)
    assert err.code == "invalid_argument", err.message
    assert fragment in err.message, err.message


def test_bad_states_and_arguments(pc):
    c, oid = pc
    for channel, state in (("support", "fuzzy"), ("fuzzy_skin", "enforce"), ("seam", {"filament": 1})):
        err = c.tool_error("paint_apply", targets=[oid], channel=channel, state=state, selector={"all": True})
        assert err.code == "invalid_argument"
    assert c.tool_error("paint_apply", targets=[oid], channel="support", selector={"all": True}).code == "invalid_argument"  # no state
    assert c.tool_error("paint_apply", targets=[oid], channel="support", state="enforce").code == "invalid_argument"  # no selector
    assert c.tool_error("paint_apply", targets=[oid], channel="support", action="clear", selector={"all": True}).code == "invalid_argument"
    assert c.tool_error("paint_apply", targets=["o999999"], channel="support", state="enforce", selector={"all": True}).code == "stale_id"
    err = c.tool_error("paint_apply", targets=[oid], channel="support", state="enforce", selector={"all": True}, resolution_mm=0.01)
    assert err.code == "invalid_argument" and "resolution_mm" in err.message


def test_budget_coarsens_or_fails(pc):
    c, oid = pc
    lo, hi, _ = box_of(c, oid)
    sel = {"sphere": {"center": [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, hi[2]], "radius": 7}}
    r = paint(c, oid, sel, resolution_mm=0.05, max_new_triangles=1000)
    assert r["coarsened"] is True and r["resolution_used_mm"] > 0.05 and r["triangles_added"] <= 1000
    assert any("coarsened" in n for n in r.get("notes", []))
    assert close(area(r, "enforce"), math.pi * 49, rel=0.05)
    clear_all(c, oid)
    err = c.tool_error("paint_apply", targets=[oid], channel="support", state="enforce", selector=sel, resolution_mm=0.05,
                       max_new_triangles=1000, coarsen=False)
    assert err.code == "too_fine" and err.details["max_new_triangles"] == 1000


# ---------------------------------------------------------------------------------------------
# GUI side: undo, gizmo, 3MF, other bodies, transforms
# ---------------------------------------------------------------------------------------------

def test_undo_redo(pc):
    c, oid = pc
    before = c.tool("paint_get", targets=[oid])
    r = paint(c, oid, {"all": True}, state="block")
    assert c.tool("history")["undo"][0] == r["undo"]
    c.tool("history", action="undo")
    assert c.tool("paint_get", targets=[oid]) == before
    c.tool("history", action="redo")
    after = c.tool("paint_get", targets=[oid], channel="support")["parts"][0]["channels"]["support"]["states"]
    assert close(after["block"]["area_mm2"], 2400)


def test_open_gizmo_is_closed_and_reloads_the_paint(pc):
    c, oid = pc
    lo, hi, _ = box_of(c, oid)
    opened = c.tool("debug_paint_gizmo", action="open", object=oid, channel="support")
    assert opened["opened"] is True
    r = paint(c, oid, {"z_range": {"min": lo[2] + 2, "max": lo[2] + 6}})
    assert any("Closed the open" in n for n in r["notes"]), r.get("notes")
    # The user opens the gizmo afterwards: it loads exactly the painted data.
    for channel, state in (("support", "enforce"), ("seam", "block"), ("fuzzy_skin", "fuzzy")):
        if channel != "support":
            paint(c, oid, {"z_range": {"min": lo[2] + 2, "max": lo[2] + 6}}, state=state, channel=channel)
        seen = c.tool("debug_paint_gizmo", action="open", object=oid, channel=channel)
        assert seen["all_equal"] is True, seen
        assert close(seen["parts"][0]["gizmo_states"][state]["area_mm2"], 320.0)
        c.tool("debug_paint_gizmo", action="close")
        # closing the gizmo wrote nothing back
        assert close(c.tool("paint_get", targets=[oid], channel=channel)["parts"][0]["channels"][channel]["states"][state]["area_mm2"], 320.0)


def test_3mf_round_trip(pc, cube_app):
    c, oid = pc
    lo, hi, _ = box_of(c, oid)
    paint(c, oid, {"sphere": {"center": [lo[0], lo[1], lo[2]], "radius": 6}}, state="enforce")
    paint(c, oid, {"z_range": {"min": lo[2] + 10, "max": lo[2] + 11}}, state="block", channel="seam")
    paint(c, oid, {"normal": {"direction": [0, 0, 1], "max_angle_deg": 1}}, state="fuzzy", channel="fuzzy_skin")
    path = cube_app.data_dir / "mcp" / "workspace" / "paint_roundtrip.3mf"  # a write root
    r = c.tool("debug_paint_3mf", path=str(path))
    assert r["all_equal"] is True, r
    painted = r["parts"][0]
    assert painted["support"]["painted"] and painted["seam"]["painted"] and painted["fuzzy_skin"]["painted"]


def test_targets_must_be_model_parts(pc):
    c, oid = pc
    fx = c.tool("modifier_add", object=oid, type="modifier", shape={"primitive": "cube", "size": [5, 5, 5]})
    fx["object"] = fx["changed"]
    modifier = fx["volume"]
    try:
        err = c.tool_error("paint_apply", targets=[modifier["id"]], channel="support", state="enforce", selector={"all": True})
        assert err.code == "not_a_part" and "modifier" in err.message
        # the object as target paints only its model part
        r = paint(c, oid, {"all": True})
        assert [p["part"] for p in r["parts"]] == [v["id"] for v in fx["object"]["volumes"] if v["type"] == "part"]
    finally:
        undo_through(c, fx["undo"])  # the modifier


def test_frames_on_transformed_instances(pc):
    """Rotated, non-uniformly scaled and mirrored instance: paint lands where the selector says in each frame."""
    c, oid = pc
    fx = c.tool("object_transform", targets=[oid], set_rotation=[0, 0, 30], set_scale=[1.5, 1, 0.5], mirror="x", frame="local")
    try:
        lo, hi, part = box_of(c, oid)
        # plate frame: the instance is 10 mm tall now; a band in its middle 2 mm high
        r = paint(c, oid, {"z_range": {"min": lo[2] + 4, "max": lo[2] + 6}})
        enforce = r["parts"][0]["after"]["enforce"]
        assert enforce["box"]["min"][2] == pytest.approx(lo[2] + 4, abs=0.15) and enforce["box"]["max"][2] == pytest.approx(lo[2] + 6, abs=0.15)
        # perimeter of the scaled square section (30 x 20) x 2 mm
        assert close(enforce["area_mm2"], 2 * (30 + 20) * 2, rel=0.01)
        clear_all(c, oid)
        # object frame: the unscaled cube's own coordinates; the top face in the object frame is the top in the plate frame
        olo, ohi = part["box_object"]["min"], part["box_object"]["max"]
        r = paint(c, oid, {"box": {"min": [olo[0] - 1, olo[1] - 1, ohi[2] - 0.5], "max": [ohi[0] + 1, ohi[1] + 1, ohi[2] + 1], "frame": "object"}})
        top = r["parts"][0]["after"]["enforce"]
        assert top["box"]["min"][2] >= hi[2] - 0.5 * 0.5 - 0.15  # scaled by 0.5 in z
        # plate area: top 30 x 20 = 600 plus side strips of 0.25 mm (perimeter 100)
        assert close(top["area_mm2"], 600 + 100 * 0.25, rel=0.01)
        # world frame = plate frame on plate 1
        clear_all(c, oid)
        a = paint(c, oid, {"sphere": {"center": [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, hi[2]], "radius": 3}})
        clear_all(c, oid)
        b = paint(c, oid, {"sphere": {"center": [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, hi[2]], "radius": 3, "frame": "world"}})
        assert area(a, "enforce") == pytest.approx(area(b, "enforce"), abs=1e-6) and close(area(a, "enforce"), math.pi * 9)
    finally:
        undo_through(c, fx["undo"])  # the transform


def test_instances_share_paint(pc):
    c, oid = pc
    fx = c.tool("object_duplicate", targets=[oid], copies=1)
    try:
        insts = [o for o in c.tool("scene_get", objects=[oid])["plates"][0]["objects"] if o["id"] == oid][0]["instances"]
        # Second instance exactly 40 mm to the right of the first.
        first, second = insts[0]["center"], insts[1]["center"]
        c.tool("object_transform", targets=[insts[1]["id"]], translate=[first[0] + 40 - second[0], first[1] - second[1], 0])
        assert len(insts) == 2
        lo, hi, _ = box_of(c, insts[1]["id"])  # boxes of the second instance
        r = paint(c, insts[1]["id"], {"sphere": {"center": [lo[0], lo[1], hi[2]], "radius": 5}})
        assert any("shared by all" in n for n in r["notes"])
        # the corner disc (quarter on 3 faces) is in the second instance's frame ...
        e = r["parts"][0]["after"]["enforce"]
        assert e["box"]["min"][0] == pytest.approx(lo[0], abs=0.2)
        # ... and the paint of the first instance is the same data
        first = c.tool("paint_get", targets=[insts[0]["id"]], channel="support")["parts"][0]["channels"]["support"]["states"]["enforce"]
        assert first["area_mm2"] == pytest.approx(e["area_mm2"], abs=1e-3)
        assert first["box"]["min"][0] == pytest.approx(lo[0] - 40, abs=0.5)
        # a world ray hitting the second instance seeds the shared surface
        clear_all(c, oid)
        r = paint(c, oid, {"connected": {"seed": {"ray": {"origin": [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, 100], "direction": [0, 0, -1]}, "frame": "world"}}})
        assert close(area(r, "enforce"), 400)
    finally:
        undo_through(c, fx["undo"])


def test_components_and_facets(pc):
    c, oid = pc
    lo, hi, _ = box_of(c, oid)
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    paint(c, oid, {"or": [{"sphere": {"center": [cx, cy, hi[2]], "radius": 4}}, {"sphere": {"center": [cx, cy, lo[2]], "radius": 3}}]})
    paint(c, oid, {"z_range": {"min": lo[2] + 9, "max": lo[2] + 11}}, state="block")
    got = c.tool("paint_get", targets=[oid], channel="support", components=True, facets=True)["parts"][0]["channels"]["support"]
    regions = got["regions"]
    assert got["regions_total"] == 3
    assert [r["state"] for r in regions] == ["block", "enforce", "enforce"]
    assert close(regions[1]["area_mm2"], math.pi * 16) and regions[1]["mean_normal"] == [0, 0, 1]
    assert regions[1]["centroid"][2] == pytest.approx(hi[2], abs=1e-3) and regions[1]["flatness"] == pytest.approx(1, abs=1e-3)
    assert regions[2]["mean_normal"] == [0, 0, -1]
    assert regions[0]["flatness"] < 0.1  # a closed band
    assert got["facet_ids"]["enforce"]["count"] == 4 and got["facet_ids"]["block"]["count"] == 8


# ---------------------------------------------------------------------------------------------
# screenshots: overlays and pixel seeds from a real camera
# ---------------------------------------------------------------------------------------------

def shot(c, out_dir, name, **arguments):
    import json as _json
    from orca_mcp.png import read_png
    result = c.call("screenshot", arguments)
    assert not result.is_error, result.data
    mime, data = result.images[0]
    (out_dir / f"{name}.png").write_bytes(data)
    (out_dir / f"{name}.json").write_text(_json.dumps(result.data, indent=1), encoding="utf-8")
    return result.data, read_png(data)


def test_overlays_match_the_statistics(pc, out_dir):
    from orca_mcp.png import close_to, hex_color
    c, oid = pc
    lo, hi, _ = box_of(c, oid)
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    # enforcer disc r 6 on top, blocker strip x > cx + 5 on top
    paint(c, oid, {"sphere": {"center": [cx - 2, cy, hi[2]], "radius": 6}})
    paint(c, oid, {"and": [{"box": {"min": [cx + 5, lo[1] - 1, hi[2] - 0.5], "max": [hi[0] + 1, hi[1] + 1, hi[2] + 1]}},
                           {"normal": {"direction": [0, 0, 1], "max_angle_deg": 1}}]}, state="block")
    args = dict(camera={"preset": "top", "projection": "orthographic", "target": [oid]}, size={"width": 600, "height": 600},
                editor={"show_bed": False, "overlay": "support_paint"})
    info, image = shot(c, out_dir, f"paint_support_overlay_{c.era}", **args)
    legend = info["legend"]["overlay"]
    assert legend["paint_channel"] == "support" and set(legend["states"]) == {"enforce", "block"}
    ppmm = info["camera"]["height"] / info["camera"]["ortho_height"]
    enforce_px = image.count(close_to(hex_color(legend["states"]["enforce"]), 6))
    block_px = image.count(close_to(hex_color(legend["states"]["block"]), 6))
    assert enforce_px == pytest.approx(math.pi * 36 * ppmm ** 2, rel=0.03)
    assert block_px == pytest.approx(5 * 20 * ppmm ** 2, rel=0.03)
    # the other channels draw nothing here; the overhang overlay from below shows the bottom face
    info, image = shot(c, out_dir, f"paint_seam_overlay_empty_{c.era}", **{**args, "editor": {"show_bed": False, "overlay": "seam_paint"}})
    assert info["legend"]["overlay"].get("empty") is True
    info, image = shot(c, out_dir, f"paint_overhang_overlay_{c.era}", camera={"preset": "bottom", "projection": "orthographic", "target": [oid]},
                       size={"width": 600, "height": 600}, editor={"show_bed": False, "overlay": "overhang"})
    ov = info["legend"]["overlay"]
    assert ov["area_mm2"] == pytest.approx(400, abs=0.01)
    ppmm = info["camera"]["height"] / info["camera"]["ortho_height"]
    assert image.count(close_to(hex_color(ov["overhang"]), 6)) == pytest.approx(400 * ppmm ** 2, rel=0.03)


def project(camera, point):
    def mul(m, v):
        return [sum(m[r][k] * v[k] for k in range(4)) for r in range(4)]
    clip = mul(camera["projection_matrix"], mul(camera["view_matrix"], list(point) + [1.0]))
    x, y = clip[0] / clip[3], clip[1] / clip[3]
    return [(x * 0.5 + 0.5) * camera["width"], (1 - (y * 0.5 + 0.5)) * camera["height"]]


def test_pixel_seed_from_a_screenshot(pc, out_dir):
    c, oid = pc
    lo, hi, _ = box_of(c, oid)
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    info, _ = shot(c, out_dir, f"paint_seed_view_{c.era}", camera={"preset": "iso", "target": [oid]}, size={"width": 640, "height": 480})
    camera = info["camera"]
    # A point of the top face as the picture shows it (plate 1: plate = world coordinates).
    pixel = project(camera, [cx + 3, cy - 2, hi[2]])
    hit = c.tool("mesh_query", kind="raycast", targets=[oid], pixel=pixel, camera=camera)["hit"]
    r = paint(c, oid, {"connected": {"seed": {"pixel": pixel, "camera": camera}}})
    seed = r["seeds"][0]
    assert seed["facet"] == hit["facet"] and seed["point"] == pytest.approx(hit["point"], abs=0.01)
    assert seed["point"] == pytest.approx([cx + 3, cy - 2, hi[2]], abs=0.01)
    assert close(area(r, "enforce"), 400.0)
    shot(c, out_dir, f"paint_seed_result_{c.era}", camera={"preset": "iso", "target": [oid]}, size={"width": 640, "height": 480},
         editor={"overlay": "support_paint"})
    # The object's reported pixel is the cube's nearest corner in this view: whichever face the ray
    # takes, exactly one face is filled.
    clear_all(c, oid)
    r = paint(c, oid, {"connected": {"seed": {"pixel": info["objects_in_view"][0]["pixel"], "camera": camera}}})
    assert close(area(r, "enforce"), 400.0)
