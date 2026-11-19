"""Shared fixtures and helpers of the edit group's tests (test_project.py, test_objects.py, test_plates.py).

The edit tools replace the project, so these tests do not use the shared session instance: each test
module starts its own private instance (``launch_app``) and every test begins with an empty project
(``fresh``). Tests run once per protocol era through the ``ec`` fixture.
"""

from __future__ import annotations

import copy
import math
import time
from pathlib import Path

import pytest

from orca_mcp import McpClient, REPO_ROOT

DATA = REPO_ROOT / "tests" / "data"
RESOURCES = REPO_ROOT / "resources"
CUBE_OBJ = DATA / "20mm_cube.obj"
STL_BOX = DATA / "test_stl" / "ASCII" / "20mmbox-LF.stl"
STEP_FILE = DATA / "cone_tilted_seam_pcurve.step"
STEP_UTF8 = DATA / "utf8_part_names.step"
DRC_FILE = RESOURCES / "handy_models" / "OrcaCube_v2.drc"
PROJECT_3MF = RESOURCES / "handy_models" / "OrcaSliced.3mf"
BADGE_3MF = RESOURCES / "handy_models" / "OrcaBadge.3mf"
TWO_SHELLS = DATA / "two_hollow_squares.obj"


def make_fixtures(name: str):
    """Fixtures for one test module: its own instance, both era clients, and ``ec`` (parametrised by era)."""

    @pytest.fixture(scope="module")
    def edit_app(launch_app):
        return launch_app(name)

    @pytest.fixture(scope="module")
    def emodern(edit_app) -> McpClient:
        return McpClient(edit_app.url, edit_app.token, era="modern", timeout=300).connect()

    @pytest.fixture(scope="module")
    def elegacy(edit_app) -> McpClient:
        return McpClient(edit_app.url, edit_app.token, era="legacy", timeout=300).connect()

    @pytest.fixture(params=["modern", "legacy"])
    def ec(request, emodern, elegacy) -> McpClient:
        client = emodern if request.param == "modern" else elegacy
        fresh(client)
        return client

    return edit_app, emodern, elegacy, ec


# ---------------------------------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------------------------------

def fresh(c: McpClient) -> dict:
    """An empty project, whatever state the previous test left."""
    return c.tool("project_new", on_unsaved="discard")


def workspace(c: McpClient) -> Path:
    roots = c.tool("app_status")["write_roots"]
    ws = next(r for r in roots if r.endswith("/mcp/workspace"))
    return Path(ws)


def scene(c: McpClient, **kw) -> dict:
    return c.tool("scene_get", **kw)


def normalized(s: dict) -> dict:
    """A scene without the fields that change by themselves (revision, dirty flag, slice state)."""
    s = copy.deepcopy(s)
    s.get("project", {}).pop("rev", None)
    s.get("project", {}).pop("dirty", None)
    for plate in s.get("plates", []):
        plate.pop("sliced", None)
        for obj in plate.get("objects", []):
            for inst in obj.get("instances", []):
                inst.pop("outside_plate", None)
    for obj in s.get("off_plate", []):
        for inst in obj.get("instances", []):
            inst.pop("outside_plate", None)
    return s


def all_objects(c: McpClient, detail="normal") -> dict:
    """{id: object} over every plate and off_plate (instances merged per object)."""
    found: dict = {}
    s = scene(c, detail=detail, limit=500)
    for plate in s["plates"]:
        for obj in plate["objects"]:
            if obj["id"] in found and "instances" in obj and isinstance(obj["instances"], list):
                found[obj["id"]]["instances"] += obj["instances"]
            else:
                found[obj["id"]] = copy.deepcopy(obj)
    for obj in s.get("off_plate", []):
        if obj["id"] in found and isinstance(obj.get("instances"), list):
            found[obj["id"]]["instances"] += obj["instances"]
        else:
            found[obj["id"]] = copy.deepcopy(obj)
    return found


def get_object(c: McpClient, oid: str) -> dict:
    return all_objects(c)[oid]


def add_cube(c: McpClient, size=(20, 20, 20), **kw) -> dict:
    return c.tool("object_add_primitive", shape="cube", size=list(size), **kw)["created"][0]


def instance_of(obj: dict, index: int = 0) -> dict:
    return obj["instances"][index]


def bbox_size(inst: dict) -> list:
    b = inst["bbox"]
    return [round(b["max"][i] - b["min"][i], 3) for i in range(3)]


def close(a, b, tol=1e-3) -> bool:
    if isinstance(a, (list, tuple)):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    return abs(a - b) <= tol


def wait_job(c: McpClient, result: dict, timeout: float = 240.0) -> dict:
    """The finished job document of a tool that may answer {status: running}."""
    deadline = time.monotonic() + timeout
    while result.get("status") == "running":
        assert time.monotonic() < deadline, f"job {result.get('job')} did not finish"
        result = c.tool("job_status", job=result["job"], wait_s=30)
    return result


def undo(c: McpClient, steps: int = 1) -> dict:
    return c.tool("history", action="undo", steps=steps)


def redo(c: McpClient, steps: int = 1) -> dict:
    return c.tool("history", action="redo", steps=steps)


def assert_undo_roundtrip(c: McpClient, before: dict, expected_undo_prefix: str = "MCP: "):
    """One undo restores `before`; one redo restores the state after the change."""
    after = normalized(scene(c, limit=500))
    names = c.tool("history")["undo"]
    assert names and names[0].startswith(expected_undo_prefix), names[:3]
    undo(c)
    assert normalized(scene(c, limit=500)) == before
    redo(c)
    assert normalized(scene(c, limit=500)) == after


def plate_count(c: McpClient) -> int:
    return len(scene(c, detail="summary")["plates"])


def angle_between(a, b) -> float:
    dot = sum(x * y for x, y in zip(a, b))
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(x * x for x in b))
    return math.degrees(math.acos(max(-1.0, min(1.0, dot / (na * nb)))))
