"""Modifier bodies and height ranges: modifier_add, modifier_update, layer_range (bodies are removed with `delete`).

Runs on the shared instance (the 20 mm cube); every test undoes what it changed.
"""

import pytest

from conftest import REPO_ROOT, cube
from settings_testutil import approx, box_center, box_size, scene_signature, undo, volume_of


# ---------------------------------------------------------------------------------------------
# modifier_add
# ---------------------------------------------------------------------------------------------

def test_add_modifier_box_top_centre_with_settings_and_undo(client):
    before = scene_signature(client)
    obj = cube(client)
    inst = obj["instances"][0]
    result = client.tool("modifier_add", object=obj["id"], type="modifier",
                         shape={"primitive": "cube", "size": [10, 10, 10]},
                         place={"center_rel": [0.5, 0.5, 1.0]}, name="dense top",
                         settings={"sparse_infill_density": "80%", "wall_loops": 4})
    assert result["ok"] is True and result["undo"].startswith("MCP: add modifier")
    assert result["slice_invalidated"] is True
    volume = result["volume"]
    assert volume["type"] == "modifier" and volume["name"] == "dense top"
    assert volume["overrides"] == {"sparse_infill_density": "80%", "wall_loops": 4}
    assert volume["size"] == [10.0, 10.0, 10.0]
    assert approx(volume["center_rel"], [0.5, 0.5, 1.0])
    placement = result["placement"]
    # Plate frame: centred over the cube, centre at the cube's top (z = 20).
    assert approx(placement["center_plate"][:2], inst["center"]) and approx(placement["center_plate"][2], 20.0)
    assert approx(box_size(placement["bbox_world"]), [10, 10, 10])
    assert placement["plate"] == 1 and placement["placed_by"] == "center_rel"
    # The object shows the new volume, sorted after the part.
    changed = result["changed"]
    assert [v["type"] for v in changed["volumes"]] == ["part", "modifier"]
    # One undo restores the scene; redo brings the same volume back.
    undo(client)
    assert scene_signature(client) == before
    client.tool("history", action="redo")
    assert volume_of(cube(client), volume["id"])["overrides"] == {"sparse_infill_density": "80%", "wall_loops": 4}
    undo(client)
    assert scene_signature(client) == before


@pytest.mark.parametrize("shape,size", [
    ({"primitive": "cube", "size": [6]}, [6, 6, 6]),
    ({"primitive": "cube", "size": [4, 6, 8]}, [4, 6, 8]),
    ({"primitive": "cylinder", "size": [8, 6]}, [8, 8, 6]),
    ({"primitive": "disc", "size": [10, 0.6]}, [10, 10, 0.6]),
    ({"primitive": "cone", "size": [8, 6]}, [8, 8, 6]),
    ({"primitive": "sphere", "size": [7]}, [7, 7, 7]),
    ({"primitive": "torus", "size": [6, 2]}, [10, 10, 2]),
    ({"primitive": "slab"}, [30, 30, 10]),
])
def test_primitives_have_the_requested_size(modern, shape, size):
    obj = cube(modern)
    result = modern.tool("modifier_add", object=obj["id"], type="modifier", shape=shape)
    try:
        assert approx(box_size(result["placement"]["bbox_world"]), size, tol=0.15)
        if shape["primitive"] == "slab":
            # The slab sits on the bed like the menu's.
            assert approx(result["placement"]["bbox_plate"]["min"][2], 0.0)
    finally:
        undo(modern)


@pytest.mark.parametrize("kind", ["part", "negative_part", "modifier", "support_blocker", "support_enforcer",
                                  "precise_seam_center", "precise_seam_left", "precise_seam_right",
                                  "precise_seam_enforced", "precise_seam_blocked", "precise_seam_neutral"])
def test_every_volume_type(modern, kind):
    obj = cube(modern)
    result = modern.tool("modifier_add", object=obj["id"], type=kind, shape={"primitive": "cube", "size": [5, 5, 5]},
                         place={"center_rel": [1.0, 0.5, 0.5]})
    try:
        assert result["volume"]["type"] == kind
        assert any(v["id"] == result["volume"]["id"] and v["type"] == kind for v in result["changed"]["volumes"])
        # Default extruder: a part takes the object's filament, other bodies inherit (0).
        assert result["volume"]["filament"] == (1 if kind == "part" else 0)
    finally:
        undo(modern)


