"""Paint group: the paint takes effect when slicing, the two end-to-end scenarios, and
plate_layer_events.

Effects are checked on real G-code: the test-only tool debug_paint_slice slices a copy of the app's
plate with the app's own configuration through libslic3r (the slice tools of another group are not
on this branch) and the G-code is parsed here: support extrusions, tool changes per layer, the seam
(start of each outer wall) and fuzzy-skin jitter.
"""

from __future__ import annotations

import math
import re
from pathlib import Path

import pytest

from orca_mcp import McpClient


# ---------------------------------------------------------------------------------------------
# meshes
# ---------------------------------------------------------------------------------------------

def write_obj(path: Path, vertices, faces):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        for v in vertices:
            f.write("v %.6f %.6f %.6f\n" % tuple(v))
        for t in faces:
            f.write("f %d %d %d\n" % (t[0] + 1, t[1] + 1, t[2] + 1))


def extrude_profile(profile, cap_quads, y0, y1):
    """A prism: a closed (x, z) profile, counter-clockwise seen from -y, extruded from y0 to y1.
    cap_quads: quads (indices into profile) that tile the profile, counter-clockwise."""
    n = len(profile)
    verts = [(x, y0, z) for x, z in profile] + [(x, y1, z) for x, z in profile]
    faces = []
    for i in range(n):
        j = (i + 1) % n
        # side wall between profile edge i -> j, outward
        faces += [(i, j, n + j), (i, n + j, n + i)]
    for a, b, c, d in cap_quads:
        faces += [(a, c, b), (a, d, c)]                     # cap at y0, facing -y
        faces += [(n + a, n + b, n + c), (n + a, n + c, n + d)]  # cap at y1, facing +y
    return verts, faces


def t_shape(path: Path):
    """A T: column 10 x 10 x 20, slab 40 x 10 x 5 on top; arms overhang 15 mm on each side."""
    profile = [(-5, 0), (5, 0), (5, 20), (20, 20), (20, 25), (5, 25), (-5, 25), (-20, 25), (-20, 20), (-5, 20)]
    quads = [(0, 1, 2, 9), (9, 2, 5, 6), (2, 3, 4, 5), (8, 9, 6, 7)]
    write_obj(path, *extrude_profile(profile, quads, -5, 5))


def slab_with_logo(path: Path):
    """A 30 x 30 x 10 slab with a raised 12 x 6 x 1.2 'logo' block in the middle of its top (one mesh, two shells)."""
    def box(x0, y0, z0, x1, y1, z1, base):
        v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
        f = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
        return v, [(a + base, b + base, c + base) for a, b, c in f]
    v1, f1 = box(-15, -15, 0, 15, 15, 10, 0)
    v2, f2 = box(-6, -3, 10, 6, 3, 11.2, 8)
    write_obj(path, v1 + v2, f1 + f2)


def cylinder(path: Path, r=10.0, h=10.0, n=96):
    verts = [(0, 0, 0), (0, 0, h)]
    for i in range(n):
        a = 2 * math.pi * i / n
        verts += [(r * math.cos(a), r * math.sin(a), 0), (r * math.cos(a), r * math.sin(a), h)]
    faces = []
    for i in range(n):
        j = (i + 1) % n
        b0, t0, b1, t1 = 2 + 2 * i, 3 + 2 * i, 2 + 2 * j, 3 + 2 * j
        faces += [(b0, b1, t1), (b0, t1, t0), (0, b1, b0), (1, t0, t1)]
    write_obj(path, verts, faces)


# ---------------------------------------------------------------------------------------------
# G-code
# ---------------------------------------------------------------------------------------------

ROLE = re.compile(r"^;\s*(?:TYPE|FEATURE):\s*(.+?)\s*$")


def parse_gcode(path):
    """Extrusion moves: list of dicts {z, x0, y0, x1, y1, role, tool}."""
    moves = []
    x = y = z = 0.0
    e_abs = False
    last_e = 0.0
    tool = 0
    role = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.strip()
            if not s:
                continue
            if s.startswith(";"):
                m = ROLE.match(s)
                if m:
                    role = m.group(1)
                continue
            code = s.split(";")[0].strip()
            if code == "M82":
                e_abs = True
            elif code == "M83":
                e_abs = False
            elif code.startswith("G92"):
                for p in code.split()[1:]:
                    if p[0] == "E":
                        last_e = float(p[1:])
            elif re.fullmatch(r"T\d+", code):
                tool = int(code[1:])
            elif code.startswith(("G0 ", "G1 ", "G2 ", "G3 ")):
                params = {}
                for p in code.split()[1:]:
                    if p and p[0] in "XYZEF":
                        try:
                            params[p[0]] = float(p[1:])
                        except ValueError:
                            pass
                nx, ny, nz = params.get("X", x), params.get("Y", y), params.get("Z", z)
                de = 0.0
                if "E" in params:
                    de = params["E"] - last_e if e_abs else params["E"]
                    if e_abs:
                        last_e = params["E"]
                if de > 0 and ("X" in params or "Y" in params):
                    moves.append({"z": round(nz, 4), "x0": x, "y0": y, "x1": nx, "y1": ny, "role": role, "tool": tool})
                x, y, z = nx, ny, nz
    return moves


