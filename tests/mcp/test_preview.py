"""screenshot view "preview": pictures of the sliced toolpaths.

Semantic checks (where, how large, which colours, what the legend says), not golden images. Every
picture is also written to <run dir>/out/preview/ for a look by eye.
"""

from __future__ import annotations

import json

import pytest

from orca_mcp import McpClient, REPO_ROOT
from orca_mcp.png import read_png

DATA = REPO_ROOT / "tests" / "data"
HANDY = REPO_ROOT / "resources" / "handy_models"
CUBE = DATA / "20mm_cube.obj"

PRESETS = ["iso", "iso_left", "iso_back", "iso_back_left", "top", "bottom", "front", "back", "left", "right"]
COLOR_BY = ["feature", "filament", "color_print", "speed", "actual_speed", "layer_height", "line_width", "flow", "actual_flow",
            "layer_time", "layer_time_log", "fan_speed", "temperature", "pressure_advance", "acceleration", "jerk"]


@pytest.fixture(scope="module")
def pv_app(launch_app, out_dir):
    roots = ";".join(str(p) for p in (DATA, HANDY, out_dir))
    return launch_app("preview", files=[str(CUBE)], extra_env={"ORCA_MCP_ROOTS": roots})


@pytest.fixture(scope="module")
def m(pv_app) -> McpClient:
    return McpClient(pv_app.url, pv_app.token, era="modern", timeout=320).connect()


@pytest.fixture(scope="module")
def lg(pv_app) -> McpClient:
    return McpClient(pv_app.url, pv_app.token, era="legacy", timeout=320).connect()


@pytest.fixture(params=["modern", "legacy"])
def c(request, m, lg) -> McpClient:
    return m if request.param == "modern" else lg


@pytest.fixture(scope="module")
def shots(out_dir):
    path = out_dir / "preview"
    path.mkdir(exist_ok=True)
    return path


def has_preview(client) -> bool:
    for tool in client.list_tools():
        if tool["name"] == "screenshot":
            return "preview" in tool["inputSchema"]["properties"]["view"]["enum"]
    return False


@pytest.fixture(scope="module")
def sliced(m):
    if not has_preview(m):
        pytest.skip("screenshot view preview not built in")
    r = m.tool("slice", wait_s=200)
    assert r["status"] == "finished", r
    return r


def shot(client, shots, name, **arguments):
    arguments.setdefault("view", "preview")
    result = client.call("screenshot", arguments)
    assert not result.is_error, result.data
    assert len(result.images) == 1
    mime, data = result.images[0]
    assert mime == "image/png"
    (shots / f"{name}.png").write_bytes(data)
    (shots / f"{name}.json").write_text(json.dumps(result.data, indent=1), encoding="utf-8")
    return result.data, read_png(data)


def coloured(pixel):
    return max(pixel) - min(pixel) > 40


def test_preview_listed(c):
    if not has_preview(c):
        pytest.skip("screenshot view preview not built in")
    tool = next(t for t in c.list_tools() if t["name"] == "screenshot")
    assert "preview" in tool["inputSchema"]["properties"]
    assert "toolpaths" in tool["description"]


def test_preview_before_slicing(launch_app):
    app = launch_app("preview_unsliced", files=[str(CUBE)])
    client = McpClient(app.url, app.token, era="modern").connect()
    if not has_preview(client):
        pytest.skip("screenshot view preview not built in")
    err = client.tool_error("screenshot", view="preview")
    assert err.code == "not_sliced"
    assert "editor" in err.hint
    app.stop()


def test_default_preview(c, sliced, shots):
    info, img = shot(c, shots, f"default_{c.era}")
    assert info["view"] == "preview" and info["size"] == [1024, 768]
    legend = info["legend"]
    assert legend["kind"] == "categories"
    keys = {item["key"] for item in legend["items"]}
    assert {"outer_wall", "inner_wall"} <= keys, keys
    assert abs(sum(i.get("percent", 0) for i in legend["items"]) - 100) < 5
    seen = info["objects_in_view"][0]
    assert coloured(img.pixel(*map(int, seen["pixel"])))