def test_frames_object_plate_world(modern):
    obj = cube(modern)
    inst = obj["instances"][0]
    plate_point = [inst["center"][0] + 4, inst["center"][1] - 3, 15.0]
    by_plate = modern.tool("modifier_add", object=obj["id"], type="modifier", shape={"primitive": "sphere", "size": [4]},
                           place={"center": plate_point, "frame": "plate"})
    try:
        placement = by_plate["placement"]
        assert approx(placement["center_plate"], plate_point)
        # Object frame: the volume's centre as scene_get reports it, which the same call reproduces.
        object_point = by_plate["volume"]["center"]
        world_point = box_center(placement["bbox_world"])
    finally:
        undo(modern)
    by_object = modern.tool("modifier_add", object=obj["id"], type="modifier", shape={"primitive": "sphere", "size": [4]},
                            place={"center": object_point, "frame": "object"})
    try:
        assert approx(by_object["placement"]["center_plate"], plate_point)
    finally:
        undo(modern)
    by_world = modern.tool("modifier_add", object=obj["id"], type="modifier", shape={"primitive": "sphere", "size": [4]},
                           place={"center": world_point, "frame": "world"})
    try:
        assert approx(by_world["placement"]["center_plate"], plate_point)
    finally:
        undo(modern)


def test_rotation_about_bed_axes(modern):
    obj = cube(modern)
    result = modern.tool("modifier_add", object=obj["id"], type="modifier", shape={"primitive": "cube", "size": [10, 4, 2]},
                         place={"center_rel": [0.5, 0.5, 0.5], "rotation": [0, 0, 90]})
    try:
        assert approx(box_size(result["placement"]["bbox_world"]), [4, 10, 2])
        assert approx(result["volume"]["rotation"], [0, 0, 90], tol=0.01)
    finally:
        undo(modern)


def test_fit_object_xy_with_margin(modern):
    obj = cube(modern)
    result = modern.tool("modifier_add", object=obj["id"], type="modifier", shape={"primitive": "cube", "size": [1, 1, 3]},
                         place={"fit": "object_xy", "margin": 1, "center_rel": [0.5, 0.5, 0.25]})
    try:
        assert approx(box_size(result["placement"]["bbox_world"]), [22, 22, 3])
        assert approx(result["placement"]["center_plate"][2], 5.0)
    finally:
        undo(modern)


def test_mesh_file_body(modern):
    obj = cube(modern)
    path = str(REPO_ROOT / "tests" / "data" / "pyramid.obj")
    result = modern.tool("modifier_add", object=obj["id"], type="negative_part", shape={"file": path, "scale": 0.1},
                         place={"center_rel": [0.5, 0.5, 0.5]})
    try:
        assert result["volume"]["type"] == "negative_part" and result["volume"]["name"] == "pyramid.obj"
        assert result["volume"]["facets"] > 4
    finally:
        undo(modern)


def test_precise_seam_outside_object_is_noted(modern):
    obj = cube(modern)
    result = modern.tool("modifier_add", object=obj["id"], type="precise_seam_center", shape={"primitive": "cube", "size": [2, 2, 2]},
                         place={"center_rel": [3.0, 3.0, 0.5]})
    try:
        assert any("no effect" in note for note in result.get("notes", []))
    finally:
        undo(modern)


