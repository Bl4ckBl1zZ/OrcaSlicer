"""Read-only settings tools: settings_search, settings_get, preset_list, filament_list."""

import pytest

from conftest import cube


# ---------------------------------------------------------------------------------------------
# settings_search
# ---------------------------------------------------------------------------------------------

def test_search_without_arguments_lists_categories(client):
    result = client.tool("settings_search")
    assert result["total"] > 500
    assert set(result["categories"]) == {"process", "filament", "printer", "project"}
    assert result["categories"]["process"]["Strength"] > 10
    assert "settings" not in result


def test_search_by_words(client):
    result = client.tool("settings_search", query="infill density")
    first = result["settings"][0]
    assert first["key"] == "sparse_infill_density", [s["key"] for s in result["settings"]]
    assert first["label"] == "Sparse infill density" and first["category"] == "Strength"
    assert first["type"] == "percent" and first["unit"] == "%" and first["min"] == 0 and first["max"] == 100
    assert first["default"].endswith("%") and first["mode"] == "simple" and first["owner"] == "process"
    assert first["scopes"] == ["process", "object", "part", "modifier", "layer_range"]
    assert "infill" in first["tooltip"].lower()
    # The exact key finds itself first.
    assert client.tool("settings_search", query="wall_loops")["settings"][0]["key"] == "wall_loops"


def test_search_enum_and_vector_entries(client):
    support = {s["key"]: s for s in client.tool("settings_search", query="support type")["settings"]}["support_type"]
    assert support["type"] == "enum"
    assert support["values"] == ["normal(auto)", "tree(auto)", "normal(manual)", "tree(manual)"]
    assert len(support["labels"]) == 4 and support["scopes"] == ["process", "object"]
    nozzle = {s["key"]: s for s in client.tool("settings_search", query="nozzle_diameter", owner="printer")["settings"]}["nozzle_diameter"]
    assert nozzle["type"] == "float" and nozzle["vector"] == "extruder" and nozzle["owner"] == "printer"
    temperature = {s["key"]: s for s in client.tool("settings_search", query="nozzle_temperature", owner="filament")["settings"]}["nozzle_temperature"]
    assert temperature["vector"] == "filament" and temperature["scopes"] == ["filament"]


def test_search_filters(client):
    for hit in client.tool("settings_search", owner="filament", limit=50)["settings"]:
        assert hit["owner"] == "filament"
    for hit in client.tool("settings_search", category="support", limit=50)["settings"]:
        assert "support" in hit["category"].lower()
    for hit in client.tool("settings_search", query="speed", mode="simple", limit=50)["settings"]:
        assert hit["mode"] == "simple"
    # Only what can be set on a part: nothing object-only such as support.
    model = cube(client)
    on_part = client.tool("settings_search", scope={"volume": model["volumes"][0]["id"]}, limit=100)["settings"]
    assert on_part and all("part" in hit["scopes"] for hit in on_part)
    assert "enable_support" not in [hit["key"] for hit in on_part]
    per_plate = [hit["key"] for hit in client.tool("settings_search", scope={"plate": 1}, limit=100)["settings"]]
    assert "curr_bed_type" in per_plate and "print_sequence" in per_plate and len(per_plate) <= 8


def test_search_paging_and_no_match(client):
    first = client.tool("settings_search", query="speed", limit=5)
    assert len(first["settings"]) == 5 and first["truncated"] is True and first["matches"] > 5
    second = client.tool("settings_search", query="speed", limit=5, offset=first["next"])
    assert not {s["key"] for s in first["settings"]} & {s["key"] for s in second["settings"]}
    nothing = client.tool("settings_search", query="flux capacitor")
    assert nothing["settings"] == [] and nothing["matches"] == 0 and nothing["notes"]


def test_search_marks_keys_the_agent_may_not_write(client):
    def entry(key):
        return {s["key"]: s for s in client.tool("settings_search", query=key, limit=50)["settings"]}[key]
    assert entry("post_process")["write"].startswith("refused")
    assert entry("printhost_apikey")["write"].startswith("refused")
    assert "G-code" in entry("machine_start_gcode")["write"]
    assert "write" not in entry("layer_height")


# ---------------------------------------------------------------------------------------------
# settings_get
# ---------------------------------------------------------------------------------------------

def test_get_process_values(client):
    result = client.tool("settings_get", scope="process", keys=["layer_height", "sparse_infill_density", "wall_loops", "enable_support",
                                                                  "support_type", "outer_wall_speed"])
    assert result["scope"] == "process" and result["preset"] and result["modified"] == []
    values = result["values"]
    assert isinstance(values["layer_height"], float) and 0.04 < values["layer_height"] < 1
    assert values["sparse_infill_density"].endswith("%")
    assert isinstance(values["wall_loops"], int) and isinstance(values["enable_support"], bool)
    assert values["support_type"] in ("normal(auto)", "tree(auto)", "normal(manual)", "tree(manual)")
    assert isinstance(values["outer_wall_speed"], list), "per-extruder-variant options are lists"


def test_get_without_keys_returns_modifications_only(client):
    result = client.tool("settings_get", scope="process")
    assert result["values"] == {} and result["modified"] == []
    model = cube(client)
    result = client.tool("settings_get", scope={"object": model["id"]})
    assert result["values"] == {} and result["notes"]


