"""screenshot (editor view): the pictures show what the camera parameters say they should.

Checks are semantic (where is the object, how large, which colours), not golden images, so they
hold across GPUs and drivers. Pictures are also written to <run dir>/out for a look by eye.
"""

import json

import pytest

from conftest import cube
from orca_mcp.png import close_to, hex_color, read_png


def is_object(pixel):
    """A strongly coloured pixel: the sample cube in its filament colour, as opposed to the grey bed and background."""
    return max(pixel) - min(pixel) > 50


def shot(client, out_dir, name, **arguments):
    """Take a screenshot; returns (info, Image or None for JPEG, raw bytes). Saves the picture as out/<name>."""
    result = client.call("screenshot", arguments)
    assert not result.is_error, result.data
    assert len(result.images) == 1
    mime, data = result.images[0]
    path = out_dir / f"{name}.{'png' if mime == 'image/png' else 'jpg'}"
    path.write_bytes(data)
    (out_dir / f"{name}.json").write_text(json.dumps(result.data, indent=1), encoding="utf-8")
    return result.data, (read_png(data) if mime == "image/png" else None), data


def apply_matrix(matrix, vector):
    return [sum(matrix[r][c] * vector[c] for c in range(4)) for r in range(4)]


def project(camera, point):
    """World point -> pixel, from the matrices the tool returned."""
    eye = apply_matrix(camera["view_matrix"], list(point) + [1.0])
    clip = apply_matrix(camera["projection_matrix"], eye)
    ndc_x, ndc_y = clip[0] / clip[3], clip[1] / clip[3]
    return (ndc_x * 0.5 + 0.5) * camera["width"], (1 - (ndc_y * 0.5 + 0.5)) * camera["height"]


# ---------------------------------------------------------------------------------------------

def test_default_view_in_both_eras(client, out_dir):
    info, image, _ = shot(client, out_dir, f"default_{client.era}")
    assert info["view"] == "editor" and info["size"] == [1024, 768] and info["format"] == "png" and info["plate"] == 1
    assert (image.width, image.height) == (1024, 768)
    camera = info["camera"]
    assert camera["projection"] == "perspective" and camera["azimuth_deg"] == 45 and camera["elevation_deg"] == 35
    # Corners are background, the middle of the picture is the bed, the cube is where the tool says.
    background = hex_color(info["legend"]["background"])
    assert close_to(background, 3)(image.pixel(5, 5)) and close_to(background, 3)(image.pixel(1018, 5))
    seen = info["objects_in_view"][0]
    assert seen["id"] == cube(client)["id"] and seen["in_picture"] is True
    assert is_object(image.pixel(*seen["pixel"]))
    box = image.bounding_box(is_object)
    # The axes are coloured too and lie outside the cube; only compare the cube's own box loosely.
    u0, v0, u1, v1 = seen["pixel_box"]
    assert image.count(is_object, (u0, v0, u1 + 1, v1 + 1)) > 0.5 * (u1 - u0) * (v1 - v0)
    assert box is not None


def test_top_orthographic_scale_and_position(modern, out_dir):
    info, image, _ = shot(modern, out_dir, "top_ortho", camera={"preset": "top", "projection": "orthographic"},
                          size={"width": 800, "height": 800}, editor={"show_bed": False})
    camera = info["camera"]
    assert camera["projection"] == "orthographic" and camera["elevation_deg"] == 90
    # Without the bed the only coloured thing is the cube: a square of 20 mm at the camera's scale.
    u0, v0, u1, v1 = image.bounding_box(is_object)
    pixels_per_mm = camera["height"] / camera["ortho_height"]
    side = 20.0 * pixels_per_mm
    assert abs((u1 - u0 + 1) - side) <= 2 and abs((v1 - v0 + 1) - side) <= 2
    # ... at the place the scene reports, in plate coordinates: X to the right, Y up.
    instance = cube(modern)["instances"][0]
    expected = project(camera, [instance["center"][0], instance["center"][1], 10.0])
    assert abs((u0 + u1) / 2 - expected[0]) <= 1.5 and abs((v0 + v1) / 2 - expected[1]) <= 1.5
    seen = info["objects_in_view"][0]
    assert abs(seen["pixel_box"][0] - u0) <= 2 and abs(seen["pixel_box"][2] - u1) <= 2
    assert abs(seen["pixel_box"][1] - v0) <= 2 and abs(seen["pixel_box"][3] - v1) <= 2
    # The square is filled: its area in pixels matches.
    assert abs(image.count(is_object) - side * side) < 0.05 * side * side