def test_modifier_add_errors(client):
    obj = cube(client)
    volume_id = obj["volumes"][0]["id"]
    rev = client.tool("history")["rev"]
    cases = [
        (dict(object="o999999", type="modifier", shape={"primitive": "cube"}), "stale_id", None),
        (dict(object=volume_id, type="modifier", shape={"primitive": "cube"}), "invalid_argument", "volume"),
        (dict(object=obj["id"], type="negative_part", shape={"primitive": "cube"}, settings={"wall_loops": 3}), "invalid_argument", "parts and modifiers"),
        (dict(object=obj["id"], type="modifier", shape={"primitive": "cube"}, settings={"wall_lops": 3}), "unknown_key", "wall_loops"),
        (dict(object=obj["id"], type="modifier", shape={"primitive": "cube"}, settings={"layer_height": 0.1}), "key_not_in_scope", "layer_range"),
        (dict(object=obj["id"], type="modifier", shape={"primitive": "cube"}, settings={"sparse_infill_density": "lots"}), "invalid_value", None),
        (dict(object=obj["id"], type="modifier", shape={"primitive": "cube"}, settings={"wall_loops": -2}), "out_of_range", None),
        (dict(object=obj["id"], type="modifier", shape={"primitive": "cube"}, settings={"sparse_infill_pattern": "zigzagx"}), "invalid_value", "gyroid"),
        (dict(object=obj["id"], type="modifier", shape={"primitive": "sphere"}, place={"fit": "object_xy"}), "invalid_argument", "cube and slab"),
        (dict(object=obj["id"], type="modifier", shape={"primitive": "cylinder", "size": [1, 2, 3]}), "invalid_argument", "diameter"),
        (dict(object=obj["id"], type="modifier", shape={"primitive": "cube"}, place={"center": [0, 0, 0], "center_rel": [0, 0, 0]}), "invalid_argument", "not both"),
        (dict(object=obj["id"], type="modifier", shape={"file": "C:/Windows/win.ini"}), "path_denied", None),
        (dict(object=obj["id"], type="modifier", shape={"primitive": "cube"}, filament=9), "invalid_argument", "slot"),
    ]
    for arguments, code, fragment in cases:
        failure = client.tool_error("modifier_add", **arguments)
        assert failure.code == code, (arguments, failure.payload)
        if fragment:
            assert fragment in (failure.message + " " + str(failure.hint) + " " + str(failure.details)), failure.payload
    # Nothing was changed by the failures.
    assert client.tool("history")["rev"] == rev


def test_key_not_in_scope_lists_the_scopes(client):
    obj = cube(client)
    failure = client.tool_error("modifier_add", object=obj["id"], type="modifier", shape={"primitive": "cube"},
                                settings={"layer_height": 0.1})
    assert set(failure.details["scopes"]) >= {"process", "object", "layer_range"}
    assert "layer_range" in failure.hint


# ---------------------------------------------------------------------------------------------
# modifier_update / delete
# ---------------------------------------------------------------------------------------------

def test_update_type_name_filament_and_pose(modern):
    obj = cube(modern)
    added = modern.tool("modifier_add", object=obj["id"], type="modifier", shape={"primitive": "cube", "size": [4, 4, 4]},
                        place={"center_rel": [0.5, 0.5, 0.5]})
    vid = added["volume"]["id"]
    try:
        retyped = modern.tool("modifier_update", target=vid, type="support_blocker", name="no support here")
        assert retyped["volume"]["type"] == "support_blocker" and retyped["volume"]["name"] == "no support here"
        assert retyped["undo"].startswith("MCP: update")
        inst = obj["instances"][0]
        moved = modern.tool("modifier_update", target=vid, place={"center": [inst["center"][0], inst["center"][1], 2], "frame": "plate"},
                            size=[12, 6, 4])
        assert approx(moved["placement"]["center_plate"], [inst["center"][0], inst["center"][1], 2])
        assert approx(box_size(moved["placement"]["bbox_world"]), [12, 6, 4])
        turned = modern.tool("modifier_update", target=vid, place={"rotation": [0, 0, 90]})
        assert approx(box_size(turned["placement"]["bbox_world"]), [6, 12, 4])
        # The centre stays where it was when only the rotation changes.
        assert approx(turned["placement"]["center_plate"], moved["placement"]["center_plate"])
        undo(modern, 3)
        back = volume_of(cube(modern), vid)
        assert back["type"] == "modifier" and back["size"] == [4.0, 4.0, 4.0]
    finally:
        undo(modern)


