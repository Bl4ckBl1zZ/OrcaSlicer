"""End-to-end scenario of the settings group, in its own instance:

global process settings -> per-object override -> modifier box with denser infill -> support blocker ->
height range with finer layers -> two filament colours, one part on the second filament -> save 3MF ->
slice it -> load it back -> every setting is still there. Screenshots of the bodies are kept in out/.
"""

import pytest

from conftest import REPO_ROOT, SAMPLE_MODEL
from gcode_stats import slice_3mf
from orca_mcp import McpClient
from settings_testutil import approx, box_size


@pytest.fixture(scope="module")
def e2e_app(launch_app):
    return launch_app("e2e", files=[str(REPO_ROOT / "tests" / "data" / SAMPLE_MODEL)])


@pytest.fixture(scope="module")
def c(e2e_app):
    return McpClient(e2e_app.url, e2e_app.token, era="modern").connect()


def the_object(client):
    return client.tool("scene_get")["plates"][0]["objects"][0]


def shoot(client, out_dir, name, **arguments):
    result = client.call("screenshot", arguments)
    assert not result.is_error, result.data
    mime, data = result.images[0]
    path = out_dir / name
    path.write_bytes(data)
    return path


def test_scenario(c, e2e_app, out_dir, run_dir):
    # 1. Global process settings.
    c.tool("settings_set", scope="process", values={"layer_height": 0.2, "wall_loops": 3, "sparse_infill_density": "20%"})
    obj = the_object(c)
    oid = obj["id"]
    cx, cy = obj["instances"][0]["center"]

    # 2. Per-object override.
    c.tool("settings_set", scope={"object": oid}, values={"sparse_infill_density": "30%", "wall_loops": 4})

    # 3. A modifier box with denser infill around the middle.
    mod = c.tool("modifier_add", object=oid, type="modifier", shape={"primitive": "cube", "size": [10, 10, 6]},
                 place={"center": [cx, cy, 10], "frame": "plate"}, name="dense core",
                 settings={"sparse_infill_density": "80%"})
    assert approx(mod["placement"]["center_plate"], [cx, cy, 10]) and approx(box_size(mod["placement"]["bbox_world"]), [10, 10, 6])

    # 4. A support blocker at one corner (supports on for the object).
    c.tool("settings_set", scope={"object": oid}, values={"enable_support": True})
    blocker = c.tool("modifier_add", object=oid, type="support_blocker", shape={"primitive": "cube", "size": [6, 6, 6]},
                     place={"center_rel": [1.0, 1.0, 0.5]}, name="no support corner")

    # 5. A height range with finer layers.
    c.tool("layer_range", action="add", object=oid, z_min=0, z_max=4, values={"layer_height": 0.12})

    # 6. Two filament colours; a top cap part on the second filament.
    c.tool("filament_edit", action="set_color", filament=1, color="#FFFFFF")
    second = c.tool("filament_edit", action="add", color="#D02020")["added"]["slot"]
    cap = c.tool("modifier_add", object=oid, type="part", shape={"primitive": "cube", "size": [12, 12, 3]},
                 place={"center": [cx, cy, 21.5], "frame": "plate"}, name="red cap", filament=second)

    shoot(c, out_dir, "e2e_iso_type.png", editor={"color_by": "type"})
    shoot(c, out_dir, "e2e_iso_filament.png")
    shoot(c, out_dir, "e2e_top.png", camera={"preset": "top", "projection": "orthographic"}, editor={"color_by": "type"})

    # 7. Save, slice the saved file.
    workspace = e2e_app.data_dir / "mcp" / "workspace"
    workspace.mkdir(parents=True, exist_ok=True)
    project = workspace / "e2e.3mf"
    c.tool("project_save", path=str(project), copy=True, overwrite=True)
    g = slice_3mf(project, run_dir / "cli" / "e2e")
    assert 0 in g.tools and second - 1 in g.tools, "the cap prints with the second filament"
    assert any(abs(layer.height - 0.12) < 1e-6 for layer in g.layers if layer.z <= 4)
    inside = ((cx - 3, cy - 3), (cx + 3, cy + 3))
    assert g.extruded("Internal solid infill", (8.5, 11.5), inside) + g.extruded("Sparse infill", (8.5, 11.5), inside) > 0

    before = the_object(c)

    # 8. Load it back: a new project, new ids, same content.
    c.tool("project_open", path=str(project), on_unsaved="discard")
    after = the_object(c)
    assert after["overrides"] == before["overrides"] == {"sparse_infill_density": "30%", "wall_loops": 4, "enable_support": True}
    def by_name(o):
        return {v["name"]: v for v in o["volumes"]}
    vb, va = by_name(before), by_name(after)
    assert set(va) == set(vb)
    for name in ("dense core", "no support corner", "red cap"):
        assert va[name]["type"] == vb[name]["type"]
        assert va[name]["overrides"] == vb[name]["overrides"]
        assert va[name]["filament"] == vb[name]["filament"]
        assert approx(va[name]["center"], vb[name]["center"], 0.01) and approx(va[name]["size"], vb[name]["size"], 0.01)
    assert va["dense core"]["overrides"] == {"sparse_infill_density": "80%"}
    assert va["red cap"]["filament"] == second
    assert after["layer_ranges"] == before["layer_ranges"]
    assert after["layer_ranges"][0]["overrides"]["layer_height"] == 0.12
    filaments = c.tool("filament_list")["filaments"]
    assert [f["color"].upper() for f in filaments] == ["#FFFFFF", "#D02020"]
    process = c.tool("settings_get", scope="process", keys=["wall_loops", "sparse_infill_density", "layer_height"])["values"]
    assert process == {"wall_loops": 3, "sparse_infill_density": "20%", "layer_height": 0.2}
    shoot(c, out_dir, "e2e_after_reload.png", editor={"color_by": "type"})
    assert blocker["volume"]["type"] == "support_blocker" and cap["volume"]["filament"] == second
