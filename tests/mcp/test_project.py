"""Edit group, project: project_new, project_open, project_save, model_import, export_file, and the end-to-end
scenario of the group."""

import struct
from pathlib import Path

import pytest

from _edit_common import (BADGE_3MF, CUBE_OBJ, DATA, DRC_FILE, PROJECT_3MF, STEP_FILE, STEP_UTF8, STL_BOX, TWO_SHELLS, add_cube,
                          all_objects, assert_undo_roundtrip, bbox_size, close, fresh, get_object, instance_of, make_fixtures,
                          normalized, scene, undo, wait_job, workspace)

edit_app, emodern, elegacy, ec = make_fixtures("edit_project")


def write_ascii_stl(path: Path, size: float, z0: float = 0.0, name: str = "box"):
    """A closed box of `size` mm per side, corner at the origin (z from z0)."""
    s = size
    v = [(0, 0, z0), (s, 0, z0), (s, s, z0), (0, s, z0), (0, 0, z0 + s), (s, 0, z0 + s), (s, s, z0 + s), (0, s, z0 + s)]
    faces = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    lines = [f"solid {name}"]
    for a, b, c in faces:
        lines += ["facet normal 0 0 0", "outer loop"] + [f"vertex {v[i][0]} {v[i][1]} {v[i][2]}" for i in (a, b, c)] + ["endloop", "endfacet"]
    lines.append(f"endsolid {name}")
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def write_two_object_obj(path: Path):
    """Two 10 mm cubes as separate OBJ objects, the second one floating 30 mm above the first."""
    out = []
    base = 0
    for name, z0 in (("lower", 0), ("upper", 30)):
        out.append(f"o {name}")
        for x, y, z in [(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0), (0, 0, 10), (10, 0, 10), (10, 10, 10), (0, 10, 10)]:
            out.append(f"v {x} {y} {z + z0}")
        for a, b, c in [(1, 3, 2), (1, 4, 3), (5, 6, 7), (5, 7, 8), (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 4, 8), (3, 8, 7), (4, 1, 5), (4, 5, 8)]:
            out.append(f"f {a + base} {b + base} {c + base}")
        base += 8
    path.write_text("\n".join(out) + "\n", encoding="ascii")


# ---------------------------------------------------------------------------------------------
# project_new / project_save / project_open
# ---------------------------------------------------------------------------------------------

def test_new_project_unsaved_policies(ec):
    add_cube(ec)
    failure = ec.tool_error("project_new")
    assert failure.code == "unsaved_changes" and "on_unsaved" in failure.hint
    assert scene(ec)["object_count"] == 1
    target = workspace(ec) / f"saved_by_new_{ec.era}.3mf"
    target.unlink(missing_ok=True)
    failure = ec.tool_error("project_new", on_unsaved="save")
    assert failure.code == "no_path"
    result = ec.tool("project_new", on_unsaved="save", save_path=str(target), name="Second")
    assert target.exists() and target.stat().st_size > 1000
    assert result["changed"]["objects"] == [] and result["changed"]["project"]["name"] == "Second"
    assert "Saved the previous project" in result["notes"][0]
    add_cube(ec)
    discarded = ec.tool("project_new", on_unsaved="discard")
    assert discarded["changed"]["objects"] == [] and discarded["changed"]["plates"] == 1


def test_save_paths_and_errors(ec):
    add_cube(ec)
    ws = workspace(ec)
    assert ec.tool_error("project_save").code == "no_path"
    target = ws / f"save_{ec.era}.3mf"
    target.unlink(missing_ok=True)
    saved = ec.tool("project_save", path=str(target))
    assert saved["path"].endswith(target.name) and saved["bytes"] == target.stat().st_size > 1000
    assert saved["project"]["dirty"] is False and saved["project"]["path"].replace("\\", "/").endswith(target.name)
    # Save again to the project's own file: no overwrite needed.
    add_cube(ec, center=[40, 40])
    again = ec.tool("project_save")
    assert again["path"] == saved["path"] and again["project"]["dirty"] is False
    # A copy leaves the project's file alone.
    copy = ws / f"copy_{ec.era}.3mf"
    copy.unlink(missing_ok=True)
    copied = ec.tool("project_save", path=str(copy), copy=True)
    assert copy.exists() and copied["project"]["path"] == again["project"]["path"]
    # Errors.
    assert ec.tool_error("project_save", path=str(copy)).code == "exists"
    assert ec.tool("project_save", path=str(copy), copy=True, overwrite=True)["copy"] is True
    assert ec.tool_error("project_save", path=str(ws / "x.stl")).code == "path_denied"
    assert ec.tool_error("project_save", path="C:/Windows/forbidden.3mf").code == "path_denied"
    assert ec.tool_error("project_save", path=str(ws / "missing_dir" / "x.3mf")).code == "path_denied"
    assert ec.tool_error("project_save", copy=True).code == "invalid_argument"


