"""Filaments: filament_edit (add, remove, set_color, auto_flush), checked through filament_list; assignment through object_update /
modifier_update. Runs in its own instance (slot changes renumber everything)."""

import pytest

from conftest import REPO_ROOT, SAMPLE_MODEL, cube
from orca_mcp import McpClient
from settings_testutil import revert_journal, undo


@pytest.fixture(scope="module")
def filaments_app(launch_app):
    return launch_app("filaments", files=[str(REPO_ROOT / "tests" / "data" / SAMPLE_MODEL)])


@pytest.fixture(scope="module", params=["modern", "legacy"])
def fc(request, filaments_app):
    return McpClient(filaments_app.url, filaments_app.token, era=request.param).connect()


def slots(client):
    return client.tool("filament_list")["filaments"]


def test_usage_lists_where_a_slot_is_used(fc):
    listing = slots(fc)
    assert listing[0]["slot"] == 1 and listing[0]["color"].startswith("#")
    obj = cube(fc)
    assert obj["id"] in listing[0]["used_by"]["objects"]


def test_add_colour_assign_remove(fc):
    count = len(slots(fc))
    added = fc.tool("filament_edit", action="add", color="#ff0000")
    assert added["added"]["slot"] == count + 1 and added["added"]["color"] == "#FF0000"
    assert len(added["filaments"]) == count + 1 and added["journal"]
    scene = fc.tool("scene_get")
    assert scene["filaments"][-1]["color"] == "#FF0000"
    slot = count + 1

    recoloured = fc.tool("filament_edit", action="set_color", slot=slot, color="#00ff00")
    assert recoloured["changed"]["color"] == "#00FF00"
    revert_journal(fc, recoloured["journal"])
    assert slots(fc)[-1]["color"] == "#FF0000"

    # Assign the cube to the new slot; removing it now needs a replacement.
    obj = cube(fc)
    fc.tool("object_update", targets=[obj["id"]], filament=slot)
    assert obj["id"] in slots(fc)[-1]["used_by"]["objects"]
    in_use = fc.tool_error("filament_edit", action="remove", slot=slot)
    assert in_use.code == "in_use" and obj["id"] in in_use.details["used_by"]["objects"]
    removed = fc.tool("filament_edit", action="remove", slot=slot, replace_with=1)
    assert removed["removed"]["slot"] == slot and len(removed["filaments"]) == count
    assert cube(fc)["filament"] == 1
    # The removal is not revertible; the assignment undo is still on the undo stack.
    undo(fc)


def test_add_with_preset_and_revert(fc):
    count = len(slots(fc))
    preset = slots(fc)[0]["preset"]
    added = fc.tool("filament_edit", action="add", preset=preset)
    assert added["added"]["preset"] == preset
    revert_journal(fc, added["journal"])
    assert len(slots(fc)) == count


def test_part_assignment_and_paint_free_removal(fc):
    added = fc.tool("filament_edit", action="add", color="#0000FF")
    slot = added["added"]["slot"]
    obj = cube(fc)
    body = fc.tool("modifier_add", object=obj["id"], type="part", shape={"primitive": "cube", "size": [5, 5, 5]},
                   place={"center_rel": [0.5, 0.5, 1.0]}, filament=slot)
    vid = body["volume"]["id"]
    assert body["volume"]["filament"] == slot
    assert vid in slots(fc)[slot - 1]["used_by"]["parts"]
    undo(fc)
    fc.tool("filament_edit", action="remove", slot=slot)
    assert len(slots(fc)) == slot - 1


def test_errors(fc):
    assert fc.tool_error("filament_edit", action="remove", slot=1).code in ("last_filament", "in_use")
    assert fc.tool_error("filament_edit", action="set_color", slot=1).code == "invalid_argument"
    assert fc.tool_error("filament_edit", action="set_color", slot=1, color="red").code == "invalid_argument"
    assert fc.tool_error("filament_edit", action="set_color", slot=7, color="#123456").code == "invalid_argument"
    assert fc.tool_error("filament_edit", action="add", preset="no such filament").code == "unknown_preset"
    assert fc.tool_error("filament_edit", action="add", slot=2).code == "invalid_argument"


def test_auto_flush(fc):
    added = fc.tool("filament_edit", action="add", color="#FFFFFF")
    try:
        flushed = fc.tool("filament_edit", action="auto_flush")
        assert flushed["flush_volumes_matrix"] and flushed["journal"]
        revert_journal(fc, flushed["journal"])
    finally:
        fc.tool("filament_edit", action="remove", slot=added["added"]["slot"])


def test_colour_reaches_the_renderer(fc):
    added = fc.tool("filament_edit", action="add", color="#123456")
    slot = added["added"]["slot"]
    try:
        ui = fc.tool("debug_settings_ui")
        assert ui["render_colors"][slot - 1].upper() == "#123456"
        assert len(ui["sidebar_filaments"]) == slot
        fc.tool("filament_edit", action="set_color", slot=slot, color="#654321")
        assert fc.tool("debug_settings_ui")["render_colors"][slot - 1].upper() == "#654321"
    finally:
        fc.tool("filament_edit", action="remove", slot=slot)
    assert len(fc.tool("debug_settings_ui")["sidebar_filaments"]) == slot - 1
