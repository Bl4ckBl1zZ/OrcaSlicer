"""Edit group, objects: object_add_primitive, object_transform, object_duplicate, delete, object_split,
object_merge, object_cut, object_arrange, object_orient, mesh_edit."""

import pytest

from _edit_common import (TWO_SHELLS, DATA, add_cube, all_objects, assert_undo_roundtrip, bbox_size, close, fresh,
                          get_object, instance_of, make_fixtures, normalized, redo, scene, undo, wait_job)

edit_app, emodern, elegacy, ec = make_fixtures("edit_objects")


# ---------------------------------------------------------------------------------------------
# object_add_primitive
# ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("shape, size, expected", [
    ("cube", [30, 20, 10], [30, 20, 10]),
    ("cylinder", [20, 10], [20, 20, 10]),
    ("cone", [20, 15], [20, 20, 15]),
    ("sphere", [20], [20, 20, 20]),
    ("disc", [30, 1], [30, 30, 1]),
    ("torus", [20, 10], [40, 40, 10]),
])
def test_add_primitive_shapes(ec, shape, size, expected):
    created = ec.tool("object_add_primitive", shape=shape, size=size)["created"][0]
    assert created["id"].startswith("o") and created["name"] == shape.capitalize()
    assert close(created["size"], expected, 0.05), created["size"]
    inst = instance_of(created)
    assert inst["plate"] == 1 and close(inst["z_min"], 0.0)
    assert close(bbox_size(inst), expected, 0.05)


def test_add_primitive_defaults_center_and_undo(ec):
    before = normalized(scene(ec))
    created = ec.tool("object_add_primitive", shape="disc", name="coaster", center=[60, 70])
    obj = created["created"][0]
    assert obj["name"] == "coaster" and close(obj["size"][2], 0.2)
    assert close(instance_of(obj)["center"], [60, 70])
    assert created["undo"] == "MCP: add disc"
    assert_undo_roundtrip(ec, before)


def test_add_primitive_errors(ec):
    failure = ec.tool_error("object_add_primitive", shape="cube", size=[10, 10])
    assert failure.code == "invalid_argument" and "[x, y, z]" in failure.hint
    failure = ec.tool_error("object_add_primitive", shape="handy_model", model="nope")
    assert failure.code == "invalid_argument" and "3dbenchy" in failure.details["models"]
    assert ec.tool_error("object_add_primitive", shape="cube", plate=7).code == "stale_id"
    assert ec.tool_error("object_add_primitive", shape="pyramid").code == "invalid_argument"
    assert ec.tool_error("object_add_primitive", shape="cube", size=[10, 0, 10]).code == "invalid_argument"
    assert scene(ec)["object_count"] == 0


def test_add_handy_model(emodern):
    fresh(emodern)
    created = emodern.tool("object_add_primitive", shape="handy_model", model="3dbenchy")["created"]
    assert len(created) == 1 and "Benchy" in created[0]["name"]
    assert created[0]["volumes"][0]["facets"] > 1000


# ---------------------------------------------------------------------------------------------
# object_transform
# ---------------------------------------------------------------------------------------------

def test_translate_move_to_and_undo(ec):
    cube = add_cube(ec)
    start = instance_of(cube)["center"]
    before = normalized(scene(ec))
    moved = ec.tool("object_transform", targets=[cube["id"]], translate=[10, -5, 0])
    inst = instance_of(moved["changed"][0])
    assert close(inst["center"], [start[0] + 10, start[1] - 5]) and close(inst["z_min"], 0)
    assert moved["undo"].startswith("MCP: transform") and moved["slice_invalidated"] is True
    assert_undo_roundtrip(ec, before)
    placed = ec.tool("object_transform", targets=[instance_of(cube)["id"]], move_to={"center": [40, 50]})
    assert close(instance_of(placed["changed"][0])["center"], [40, 50])