def is_support(role):
    return role is not None and "support" in role.lower()


def outer_wall_starts(moves):
    """Per layer z: the first point of the first outer wall loop."""
    starts = {}
    prev_role = None
    for m in moves:
        if m["role"] and m["role"].lower() in ("outer wall", "external perimeter") and prev_role != m["role"] and m["z"] not in starts:
            starts[m["z"]] = (m["x0"], m["y0"])
        prev_role = m["role"]
    return starts


# ---------------------------------------------------------------------------------------------
# fixtures
# ---------------------------------------------------------------------------------------------

@pytest.fixture(scope="module")
def models(run_dir):
    d = run_dir / "paint_models"
    d.mkdir(exist_ok=True)
    paths = {"t": d / "t_shape.obj", "logo": d / "slab_logo.obj", "cyl": d / "cylinder.obj"}
    t_shape(paths["t"])
    slab_with_logo(paths["logo"])
    cylinder(paths["cyl"])
    return paths


def start(launch_app, name, model):
    app = launch_app(name, files=[str(model)])
    clients = {era: McpClient(app.url, app.token, era=era).connect() for era in ("modern", "legacy")}
    return app, clients


def only_object(c):
    scene = c.tool("scene_get")
    objs = [o for p in scene["plates"] for o in p["objects"]]
    assert len(objs) == 1
    return objs[0]


def part_box(c, oid):
    p = c.tool("paint_get", targets=[oid])["parts"][0]
    return p["box"]["min"], p["box"]["max"]


def slice_to(c, app, name, **overrides):
    path = app.data_dir / f"{name}.gcode"
    # The fixture plate leaves no room for a prime tower next to the model; it is not what is tested.
    overrides.setdefault("enable_prime_tower", 0)
    c.tool("debug_paint_slice", path=str(path), overrides={k: str(v) for k, v in overrides.items()})
    return parse_gcode(path)


@pytest.fixture(scope="module")
def t_app(launch_app, models):
    return start(launch_app, "paint_t", models["t"])


@pytest.fixture(scope="module")
def logo_app(launch_app, models):
    app, clients = start(launch_app, "paint_logo", models["logo"])
    clients["modern"].tool("debug_paint_fixture", op="add_filament", count=1)
    clients["modern"].tool("debug_paint_fixture", op="filament_color", count=2, color="#FFFFFF")
    return app, clients


@pytest.fixture(scope="module")
def cyl_app(launch_app, models):
    return start(launch_app, "paint_cyl", models["cyl"])


