"""settings_set at every scope: process, printer, filament slot, project, plate, object, part, modifier, layer range.

Runs on the shared instance; preset changes are reverted through the journal, model changes by undo.
"""

import pytest

from conftest import cube
from settings_testutil import revert_journal, scene_signature, undo, volume_of


# ---------------------------------------------------------------------------------------------
# preset scopes
# ---------------------------------------------------------------------------------------------

def test_process_scope_batch_journal_and_revert(client):
    current = client.tool("settings_get", scope="process", keys=["wall_loops"])["values"]
    loops = 6 if current["wall_loops"] != 6 else 5
    result = client.tool("settings_set", scope="process", values={"layer_height": 0.16, "wall_loops": loops, "sparse_infill_density": "27%"})
    try:
        changes = {c["key"]: c for c in result["changed"]}
        assert changes["layer_height"]["new"] == 0.16 and changes["wall_loops"]["new"] == loops
        assert changes["sparse_infill_density"]["new"] == "27%"
        assert result["modified"] is True and result["journal"]
        scene = client.tool("scene_get")
        assert {"layer_height", "wall_loops", "sparse_infill_density"} <= set(scene["process"]["modified"])
    finally:
        # The journal reverts the edit through the same path.
        revert_journal(client, result["journal"])
    scene = client.tool("scene_get")
    for key in ("layer_height", "wall_loops", "sparse_infill_density"):
        assert key not in scene["process"]["modified"]


def test_process_reset_goes_back_to_saved_value(modern):
    first = modern.tool("settings_set", scope="process", values={"wall_loops": 5})
    old = first["changed"][0]["old"]
    reset = modern.tool("settings_set", scope="process", reset=["wall_loops"])
    assert reset["changed"][0]["new"] == old
    assert "wall_loops" not in modern.tool("scene_get")["process"]["modified"]


def test_dependent_options_are_side_effects(modern):
    # Spiral vase mode makes the app propose to adjust walls, top layers, supports ...
    result = modern.tool("settings_set", scope="process", values={"spiral_mode": True}, auto_adjust=True)
    try:
        keys = {c["key"] for c in result["side_effects"]}
        assert keys, result
        assert result.get("dialogs") is not None or keys
    finally:
        revert_journal(modern, result["journal"])
    assert "spiral_mode" not in modern.tool("scene_get")["process"]["modified"]


def test_printer_and_filament_scopes(modern):
    printer = modern.tool("settings_set", scope="printer", values={"retraction_length": 0.7})
    try:
        change = printer["changed"][0]
        assert change["key"] == "retraction_length" and change["new"] == [0.7]
    finally:
        revert_journal(modern, printer["journal"])
    filament = modern.tool("settings_set", scope={"filament": 1}, values={"nozzle_temperature": 212})
    try:
        assert filament["changed"][0]["new"] == [212]
        assert filament["slots_sharing_preset"] == [1]
    finally:
        revert_journal(modern, filament["journal"])


@pytest.mark.parametrize("scope,values,code,fragment", [
    ("process", {"wall_lops": 2}, "unknown_key", "wall_loops"),
    ("process", {"wall_loops": "three"}, "invalid_value", "integer"),
    ("process", {"wall_loops": 2.5}, "invalid_value", "integer"),
    ("process", {"layer_height": -1}, "out_of_range", None),
    ("process", {"sparse_infill_pattern": "honeycombish"}, "invalid_value", "gyroid"),
    ("process", {"enable_support": "yes"}, "invalid_value", "true or false"),
    ("process", {"post_process": "evil.exe"}, "forbidden_key", None),
    ("printer", {"printhost_apikey": "x"}, "forbidden_key", None),
    ("printer", {"machine_start_gcode": "G28"}, "forbidden_key", "G-code"),
    ("printer", {"wall_loops": 2}, "key_not_in_scope", "process"),
    ("process", {"nozzle_temperature": 200}, "key_not_in_scope", "filament"),
    ({"filament": 9}, {"nozzle_temperature": 200}, "invalid_argument", "slot"),
    ("project", {"wall_loops": 2}, "key_not_in_scope", None),
])
def test_validation_errors(client, scope, values, code, fragment):
    failure = client.tool_error("settings_set", scope=scope, values=values)
    assert failure.code == code, failure.payload
    if fragment:
        assert fragment in failure.message + " " + failure.hint + " " + str(failure.details), failure.payload


def test_batch_is_atomic(client):
    before = client.tool("scene_get")["process"]["modified"]
    failure = client.tool_error("settings_set", scope="process", values={"wall_loops": 4, "sparse_infill_density": "lots"})
    assert failure.code == "invalid_value"
    assert client.tool("scene_get")["process"]["modified"] == before


