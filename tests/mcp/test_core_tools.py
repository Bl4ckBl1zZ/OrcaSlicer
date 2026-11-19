"""The tools of the core group: app_status, scene_get, object_update, history; argument validation."""

import pytest

from conftest import SAMPLE_MODEL, cube, objects_by_name


# ---------------------------------------------------------------------------------------------
# app_status
# ---------------------------------------------------------------------------------------------

def test_app_status(client):
    status = client.tool("app_status")
    assert status["app"] == "OrcaSlicer" and status["technology"] == "FFF"
    assert status["printer"]["preset"] and status["printer"]["extruders"] >= 1
    assert len(status["printer"]["bed"]) == 2 and status["printer"]["height"] > 0
    assert isinstance(status["printer"]["bed_type"], str) and status["printer"]["bed_type"]
    assert status["process"]["preset"]
    assert status["filaments"][0]["slot"] == 1 and status["filaments"][0]["color"].startswith("#")
    assert status["busy"] == {"slicing": False, "jobs": [], "modal_dialog": None, "gizmo": None, "blocks_changes": None}
    assert any(root.endswith("/mcp/workspace") for root in status["write_roots"])
    assert set(status["write_roots"]) <= set(status["read_roots"])
    assert "mm" in status["conventions"]["units"]
    assert status["server"]["debug_tools"] is True and status["server"]["tools"] >= 5


# ---------------------------------------------------------------------------------------------
# scene_get
# ---------------------------------------------------------------------------------------------

def test_scene_normal(client):
    scene = client.tool("scene_get")
    assert scene["project"]["rev"] >= 0 and "dirty" in scene["project"]
    plate = scene["plates"][0]
    assert plate["plate"] == 1 and plate["id"].startswith("pl") and plate["current"] is True and plate["sliced"] is False
    model = cube(client)
    assert model["printable"] is True and model["filament"] == 1
    assert model["size"] == [20.0, 20.0, 20.0]
    assert model["paint"] == {"support": False, "seam": False, "color": False, "fuzzy_skin": False}
    instance = model["instances"][0]
    assert instance["id"].startswith("i") and instance["plate"] == 1 and instance["z_min"] == 0.0
    assert instance["rotation"] == [0.0, 0.0, 0.0] and instance["scale"] == [1.0, 1.0, 1.0]
    # Positions are in the plate frame: the box is 20 mm wide around the centre and sits on the bed.
    box = instance["bbox"]
    assert [round(box["max"][i] - box["min"][i], 3) for i in range(3)] == [20.0, 20.0, 20.0]
    assert round((box["min"][0] + box["max"][0]) / 2, 3) == instance["center"][0]
    bed = scene["printer"]["bed"]
    assert 0 < instance["center"][0] < bed[0] and 0 < instance["center"][1] < bed[1]
    assert instance["outside_plate"] is False
    volume = model["volumes"][0]
    assert volume["id"].startswith("v") and volume["type"] == "part" and volume["facets"] == 12
    assert volume["size"] == [20.0, 20.0, 20.0] and volume["overrides"] == {}


def test_scene_detail_levels(client):
    summary = client.tool("scene_get", detail="summary")["plates"][0]["objects"][0]
    assert summary["instances"] == 1 and summary["parts"] == 1 and summary["modifiers"] == 0
    assert "volumes" not in summary
    full = client.tool("scene_get", detail="full")["plates"][0]["objects"][0]
    volume = full["volumes"][0]
    assert len(volume["matrix"]) == 4 and len(volume["matrix"][0]) == 4
    assert volume["mesh"]["facets"] == 12 and volume["mesh"]["open_edges"] == 0
    assert round(volume["mesh"]["volume_mm3"]) == 8000
    assert len(full["instances"][0]["matrix_world"]) == 4


def test_scene_filters_and_paging(client):
    model = cube(client)
    by_plate = client.tool("scene_get", plate=1)
    assert len(by_plate["plates"]) == 1 and "off_plate" not in by_plate
    assert client.tool("scene_get", plate="current")["plates"][0]["plate"] == 1
    assert client.tool("scene_get", plate=by_plate["plates"][0]["id"])["plates"][0]["plate"] == 1
    # A volume id selects its object.
    by_volume = client.tool("scene_get", objects=[model["volumes"][0]["id"]])
    assert by_volume["plates"][0]["objects"][0]["id"] == model["id"]
    paged = client.tool("scene_get", offset=1)
    assert paged["plates"][0]["objects"] == [] and paged["object_count"] == 1