def test_bed_grid_and_axes(modern, out_dir):
    info, image, _ = shot(modern, out_dir, "top_bed", camera={"preset": "top", "projection": "orthographic"}, size={"width": 800, "height": 800})
    camera = info["camera"]
    bed = modern.tool("app_status")["printer"]["bed"]
    fill = hex_color(info["legend"]["bed"]["fill"])
    # A point in the middle of a grid cell is bed-coloured; outside the plate it is background.
    u, v = project(camera, [5.0, 5.0, 0.0])
    assert close_to(fill, 6)(image.pixel(round(u), round(v)))
    u, v = project(camera, [bed[0] + 6.0, bed[1] / 2, 0.0])
    assert close_to(hex_color(info["legend"]["background"]), 4)(image.pixel(round(u), round(v)))
    # X axis red to the right of the origin, Y axis green above it.
    ou, ov = project(camera, [0.0, 0.0, 0.0])
    # (One-pixel lines are antialiased, so the colours are blended with what is underneath.)
    red = image.count(lambda p: p[0] > p[1] + 40 and p[0] > p[2] + 40, (round(ou), round(ov) - 3, round(ou) + 60, round(ov) + 4))
    green = image.count(lambda p: p[1] > p[0] + 30 and p[1] > p[2] + 30, (round(ou) - 3, round(ov) - 60, round(ou) + 4, round(ov)))
    assert red > 20 and green > 20, (red, green)


@pytest.mark.parametrize("preset, azimuth, elevation", [("front", 0, 0), ("right", 90, 0), ("back", 180, 0), ("left", 270, 0),
                                                         ("top", 0, 90), ("bottom", 0, -90), ("iso", 45, 35), ("iso_left", 315, 35),
                                                         ("iso_back", 135, 35), ("iso_back_left", 225, 35)])
def test_presets(modern, out_dir, preset, azimuth, elevation):
    info, image, _ = shot(modern, out_dir, f"preset_{preset}", camera={"preset": preset, "target": "objects"}, size={"width": 480, "height": 360})
    camera = info["camera"]
    assert camera["azimuth_deg"] == azimuth and camera["elevation_deg"] == elevation
    # The camera is where the angles say, looking at the cube, which fills most of the height.
    instance = cube(modern)["instances"][0]
    dx = camera["position"][0] - instance["center"][0]
    dy = camera["position"][1] - instance["center"][1]
    dz = camera["position"][2] - 10.0
    if elevation == 90:
        assert dz > 0 and abs(dx) < 1e-3 and abs(dy) < 1e-3
    elif elevation == -90:
        assert dz < 0 and abs(dx) < 1e-3
    else:
        expected = {0: (0, -1), 90: (1, 0), 180: (0, 1), 270: (-1, 0), 45: (1, -1), 315: (-1, -1), 135: (1, 1), 225: (-1, 1)}[azimuth]
        assert (dx > 1e-3) - (dx < -1e-3) == expected[0] and (dy > 1e-3) - (dy < -1e-3) == expected[1]
    u0, v0, u1, v1 = info["objects_in_view"][0]["pixel_box"]
    assert 0 <= u0 < u1 <= 480 and 0 <= v0 < v1 <= 360
    assert (v1 - v0) > 0.5 * 360
    assert is_object(image.pixel(*info["objects_in_view"][0]["pixel"]))


def test_camera_invariants(modern, out_dir):
    _, _, a = shot(modern, out_dir, "inv_az30", camera={"azimuth_deg": 30, "elevation_deg": 25})
    _, _, b = shot(modern, out_dir, "inv_az390", camera={"azimuth_deg": 390, "elevation_deg": 25})
    assert a == b, "azimuth is periodic"
    _, _, c = shot(modern, out_dir, "inv_top", camera={"preset": "top"})
    _, _, d = shot(modern, out_dir, "inv_el90", camera={"azimuth_deg": 0, "elevation_deg": 90})
    assert c == d
    _, _, e = shot(modern, out_dir, "inv_repeat", camera={"azimuth_deg": 30, "elevation_deg": 25})
    assert a == e, "the same call gives the same picture"


def test_zoom_and_distance(modern, out_dir):
    one, _, _ = shot(modern, out_dir, "zoom1", camera={"preset": "iso", "target": "objects"})
    two, _, _ = shot(modern, out_dir, "zoom2", camera={"preset": "iso", "target": "objects", "zoom": 2})
    assert abs(two["camera"]["distance"] * 2 - one["camera"]["distance"]) < 1e-3
    b1, b2 = one["objects_in_view"][0]["pixel_box"], two["objects_in_view"][0]["pixel_box"]
    assert 1.8 < (b2[2] - b2[0]) / (b1[2] - b1[0]) < 2.4
    far, _, _ = shot(modern, out_dir, "distance500", camera={"preset": "iso", "target": "objects", "distance": 500})
    assert far["camera"]["distance"] == 500
    ortho, _, _ = shot(modern, out_dir, "ortho_zoom", camera={"preset": "front", "target": "objects", "projection": "orthographic", "zoom": 0.5})
    assert ortho["camera"]["ortho_height"] > 20 / 0.9 / 0.5 - 1