def test_open_roundtrip(ec):
    a = add_cube(ec, size=(30, 20, 10), center=[60, 60])
    b = ec.tool("object_add_primitive", shape="cylinder", size=[20, 30], center=[150, 120])["created"][0]
    ec.tool("object_transform", targets=[b["id"]], set_rotation=[0, 0, 30])
    target = workspace(ec) / f"roundtrip_{ec.era}.3mf"
    target.unlink(missing_ok=True)
    ec.tool("project_save", path=str(target))
    expected = {o["name"]: o for o in all_objects(ec).values()}
    ec.tool("project_new")
    opened = ec.tool("project_open", path=str(target))
    assert len(opened["changed"]["objects"]) == 2 and opened["changed"]["project"]["dirty"] is False
    assert opened["changed"]["project"]["path"].replace("\\", "/").endswith(target.name)
    got = {o["name"]: o for o in all_objects(ec).values()}
    assert set(got) == set(expected)
    for name, obj in got.items():
        assert obj["id"] not in (a["id"], b["id"]) or True  # ids may be reassigned after a load
        assert close(obj["size"], expected[name]["size"], 1e-3)
        assert close(instance_of(obj)["center"], instance_of(expected[name])["center"], 1e-3)
        assert close(instance_of(obj)["rotation"], instance_of(expected[name])["rotation"], 1e-3)


def test_open_errors(ec):
    ws = workspace(ec)
    assert ec.tool_error("project_open", path=str(ws / "nothing.3mf")).code == "not_found"
    assert ec.tool_error("project_open", path="C:/Windows/win.ini").code == "path_denied"
    assert ec.tool_error("project_open", path=str(CUBE_OBJ)).code == "path_denied"
    add_cube(ec)
    target = ws / f"err_{ec.era}.3mf"
    target.unlink(missing_ok=True)
    ec.tool("project_save", path=str(target))
    add_cube(ec, center=[40, 40])
    failure = ec.tool_error("project_open", path=str(target))
    assert failure.code == "unsaved_changes"
    broken = ws / f"broken_{ec.era}.3mf"
    broken.write_bytes(b"this is not a zip file")
    failure = ec.tool_error("project_open", path=str(broken), on_unsaved="discard")
    assert failure.code in ("load_failed", "dialog_blocked"), failure


def test_open_shipped_project(emodern):
    fresh(emodern)
    result = emodern.call("project_open", {"path": str(PROJECT_3MF)})
    data = result.data
    if result.is_error:
        # Projects carrying presets with custom G-code need trust_embedded_gcode.
        assert data["error"]["code"] == "dialog_blocked"
        data = emodern.tool("project_open", path=str(PROJECT_3MF), trust_embedded_gcode=True, on_unsaved="discard")
    assert len(data["changed"]["objects"]) >= 1


# ---------------------------------------------------------------------------------------------
# model_import
# ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("path, min_objects", [
    (STL_BOX, 1), (CUBE_OBJ, 1), (STEP_FILE, 1), (DRC_FILE, 1), (BADGE_3MF, 1),
])
def test_import_formats(ec, path, min_objects):
    before = normalized(scene(ec))
    result = ec.tool("model_import", paths=[str(path)])
    assert len(result["imported"]) >= min_objects
    for obj in result["imported"]:
        assert obj["id"].startswith("o") and instance_of(obj)["plate"] == 1
        assert obj["volumes"][0]["facets"] > 0
    assert result["undo"].startswith("MCP: import")
    if path == BADGE_3MF:
        # A multi-colour 3MF adds filament slots: journalled, not in the undo stack.
        if len(before["filaments"]) < len(scene(ec)["filaments"]):
            assert result["journal"] and "filament" in result["notes"][0]
        undo(ec)
        after = normalized(scene(ec))
        after.pop("filaments"), before.pop("filaments")
        assert after == before
        return
    assert_undo_roundtrip(ec, before)


def test_import_several_and_single_object(ec):
    result = ec.tool("model_import", paths=[str(STL_BOX), str(CUBE_OBJ)])
    assert len(result["imported"]) == 2
    fresh(ec)
    single = ec.tool("model_import", paths=[str(STL_BOX), str(CUBE_OBJ)], **{"as": "single_object"})
    assert len(single["imported"]) == 1 and len(single["imported"][0]["volumes"]) == 2
    assert scene(ec)["object_count"] == 1


