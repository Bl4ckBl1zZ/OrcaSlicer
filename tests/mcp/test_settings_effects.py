"""Effect tests: settings, modifiers, height ranges and filament assignment must change the sliced result.

The project is saved to a 3MF with debug_settings_project and sliced with the application's own
command-line slicer (gcode_stats.slice_3mf) in a private data directory; the slicing tools are on
another branch. Every test compares with the unmodified cube and undoes its changes.
"""

import pytest

from conftest import REPO_ROOT, SAMPLE_MODEL, cube
from gcode_stats import slice_3mf
from orca_mcp import McpClient
from settings_testutil import undo


@pytest.fixture(scope="module")
def effects_app(launch_app):
    return launch_app("effects", files=[str(REPO_ROOT / "tests" / "data" / SAMPLE_MODEL)])


@pytest.fixture(scope="module")
def ec(effects_app):
    return McpClient(effects_app.url, effects_app.token, era="modern").connect()


@pytest.fixture(scope="module")
def slicer(effects_app, ec, run_dir):
    workspace = effects_app.data_dir / "mcp" / "workspace"
    workspace.mkdir(parents=True, exist_ok=True)
    counter = {"n": 0}

    def run(tag):
        counter["n"] += 1
        project = workspace / f"effect_{counter['n']}_{tag}.3mf"
        ec.tool("debug_settings_project", action="save", path=str(project))
        return slice_3mf(project, run_dir / "cli" / tag)

    return run


@pytest.fixture(scope="module")
def baseline(slicer):
    return slicer("baseline")


def centre(client):
    inst = cube(client)["instances"][0]
    return inst["center"][0], inst["center"][1]


def box(cx, cy, half):
    return ((cx - half, cy - half), (cx + half, cy + half))


def support_extrusion(gcode):
    return sum(gcode.extruded(feature=f) for f in gcode.features() if "Support" in f or "support" in f)


def test_baseline_is_a_plain_cube(baseline):
    assert 100 <= len(baseline.layers) <= 101 and baseline.extruded("Sparse infill") > 0
    assert support_extrusion(baseline) == 0 and baseline.tools <= {0}


def test_object_override_wall_loops(ec, slicer, baseline):
    obj = cube(ec)
    ec.tool("settings_set", scope={"object": obj["id"]}, values={"wall_loops": 6})
    try:
        g = slicer("object_walls")
    finally:
        undo(ec)
    assert g.extruded("Inner wall") > 2 * baseline.extruded("Inner wall")


def test_modifier_with_full_infill(ec, slicer, baseline):
    obj = cube(ec)
    cx, cy = centre(ec)
    ec.tool("modifier_add", object=obj["id"], type="modifier", shape={"primitive": "cube", "size": [8, 8, 6]},
            place={"center": [cx, cy, 10], "frame": "plate"}, settings={"sparse_infill_density": "100%"})
    try:
        g = slicer("modifier")
    finally:
        undo(ec)
    inside, zr = box(cx, cy, 2.5), (8.0, 12.0)
    assert baseline.extruded("Sparse infill", zr, inside) > 0
    assert g.extruded("Sparse infill", zr, inside) < 0.05 * baseline.extruded("Sparse infill", zr, inside)
    assert g.extruded("Internal solid infill", zr, inside) > 0
    # Outside the modifier's height the infill is unchanged.
    assert g.extruded("Sparse infill", (2.0, 6.0), inside) > 0


def test_negative_part_makes_a_hole(ec, slicer, baseline):
    obj = cube(ec)
    cx, cy = centre(ec)
    ec.tool("modifier_add", object=obj["id"], type="negative_part", shape={"primitive": "cylinder", "size": [8, 30]},
            place={"center": [cx, cy, 10], "frame": "plate"})
    try:
        g = slicer("negative")
    finally:
        undo(ec)
    hole, zr = box(cx, cy, 2.5), (3.0, 17.0)
    assert baseline.extruded(None, zr, hole) > 0
    assert g.extruded(None, zr, hole) == 0
    assert g.extruded("Inner wall", zr) + g.extruded("Outer wall", zr) > baseline.extruded("Inner wall", zr) + baseline.extruded("Outer wall", zr)


def overhang_part(client, cx, cy):
    """A 10 x 10 x 4 mm part sticking out of the cube's +X side at z 14..18."""
    obj = cube(client)
    return client.tool("modifier_add", object=obj["id"], type="part", shape={"primitive": "cube", "size": [10, 10, 4]},
                       place={"center": [cx + 14, cy, 16], "frame": "plate"})


def test_support_blocker_and_enforcer(ec, slicer):
    obj = cube(ec)
    cx, cy = centre(ec)
    overhang_part(ec, cx, cy)
    ec.tool("settings_set", scope={"object": obj["id"]}, values={"enable_support": True, "support_type": "normal(auto)"})
    try:
        supported = slicer("support_auto")
        ec.tool("modifier_add", object=obj["id"], type="support_blocker", shape={"primitive": "cube", "size": [14, 14, 15]},
                place={"center": [cx + 14, cy, 7], "frame": "plate"})
        blocked = slicer("support_blocked")
        undo(ec)
        ec.tool("settings_set", scope={"object": obj["id"]}, values={"support_type": "normal(manual)"})
        manual = slicer("support_manual")
        ec.tool("modifier_add", object=obj["id"], type="support_enforcer", shape={"primitive": "cube", "size": [12, 12, 4]},
                place={"center": [cx + 14, cy, 14], "frame": "plate"})
        enforced = slicer("support_enforced")
        undo(ec, 2)
    finally:
        undo(ec, 2)
    assert support_extrusion(supported) > 0
    assert support_extrusion(blocked) < 0.1 * support_extrusion(supported)
    assert support_extrusion(manual) == 0
    assert support_extrusion(enforced) > 0


def test_height_range_with_finer_layers(ec, slicer, baseline):
    obj = cube(ec)
    ec.tool("layer_range", action="add", object=obj["id"], z_min=0, z_max=5, values={"layer_height": 0.1})
    try:
        g = slicer("height_range")
    finally:
        undo(ec)
    low = [layer for layer in g.layers if layer.z <= 5.0 + 1e-6]
    assert len(low) >= 2 * len([layer for layer in baseline.layers if layer.z <= 5.0 + 1e-6]) - 2
    assert any(abs(layer.height - 0.1) < 1e-6 for layer in low)
    assert all(abs(layer.height - 0.1) < 0.011 for layer in low[1:])
    assert len(g.layers) >= len(baseline.layers) + 20


def test_filament_assignment_changes_tool(ec, slicer):
    cx, cy = centre(ec)
    added = ec.tool("filament_edit", action="add", color="#FF0000")
    slot = added["added"]["slot"]
    body = overhang_part(ec, cx, cy)
    ec.tool("modifier_update", target=body["volume"]["id"], filament=slot)
    try:
        g = slicer("filament")
    finally:
        undo(ec, 2)
        ec.tool("filament_edit", action="remove", slot=slot)
    assert slot - 1 in g.tools and 0 in g.tools