def test_rotate_and_set_rotation(ec):
    cube = add_cube(ec, size=(30, 10, 10))
    oid = cube["id"]
    rotated = ec.tool("object_transform", targets=[oid], rotate={"axis": "z", "angle_deg": 90})
    inst = instance_of(rotated["changed"][0])
    assert close(inst["rotation"], [0, 0, 90], 1e-2)
    assert close(bbox_size(inst), [10, 30, 10], 1e-2)
    assert close(inst["center"], instance_of(cube)["center"], 1e-2)  # about the centre
    # Absolute rotation, then reset.
    tilted = ec.tool("object_transform", targets=[oid], set_rotation=[0, 0, 45])
    assert close(instance_of(tilted["changed"][0])["rotation"], [0, 0, 45], 1e-2)
    reset = ec.tool("object_transform", targets=[oid], reset=["rotation"])
    inst = instance_of(reset["changed"][0])
    assert close(inst["rotation"], [0, 0, 0], 1e-2) and close(bbox_size(inst), [30, 10, 10], 1e-2)
    # Rotation about X puts the long side up; the result sits on the bed.
    upright = ec.tool("object_transform", targets=[oid], rotate={"axis": [0, 1, 0], "angle_deg": 90})
    inst = instance_of(upright["changed"][0])
    assert close(bbox_size(inst), [10, 10, 30], 1e-2) and close(inst["z_min"], 0)


def test_local_frame_rotation(ec):
    cube = add_cube(ec, size=(30, 10, 10))
    oid = cube["id"]
    ec.tool("object_transform", targets=[oid], set_rotation=[0, 0, 90])
    # Local X is now world Y: rotating 90 deg about local X stands the long side ... still along world Y.
    turned = ec.tool("object_transform", targets=[oid], frame="local", rotate={"axis": "x", "angle_deg": 90})
    assert close(bbox_size(instance_of(turned["changed"][0])), [10, 30, 10], 1e-2)


def test_scale_set_scale_set_size(ec):
    cube = add_cube(ec)
    oid = cube["id"]
    doubled = ec.tool("object_transform", targets=[oid], scale=2)
    assert close(doubled["changed"][0]["size"], [40, 40, 40], 1e-2)
    stretched = ec.tool("object_transform", targets=[oid], set_scale=[1, 2, 0.5])
    assert close(stretched["changed"][0]["size"], [20, 40, 10], 1e-2)
    assert close(instance_of(stretched["changed"][0])["scale"], [1, 2, 0.5], 1e-4)
    sized = ec.tool("object_transform", targets=[oid], set_size={"size": [50, None, None]})
    assert close(sized["changed"][0]["size"], [50, 100, 25], 1e-2)
    free = ec.tool("object_transform", targets=[oid], set_size={"size": [10, 10, 10], "uniform": False})
    assert close(free["changed"][0]["size"], [10, 10, 10], 1e-2)
    assert ec.tool_error("object_transform", targets=[oid], scale=0).code == "invalid_argument"
    assert ec.tool_error("object_transform", targets=[oid], set_scale=[1, -1, 1]).code == "invalid_argument"


def test_mirror(ec):
    cube = add_cube(ec, size=(30, 10, 10))
    mirrored = ec.tool("object_transform", targets=[cube["id"]], mirror="x")
    inst = instance_of(mirrored["changed"][0])
    assert inst.get("mirror") == [-1.0, 1.0, 1.0]
    assert close(inst["center"], instance_of(cube)["center"], 1e-3)
    back = ec.tool("object_transform", targets=[cube["id"]], reset=["mirror"])
    assert "mirror" not in instance_of(back["changed"][0])


def test_lay_flat_normal_and_auto(ec):
    box = add_cube(ec, size=(30, 20, 10))
    oid = box["id"]
    # The +X face down: the 30 mm side becomes the height.
    flat = ec.tool("object_transform", targets=[oid], lay_flat={"normal": [1, 0, 0]})
    inst = instance_of(flat["changed"][0])
    assert close(bbox_size(inst)[2], 30, 1e-2) and close(inst["z_min"], 0)
    assert flat["details"][0]["lay_flat"]["normal_world_before"] == [1.0, 0.0, 0.0]
    # Tilt it, then let auto pick the largest face (30 x 20): the height becomes 10 again.
    ec.tool("object_transform", targets=[oid], rotate={"axis": "y", "angle_deg": 33})
    auto = ec.tool("object_transform", targets=[oid], lay_flat={"auto": True})
    info = auto["details"][0]["lay_flat"]
    assert close(info["face_area_mm2"], 600, 0.5) and info["rank"] == 1 and len(info["candidates"]) >= 3
    assert close(bbox_size(instance_of(auto["changed"][0]))[2], 10, 1e-2)
    second = ec.tool("object_transform", targets=[oid], lay_flat={"auto": True, "rank": 2})
    assert close(second["details"][0]["lay_flat"]["face_area_mm2"], 600, 0.5)  # the opposite 30 x 20 face