def test_named_views_match_editor_camera(m, sliced, shots):
    for preset in PRESETS:
        info, img = shot(m, shots, f"view_{preset}", camera={"preset": preset})
        box = img.bounding_box(coloured)
        assert box is not None, preset
        editor = m.call("screenshot", {"view": "editor", "camera": {"preset": preset}})
        cam_e, cam_p = editor.data["camera"], info["camera"]
        # Same request -> same camera as the editor picture: pictures of a plate line up.
        assert cam_e["azimuth_deg"] == cam_p["azimuth_deg"] and cam_e["elevation_deg"] == cam_p["elevation_deg"]
        for a, b in zip(cam_e["position"], cam_p["position"]):
            assert abs(a - b) < 1.0, (preset, cam_e["position"], cam_p["position"])
        # The cube's pixel box agrees between the editor and the preview picture.
        pe = editor.data["objects_in_view"][0]["pixel_box"]
        pp = info["objects_in_view"][0]["pixel_box"]
        assert all(abs(a - b) <= 2 for a, b in zip(pe, pp)), (preset, pe, pp)


def test_every_color_by(m, sliced, shots):
    for color_by in COLOR_BY:
        info, img = shot(m, shots, f"color_{color_by}", preview={"color_by": color_by}, size={"width": 512, "height": 384})
        legend = info["legend"]
        assert legend["kind"] in ("categories", "range"), (color_by, legend)
        if legend["kind"] == "range":
            assert legend["range"][0] <= legend["range"][1]
            assert legend["items"]
        assert img.bounding_box(coloured) is not None or color_by in ("fan_speed", "temperature", "pressure_advance", "jerk")


def test_layer_sweep_grows(m, sliced, shots):
    layers = m.tool("slice_result")["summary"]["layers"]
    areas = []
    for top in (1, layers // 4, layers // 2, layers):
        info, img = shot(m, shots, f"sweep_front_{top}", camera={"preset": "front"}, preview={"layers": {"from": 1, "to": top}},
                         size={"width": 512, "height": 384})
        areas.append(img.count(coloured, step=2))
    assert areas == sorted(areas) and areas[0] < areas[-1], areas


def test_section_top_view(m, sliced, shots):
    info, img = shot(m, shots, "section_z10_top", camera={"preset": "top", "projection": "orthographic"},
                     preview={"z_range": {"min": 9.9, "max": 10.1}, "section": True})
    box = img.bounding_box(coloured)
    assert box is not None
    u0, v0, u1, v1 = box
    # Top view of a 20 mm square section: square on screen.
    assert abs((u1 - u0) - (v1 - v0)) <= 4, box


def test_toggles(m, sliced, shots):
    _, plain = shot(m, shots, "travel_off", camera={"preset": "top"}, size={"width": 512, "height": 384})
    info, travel = shot(m, shots, "travel_on", camera={"preset": "top"}, preview={"show": {"travel": True}}, size={"width": 512, "height": 384})
    assert any(o["key"] in ("travels", "travel") for o in info["legend"].get("options", [])), info["legend"]
    info, hidden = shot(m, shots, "hide_walls", camera={"preset": "top"},
                        preview={"hide_features": ["outer_wall", "Inner wall"]}, size={"width": 512, "height": 384})
    hidden_keys = {i["key"] for i in info["legend"]["items"] if not i.get("visible", True)}
    assert {"outer_wall", "inner_wall"} <= hidden_keys
    err = m.tool_error("screenshot", view="preview", preview={"hide_features": ["walls_of_jericho"]})
    assert err.code == "invalid_argument"


def test_bad_options(m, sliced):
    assert m.call("screenshot", {"view": "preview", "preview": {"color_by": "mood"}}).error_code == "invalid_argument"
    assert m.call("screenshot", {"view": "preview", "preview": {"layers": {"from": 5, "to": 2}}}).error_code == "invalid_argument"


def test_user_preview_not_disturbed(m, sliced, shots):
    before_window = m.tool("debug_window")
    before_viewer = m.tool("debug_slice_fixture", action="viewer_state")
    for preset in ("iso", "top"):
        shot(m, shots, f"undisturbed_{preset}", camera={"preset": preset}, preview={"layers": {"from": 2, "to": 5}, "color_by": "speed"})
    after_window = m.tool("debug_window")
    after_viewer = m.tool("debug_slice_fixture", action="viewer_state")
    assert before_window["user_camera"] == after_window["user_camera"]
    assert before_window["preview_tab"] == after_window["preview_tab"]
    assert before_viewer["layers_view_range"] == after_viewer["layers_view_range"]