def test_update_last_part_and_validation(client):
    obj = cube(client)
    part = obj["volumes"][0]["id"]
    assert client.tool_error("modifier_update", target=part, type="modifier").code == "last_part"
    assert client.tool_error("modifier_update", target=part).code == "invalid_argument"
    assert client.tool_error("modifier_update", target=obj["id"], name="x").code == "invalid_argument"
    assert client.tool_error("modifier_update", target=part, size=[1, 1, 1], scale=[1, 1, 1]).code == "invalid_argument"


def test_delete_body_and_last_part(modern):
    obj = cube(modern)
    added = modern.tool("modifier_add", object=obj["id"], type="support_enforcer", shape={"primitive": "cube", "size": [3, 3, 3]})
    vid = added["volume"]["id"]
    try:
        # Bodies are deleted with the general `delete` tool (modifier_delete was folded into it).
        assert modern.tool_error("delete", targets=[obj["volumes"][0]["id"]]).code == "last_part"
        assert "modifier_delete" not in {t["name"] for t in modern.list_tools()}
        deleted = modern.tool("delete", targets=[vid])
        assert deleted["removed"] == [vid]
        assert len(cube(modern)["volumes"]) == 1
        undo(modern)
        assert any(v["id"] == vid for v in cube(modern)["volumes"])
    finally:
        undo(modern)


# ---------------------------------------------------------------------------------------------
# layer_range
# ---------------------------------------------------------------------------------------------

def test_layer_range_lifecycle_and_undo(client):
    before = scene_signature(client)
    obj = cube(client)
    assert client.tool("layer_range", object=obj["id"])["layer_ranges"] == []
    added = client.tool("layer_range", action="add", object=obj["id"], z_min=0, z_max=5, values={"layer_height": 0.1})
    assert added["layer_ranges"] == [{"z_min": 0.0, "z_max": 5.0, "overrides": {"layer_height": 0.1}}]
    assert added["changed"]["layer_ranges"][0]["z_max"] == 5.0
    second = client.tool("layer_range", action="add", object=obj["id"], z_min=5, z_max=10,
                         values={"sparse_infill_density": "50%"})
    assert len(second["layer_ranges"]) == 2
    # The default layer height of a new range is the process's.
    assert second["layer_ranges"][1]["overrides"]["layer_height"] > 0
    updated = client.tool("layer_range", action="update", object=obj["id"], z_min=5, z_max=10, new_z_max=12,
                          values={"wall_loops": 5}, reset=["sparse_infill_density"])
    assert updated["layer_ranges"][1]["z_max"] == 12.0
    assert updated["layer_ranges"][1]["overrides"].get("wall_loops") == 5
    assert "sparse_infill_density" not in updated["layer_ranges"][1]["overrides"]
    deleted = client.tool("layer_range", action="delete", object=obj["id"], z_min=0, z_max=5)
    assert len(deleted["layer_ranges"]) == 1
    undo(client, 4)
    assert scene_signature(client) == before


def test_layer_range_errors(client):
    obj = cube(client)
    client.tool("layer_range", action="add", object=obj["id"], z_min=2, z_max=6, values={"layer_height": 0.12})
    try:
        overlap = client.tool_error("layer_range", action="add", object=obj["id"], z_min=5, z_max=8)
        assert overlap.code == "overlap" and overlap.details["conflicts_with"] == {"z_min": 2.0, "z_max": 6.0}
        # Touching is fine; checked through update of a missing range instead of adding.
        assert client.tool_error("layer_range", action="update", object=obj["id"], z_min=1, z_max=2,
                                 values={"wall_loops": 2}).code == "invalid_argument"
        assert client.tool_error("layer_range", action="add", object=obj["id"], z_min=7, z_max=6).code == "invalid_argument"
        assert client.tool_error("layer_range", action="add", object=obj["id"], z_min=25, z_max=30).code == "invalid_argument"
        assert client.tool_error("layer_range", action="add", object=obj["id"], z_min=10, z_max=12,
                                 values={"layer_height": 5}).code == "out_of_range"
        assert client.tool_error("layer_range", action="add", object=obj["id"], z_min=10, z_max=12,
                                 values={"enable_support": True}).code == "key_not_in_scope"
        assert client.tool_error("layer_range", action="update", object=obj["id"], z_min=2, z_max=6,
                                 reset=["layer_height"]).code == "invalid_argument"
        assert client.tool_error("layer_range", action="add", object=obj["id"]).code == "invalid_argument"
    finally:
        undo(client)
    assert cube(client)["layer_ranges"] == []


