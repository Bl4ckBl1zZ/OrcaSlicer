"""Presets: preset_select (with each on_unsaved policy), preset_save, preset_delete, preset_rename, preset_diff.

Runs in its own instance: it creates and deletes user presets and switches presets.
"""

import pytest

from conftest import REPO_ROOT, SAMPLE_MODEL
from orca_mcp import McpClient
from settings_testutil import revert_journal


@pytest.fixture(scope="module")
def presets_app(launch_app):
    return launch_app("presets", files=[str(REPO_ROOT / "tests" / "data" / SAMPLE_MODEL)])


@pytest.fixture(scope="module", params=["modern", "legacy"])
def pc(request, presets_app):
    return McpClient(presets_app.url, presets_app.token, era=request.param).connect()


def offered(client, kind, slot=None):
    args = dict(type=kind, name="no such preset at all")
    if slot:
        args["filament"] = slot
    failure = client.tool_error("preset_select", **args)
    assert failure.code == "unknown_preset"
    return failure.details["offered"]


def selected(client, kind):
    status = client.tool("app_status")
    return status["process"]["preset"] if kind == "process" else status["printer"]["preset"]


def other_process(client):
    current = selected(client, "process")
    names = [p["name"] for p in client.tool("preset_list", type="process", limit=200)["presets"] if p["name"] != current]
    assert names, "the fixture printer offers only one process preset"
    return current, names[0]


def test_select_process_journal_and_revert(pc):
    current, other = other_process(pc)
    result = pc.tool("preset_select", type="process", name=other)
    assert result["changed"] is True and result["before"] == current and result["selected"] != current
    assert result["journal"] and result["scene"]["process"] == result["selected"]
    assert pc.tool("preset_select", type="process", name=other)["changed"] is False
    revert_journal(pc, result["journal"])
    assert selected(pc, "process") == current


def test_unknown_and_incompatible(pc):
    failure = pc.tool_error("preset_select", type="process", name="definitely not a preset")
    assert failure.code == "unknown_preset" and failure.details["offered"]
    assert pc.tool_error("preset_select", type="filament", name="x").code == "invalid_argument"  # no slot
    assert pc.tool_error("preset_select", type="process", name="x", filament=1).code == "invalid_argument"
    assert pc.tool_error("preset_select", type="process", name="x", save_as="y").code == "invalid_argument"


def test_unsaved_fail_discard_carry_over_save(pc):
    current, other = other_process(pc)
    edit = pc.tool("settings_set", scope="process", values={"wall_loops": 7})
    # fail (default): nothing changes, the modified keys are listed.
    failure = pc.tool_error("preset_select", type="process", name=other)
    assert failure.code == "unsaved_changes" and failure.details["process"]["modified"] == {"wall_loops": 7}
    assert selected(pc, "process") == current

    # carry_over: the edit moves to the new preset.
    carried = pc.tool("preset_select", type="process", name=other, on_unsaved="carry_over")
    assert "wall_loops" in carried["carried_over"] and carried["discarded"]["process"] == {"wall_loops": 7}
    diff = pc.tool("preset_diff", type="process")
    assert any(d["key"] == "wall_loops" and d["a"] == 7 for d in diff["differences"])

    # discard: back to the first preset, dropping the edit.
    discarded = pc.tool("preset_select", type="process", name=current, on_unsaved="discard")
    assert discarded["discarded"]["process"]["wall_loops"] == 7
    assert pc.tool("preset_diff", type="process")["count"] == 0

    # save: the edit is kept in a new user preset, then the switch happens.
    pc.tool("settings_set", scope="process", values={"wall_loops": 6})
    name = f"MCP test process {pc.era}"
    saved = pc.tool("preset_select", type="process", name=other, on_unsaved="save", save_as=name)
    assert saved["saved_as"] == name
    assert name in offered(pc, "process")
    pc.tool("preset_select", type="process", name=current)
    deleted = pc.tool("preset_delete", type="process", name=name)
    assert deleted["deleted"] == name and name not in offered(pc, "process")
    assert edit["journal"]