def test_lay_flat_facet(ec):
    box = add_cube(ec, size=(30, 20, 10))
    oid = box["id"]
    facets = box["volumes"][0]["facets"]
    flat = ec.tool("object_transform", targets=[oid], lay_flat={"facet": 0})
    normal = flat["details"][0]["lay_flat"]["normal_world_before"]
    axis = max(range(3), key=lambda i: abs(normal[i]))
    assert close(bbox_size(instance_of(flat["changed"][0]))[2], [30, 20, 10][axis], 1e-2)
    failure = ec.tool_error("object_transform", targets=[oid], lay_flat={"facet": facets + 5})
    assert failure.code == "invalid_argument" and "out of range" in failure.message


def test_transform_part_in_object_frame(ec):
    a = add_cube(ec)
    b = add_cube(ec, size=(10, 10, 10), center=[150, 150])
    merged = ec.tool("object_merge", targets=[a["id"], b["id"]])["created"][0]
    small = next(v for v in merged["volumes"] if v["size"] == [10.0, 10.0, 10.0])
    moved = ec.tool("object_transform", targets=[small["id"]], translate=[0, 0, 5])
    detail = moved["details"][0]
    assert detail["id"] == small["id"] and close(detail["size"], [10, 10, 10])
    failure = ec.tool_error("object_transform", targets=[small["id"]], move_to={"center": [1, 1]})
    assert failure.code == "invalid_argument" and "parts" in failure.message


def test_transform_errors(ec):
    cube = add_cube(ec)
    assert ec.tool_error("object_transform", targets=["o999999"], translate=[1, 0, 0]).code == "stale_id"
    assert ec.tool_error("object_transform", targets=[cube["id"]]).code == "invalid_argument"
    assert ec.tool_error("object_transform", targets=[cube["id"]], rotate={"axis": "w", "angle_deg": 1}).code == "invalid_argument"
    assert ec.tool_error("object_transform", targets=[cube["id"]], lay_flat={"normal": [0, 0, 0]}).code == "invalid_argument"
    assert ec.tool_error("object_transform", targets=[cube["id"]], move_to={"plate": 5}).code == "stale_id"
    assert ec.tool_error("object_transform", targets=[cube["id"]], translatee=[1, 0, 0]).code == "invalid_argument"


def test_move_to_other_plate_and_locked(ec):
    cube = add_cube(ec)
    ec.tool("plate_manage", action="add")
    before = normalized(scene(ec))
    moved = ec.tool("object_transform", targets=[cube["id"]], move_to={"plate": 2})
    inst = instance_of(moved["changed"][0])
    assert inst["plate"] == 2 and close(inst["center"], instance_of(cube)["center"], 1e-3)
    assert [len(p["objects"]) for p in scene(ec)["plates"]] == [0, 1]
    assert_undo_roundtrip(ec, before)
    ec.tool("plate_manage", action="lock", plate=2)
    failure = ec.tool_error("object_transform", targets=[cube["id"]], move_to={"plate": 2})
    assert failure.code == "plate_locked" and "unlock" in failure.hint


def test_float_with_z_min(ec):
    cube = add_cube(ec)
    up = ec.tool("object_transform", targets=[cube["id"]], move_to={"z_min": 5})
    assert close(instance_of(up["changed"][0])["z_min"], 5) and "auto-drop" in up["notes"][0]


# ---------------------------------------------------------------------------------------------
# object_duplicate and delete
# ---------------------------------------------------------------------------------------------

def test_duplicate_as_instances_and_objects(ec):
    cube = add_cube(ec)
    before = normalized(scene(ec))
    dup = ec.tool("object_duplicate", targets=[cube["id"]], copies=3)
    assert len(dup["created"]) == 3 and all(i.startswith("i") for i in dup["created"])
    obj = dup["changed"][0]
    assert len(obj["instances"]) == 4
    centers = {tuple(i["center"]) for i in obj["instances"]}
    assert len(centers) == 4  # not stacked
    assert_undo_roundtrip(ec, before)
    undo(ec)
    objs = ec.tool("object_duplicate", targets=[cube["id"]], copies=2, **{"as": "objects"})
    assert len(objs["created"]) == 2 and len(all_objects(ec)) == 3
    one = ec.tool("object_duplicate", targets=[instance_of(cube)["id"]])
    assert len(one["created"]) == 1