# ---------------------------------------------------------------------------------------------
# Scenario 1: supports only where needed
# ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("era", ["modern", "legacy"])
def test_scenario_supports_only_where_needed(t_app, era):
    app, clients = t_app
    c = clients[era]
    oid = only_object(c)["id"]
    c.tool("paint_apply", targets=[oid], channel="support", op="clear")
    lo, hi = part_box(c, oid)
    cx, cy, z0 = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]

    # No paint, manual supports: no support at all.
    manual = {"enable_support": 1, "support_type": "normal(manual)"}
    assert not [m for m in slice_to(c, app, f"t_none_{era}", **manual) if is_support(m["role"])]

    # Enforce: overhangs steeper than 45 deg, only inside a box around the right arm (overhang AND box).
    r = c.tool("paint_apply", targets=[oid], channel="support", state="enforce", resolution_mm=0.2,
               selector={"and": [{"box": {"min": [cx + 2, cy - 8, z0 + 15], "max": [cx + 25, cy + 8, z0 + 22]}},
                                 {"overhang": {"threshold_deg": 45}}]})
    assert r["changed"] and r["parts"][0]["after"]["enforce"]["area_mm2"] == pytest.approx(150.0, abs=0.5)  # 15 x 10 underside
    assert any("enable_support" in n for n in r["notes"]), r.get("notes")  # supports are off in the process: say so
    box = r["parts"][0]["after"]["enforce"]["box"]
    assert box["min"][0] == pytest.approx(cx + 5, abs=0.01) and box["max"][0] == pytest.approx(cx + 20, abs=0.01)
    assert box["min"][2] == pytest.approx(z0 + 20, abs=0.01) and box["max"][2] == pytest.approx(z0 + 20, abs=0.01)
    # Block everything around the left arm.
    r = c.tool("paint_apply", targets=[oid], channel="support", state="block", resolution_mm=0.2,
               selector={"box": {"min": [cx - 21, cy - 6, z0 + 19], "max": [cx - 5.5, cy + 6, z0 + 26]}})
    assert r["parts"][0]["after"]["block"]["area_mm2"] == pytest.approx(485.0, rel=0.01)

    # The picture: enforcer green under the right arm, blocker red around the left arm.
    from orca_mcp.png import close_to, hex_color, read_png
    shot = c.call("screenshot", {"camera": {"azimuth_deg": 20, "elevation_deg": -25, "target": [oid]}, "size": {"width": 800, "height": 600},
                                 "editor": {"overlay": "support_paint", "show_bed": False}})
    assert not shot.is_error, shot.data
    (app.data_dir.parent / "out").mkdir(exist_ok=True)
    (app.data_dir.parent / "out" / f"paint_t_supports_{era}.png").write_bytes(shot.images[0][1])
    image, states = read_png(shot.images[0][1]), shot.data["legend"]["overlay"]["states"]
    green = image.bounding_box(close_to(hex_color(states["enforce"]), 6))
    red = image.bounding_box(close_to(hex_color(states["block"]), 6))
    assert green and red and green[0] > red[2], (green, red)  # enforcers right of the blockers in the picture

    # Manual supports: support only under the right arm.
    support = [m for m in slice_to(c, app, f"t_manual_{era}", **manual) if is_support(m["role"])]
    assert support, "enforcers must produce support"
    xs = [p for m in support for p in (m["x0"], m["x1"])]
    # Nothing under the left arm (x < cx - 5); support under the right arm (cx + 5 .. cx + 20). The first
    # layer's support base may wrap the column (cx - 5 .. cx + 5).
    assert min(xs) > cx - 5 and max(xs) > cx + 15 and max(xs) < cx + 25, (min(xs), max(xs), cx)
    assert not [m for m in support if m["z"] > 1.0 and min(m["x0"], m["x1"]) < cx]
    assert max(m["z"] for m in support) <= z0 + 20.01

    # Automatic supports: the arm on the left is blocked, the right one still supported.
    auto = [m for m in slice_to(c, app, f"t_auto_{era}", enable_support=1, support_type="normal(auto)") if is_support(m["role"])]
    xs = [p for m in auto for p in (m["x0"], m["x1"])]
    assert xs and min(xs) > cx - 5 and max(xs) > cx + 15, (min(xs), max(xs))

    # The same without the blocker supports both arms (control).
    c.tool("paint_apply", targets=[oid], channel="support", op="erase", selector={"box": {"min": [cx - 21, cy - 6, z0 + 19], "max": [cx - 5.5, cy + 6, z0 + 26]}})
    auto = [m for m in slice_to(c, app, f"t_auto_noblock_{era}", enable_support=1, support_type="normal(auto)") if is_support(m["role"])]
    xs = [p for m in auto for p in (m["x0"], m["x1"])]
    assert min(xs) < cx - 15 and max(xs) > cx + 15
    c.tool("paint_apply", targets=[oid], channel="support", op="clear")


# ---------------------------------------------------------------------------------------------
# Scenario 2: logo in white
# ---------------------------------------------------------------------------------------------

def top_camera(cx, cy, z_eye=100.0, half_h=25.0, width=400, height=300):
    half_w = half_h * width / height
    near, far = 1.0, 300.0
    view = [[1, 0, 0, -cx], [0, 1, 0, -cy], [0, 0, 1, -z_eye], [0, 0, 0, 1]]
    proj = [[1 / half_w, 0, 0, 0], [0, 1 / half_h, 0, 0], [0, 0, -2 / (far - near), -(far + near) / (far - near)], [0, 0, 0, 1]]
    return {"projection": "orthographic", "view_matrix": view, "projection_matrix": proj, "width": width, "height": height}, half_w, half_h