def test_save_discard_rename_delete(pc):
    current = selected(pc, "process")
    assert pc.tool_error("preset_save", type="process", action="discard").code == "nothing_modified"
    assert pc.tool_error("preset_save", type="process").code in ("system_preset", "nothing_modified")
    pc.tool("settings_set", scope="process", values={"sparse_infill_density": "33%"})
    assert pc.tool_error("preset_save", type="process").code == "system_preset"
    assert pc.tool_error("preset_save", type="process", name=current).code == "name_exists"
    discarded = pc.tool("preset_save", type="process", action="discard")
    assert discarded["discarded"] == {"sparse_infill_density": "33%"}
    # history revert re-applies the discarded edit.
    revert_journal(pc, discarded["journal"])
    assert pc.tool("preset_diff", type="process")["differences"][0]["a"] == "33%"

    name = f"MCP saved {pc.era}"
    saved = pc.tool("preset_save", type="process", name=name)
    assert saved["name"] == name and saved["saved_keys"] == ["sparse_infill_density"]
    assert selected(pc, "process") == name
    assert pc.tool_error("preset_save", type="process").code == "nothing_modified"
    # overwrite rules
    pc.tool("settings_set", scope="process", values={"sparse_infill_density": "34%"})
    assert pc.tool("preset_save", type="process")["replaced"] is True  # own user preset: overwrite in place

    renamed_to = name + " renamed"
    renamed = pc.tool("preset_rename", type="process", name=name, new_name=renamed_to)
    assert renamed["renamed"] == {"from": name, "to": renamed_to} and selected(pc, "process") == renamed_to
    revert_journal(pc, renamed["journal"])
    assert selected(pc, "process") == name
    assert pc.tool_error("preset_rename", type="process", name=current, new_name="x").code == "system_preset"
    assert pc.tool_error("preset_delete", type="process", name=current).code == "system_preset"

    deleted = pc.tool("preset_delete", type="process", name=name)
    assert deleted["deleted"] == name and selected(pc, "process") != name
    pc.tool("preset_select", type="process", name=current, on_unsaved="discard")


def test_diff_two_presets(pc):
    current, other = other_process(pc)
    diff = pc.tool("preset_diff", type="process", a=current, b=other)
    assert diff["count"] > 0 and all({"key", "a", "b"} <= set(d) for d in diff["differences"])
    assert pc.tool_error("preset_diff", type="process", a="nope").code == "unknown_preset"


def test_printer_select_through_selector(pc):
    printer = selected(pc, "printer")
    pc.tool("settings_set", scope="printer", values={"retraction_length": 0.9})
    name = f"MCP printer {pc.era}"
    pc.tool("preset_save", type="printer", name=name)
    try:
        assert selected(pc, "printer") == name
        back = pc.tool("preset_select", type="printer", name=printer)
        assert back["selected"] == printer and back["scene"]["printer"] == printer
        # Printer edits are not carried over, as in the GUI.
        pc.tool("settings_set", scope="printer", values={"retraction_length": 1.1})
        assert pc.tool_error("preset_select", type="printer", name=name, on_unsaved="carry_over").code == "invalid_argument"
        switched = pc.tool("preset_select", type="printer", name=name, on_unsaved="discard")
        assert switched["discarded"]["printer"]["retraction_length"] == [1.1]
        pc.tool("preset_select", type="printer", name=printer)
    finally:
        pc.tool("preset_delete", type="printer", name=name, on_unsaved="discard")
    assert selected(pc, "printer") == printer


def test_filament_slot_select(pc):
    names = [p["name"] for p in pc.tool("preset_list", type="filament", limit=200)["presets"]]
    status = pc.tool("app_status")
    current = status["filaments"][0]["preset"]
    other = next((n for n in names if n != current), None)
    if other is None:
        pytest.skip("only one filament preset offered")
    result = pc.tool("preset_select", type="filament", filament=1, name=other)
    assert result["filament"] == 1 and result["filaments"][0]["preset"] != current
    revert_journal(pc, result["journal"])
    assert pc.tool("app_status")["filaments"][0]["preset"] == current
