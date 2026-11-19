"""Edit group, plates: plate_manage (add, delete, duplicate, rename, lock, unlock, select, move)."""

from _edit_common import (add_cube, fresh, assert_undo_roundtrip, close, get_object, instance_of, make_fixtures, normalized,
                          scene, undo)

edit_app, emodern, elegacy, ec = make_fixtures("edit_plates")


def plates_of(result) -> list:
    return [(p["plate"], p["name"], p["locked"], p["instances"]) for p in result["plates"]]


def test_add_plates_keeps_current_and_undo(ec):
    before = normalized(scene(ec))
    added = ec.tool("plate_manage", action="add", count=2)
    assert added["added"] == [2, 3] and len(added["plates"]) == 3
    assert [p["current"] for p in added["plates"]] == [True, False, False]
    assert added["undo"] == "MCP: add plate"
    assert_undo_roundtrip(ec, before)


def test_rename_and_lock(ec):
    ec.tool("plate_manage", action="add")
    before = normalized(scene(ec))
    renamed = ec.tool("plate_manage", action="rename", plate=2, name="Spare parts")
    assert renamed["plates"][1]["name"] == "Spare parts"
    assert_undo_roundtrip(ec, before)
    locked = ec.tool("plate_manage", action="lock", plate="pl" + scene(ec)["plates"][0]["id"][2:])
    assert locked["plates"][0]["locked"] is True
    assert scene(ec)["plates"][0]["locked"] is True
    unlocked = ec.tool("plate_manage", action="unlock")
    assert unlocked["plates"][0]["locked"] is False
    assert ec.tool_error("plate_manage", action="rename", plate=2).code == "invalid_argument"


def test_select_plate(ec):
    ec.tool("plate_manage", action="add")
    selected = ec.tool("plate_manage", action="select", plate=2)
    assert [p["current"] for p in selected["plates"]] == [False, True]
    assert "plate 2" in selected["notes"][0]
    # Tools that default to the current plate now use plate 2.
    cube = add_cube(ec)
    assert instance_of(cube)["plate"] == 2
    ec.tool("plate_manage", action="select", plate=1)
    assert ec.tool_error("plate_manage", action="select").code == "invalid_argument"


def test_duplicate_plate_with_objects(ec):
    cube = add_cube(ec, center=[70, 80])
    before = normalized(scene(ec))
    dup = ec.tool("plate_manage", action="duplicate", plate=1)
    assert len(dup["plates"]) == 2 and dup["plates"][1]["instances"] == 1
    copy = dup["created_objects"][0]
    assert copy["id"] != cube["id"] and instance_of(copy)["plate"] == 2
    assert close(instance_of(copy)["center"], instance_of(cube)["center"], 1e-3)
    assert_undo_roundtrip(ec, before)


def test_delete_plate_with_objects(ec):
    keep = add_cube(ec)
    ec.tool("plate_manage", action="add")
    gone = add_cube(ec, plate=2)
    before = normalized(scene(ec))
    failure = ec.tool_error("plate_manage", action="delete", plate=2, objects="fail")
    assert failure.code == "invalid_argument"
    deleted = ec.tool("plate_manage", action="delete", plate=2)
    assert deleted["deleted"] == 2 and deleted["removed_objects"] == [gone["id"]] and len(deleted["plates"]) == 1
    assert get_object(ec, keep["id"])["id"] == keep["id"]
    assert scene(ec)["object_count"] == 1
    assert_undo_roundtrip(ec, before)
    fresh(ec)
    failure = ec.tool_error("plate_manage", action="delete", plate=1)
    assert failure.code == "last_plate"


def test_move_plate(ec):
    first = add_cube(ec, center=[60, 60])
    ec.tool("plate_manage", action="add")
    second = add_cube(ec, plate=2, size=(10, 10, 10))
    ids = [p["id"] for p in scene(ec)["plates"]]
    before = normalized(scene(ec))
    moved = ec.tool("plate_manage", action="move", plate=2, to=1)
    assert [p["id"] for p in moved["plates"]] == [ids[1], ids[0]]
    # Objects travel with their plate.
    assert instance_of(get_object(ec, second["id"]))["plate"] == 1
    assert instance_of(get_object(ec, first["id"]))["plate"] == 2
    assert_undo_roundtrip(ec, before)
    assert ec.tool_error("plate_manage", action="move", plate=1, to=1).code == "invalid_argument"
    assert ec.tool_error("plate_manage", action="move", plate=1, to=9).code == "invalid_argument"
    assert ec.tool_error("plate_manage", action="move", plate=1).code == "invalid_argument"


def test_plate_errors(ec):
    assert ec.tool_error("plate_manage", action="rename", plate=4, name="x").code == "stale_id"
    assert ec.tool_error("plate_manage", action="explode").code == "invalid_argument"
    assert ec.tool_error("plate_manage", action="lock", plate="pl999999").code == "stale_id"
    assert ec.tool_error("plate_manage", action="add", count=0).code == "invalid_argument"