@pytest.mark.parametrize("era", ["modern", "legacy"])
def test_scenario_logo_in_white(logo_app, era):
    app, clients = logo_app
    c = clients[era]
    oid = only_object(c)["id"]
    c.tool("paint_apply", targets=[oid], channel="color", op="clear")
    lo, hi = part_box(c, oid)
    cx, cy, z0 = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]

    # Filament 3 does not exist: actionable error.
    err = c.tool_error("paint_apply", targets=[oid], channel="color", state={"filament": 3}, selector={"all": True})
    assert err.code == "invalid_argument" and "filament_edit" in err.hint

    # Point at the logo in a top view and smart-fill it with filament 2: its top face only (sides are 90 deg).
    camera, half_w, half_h = top_camera(cx, cy)
    u, v = (2.0 / half_w) * 200 + 200, (1 - (1.0 / half_h)) * 150
    r = c.tool("paint_apply", targets=[oid], channel="color", state={"filament": 2},
               selector={"connected": {"seed": {"pixel": [u, v], "camera": camera}, "angle_deg": 20}})
    assert r["seeds"][0]["point"][2] == pytest.approx(z0 + 11.2, abs=1e-3)
    assert r["parts"][0]["after"]["filament_2"]["area_mm2"] == pytest.approx(72.0, abs=0.01)  # 12 x 6
    # ... then add its sides with a box: 2 x (12 + 6) x 1.15
    r = c.tool("paint_apply", targets=[oid], channel="color", state={"filament": 2}, resolution_mm=0.1,
               selector={"box": {"min": [cx - 7, cy - 4, z0 + 10.05], "max": [cx + 7, cy + 4, z0 + 12]}})
    # (the box starts just above the slab's top, so neither it nor the logo's hidden bottom face is painted)
    assert r["parts"][0]["after"]["filament_2"]["area_mm2"] == pytest.approx(72.0 + 2 * (12 + 6) * 1.15, rel=0.01)
    scene_obj = only_object(c)
    assert scene_obj["paint"]["color"] is True

    # The colour shows in the normal editor view (no overlay): seen from the top, the logo's top face
    # in filament 2's colour, 12 x 6 mm at the camera's scale.
    from orca_mcp.png import close_to, hex_color, read_png
    shot = c.call("screenshot", {"camera": {"preset": "top", "projection": "orthographic", "target": [oid]},
                                 "size": {"width": 600, "height": 600}, "editor": {"show_bed": False}})
    assert not shot.is_error, shot.data
    png = shot.images[0][1]
    (app.data_dir.parent / "out").mkdir(exist_ok=True)
    (app.data_dir.parent / "out" / f"paint_logo_top_{era}.png").write_bytes(png)
    image, info = read_png(png), shot.data
    second = hex_color(c.tool("scene_get")["filaments"][1]["color"])
    ppmm = info["camera"]["height"] / info["camera"]["ortho_height"]
    assert second == (255, 255, 255)
    background = close_to(hex_color(info["legend"]["background"]), 4)
    assert image.count(lambda px: min(px) > 200 and not background(px)) == pytest.approx(72 * ppmm ** 2, rel=0.06)
    iso = c.call("screenshot", {"camera": {"preset": "iso", "target": [oid]}, "size": {"width": 800, "height": 600}})
    (app.data_dir.parent / "out" / f"paint_logo_iso_{era}.png").write_bytes(iso.images[0][1])

    # Filament 2 is printed only in the logo's layers, and only inside the logo's outline.
    moves = slice_to(c, app, f"logo_{era}")
    second = [m for m in moves if m["tool"] == 1]
    assert second, "filament 2 must be used"
    assert min(m["z"] for m in second) > z0 + 9.0, min(m["z"] for m in second)
    xs = [p for m in second for p in (m["x0"], m["x1"])]
    ys = [p for m in second for p in (m["y0"], m["y1"])]
    assert min(xs) > cx - 6.8 and max(xs) < cx + 6.8 and min(ys) > cy - 3.8 and max(ys) < cy + 3.8
    first_layers = {m["z"] for m in moves if m["tool"] == 0}
    assert min(first_layers) < z0 + 0.5