def test_duplicate_errors(ec):
    cube = add_cube(ec)
    assert ec.tool_error("object_duplicate", targets=[cube["volumes"][0]["id"]]).code == "invalid_argument"
    assert ec.tool_error("object_duplicate", targets=[cube["id"]], copies=0).code == "invalid_argument"
    assert ec.tool_error("object_duplicate", targets=[cube["id"]], copies=201).code == "invalid_argument"


def test_delete_instances_objects_parts(ec):
    cube = add_cube(ec)
    ec.tool("object_duplicate", targets=[cube["id"]], copies=2)
    obj = get_object(ec, cube["id"])
    before = normalized(scene(ec))
    gone = ec.tool("delete", targets=[obj["instances"][1]["id"]])
    assert gone["removed"] == [obj["instances"][1]["id"]]
    assert len(get_object(ec, cube["id"])["instances"]) == 2
    assert_undo_roundtrip(ec, before)
    failure = ec.tool_error("delete", targets=[cube["volumes"][0]["id"]])
    assert failure.code == "last_part" and cube["id"] in failure.hint
    other = add_cube(ec, center=[30, 30])
    ec.tool("delete", targets=[cube["id"], other["id"]])
    assert scene(ec)["object_count"] == 0
    assert ec.tool_error("delete", targets=[cube["id"]]).code == "stale_id"


def test_delete_part_of_multipart(ec):
    a, b = add_cube(ec), add_cube(ec, size=(10, 10, 10), center=[160, 160])
    merged = ec.tool("object_merge", targets=[a["id"], b["id"]])["created"][0]
    result = ec.tool("delete", targets=[merged["volumes"][1]["id"]])
    assert len(get_object(ec, merged["id"])["volumes"]) == 1 and result["removed"] == [merged["volumes"][1]["id"]]


# ---------------------------------------------------------------------------------------------
# split, merge, cut
# ---------------------------------------------------------------------------------------------

def test_split_to_objects_and_parts(ec):
    imported = ec.tool("model_import", paths=[str(TWO_SHELLS)])["imported"]
    assert len(imported) == 1
    oid = imported[0]["id"]
    before = normalized(scene(ec))
    split = ec.tool("object_split", target=oid)
    assert split["removed"] == [oid] and len(split["created"]) >= 2
    assert_undo_roundtrip(ec, before)
    undo(ec)
    parts = ec.tool("object_split", target=oid, into="parts")
    assert len(parts["changed"][0]["volumes"]) >= 2 and parts["changed"][0]["id"] == oid
    cube = add_cube(ec, center=[30, 30])
    failure = ec.tool_error("object_split", target=cube["id"])
    assert failure.code == "not_splittable"
    assert ec.tool_error("object_split", target=cube["id"], into="parts").code == "not_splittable"


def test_merge_keeps_positions(ec):
    a = add_cube(ec, center=[50, 50])
    b = add_cube(ec, size=(10, 10, 10), center=[100, 50])
    before = normalized(scene(ec))
    merged = ec.tool("object_merge", targets=[a["id"], b["id"]], name="pair")
    obj = merged["created"][0]
    assert obj["name"] == "pair" and len(obj["volumes"]) == 2 and sorted(merged["removed"]) == sorted([a["id"], b["id"]])
    box = instance_of(obj)["bbox"]
    assert close(box["min"][0], 40, 1e-2) and close(box["max"][0], 105, 1e-2)
    assert_undo_roundtrip(ec, before)
    assert ec.tool_error("object_merge", targets=[a["id"]]).code == "invalid_argument"


def test_cut_horizontal(ec):
    cube = add_cube(ec)
    before = normalized(scene(ec))
    cut = ec.tool("object_cut", target=cube["id"], plane={"z": 8})
    sizes = sorted(round(o["size"][2], 2) for o in cut["created"])
    assert cut["removed"] == [cube["id"]] and sizes == [8.0, 12.0]
    assert_undo_roundtrip(ec, before)
    undo(ec)
    upper = ec.tool("object_cut", target=cube["id"], plane={"z": 5}, keep="upper")
    assert len(upper["created"]) == 1 and close(upper["created"][0]["size"][2], 15, 1e-2)
    undo(ec)
    parts = ec.tool("object_cut", target=cube["id"], plane={"z": 10}, as_parts=True)
    assert len(parts["created"]) == 1 and len(parts["created"][0]["volumes"]) == 2