def test_target_point_and_ids(modern, out_dir):
    model = cube(modern)
    instance = model["instances"][0]
    by_id, _, _ = shot(modern, out_dir, "target_id", camera={"preset": "iso", "target": [model["id"]]})
    by_objects, _, _ = shot(modern, out_dir, "target_objects", camera={"preset": "iso", "target": "objects"})
    assert by_id["camera"]["target"] == by_objects["camera"]["target"]
    # A point target is in plate coordinates and ends up in the middle of the picture.
    point = [instance["center"][0], instance["center"][1], 20.0]
    at_point, _, _ = shot(modern, out_dir, "target_point", camera={"preset": "iso", "target": point})
    u, v = project(at_point["camera"], point)
    assert abs(u - 512) < 1 and abs(v - 384) < 1


def test_color_schemes_and_highlight(modern, out_dir):
    model = cube(modern)
    args = {"camera": {"preset": "iso", "target": "objects"}, "size": {"width": 400, "height": 300}}
    by_type, image, _ = shot(modern, out_dir, "color_type", editor={"color_by": "type"}, **args)
    assert "part" in by_type["legend"]["colors"]
    assert not is_object(image.pixel(*by_type["objects_in_view"][0]["pixel"])), "parts are grey in the type scheme"
    by_object, image, _ = shot(modern, out_dir, "color_object", editor={"color_by": "object"}, **args)
    assert model["id"] in by_object["legend"]["colors"]
    highlighted, image, _ = shot(modern, out_dir, "highlight", editor={"highlight": [model["id"]]}, **args)
    assert highlighted["legend"]["colors"]["highlight"] == "#FF6B00"
    pixel = image.pixel(*highlighted["objects_in_view"][0]["pixel"])
    assert pixel[0] > 100 and pixel[0] > 2 * pixel[1] > 2 * pixel[2], f"orange expected, got {pixel}"
    seethrough, image, _ = shot(modern, out_dir, "transparent", editor={"transparent": [model["id"]]}, **args)
    opaque, opaque_image, _ = shot(modern, out_dir, "opaque", **args)
    centre = seethrough["objects_in_view"][0]["pixel"]
    assert image.pixel(*centre) != opaque_image.pixel(*centre)
    # "only" with another id than the cube's draws nothing of it.
    hidden, image, _ = shot(modern, out_dir, "only_volume", editor={"only": [model["volumes"][0]["id"]], "show_bed": False}, **args)
    assert image.count(is_object) > 1000


def test_jpeg(modern, out_dir):
    info, image, data = shot(modern, out_dir, "jpeg", format="jpeg", size={"width": 320, "height": 240})
    assert info["format"] == "jpeg" and image is None and data[:3] == b"\xff\xd8\xff"


def test_the_users_view_is_untouched_in_every_window_state(modern, out_dir):
    """Rendering works with the window minimised and on the Preview tab, and never moves the user's camera.

    Test instances start minimised (ORCA_MCP_WINDOW); run with ORCA_MCP_WINDOW=normal to also compare
    a window on screen with a minimised one.
    """
    args = {"camera": {"azimuth_deg": 200, "elevation_deg": 50}, "size": {"width": 640, "height": 480}}
    before = modern.tool("debug_window")
    _, _, reference = shot(modern, out_dir, "state_minimised" if before["minimized"] else "state_normal", **args)
    assert modern.tool("debug_window")["user_camera"] == before["user_camera"]
    try:
        modern.tool("debug_window", action="preview")
        assert modern.tool("debug_window")["preview_tab"] is True  # the switch takes an event-loop turn
        _, _, on_preview = shot(modern, out_dir, "state_preview_tab", **args)
        assert on_preview == reference
        modern.tool("debug_window", action="prepare")

        if not before["minimized"]:
            assert modern.tool("debug_window", action="minimize")["minimized"] is True
            _, _, minimised = shot(modern, out_dir, "state_minimised", **args)
            assert minimised == reference
    finally:
        modern.tool("debug_window", action="prepare")
        state = modern.tool("debug_window", action="minimize" if before["minimized"] else "restore")
    assert state["minimized"] == before["minimized"]
    assert state["user_camera"] == before["user_camera"]


def test_errors(modern):
    assert modern.tool_error("screenshot", editor={"only": ["o999999"]}).code == "stale_id"
    assert modern.tool_error("screenshot", plate=7).code == "stale_id"
    assert modern.tool_error("screenshot", camera={"target": ["o999999"]}).code == "stale_id"
    failure = modern.tool_error("screenshot", camera={"projection": "orthographic", "distance": 100})
    assert failure.code == "invalid_argument" and "zoom" in failure.hint
    for bad in ({"size": {"width": 10, "height": 10}}, {"size": {"width": 5000, "height": 100}}, {"camera": {"preset": "sideways"}},
                {"camera": {"elevation_deg": 120}}, {"camera": {"zoom": 0}}, {"view": "xray"}, {"editor": {"colour_by": "type"}},
                {"format": "gif"}):
        assert modern.tool_error("screenshot", **bad).code == "invalid_argument", bad