@pytest.mark.parametrize("era", ["modern", "legacy"])
def test_color_band_changes_filament_in_exactly_its_layers(logo_app, era):
    app, clients = logo_app
    c = clients[era]
    oid = only_object(c)["id"]
    c.tool("paint_apply", targets=[oid], channel="color", op="clear")
    lo, hi = part_box(c, oid)
    z0 = lo[2]
    c.tool("paint_apply", targets=[oid], channel="color", state={"filament": 2}, resolution_mm=0.1,
           selector={"z_range": {"min": z0 + 3.1, "max": z0 + 5.1}})
    moves = slice_to(c, app, f"band_{era}", layer_height=0.2, initial_layer_print_height=0.2)
    zs = sorted({m["z"] for m in moves if m["tool"] == 1})
    # layers 0.2 mm: tops 3.2 .. 5.2 have slice planes (z - 0.1) inside 3.1..5.1
    assert zs and zs[0] == pytest.approx(z0 + 3.2, abs=1e-3) and zs[-1] == pytest.approx(z0 + 5.2, abs=1e-3), zs
    c.tool("paint_apply", targets=[oid], channel="color", op="clear")


# ---------------------------------------------------------------------------------------------
# seam and fuzzy skin
# ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("era", ["modern", "legacy"])
def test_seam_paint_moves_the_seam(cyl_app, era):
    app, clients = cyl_app
    c = clients[era]
    oid = only_object(c)["id"]
    for ch in ("seam", "fuzzy_skin"):
        c.tool("paint_apply", targets=[oid], channel=ch, op="clear")
    lo, hi = part_box(c, oid)
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    # A vertical strip on the +x side (pointing away from the default "aligned/back" choice).
    r = c.tool("paint_apply", targets=[oid], channel="seam", state="enforce", resolution_mm=0.2,
               selector={"box": {"min": [cx + 8, cy - 1.5, lo[2] - 1], "max": [cx + 11, cy + 1.5, hi[2] + 1]}})
    assert r["changed"]
    starts = outer_wall_starts(slice_to(c, app, f"seam_{era}", seam_position="back"))
    assert len(starts) > 20
    near = [z for z, (x, y) in starts.items() if x > cx + 8 and abs(y - cy) < 2.5]
    assert len(near) >= 0.9 * len(starts), (len(near), len(starts))
    c.tool("paint_apply", targets=[oid], channel="seam", op="clear")
    starts = outer_wall_starts(slice_to(c, app, f"seam_none_{era}", seam_position="back"))
    near = [z for z, (x, y) in starts.items() if x > cx + 8 and abs(y - cy) < 2.5]
    assert len(near) < 0.1 * len(starts)


def wall_jitter(moves, cx, cy, r):
    """Per layer: standard deviation of the outer-wall points' distance from the cylinder axis
    (about 0 for a smooth wall, the fuzzy-skin thickness scale when fuzzy)."""
    out = {}
    for m in moves:
        if m["role"] and m["role"].lower() in ("outer wall", "external perimeter"):
            out.setdefault(m["z"], []).append(math.hypot(m["x1"] - cx, m["y1"] - cy))
    def std(v):
        mean = sum(v) / len(v)
        return math.sqrt(sum((x - mean) ** 2 for x in v) / len(v))
    return {z: std(v) for z, v in out.items() if len(v) > 3}


@pytest.mark.parametrize("era", ["modern", "legacy"])
def test_fuzzy_skin_only_where_painted(cyl_app, era):
    app, clients = cyl_app
    c = clients[era]
    oid = only_object(c)["id"]
    c.tool("paint_apply", targets=[oid], channel="fuzzy_skin", op="clear")
    lo, hi = part_box(c, oid)
    cx, cy, z0 = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]
    r = c.tool("paint_apply", targets=[oid], channel="fuzzy_skin", state="fuzzy", resolution_mm=0.1,
               selector={"z_range": {"min": z0 + 3, "max": z0 + 6}})
    # 96-gon of radius 10 x 3 mm; its side triangles are 10 mm tall and 0.65 mm wide
    assert any("fuzzy_skin" in n for n in r["notes"]), "the default (disabled) makes painted fuzzy skin ineffective: say so"
    perimeter = 2 * 96 * 10 * math.sin(math.pi / 96)
    assert r["parts"][0]["after"]["fuzzy"]["area_mm2"] == pytest.approx(perimeter * 3, rel=0.02)
    jitter = wall_jitter(slice_to(c, app, f"fuzzy_{era}", layer_height=0.2, initial_layer_print_height=0.2, fuzzy_skin="none"), cx, cy, 10.0)
    inside = [j for z, j in jitter.items() if z0 + 3.4 <= z <= z0 + 5.8]
    outside = [j for z, j in jitter.items() if z <= z0 + 2.6 or z >= z0 + 6.6]
    assert inside and outside
    assert min(inside) > 0.05 and max(outside) < 0.02 and min(inside) > 3 * max(outside), (min(inside), max(outside))
    c.tool("paint_apply", targets=[oid], channel="fuzzy_skin", op="clear")


