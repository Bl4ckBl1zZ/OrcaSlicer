"""Hardening: what a project file (3MF) may bring in, names written into the G-code, and numbers under a
locale with a decimal comma.

Projects are crafted from ones the instance saves itself: the archive is rewritten with the change under
test (a preset value in Metadata/project_settings.config, a name in the model, plate custom G-code).

Two instances of their own: one with the user's "Allow the agent to edit G-code settings" preference
(needed to put G-code into the start G-code and plate events), one in German (decimal comma).
"""

import json
import re
import zipfile
from html import escape
from pathlib import Path

import pytest

from orca_mcp import McpClient

PROJECT_SETTINGS = "Metadata/project_settings.config"
MODEL_SETTINGS = "Metadata/model_settings.config"
MAIN_MODEL = "3D/3dmodel.model"


@pytest.fixture(scope="module")
def hd_app(launch_app):
    return launch_app("hardening", app_config={"mcp_allow_gcode_edit": "true"})


@pytest.fixture(scope="module")
def hd(hd_app) -> McpClient:
    return McpClient(hd_app.url, hd_app.token, era="modern", timeout=300).connect()


def workspace(c: McpClient) -> Path:
    roots = c.tool("app_status")["write_roots"]
    return Path(next(r for r in roots if r.endswith("/mcp/workspace")))


def add_cube(c: McpClient) -> str:
    return c.tool("object_add_primitive", shape="cube", size=[20, 20, 20])["created"][0]["id"]


def saved_project(c: McpClient, name: str) -> Path:
    """A fresh project with one cube, saved on the instance's current presets."""
    c.tool("project_new", on_unsaved="discard")
    add_cube(c)
    path = workspace(c) / f"{name}.3mf"
    c.tool("export_file", kind="3mf", path=str(path), overwrite=True)
    return path


def rewrite(source: Path, target: Path, edit) -> Path:
    """Copies the archive `source` to `target`; `edit(name, text) -> text` may change each text entry
    (None drops it)."""
    with zipfile.ZipFile(source) as zin, zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as zout:
        for info in zin.infolist():
            data = zin.read(info.filename)
            if info.filename.endswith((".config", ".model", ".xml", ".rels")):
                changed = edit(info.filename, data.decode("utf-8"))
                if changed is None:
                    continue
                data = changed.encode("utf-8")
            zout.writestr(info, data)
    return target


def set_settings(settings: dict, process: dict | None = None, printer: dict | None = None) -> dict:
    """Sets values of the process / printer preset in a project's settings and lists them as changed from
    the system preset (different_settings_to_system: process first, then each filament, printer last);
    a project on system presets takes only the values that list names."""
    different = list(settings.get("different_settings_to_system") or [])
    slots = len(settings.get("filament_settings_id") or []) + 2
    different += [""] * (slots - len(different))
    for index, values in ((0, process or {}), (slots - 1, printer or {})):
        settings.update(values)
        keys = [k for k in different[index].split(";") if k]
        different[index] = ";".join(keys + [k for k in values if k not in keys])
    settings["different_settings_to_system"] = different
    return settings


def with_settings(source: Path, target: Path, process: dict | None = None, printer: dict | None = None) -> Path:
    """`source` with values of its process / printer preset changed."""
    def edit(name, text):
        if name != PROJECT_SETTINGS:
            return text
        return json.dumps(set_settings(json.loads(text), process, printer), indent=4)
    return rewrite(source, target, edit)


def discard_edits(c: McpClient):
    """Back to the selected presets as saved."""
    c.tool("project_new", on_unsaved="discard")
    for kind in ("process", "printer"):
        c.tool("preset_select", type=kind, name=c.tool("preset_list", type=kind)["selected"], on_unsaved="discard")


def object_names(c: McpClient) -> list[str]:
    scene = c.tool("scene_get")
    return [o["name"] for plate in scene["plates"] for o in plate["objects"]]


def value(c: McpClient, scope, key: str):
    return c.tool("settings_get", scope=scope, keys=[key])["values"][key]