def test_cut_with_normal_and_errors(ec):
    cube = add_cube(ec)
    center = instance_of(cube)["center"]
    cut = ec.tool("object_cut", target=cube["id"], plane={"point": [center[0] + 5, center[1], 10], "normal": [1, 0, 0]})
    widths = sorted(round(o["size"][0], 2) for o in cut["created"])
    assert widths == [5.0, 15.0]
    other = add_cube(ec, center=[40, 40])
    failure = ec.tool_error("object_cut", target=other["id"], plane={"z": 50})
    assert failure.code == "invalid_argument" and "does not cross" in failure.message
    assert ec.tool_error("object_cut", target=other["id"], plane={"z": 5}, keep="upper", as_parts=True).code == "invalid_argument"
    assert ec.tool_error("object_cut", target=other["volumes"][0]["id"], plane={"z": 5}).code == "invalid_argument"


# ---------------------------------------------------------------------------------------------
# arrange, orient (jobs)
# ---------------------------------------------------------------------------------------------

def overlaps(a, b) -> bool:
    return all(a["min"][i] < b["max"][i] - 1e-6 and b["min"][i] < a["max"][i] - 1e-6 for i in range(2))


def test_arrange_plate(ec):
    cube = add_cube(ec)
    ec.tool("object_duplicate", targets=[cube["id"]], copies=3, spacing=0)
    # Pile them up on one spot.
    obj = get_object(ec, cube["id"])
    for inst in obj["instances"]:
        ec.tool("object_transform", targets=[inst["id"]], move_to={"center": [100, 100]})
    before = normalized(scene(ec))
    result = wait_job(ec, ec.tool("object_arrange", spacing=4, wait_s=60))
    assert result["status"] == "finished" and result["kind"] == "arrange"
    boxes = [i["bbox"] for i in get_object(ec, cube["id"])["instances"]]
    assert len(boxes) == 4
    assert not any(overlaps(boxes[i], boxes[j]) for i in range(4) for j in range(i + 1, 4))
    assert all(p["plate"] == 1 for p in result["result"]["placements"])
    names = ec.tool("history")["undo"]
    assert names[0].startswith("MCP: arrange plate 1")
    undo(ec)
    assert normalized(scene(ec)) == before


def test_arrange_errors(ec):
    assert ec.tool_error("object_arrange").code == "nothing_to_arrange"
    assert ec.tool_error("object_arrange", scope="all").code == "nothing_to_arrange"
    add_cube(ec)
    ec.tool("plate_manage", action="lock", plate=1)
    failure = ec.tool_error("object_arrange", plate=1)
    assert failure.code == "plate_locked" and "unlock" in failure.hint
    assert ec.tool_error("object_arrange", plate=3).code == "stale_id"


def test_orient(ec):
    box = add_cube(ec, size=(40, 30, 4))
    ec.tool("object_transform", targets=[box["id"]], rotate={"axis": "x", "angle_deg": 70})
    tilted = get_object(ec, box["id"])
    assert bbox_size(instance_of(tilted))[2] > 20
    result = wait_job(ec, ec.tool("object_orient", targets=[box["id"]], wait_s=120))
    assert result["status"] == "finished", result
    changed = result["result"]["changed"][0]
    assert changed["id"] == box["id"]
    assert bbox_size(instance_of(changed))[2] < bbox_size(instance_of(tilted))[2]
    assert ec.tool("history")["undo"][0].startswith("MCP: orient")


def test_orient_locked_and_empty(ec):
    assert ec.tool_error("object_orient").code == "nothing_to_arrange"
    cube = add_cube(ec)
    ec.tool("plate_manage", action="lock", plate=1)
    assert ec.tool_error("object_orient", targets=[cube["id"]]).code == "plate_locked"
    assert ec.tool_error("object_orient", targets=[cube["volumes"][0]["id"]]).code == "invalid_argument"


# ---------------------------------------------------------------------------------------------
# mesh_edit
# ---------------------------------------------------------------------------------------------