# ---------------------------------------------------------------------------------------------
# plate_layer_events
# ---------------------------------------------------------------------------------------------

@pytest.mark.parametrize("era", ["modern", "legacy"])
def test_layer_events(logo_app, era):
    app, clients = logo_app
    c = clients[era]
    oid = only_object(c)["id"]
    c.tool("paint_apply", targets=[oid], channel="color", op="clear")
    c.tool("plate_layer_events", action="clear")
    listed = c.tool("plate_layer_events")
    assert listed["events"] == [] and listed["layers"]["count"] > 10 and "ok" not in listed

    r = c.tool("plate_layer_events", action="add", at={"z": 5.05}, type="filament_change", filament=2)
    ev = r["events"][0]
    assert ev["type"] == "filament_change" and ev["filament"] == 2 and ev["z"] == pytest.approx(5.2, abs=0.11) and ev["color"].startswith("#")
    assert r["slice_invalidated"] is True and r["journal"]
    r = c.tool("plate_layer_events", action="add", at={"layer": 30}, type="pause", message="insert magnet")
    assert [e["type"] for e in r["events"]] == ["filament_change", "pause"] and r["events"][1]["layer"] == 30
    # one event per layer
    err = c.tool_error("plate_layer_events", action="add", at={"layer": 30}, type="pause")
    assert err.code == "invalid_argument" and "remove" in err.hint
    # validation
    assert c.tool_error("plate_layer_events", action="add", at={"z": 500}, type="pause").code == "invalid_argument"
    assert c.tool_error("plate_layer_events", action="add", at={"z": 3}, type="filament_change", filament=7).code == "invalid_argument"
    err = c.tool_error("plate_layer_events", action="add", at={"z": 3}, type="custom", gcode="M117 hi")
    assert err.code == "forbidden_key" and "Preferences" in err.hint

    # The G-code switches to filament 2 at that layer and pauses at layer 30.
    moves = slice_to(c, app, f"events_{era}")
    zs2 = sorted({m["z"] for m in moves if m["tool"] == 1})
    assert zs2 and zs2[0] == pytest.approx(ev["z"], abs=0.21), zs2[:3]
    assert not [m for m in moves if m["tool"] == 1 and m["z"] < ev["z"] - 0.21]

    # Saved in the 3MF.
    rt = c.tool("debug_paint_3mf", path=str(app.data_dir / f"events_{era}.3mf"))
    assert rt["layer_events"]["1"]["equal"] is True and rt["layer_events"]["1"]["events"] == 2

    # remove one, revert through the journal
    r = c.tool("plate_layer_events", action="remove", at={"layer": 30})
    assert [e["type"] for e in r["events"]] == ["filament_change"]
    c.tool("history", action="revert", journal_seq=r["journal"])
    assert [e["type"] for e in c.tool("plate_layer_events")["events"]] == ["filament_change", "pause"]
    r = c.tool("plate_layer_events", action="clear")
    assert r["events"] == []


def test_custom_gcode_allowed_by_preference(launch_app, run_dir, models):
    """With mcp_allow_gcode_edit on, custom G-code is accepted and ends up in the G-code."""
    import json as _json
    from orca_mcp.app import fresh_data_dir, seed_data_dir
    data_dir = fresh_data_dir(run_dir, "paint_gcode_pref")
    seed_data_dir(data_dir)
    conf = data_dir / "OrcaSlicer.conf"
    cfg = _json.loads(conf.read_text(encoding="utf-8"))
    cfg["app"]["mcp_allow_gcode_edit"] = "true"
    conf.write_text(_json.dumps(cfg, indent=4) + "\n", encoding="utf-8", newline="\n")
    from orca_mcp import OrcaApp
    app = OrcaApp(data_dir, files=[str(models["cyl"])]).start()
    try:
        c = McpClient(app.url, app.token, era="modern").connect()
        r = c.tool("plate_layer_events", action="add", at={"layer": 10}, type="custom", gcode="M117 MCP-PAINT-MARKER")
        assert r["events"][0]["type"] == "custom" and r["events"][0]["gcode"] == "M117 MCP-PAINT-MARKER"
        path = app.data_dir / "custom.gcode"
        c.tool("debug_paint_slice", path=str(path))
        assert "M117 MCP-PAINT-MARKER" in path.read_text(encoding="utf-8", errors="replace")
    finally:
        app.stop()