def test_get_printer_and_filament_values(client):
    printer = client.tool("settings_get", scope="printer", keys=["nozzle_diameter", "printable_area", "printable_height", "machine_start_gcode"])
    assert printer["values"]["nozzle_diameter"] == [0.4]
    area = printer["values"]["printable_area"]
    assert area[0] == [0.0, 0.0] and len(area) == 4 and area[2][0] > 100, "points are [x, y] pairs"
    assert "\\n" not in printer["values"]["machine_start_gcode"] and "\n" in printer["values"]["machine_start_gcode"]
    filament = client.tool("settings_get", scope={"filament": 1}, keys=["filament_type", "nozzle_temperature", "filament_max_volumetric_speed"])
    assert filament["values"]["filament_type"] == ["PLA"]
    assert 150 < filament["values"]["nozzle_temperature"][0] < 300
    project = client.tool("settings_get", scope="project", keys=["filament_colour"])
    assert project["values"]["filament_colour"][0].startswith("#")


def test_get_effective_value_and_source(client):
    model = cube(client)
    keys = ["sparse_infill_density", "layer_height", "nozzle_diameter", "filament_type", "curr_bed_type"]
    for scope in ({"object": model["id"]}, {"volume": model["volumes"][0]["id"]}):
        values = client.tool("settings_get", scope=scope, keys=keys, effective=True)["values"]
        assert values["sparse_infill_density"]["source"] == "process" and values["sparse_infill_density"]["value"].endswith("%")
        assert values["layer_height"]["source"] == "process"
        assert values["nozzle_diameter"] == {"value": [0.4], "source": "printer"}
        assert values["filament_type"] == {"value": ["PLA"], "source": "filament"}
        assert values["curr_bed_type"]["source"] in ("project", "plate:1")
    # At the scope itself nothing is set.
    stored = client.tool("settings_get", scope={"object": model["id"]}, keys=["sparse_infill_density"])["values"]
    assert stored == {"sparse_infill_density": None}


def test_get_errors(client):
    model = cube(client)
    failure = client.tool_error("settings_get", scope="process", keys=["sparse_infil_density"])
    assert failure.code == "unknown_key" and "sparse_infill_density" in failure.details["nearest"]
    assert "sparse_infill_density" in failure.hint
    failure = client.tool_error("settings_get", scope="process", keys=["nozzle_temperature"])
    assert failure.code == "key_not_in_scope" and failure.details["scopes"] == ["filament"]
    failure = client.tool_error("settings_get", scope={"volume": model["volumes"][0]["id"]}, keys=["enable_support"])
    assert failure.code == "key_not_in_scope" and "object" in failure.details["scopes"] and "effective" in failure.hint
    assert client.tool_error("settings_get", scope={"object": "o999999"}).code == "stale_id"
    assert client.tool_error("settings_get", scope={"filament": 9}).code == "invalid_argument"
    assert client.tool_error("settings_get", scope={"plate": 5}).code == "stale_id"
    failure = client.tool_error("settings_get", scope={"layer_range": {"object": model["id"], "z_min": 0, "z_max": 5}})
    assert failure.code == "stale_id" and failure.details["ranges"] == []
    for bad in ("everything", {"object": model["id"], "plate": 1}, {"part": model["volumes"][0]["id"]}, 5):
        assert client.tool_error("settings_get", scope=bad).code == "invalid_argument", bad
    assert client.tool_error("settings_get").code == "invalid_argument"


# ---------------------------------------------------------------------------------------------
# preset_list, filament_list
# ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("kind", ["process", "printer", "filament"])
def test_preset_list(client, kind):
    result = client.tool("preset_list", type=kind)
    assert result["type"] == kind and result["selected"]
    names = [preset["name"] for preset in result["presets"]]
    assert len(names) == len(set(names)) and result["total"] >= len(names) >= 1
    selected = [preset for preset in result["presets"] if preset.get("selected")]
    assert [preset["name"] for preset in selected] == [result["selected"]]
    assert all(isinstance(preset["system"], bool) and preset["compatible"] is True for preset in result["presets"])
    if kind == "filament":
        used = [preset for preset in result["presets"] if preset.get("slots")]
        assert used and used[0]["slots"] == [1] and used[0]["filament_type"]


def test_preset_list_query_paging_and_compatibility(client):
    status = client.tool("app_status")
    assert client.tool("preset_list", type="process")["selected"] == status["process"]["preset"]
    assert client.tool("preset_list", type="printer")["selected"] == status["printer"]["preset"]
    filtered = client.tool("preset_list", type="process", query="0.20")
    assert filtered["presets"] and all("0.20" in preset["name"] for preset in filtered["presets"])
    page = client.tool("preset_list", type="filament", limit=2)
    if page["total"] > 2:
        assert page["truncated"] is True and len(page["presets"]) == 2
        following = client.tool("preset_list", type="filament", limit=2, offset=page["next"])
        assert following["presets"][0]["name"] != page["presets"][0]["name"]
    everything = client.tool("preset_list", type="filament", compatible_only=False, limit=200)
    assert everything["total"] >= client.tool("preset_list", type="filament", limit=200)["total"]
    assert client.tool_error("preset_list").code == "invalid_argument"
    assert client.tool_error("preset_list", type="nozzle").code == "invalid_argument"


def test_filament_list(client):
    model = cube(client)
    filaments = client.tool("filament_list")["filaments"]
    assert filaments[0]["slot"] == 1 and filaments[0]["preset"] and filaments[0]["type"] and filaments[0]["color"].startswith("#")
    assert filaments[0]["used_by"] == {"objects": [model["id"]], "parts": [], "paint": []}
    assert filaments == [dict(entry, used_by=entry["used_by"]) for entry in filaments]
    assert [entry["slot"] for entry in filaments] == list(range(1, len(filaments) + 1))