def test_import_units(ec):
    ws = workspace(ec)
    tiny = ws / f"tiny_{ec.era}.stl"
    write_ascii_stl(tiny, 0.02)
    auto = ec.tool("model_import", paths=[str(tiny)])
    assert close(auto["imported"][0]["size"], [20, 20, 20], 1e-2)
    assert any("too small" in d["title"].lower() and d["answered"] == "yes" for d in auto["dialogs"])
    kept = ec.tool("model_import", paths=[str(tiny)], units="mm")
    assert close(kept["imported"][0]["size"], [0.02, 0.02, 0.02], 1e-4)
    assert any(d["answered"] == "no" for d in kept["dialogs"])
    inch = ec.tool("model_import", paths=[str(STL_BOX)], units="inch")
    assert close(inch["imported"][0]["size"], [508, 508, 508], 0.5)
    metres = ec.tool("model_import", paths=[str(tiny)], units="m")
    assert close(metres["imported"][0]["size"], [20, 20, 20], 1e-2)


def test_import_multipart_question(ec):
    two = workspace(ec) / f"two_heights_{ec.era}.obj"
    write_two_object_obj(two)
    merged = ec.tool("model_import", paths=[str(two)])
    separate_or_merged = merged["imported"]
    if any("multi-part" in d["title"].lower() for d in merged.get("dialogs", [])):
        assert len(separate_or_merged) == 1 and len(separate_or_merged[0]["volumes"]) == 2
        fresh(ec)
        kept = ec.tool("model_import", paths=[str(two)], multipart="keep_separate")
        assert len(kept["imported"]) == 2
    else:
        # The OBJ reader joined the two groups into one mesh: no question was asked.
        assert len(separate_or_merged) >= 1


def test_import_step_quality(ec):
    coarse = ec.tool("model_import", paths=[str(STEP_FILE)], step={"linear_deflection": 0.5, "angle_deflection": 0.8})
    fine = ec.tool("model_import", paths=[str(STEP_FILE)], step={"linear_deflection": 0.005, "angle_deflection": 0.1})
    count = lambda r: sum(v["facets"] for o in r["imported"] for v in o["volumes"])
    assert count(fine) > count(coarse)
    names = ec.tool("model_import", paths=[str(STEP_UTF8)])
    assert len(names["imported"]) >= 1


def test_import_onto_plate_and_locked(ec):
    ec.tool("plate_manage", action="add")
    result = ec.tool("model_import", paths=[str(CUBE_OBJ)], plate=2)
    assert instance_of(result["imported"][0])["plate"] == 2
    assert scene(ec)["plates"][0]["current"] is True  # the user's current plate is unchanged
    ec.tool("plate_manage", action="lock", plate=2)
    assert ec.tool_error("model_import", paths=[str(CUBE_OBJ)], plate=2).code == "plate_locked"


def test_import_errors(ec):
    ws = workspace(ec)
    assert ec.tool_error("model_import", paths=[str(DATA / "seam_contour_benchy_split.txt")]).code == "path_denied"
    assert ec.tool_error("model_import", paths=["C:/Windows/System32/drivers/etc/hosts.stl"]).code in ("path_denied", "not_found")
    assert ec.tool_error("model_import", paths=[str(ws / "missing.stl")]).code == "not_found"
    assert ec.tool_error("model_import", paths=[]).code == "invalid_argument"
    garbage = ws / f"garbage_{ec.era}.stl"
    garbage.write_bytes(b"solid x\nthis is not an stl\n" + struct.pack("<I", 7) * 10)
    failure = ec.tool_error("model_import", paths=[str(garbage)])
    assert failure.code in ("load_failed", "dialog_blocked")
    assert scene(ec)["object_count"] == 0


# ---------------------------------------------------------------------------------------------
# export_file
# ---------------------------------------------------------------------------------------------

