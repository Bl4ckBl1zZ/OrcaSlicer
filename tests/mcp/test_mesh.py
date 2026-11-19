"""mesh_query: stats, overhangs, flat faces, ray casting, distance."""

import pytest

from conftest import cube
from orca_mcp import McpClient, REPO_ROOT


@pytest.fixture(scope="module")
def overhang_app(launch_app):
    """An instance of its own with tests/data/overhang.obj: a model that does have overhangs."""
    return launch_app("mesh_overhang", files=[str(REPO_ROOT / "tests" / "data" / "overhang.obj")])


def test_stats_of_the_cube(client):
    model = cube(client)
    stats = client.tool("mesh_query", kind="stats", targets=[model["id"]])["stats"][0]
    assert stats["id"] == model["id"] and stats["facets"] == 12
    assert abs(stats["volume_mm3"] - 8000) < 0.01 and abs(stats["area_mm2"] - 2400) < 0.01
    assert stats["manifold"] is True and stats["open_edges"] == 0 and stats["shells"] == 1
    assert stats["size"] == [20.0, 20.0, 20.0]
    assert stats["bbox"] == model["instances"][0]["bbox"], "boxes are in plate coordinates, like scene_get"
    # A volume id and an instance id address the same geometry here.
    by_volume = client.tool("mesh_query", kind="stats", targets=[model["volumes"][0]["id"], model["instances"][0]["id"]])["stats"]
    assert [entry["volume_mm3"] for entry in by_volume] == [stats["volume_mm3"]] * 2


def test_cube_has_no_overhangs(client):
    model = cube(client)
    report = client.tool("mesh_query", kind="overhangs", targets=[model["id"]])["overhangs"][0]
    assert report["area_mm2"] == 0 and report["facets"] == 0 and report["clusters"] == [] and report["threshold_deg"] == 45
    # Even at 90 degrees: the only downward face rests on the bed.
    assert client.tool("mesh_query", kind="overhangs", targets=[model["id"]], threshold_deg=90)["overhangs"][0]["area_mm2"] == 0


def test_overhang_model(overhang_app):
    client = McpClient(overhang_app.url, overhang_app.token).connect()
    obj = client.tool("scene_get")["plates"][0]["objects"][0]
    box = obj["instances"][0]["bbox"]
    report = client.tool("mesh_query", kind="overhangs", targets=[obj["id"]], threshold_deg=45)["overhangs"][0]
    assert report["area_mm2"] > 1 and report["facets"] > 0 and 0 < report["share_of_surface"] < 1
    assert report["lowest_z"] > 0.05, "overhangs are above the bed"
    cluster = report["clusters"][0]
    assert cluster["volume"] == obj["volumes"][0]["id"] and cluster["area_mm2"] <= report["area_mm2"] + 1e-6
    for axis in range(3):
        assert box["min"][axis] - 1e-3 <= cluster["box"]["min"][axis] <= cluster["box"]["max"][axis] <= box["max"][axis] + 1e-3
    assert report["cluster_count"] >= len(report["clusters"]) >= 1
    # A wider threshold finds at least as much.
    wider = client.tool("mesh_query", kind="overhangs", targets=[obj["id"]], threshold_deg=80)["overhangs"][0]
    assert wider["area_mm2"] >= report["area_mm2"]
    narrow = client.tool("mesh_query", kind="overhangs", targets=[obj["id"]], threshold_deg=1, limit=1)["overhangs"][0]
    assert narrow["area_mm2"] <= report["area_mm2"] and len(narrow["clusters"]) <= 1


def test_flat_faces_of_the_cube(client):
    model = cube(client)
    faces = client.tool("mesh_query", kind="flat_faces", targets=[model["id"]])["flat_faces"][0]["faces"]
    assert len(faces) == 6 and [face["face"] for face in faces] == list(range(6))
    assert all(abs(face["area_mm2"] - 400) < 0.01 for face in faces)
    normals = sorted(tuple(round(n) for n in face["normal"]) for face in faces)
    assert normals == sorted([(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)])
    assert [face for face in faces if face.get("on_bed")] == [face for face in faces if round(face["normal"][2]) == -1]