# ---------------------------------------------------------------------------------------------
# G-code a project brings: refused unless trusted
# ---------------------------------------------------------------------------------------------

def test_foreign_start_gcode_refused(hd):
    """The application's own question relies on a list the file provides; the G-code is compared too."""
    base = saved_project(hd, "guard_base")
    crafted = with_settings(base, base.with_name("guard_start_gcode.3mf"),
                            printer={"machine_start_gcode": "G28\nM104 S300 ; MCP-FOREIGN-START"})
    failure = hd.tool_error("project_open", path=str(crafted), on_unsaved="discard")
    assert failure.code == "untrusted_gcode", failure
    settings = failure.details["settings"]
    assert any(s["key"] == "machine_start_gcode" and s["scope"] == "printer" for s in settings), settings
    # The load was undone.
    assert hd.tool("scene_get")["object_count"] == 0
    assert "MCP-FOREIGN-START" not in value(hd, "printer", "machine_start_gcode")
    # Trusted (the instance has the user's preference): opens with the file's G-code.
    hd.tool("project_open", path=str(crafted), trust_embedded_gcode=True, on_unsaved="discard")
    assert "MCP-FOREIGN-START" in value(hd, "printer", "machine_start_gcode")
    discard_edits(hd)


def test_unchanged_project_opens(hd):
    """A project saved on the user's own presets carries no foreign G-code."""
    base = saved_project(hd, "guard_plain")
    result = hd.tool("project_open", path=str(base), on_unsaved="discard")
    assert len(result["changed"]["objects"]) == 1


def test_plate_custom_gcode_refused(hd):
    hd.tool("project_new", on_unsaved="discard")
    add_cube(hd)
    hd.tool("plate_layer_events", action="add", at={"layer": 5}, type="custom", gcode="M117 MCP-PLATE-EVENT")
    path = workspace(hd) / "guard_plate_event.3mf"
    hd.tool("export_file", kind="3mf", path=str(path), overwrite=True)
    failure = hd.tool_error("project_open", path=str(path), on_unsaved="discard")
    assert failure.code == "untrusted_gcode", failure
    assert any(s["scope"] == "plate" and s["key"] == "custom_gcode" for s in failure.details["settings"]), failure.details
    assert hd.tool("scene_get")["object_count"] == 0
    hd.tool("project_open", path=str(path), trust_embedded_gcode=True, on_unsaved="discard")
    assert hd.tool("scene_get")["object_count"] == 1


# ---------------------------------------------------------------------------------------------
# Scripts, plug-ins and the printer's connection: never taken from a project
# ---------------------------------------------------------------------------------------------

def test_scripts_plugins_and_network_reset(hd):
    base = saved_project(hd, "guard_unsafe_base")
    host_type_before = value(hd, "printer", "host_type")
    crafted = with_settings(base, base.with_name("guard_unsafe.3mf"),
                            process={"post_process": ["C:/mcp-test/evil.bat"], "slicing_pipeline_plugin": ["mcp-test-plugin"]},
                            printer={"print_host": "192.0.2.99", "host_type": "prusalink", "printer_agent": "mcp-test-agent"})
    result = hd.tool("project_open", path=str(crafted), on_unsaved="discard")
    assert len(result["changed"]["objects"]) == 1
    notes = " ".join(result.get("notes", []))
    # print_host and the other credentials are dropped by the application's own load already.
    for key in ("post_process", "slicing_pipeline_plugin", "host_type", "printer_agent"):
        assert key in notes, notes

    def assert_clean():
        assert value(hd, "process", "post_process") in ([], [""]), value(hd, "process", "post_process")
        assert value(hd, "process", "slicing_pipeline_plugin") in ([], [""])
        assert value(hd, "printer", "print_host") == ""  # redacted values read "" when not set
        assert value(hd, "printer", "host_type") == host_type_before
        assert value(hd, "printer", "printer_agent") != "mcp-test-agent"

    assert_clean()
    # Discarding the edits must not bring them back (the presets the file brought were cleaned too).
    for kind in ("process", "printer"):
        name = hd.tool("preset_list", type=kind)["selected"]
        hd.tool("preset_select", type=kind, name=name, on_unsaved="discard")
    assert_clean()
    # Nothing to refuse when slicing: the scripts are gone.
    assert hd.tool("slice", wait_s=280)["status"] == "finished"
    discard_edits(hd)


