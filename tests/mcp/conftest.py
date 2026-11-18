"""pytest fixtures for the MCP client tests.

The session starts ONE private OrcaSlicer instance (own data directory, MCP on a free port, debug
tools on) with a sample model loaded, and every test file shares it. Tests must therefore leave
the scene as they found it (undo what they change) or use the `launch_app` factory for an instance
of their own.

Environment:
    ORCA_MCP_APP       path to the orca-slicer executable (default: build/OrcaSlicer/orca-slicer[.exe])
    ORCA_MCP_RUN_DIR   where data directories and outputs go (default: the temp directory of pytest)
    ORCA_MCP_URL, ORCA_MCP_TOKEN   attach to an already running instance instead of launching one
    ORCA_MCP_WINDOW    "minimized" (default) or "normal": how the test instance shows its window
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from orca_mcp import McpClient, OrcaApp, REPO_ROOT, fresh_data_dir  # noqa: E402

# Model loaded into the shared instance: a 20 mm cube, named after its file. (Several files on the
# command line would be merged into one multi-part object, so the fixture uses one.)
SAMPLE_MODEL = "20mm_cube.obj"


class Attached:
    """Stands in for OrcaApp when the tests attach to a running instance."""

    def __init__(self, url: str, token: str):
        self.url = url
        self.token = token
        self.data_dir = None

    def stop(self):
        pass


@pytest.fixture(scope="session")
def run_dir(tmp_path_factory) -> Path:
    base = os.environ.get("ORCA_MCP_RUN_DIR")
    if base:
        path = Path(base)
        path.mkdir(parents=True, exist_ok=True)
        return path
    return tmp_path_factory.mktemp("orca_mcp")


@pytest.fixture(scope="session")
def launch_app(run_dir):
    """Factory: launch_app(name, files=[...], **OrcaApp options) -> started OrcaApp. All are stopped at session end."""
    started = []

    def launch(name: str, files=None, **options) -> OrcaApp:
        app = OrcaApp(fresh_data_dir(run_dir, name), files=files, **options).start()
        started.append(app)
        return app

    yield launch
    for app in started:
        app.stop()


@pytest.fixture(scope="session")
def app(launch_app):
    url = os.environ.get("ORCA_MCP_URL")
    if url:
        return Attached(url, os.environ.get("ORCA_MCP_TOKEN", ""))
    return launch_app("shared", files=[str(REPO_ROOT / "tests" / "data" / SAMPLE_MODEL)])


@pytest.fixture(scope="session")
def modern(app) -> McpClient:
    return McpClient(app.url, app.token, era="modern").connect()


@pytest.fixture(scope="session")
def legacy(app) -> McpClient:
    return McpClient(app.url, app.token, era="legacy").connect()


@pytest.fixture(params=["modern", "legacy"])
def client(request, modern, legacy) -> McpClient:
    """Runs the test once per protocol era."""
    return modern if request.param == "modern" else legacy


@pytest.fixture(scope="session")
def out_dir(run_dir) -> Path:
    path = run_dir / "out"
    path.mkdir(exist_ok=True)
    return path


def objects_by_name(client: McpClient) -> dict:
    """{object name: object dict} from scene_get."""
    scene = client.tool("scene_get")
    found = {}
    for plate in scene["plates"]:
        for obj in plate["objects"]:
            found[obj["name"]] = obj
    for obj in scene.get("off_plate", []):
        found.setdefault(obj["name"], obj)
    return found


def cube(client: McpClient) -> dict:
    """The sample cube as scene_get reports it."""
    return objects_by_name(client)[SAMPLE_MODEL]