def test_raycast_from_above(client):
    model = cube(client)
    centre = model["instances"][0]["center"]
    result = client.tool("mesh_query", kind="raycast", origin=[centre[0] + 3, centre[1] - 2, 100], direction=[0, 0, -2])
    hit = result["hit"]
    assert hit["object"] == model["id"] and hit["instance"] == model["instances"][0]["id"] and hit["volume"] == model["volumes"][0]["id"]
    assert hit["point"] == [centre[0] + 3, centre[1] - 2, 20.0] and hit["normal"] == [0.0, 0.0, 1.0] and hit["distance"] == 80.0
    assert 0 <= hit["facet"] < 12
    assert result["ray"]["direction"] == [0.0, 0.0, -1.0]
    # Beside the cube the ray hits nothing.
    assert client.tool("mesh_query", kind="raycast", origin=[centre[0] + 30, centre[1], 100], direction=[0, 0, -1])["hit"] is None
    # From the front: the face at y = centre - 10, normal towards -Y.
    front = client.tool("mesh_query", kind="raycast", origin=[centre[0], 0, 10], direction=[0, 1, 0], targets=[model["id"]])["hit"]
    assert front["point"] == [centre[0], centre[1] - 10, 10.0] and front["normal"] == [0.0, -1.0, 0.0]


def test_pixel_of_a_screenshot_hits_the_object_shown_there(client):
    """Round trip: look at the picture, point at a pixel, get the point on the model."""
    model = cube(client)
    for camera_args in ({"preset": "iso"}, {"preset": "top", "projection": "orthographic"}, {"azimuth_deg": 250, "elevation_deg": 15, "target": "objects"}):
        shot = client.call("screenshot", {"camera": camera_args, "size": {"width": 640, "height": 480}}).data
        seen = shot["objects_in_view"][0]
        hit = client.tool("mesh_query", kind="raycast", pixel=seen["pixel"], camera=shot["camera"])["hit"]
        assert hit is not None and hit["object"] == model["id"], camera_args
        box = model["instances"][0]["bbox"]
        for axis in range(3):
            assert box["min"][axis] - 1e-3 <= hit["point"][axis] <= box["max"][axis] + 1e-3
        # A corner of the picture shows the background: nothing is hit.
        assert client.tool("mesh_query", kind="raycast", pixel=[2, 2], camera=shot["camera"])["hit"] is None


def test_distance(client):
    result = client.tool("mesh_query", kind="distance", a=[0, 0, 0], b=[3, 4, 12])
    assert result == {"distance": 13.0, "delta": [3.0, 4.0, 12.0], "horizontal": 5.0}


def test_errors(client):
    model = cube(client)
    assert client.tool_error("mesh_query", kind="stats").code == "invalid_argument"
    assert client.tool_error("mesh_query", kind="stats", targets=["o999999"]).code == "stale_id"
    assert client.tool_error("mesh_query", kind="volume", targets=[model["id"]]).code == "invalid_argument"
    assert client.tool_error("mesh_query", kind="raycast").code == "invalid_argument"
    assert client.tool_error("mesh_query", kind="raycast", pixel=[10, 10]).code == "invalid_argument"
    failure = client.tool_error("mesh_query", kind="raycast", pixel=[10, 10], camera={"projection": "perspective"})
    assert failure.code == "invalid_argument" and "screenshot" in failure.hint
    assert client.tool_error("mesh_query", kind="raycast", origin=[0, 0, 0], direction=[0, 0, 0]).code == "invalid_argument"
    assert client.tool_error("mesh_query", kind="distance", a=[0, 0, 0]).code == "invalid_argument"
    assert client.tool_error("mesh_query", kind="overhangs", targets=[model["id"]], threshold_deg=120).code == "invalid_argument"