def test_simplify_and_undo(ec):
    sphere = ec.tool("object_add_primitive", shape="sphere", size=[30])["created"][0]
    facets = sphere["volumes"][0]["facets"]
    before = normalized(scene(ec))
    result = wait_job(ec, ec.tool("mesh_edit", action="simplify", targets=[sphere["id"]], target_ratio=0.25))
    assert result["status"] == "finished", result
    vol = result["result"]["volumes"][0]
    assert vol["modified"] is True and vol["facets"] <= facets * 0.3 and vol["facets_before"] == facets
    assert vol["id"] != vol["old_id"]  # a new mesh gets a new id
    assert get_object(ec, sphere["id"])["volumes"][0]["facets"] == vol["facets"]
    undo(ec)
    assert normalized(scene(ec)) == before


def test_repair(ec):
    imported = ec.tool("model_import", paths=[str(DATA / "cube_with_hole.obj")])["imported"][0]
    result = wait_job(ec, ec.tool("mesh_edit", action="repair", targets=[imported["id"]]))
    assert result["status"] == "finished", result
    vol = result["result"]["volumes"][0]
    assert "repair" in vol
    full = scene(ec, detail="full")["plates"][0]["objects"][0]
    assert full["volumes"][0]["mesh"]["open_edges"] == 0


def test_mesh_job_cancel(emodern):
    fresh(emodern)
    bunny = emodern.tool("object_add_primitive", shape="handy_model", model="stanford_bunny")["created"][0]
    before = normalized(scene(emodern))
    started = emodern.tool("mesh_edit", action="simplify", targets=[bunny["id"]], target_facets=200, wait_s=0)
    if started.get("status") == "running":
        cancelled = emodern.tool("job_cancel", job=started["job"])
        final = wait_job(emodern, cancelled if cancelled.get("status") == "running" else {"status": "running", "job": started["job"]})
        assert final["status"] in ("cancelled", "finished")
        if final["status"] == "cancelled":
            # Nothing was applied; the undo step taken at the start leaves the scene as it was.
            assert normalized(scene(emodern)) == before
    else:
        assert started["status"] == "finished"


def test_convert_units_and_scale_to_fit(ec):
    cube = add_cube(ec, size=(2, 2, 2))
    inch = ec.tool("mesh_edit", action="convert_units", targets=[cube["id"]], **{"from": "inch"})
    assert close(inch["changed"][0]["size"], [50.8, 50.8, 50.8], 1e-2)
    back = ec.tool("mesh_edit", action="convert_units", targets=[cube["id"]], **{"from": "to_inch"})
    assert close(back["changed"][0]["size"], [2, 2, 2], 1e-3)
    bed = scene(ec)["printer"]["bed"]
    fit = ec.tool("mesh_edit", action="scale_to_fit", targets=[cube["id"]], margin=10)
    size = fit["changed"][0]["size"]
    assert close(size[0], min(bed) - 20, 0.05) or close(size[2], scene(ec)["printer"]["height"], 0.05)
    assert ec.tool_error("mesh_edit", action="simplify", targets=[cube["id"]], target_ratio=0).code == "invalid_argument"
    assert ec.tool_error("mesh_edit", action="convert_units", targets=[instance_of(cube)["id"]], **{"from": "inch"}).code == "invalid_argument"


def test_lay_flat_face_from_mesh_query(ec):
    box = add_cube(ec, size=(30, 20, 10))
    oid = box["id"]
    faces = ec.tool("mesh_query", kind="flat_faces", targets=[oid])["flat_faces"][0]["faces"]
    choice = next(f for f in faces if not f.get("on_bed") and abs(f["area_mm2"] - 200) < 1)  # a 20 x 10 end
    flat = ec.tool("object_transform", targets=[oid], lay_flat={"face": choice["face"]})
    assert flat["details"][0]["lay_flat"]["face"] == choice["face"]
    assert close(bbox_size(instance_of(flat["changed"][0]))[2], 30, 1e-2)
    after = ec.tool("mesh_query", kind="flat_faces", targets=[oid])["flat_faces"][0]["faces"]
    assert any(f.get("on_bed") and abs(f["area_mm2"] - 200) < 1 for f in after)
    failure = ec.tool_error("object_transform", targets=[oid], lay_flat={"face": 99})
    assert failure.code == "invalid_argument" and "mesh_query" in failure.hint