# ---------------------------------------------------------------------------------------------
# What the window shows
# ---------------------------------------------------------------------------------------------

def list_rows(client, name):
    for row in client.tool("debug_settings_ui")["object_list"]:
        if row["name"] == name:
            return row
    raise KeyError(name)


def kinds(row):
    return [child["type"] for child in row.get("children", [])]


def test_object_list_shows_bodies_settings_and_ranges(modern):
    obj = cube(modern)
    assert "volume" not in kinds(list_rows(modern, obj["name"]))
    added = modern.tool("modifier_add", object=obj["id"], type="modifier", shape={"primitive": "cube", "size": [5, 5, 5]},
                        name="list check", settings={"sparse_infill_density": "90%"})
    try:
        row = list_rows(modern, obj["name"])
        volumes = [c for c in row["children"] if c["type"] == "volume"]
        assert [v["name"] for v in volumes][-1] == "list check" and len(volumes) == 2
        # Overrides show as the row's settings icon (Orca draws no separate settings row).
        assert volumes[-1]["overrides_icon"] is True and volumes[0]["overrides_icon"] is False
        assert list_rows(modern, obj["name"])["overrides_icon"] is False
        modern.tool("settings_set", scope={"object": obj["id"]}, values={"sparse_infill_density": "35%"})
        assert list_rows(modern, obj["name"])["overrides_icon"] is True
        modern.tool("layer_range", action="add", object=obj["id"], z_min=0, z_max=3)
        layers = [c for c in list_rows(modern, obj["name"])["children"] if c["type"] == "layers"]
        assert layers and len(layers[0]["children"]) == 1
    finally:
        undo(modern, 3)
    row = list_rows(modern, obj["name"])
    assert row["overrides_icon"] is False and "layers" not in kinds(row) and "volume" not in kinds(row)


# ---------------------------------------------------------------------------------------------
# Screenshots (kept in out/ for a look by eye)
# ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("label,shape,place", [
    ("cube_top_centre", {"primitive": "cube", "size": [8, 8, 8]}, {"center_rel": [0.5, 0.5, 1.0]}),
    ("cylinder_plate_right", {"primitive": "cylinder", "size": [6, 24]}, {"center": "plate_right", "frame": "plate"}),
    ("sphere_world_front", {"primitive": "sphere", "size": [10]}, {"center": "world_front", "frame": "world"}),
    ("cone_object_above_centre", {"primitive": "cone", "size": [12, 10]}, {"center": [0, 0, 10], "frame": "object"}),
    ("torus_rotated", {"primitive": "torus", "size": [16, 4]}, {"center_rel": [0.5, 0.5, 0.5], "rotation": [90, 0, 0]}),
    ("slab_default", {"primitive": "slab"}, None),
])
def test_screenshot_of_placement(modern, out_dir, label, shape, place):
    obj = cube(modern)
    cx, cy = obj["instances"][0]["center"]
    if place and place.get("center") == "plate_right":
        place = {"center": [cx + 10, cy, 12], "frame": "plate"}
    if place and place.get("center") == "world_front":
        place = {"center": [cx, cy - 10, 10], "frame": "world"}  # plate 1 has its origin at the world origin
    arguments = dict(object=obj["id"], type="modifier", shape=shape)
    if place:
        arguments["place"] = place
    result = modern.tool("modifier_add", **arguments)
    try:
        shot = modern.call("screenshot", {"editor": {"color_by": "type"}, "camera": {"preset": "iso", "target": [obj["id"]], "zoom": 0.8}})
        assert not shot.is_error, shot.data
        (out_dir / f"modifier_{label}.png").write_bytes(shot.images[0][1])
        (out_dir / f"modifier_{label}.json").write_text(str(result["placement"]), encoding="utf-8")
    finally:
        undo(modern)