def test_published_project_scripts_reset(hd):
    """A "published" project overlays the author's process values on the user's presets, without the
    application's G-code question: scripts it lists are put back too, printer G-code is not taken."""
    base = saved_project(hd, "guard_published_base")
    payload = "post_process = C:/mcp-test/evil.bat\nmachine_start_gcode = M104 S300 ; MCP-PUBLISHED\n"
    keys = json.dumps(["post_process", "machine_start_gcode"])

    def edit(name, text):
        if name == PROJECT_SETTINGS:
            return None  # a published file carries its values in the model's metadata instead
        if name == MAIN_MODEL:
            meta = (f'\n <metadata name="orca_published">1</metadata>'
                    f'\n <metadata name="orca_published_keys">{escape(keys)}</metadata>'
                    f'\n <metadata name="orca_published_config">{escape(payload)}</metadata>')
            return re.sub(r"(<model\b[^>]*>)", lambda m: m.group(1) + meta, text, count=1)
        return text

    crafted = rewrite(base, base.with_name("guard_published.3mf"), edit)
    result = hd.call("project_open", {"path": str(crafted), "on_unsaved": "discard"})
    if result.is_error:
        assert result.error_code == "untrusted_gcode", result.data
    else:
        assert value(hd, "process", "post_process") in ([], [""]), result.data
        assert "MCP-PUBLISHED" not in value(hd, "printer", "machine_start_gcode")
    discard_edits(hd)


def test_import_strips_object_settings(hd):
    """model_import of a 3MF never brings per-object code, scripts or printer connection settings."""
    hd.tool("project_new", on_unsaved="discard")
    oid = add_cube(hd)
    hd.tool("settings_set", scope={"object": oid}, values={"wall_loops": 4})
    base = workspace(hd) / "guard_import_base.3mf"
    hd.tool("export_file", kind="3mf", path=str(base), overwrite=True)

    def edit(name, text):
        if name != MODEL_SETTINGS:
            return text
        extra = ('<metadata key="post_process" value="C:/mcp-test/evil.bat"/>'
                 '<metadata key="print_host" value="192.0.2.98"/>'
                 '<metadata key="filament_start_gcode" value="M104 S300 ; MCP-OBJECT-CODE"/>')
        changed = re.sub(r'(<object id="\d+"[^>]*>)', lambda m: m.group(1) + extra, text, count=1)
        assert changed != text
        return changed

    crafted = rewrite(base, base.with_name("guard_import.3mf"), edit)
    hd.tool("project_new", on_unsaved="discard")
    result = hd.tool("model_import", paths=[str(crafted)])
    # The file's object settings were loaded by the application, then removed.
    notes = " ".join(result.get("notes", []))
    for key in ("post_process", "print_host", "filament_start_gcode"):
        assert key in notes, notes
    oid = result["imported"][0]["id"]
    stored = hd.tool("settings_get", scope={"object": oid})["values"]
    for key in ("post_process", "print_host", "filament_start_gcode"):
        assert key not in stored, stored


# ---------------------------------------------------------------------------------------------
# Names from a project in the G-code: never a line of their own
# ---------------------------------------------------------------------------------------------

