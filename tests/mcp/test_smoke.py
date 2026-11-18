"""The shortest end-to-end check: both eras connect, list tools, read the scene, change and undo.

Run this file first after building:  python -m pytest tests/mcp/test_smoke.py
"""

from conftest import SAMPLE_MODEL, cube, objects_by_name


def test_connects_and_lists_tools(client):
    assert client.server_info["name"] == "orcaslicer"
    assert client.instructions and "scene_get" in client.instructions
    names = [tool["name"] for tool in client.list_tools()]
    for expected in ("app_status", "scene_get", "object_update", "history", "job_status"):
        assert expected in names


def test_reads_the_loaded_scene(client):
    status = client.tool("app_status")
    assert status["app"] and status["version"]
    model = cube(client)
    assert model["id"].startswith("o")
    assert [round(v) for v in model["size"]] == [20, 20, 20]


def test_change_and_undo(modern):
    model = cube(modern)
    result = modern.tool("object_update", targets=[model["id"]], name="renamed cube")
    assert result["ok"] and result["undo"].startswith("MCP: ")
    assert "renamed cube" in objects_by_name(modern)
    undone = modern.tool("history", action="undo")
    assert undone["undone"] == [result["undo"]]
    assert SAMPLE_MODEL in objects_by_name(modern)