def test_nothing_to_do_and_duplicates(client):
    assert client.tool_error("settings_set", scope="process", values={}).code == "invalid_argument"
    assert client.tool_error("settings_set", scope="process", values={"wall_loops": 2}, reset=["wall_loops"]).code == "invalid_argument"


# ---------------------------------------------------------------------------------------------
# model scopes
# ---------------------------------------------------------------------------------------------

def test_object_scope_override_and_undo(client):
    before = scene_signature(client)
    obj = cube(client)
    result = client.tool("settings_set", scope={"object": obj["id"]}, values={"sparse_infill_density": "40%", "layer_height": 0.12})
    assert result["undo"].startswith("MCP: settings of")
    assert result["object"]["overrides"] == {"sparse_infill_density": "40%", "layer_height": 0.12}
    assert {c["key"]: c["old"] for c in result["changed"]} == {"sparse_infill_density": None, "layer_height": None}
    reset = client.tool("settings_set", scope={"object": obj["id"]}, reset=["layer_height"])
    assert reset["object"]["overrides"] == {"sparse_infill_density": "40%"}
    undo(client, 2)
    assert scene_signature(client) == before


def test_object_scope_rejects_printer_and_filament_keys(client):
    obj = cube(client)
    failure = client.tool_error("settings_set", scope={"object": obj["id"]}, values={"nozzle_diameter": [0.6]})
    assert failure.code == "key_not_in_scope" and "printer" in failure.details["scopes"]


def test_part_and_modifier_scopes(modern):
    obj = cube(modern)
    part = obj["volumes"][0]["id"]
    result = modern.tool("settings_set", scope={"volume": part}, values={"wall_loops": 6})
    try:
        assert volume_of(result["object"], part)["overrides"] == {"wall_loops": 6}
        # layer_height cannot be set on a part; the hint points at layer_range.
        failure = modern.tool_error("settings_set", scope={"volume": part}, values={"layer_height": 0.1})
        assert failure.code == "key_not_in_scope" and "layer_range" in failure.hint
    finally:
        undo(modern)
    blocker = modern.tool("modifier_add", object=obj["id"], type="support_blocker", shape={"primitive": "cube", "size": [3, 3, 3]})
    try:
        failure = modern.tool_error("settings_set", scope={"volume": blocker["volume"]["id"]}, values={"wall_loops": 2})
        assert failure.code == "invalid_argument" and "support_blocker" in failure.message
    finally:
        undo(modern)
    modifier = modern.tool("modifier_add", object=obj["id"], type="modifier", shape={"primitive": "cube", "size": [5, 5, 5]})
    try:
        vid = modifier["volume"]["id"]
        set_ = modern.tool("settings_set", scope={"volume": vid}, values={"sparse_infill_density": "100%"})
        assert volume_of(set_["object"], vid)["overrides"] == {"sparse_infill_density": "100%"}
        undo(modern)
        assert volume_of(cube(modern), vid)["overrides"] == {}
    finally:
        undo(modern)


def test_layer_range_scope(modern):
    obj = cube(modern)
    modern.tool("layer_range", action="add", object=obj["id"], z_min=0, z_max=4)
    try:
        result = modern.tool("settings_set", scope={"layer_range": {"object": obj["id"], "z_min": 0, "z_max": 4}},
                             values={"layer_height": 0.08, "sparse_infill_density": "60%"})
        overrides = result["object"]["layer_ranges"][0]["overrides"]
        assert overrides["layer_height"] == 0.08 and overrides["sparse_infill_density"] == "60%"
        assert modern.tool_error("settings_set", scope={"layer_range": {"object": obj["id"], "z_min": 1, "z_max": 4}},
                                 values={"wall_loops": 2}).code == "invalid_argument"
        assert modern.tool_error("settings_set", scope={"layer_range": {"object": obj["id"], "z_min": 0, "z_max": 4}},
                                 reset=["layer_height"]).code == "invalid_argument"
        undo(modern)
    finally:
        undo(modern)


def test_plate_scope(modern):
    before = scene_signature(modern)
    result = modern.tool("settings_set", scope={"plate": 1}, values={"print_sequence": "by object", "spiral_mode": False})
    assert result["plate"]["overrides"].get("print_sequence") == "by object"
    reset = modern.tool("settings_set", scope={"plate": 1}, reset=["print_sequence"])
    assert "print_sequence" not in reset["plate"]["overrides"]
    assert modern.tool_error("settings_set", scope={"plate": 1}, values={"wall_loops": 2}).code == "key_not_in_scope"
    undo(modern, 2)
    assert scene_signature(modern) == before