def test_names_from_a_project_stay_comments(hd):
    base = saved_project(hd, "names_base")
    model_name = "Model&#10;G28 ; MODEL-INJECTED"
    object_name = "Cube&#10;G28 ; OBJECT-INJECTED"
    plate_name = "Plate&#13;&#10;M84 ; PLATE-INJECTED"

    def edit(name, text):
        if name == MAIN_MODEL:
            if '<metadata name="Title">' in text:
                return re.sub(r'<metadata name="Title">[^<]*</metadata>', f'<metadata name="Title">{model_name}</metadata>', text, count=1)
            return re.sub(r"(<model\b[^>]*>)", rf'\1\n <metadata name="Title">{model_name}</metadata>', text, count=1)
        if name == MODEL_SETTINGS:
            text = re.sub(r'(<object id="\d+"[^>]*>\s*<metadata key="name" value=")[^"]*"', rf'\1{object_name}"', text, count=1)
            if 'key="plater_name"' in text:
                return re.sub(r'<metadata key="plater_name" value="[^"]*"', f'<metadata key="plater_name" value="{plate_name}"', text, count=1)
            return re.sub(r'(<plate>\s*<metadata key="plater_id" value="1"/>)',
                          rf'\1\n    <metadata key="plater_name" value="{plate_name}"/>', text, count=1)
        return text

    crafted = rewrite(base, base.with_name("names_injected.3mf"), edit)
    hd.tool("project_open", path=str(crafted), on_unsaved="discard")
    names = object_names(hd)
    assert any("OBJECT-INJECTED" in n for n in names), names

    start = value(hd, "printer", "machine_start_gcode")
    hd.tool("settings_set", scope="printer", values={
        "machine_start_gcode": start + "\n; MCP-MODEL=[model_name]\n; MCP-PLATE=[plate_name]\n"})
    hd.tool("settings_set", scope="process", values={"gcode_label_objects": True, "exclude_object": True})
    r = hd.tool("slice", wait_s=280)
    assert r["status"] == "finished", r
    path = workspace(hd) / "names_injected.gcode"
    hd.tool("export_file", kind="gcode", path=str(path), overwrite=True)
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()

    for marker, command in (("MODEL-INJECTED", "G28"), ("OBJECT-INJECTED", "G28"), ("PLATE-INJECTED", "M84")):
        carrying = [line for line in lines if marker in line]
        assert carrying, f"{marker} not in the G-code"
        for line in carrying:
            assert not line.lstrip().startswith(f"{command} ; {marker}"), line
            # Inside a comment, or a sanitised macro argument: never a command of its own.
            before = line.split(marker)[0]
            assert ";" in before.replace(f"{command} ;", "") or "NAME=" in before, line
    assert any(line.startswith("; MCP-MODEL=") and "MODEL-INJECTED" in line for line in lines)
    assert any(line.startswith("; MCP-PLATE=") and "PLATE-INJECTED" in line for line in lines)
    assert any("printing object" in line and "OBJECT-INJECTED" in line for line in lines)
    discard_edits(hd)


# ---------------------------------------------------------------------------------------------
# Numbers under a locale with a decimal comma
# ---------------------------------------------------------------------------------------------

@pytest.fixture(scope="module")
def de(launch_app) -> McpClient:
    app = launch_app("hardening_de", app_config={"language": "de_DE"})
    return McpClient(app.url, app.token, era="modern", timeout=300).connect()


def test_decimal_comma_locale(de):
    de.tool("settings_set", scope="process", values={"layer_height": 0.15, "sparse_infill_density": "15.5%",
                                                     "outer_wall_line_width": 0.42, "inner_wall_line_width": "105.5%"})
    got = de.tool("settings_get", scope="process",
                  keys=["layer_height", "sparse_infill_density", "outer_wall_line_width", "inner_wall_line_width"])["values"]
    assert got["layer_height"] == pytest.approx(0.15), got
    assert got["sparse_infill_density"] == "15.5%", got
    assert got["outer_wall_line_width"] == pytest.approx(0.42), got
    assert got["inner_wall_line_width"] == "105.5%", got
    # Per-object values go through the scene's reader.
    de.tool("project_new", on_unsaved="discard")
    oid = add_cube(de)
    de.tool("settings_set", scope={"object": oid}, values={"layer_height": 0.12, "sparse_infill_density": "7.5%"})
    stored = de.tool("settings_get", scope={"object": oid})["values"]
    assert stored["layer_height"] == pytest.approx(0.12), stored
    assert stored["sparse_infill_density"] == "7.5%", stored
    # Strings with a comma are not numbers to the tools (the profiles use a decimal point).
    failure = de.tool_error("settings_set", scope="process", values={"sparse_infill_density": "15,5%"})
    assert failure.code == "invalid_value", failure