def test_stale_and_wrong_ids(client):
    model = cube(client)
    failure = client.tool_error("scene_get", objects=["o999999"])
    assert failure.code == "stale_id" and "scene_get" in failure.hint
    assert client.tool_error("scene_get", plate=99).code == "stale_id"
    assert client.tool_error("scene_get", plate="pl999999").code == "stale_id"
    # The prefix says what kind of thing an id is; a mismatch is reported, not silently accepted.
    number = model["id"][1:]
    failure = client.tool_error("object_update", targets=["v" + number], name="x")
    assert failure.code == "invalid_argument" and "prefix" in failure.message


# ---------------------------------------------------------------------------------------------
# Argument validation (JSON Schema) - answered without touching the main thread
# ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("arguments, fragments", [
    ({"detail": "everything"}, ["`detail`", "summary", "normal", "full"]),
    ({"detial": "full"}, ["Unknown argument `detial`", "Did you mean `detail`?"]),
    ({"limit": 0}, ["`limit`", ">= 1"]),
    ({"limit": "ten"}, ["`limit`", "integer"]),
    ({"objects": ["cube"]}, ["`objects[0]`", "o<N>"]),
    ({"objects": "o1"}, ["`objects`", "array"]),
    ({"plate": 0}, ["`plate`"]),
    ({"plate": "first"}, ["`plate`"]),
])
def test_schema_violations_are_tool_errors_with_guidance(client, arguments, fragments):
    failure = client.tool_error("scene_get", **arguments)
    assert failure.code == "invalid_argument"
    for fragment in fragments:
        assert fragment in failure.message, f"{fragment!r} not in {failure.message!r}"
    assert "scene_get" in failure.hint


def test_missing_required_argument(client):
    failure = client.tool_error("object_update", name="x")
    assert failure.code == "invalid_argument" and "Missing required argument `targets`" in failure.message


def test_arguments_must_be_an_object(client):
    message, headers = client.build("tools/call", {"name": "scene_get", "arguments": [1, 2]})
    reply = client.post(message, headers)
    assert reply.json()["error"]["code"] == -32602


# ---------------------------------------------------------------------------------------------
# object_update and history
# ---------------------------------------------------------------------------------------------

def test_object_update_validation(modern):
    model = cube(modern)
    assert modern.tool_error("object_update", targets=[model["id"]]).code == "invalid_argument"
    failure = modern.tool_error("object_update", targets=[model["id"]], filament=9)
    assert failure.code == "invalid_argument" and "filament slot 9" in failure.message
    failure = modern.tool_error("object_update", targets=[model["instances"][0]["id"]], name="x")
    assert failure.code == "invalid_argument" and "instance" in failure.message
    failure = modern.tool_error("object_update", targets=[model["volumes"][0]["id"]], printable=False)
    assert failure.code == "invalid_argument"
    assert modern.tool_error("object_update", targets=[model["id"]], filament=0).code == "invalid_argument"
    # Nothing was changed and no undo step was added by the failed calls.
    assert cube(modern) == model


def test_object_update_rename_printable_filament_and_undo(modern):
    model = cube(modern)
    history = modern.tool("history")

    renamed = modern.tool("object_update", targets=[model["id"]], name="calibration cube")
    assert renamed["undo"] == "MCP: update " + model["id"]
    changed = renamed["changed"][0]
    assert changed["id"] == model["id"], "ids survive the change"
    assert changed["name"] == "calibration cube" and changed["volumes"][0]["name"] == "calibration cube"
    assert renamed["slice_invalidated"] is True and renamed["rev"] != history["rev"]

    hidden = modern.tool("object_update", targets=[model["instances"][0]["id"]], printable=False)
    assert hidden["changed"][0]["instances"][0]["printable"] is False

    part = modern.tool("object_update", targets=[model["volumes"][0]["id"]], filament=1, name="body")
    assert part["changed"][0]["volumes"][0]["filament"] == 1 and part["changed"][0]["volumes"][0]["name"] == "body"

    steps = modern.tool("history")["undo"]
    assert steps[:3] == [part["undo"], hidden["undo"], renamed["undo"]]

    # Undo all three: the scene is exactly what it was, under the same ids.
    undone = modern.tool("history", action="undo", steps=3)
    assert undone["undone"] == [part["undo"], hidden["undo"], renamed["undo"]]
    assert cube(modern) == model
    assert modern.tool("history")["rev"] != part["rev"]

    # Redo one, then undo it again.
    redone = modern.tool("history", action="redo")
    assert redone["redone"] == [renamed["undo"]]
    assert "calibration cube" in objects_by_name(modern)
    modern.tool("history", action="undo")
    assert SAMPLE_MODEL in objects_by_name(modern)


def test_history_nothing_to_redo(modern):
    listing = modern.tool("history")
    if not listing["redo"]:
        assert modern.tool_error("history", action="redo").code == "nothing_to_redo"