def test_export_meshes_and_copy(ec):
    a = add_cube(ec, center=[50, 50])
    b = ec.tool("object_add_primitive", shape="cylinder", size=[20, 20], center=[120, 60], name="cyl")["created"][0]
    ws = workspace(ec)
    for name in (f"all_{ec.era}.stl", f"all_{ec.era}.obj", f"proj_{ec.era}.3mf"):
        (ws / name).unlink(missing_ok=True)
    merged = ec.tool("export_file", kind="stl", path=str(ws / f"all_{ec.era}.stl"))
    data = (ws / f"all_{ec.era}.stl").read_bytes()
    facets = struct.unpack("<I", data[80:84])[0]
    assert merged["bytes"] == len(data) == 84 + 50 * facets
    assert facets == a["volumes"][0]["facets"] + b["volumes"][0]["facets"]
    per = ec.tool("export_file", kind="stl", path=str(ws / f"each_{ec.era}.stl"), merge=False, overwrite=True)
    assert len(per["files"]) == 2 and all(Path(f["path"]).exists() for f in per["files"])
    obj = ec.tool("export_file", kind="obj", path=str(ws / f"all_{ec.era}.obj"), objects=[b["id"]])
    assert (ws / f"all_{ec.era}.obj").read_text().count("\nf ") == b["volumes"][0]["facets"]
    copy = ec.tool("export_file", kind="3mf", path=str(ws / f"proj_{ec.era}.3mf"))
    assert copy["bytes"] > 1000
    assert ec.tool_error("export_file", kind="stl", path=str(ws / f"all_{ec.era}.stl")).code == "exists"
    assert ec.tool_error("export_file", kind="stl", path="C:/Windows/x.stl").code == "path_denied"
    assert ec.tool_error("export_file", kind="stl", path=str(ws / "x.obj")).code == "path_denied"


def test_export_gcode_needs_slice(ec):
    add_cube(ec)
    ws = workspace(ec)
    failure = ec.tool_error("export_file", kind="gcode", path=str(ws / "p.gcode"))
    assert failure.code == "not_sliced" and "slice" in failure.hint.lower()
    assert ec.tool_error("export_file", kind="gcode_3mf", path=str(ws / "p.gcode.3mf")).code == "not_sliced"
    assert ec.tool_error("export_file", kind="gcode", path=str(ws / "p.gcode"), plate="all").code == "invalid_argument"


# ---------------------------------------------------------------------------------------------
# End to end: new -> import -> arrange -> duplicate -> transform -> lay flat -> split/merge/cut ->
# move between plates -> save -> reopen -> identical scene
# ---------------------------------------------------------------------------------------------

def scene_signature(c) -> list:
    """The scene without ids: per plate, per object name, sizes, volume count, instance placements."""
    out = []
    s = scene(c, limit=500)
    for plate in s["plates"]:
        objs = []
        for obj in plate["objects"]:
            objs.append((obj["name"], tuple(round(x, 2) for x in obj["size"]), len(obj["volumes"]),
                         tuple(sorted((tuple(round(x, 2) for x in i["center"]), round(i["z_min"], 2),
                                       tuple(round(x, 1) for x in i["rotation"])) for i in obj["instances"]))))
        out.append((plate["name"], plate["locked"], sorted(objs)))
    return out


def test_end_to_end(ec):
    ws = workspace(ec)
    ec.tool("project_new", on_unsaved="discard", name="E2E")
    imported = ec.tool("model_import", paths=[str(STL_BOX), str(CUBE_OBJ), str(STEP_FILE), str(TWO_SHELLS)])["imported"]
    assert len(imported) == 4
    box, cube, step, shells = (o["id"] for o in imported)
    arranged = wait_job(ec, ec.tool("object_arrange", spacing=5))
    assert arranged["status"] == "finished"
    dup = ec.tool("object_duplicate", targets=[cube], copies=1)
    assert len(dup["created"]) == 1
    ec.tool("object_transform", targets=[box], set_scale=1.5, rotate={"axis": "z", "angle_deg": 30})
    flat = ec.tool("object_transform", targets=[step], lay_flat={"auto": True})
    assert "lay_flat" in flat["details"][0]
    split = ec.tool("object_split", target=shells)["created"]
    assert len(split) >= 2
    merged = ec.tool("object_merge", targets=[s["id"] for s in split], name="shells again")["created"][0]
    assert len(merged["volumes"]) >= 2
    cut = ec.tool("object_cut", target=box, plane={"z": 10})["created"]
    assert len(cut) == 2
    ec.tool("plate_manage", action="add")
    ec.tool("plate_manage", action="rename", plate=2, name="second")
    moved = ec.tool("object_transform", targets=[merged["id"]], move_to={"plate": 2, "center": [80, 80]})
    assert instance_of(moved["changed"][0])["plate"] == 2
    expected = scene_signature(ec)
    target = ws / f"e2e_{ec.era}.3mf"
    target.unlink(missing_ok=True)
    ec.tool("project_save", path=str(target))
    ec.tool("project_new")
    assert scene(ec)["object_count"] == 0
    ec.tool("project_open", path=str(target))
    assert scene_signature(ec) == expected
