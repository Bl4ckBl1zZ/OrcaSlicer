"""Launch a private OrcaSlicer instance for MCP tests.

Every instance gets its own data directory (``--datadir``), so it shares no settings, profiles,
login, single-instance lock or WebView2 profile with an OrcaSlicer the user may be running, and the
Bambu Linux bridge is switched off (``SLICER_LINUX_RUNTIME_ENABLED=0``) so no WSL distro is touched.
The MCP server is enabled through environment variables only; nothing is written to preferences.
"""

from __future__ import annotations

import json
import os
import secrets
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

# Printer used by the fixture data directory: a generic one, so no vendor plugin is involved.
FIXTURE_PRINTER = {"vendor": "Custom", "model": "Generic Klipper Printer", "nozzle_diameter": "0.4"}


def find_app() -> Path:
    """The application under test: $ORCA_MCP_APP, else the install folder of the default build tree."""
    env = os.environ.get("ORCA_MCP_APP")
    if env:
        return Path(env)
    name = "orca-slicer.exe" if sys.platform == "win32" else "orca-slicer"
    for candidate in (REPO_ROOT / "build" / "OrcaSlicer" / name, REPO_ROOT / "build" / "package" / "bin" / name):
        if candidate.exists():
            return candidate
    raise FileNotFoundError("Set ORCA_MCP_APP to the orca-slicer executable to test.")


def seed_data_dir(data_dir: Path) -> None:
    """Write the minimum configuration that skips the first-run wizard: one printer, wizard finished.

    The application fills in everything else on first start (system profiles are copied from the
    resources folder).
    """
    data_dir.mkdir(parents=True, exist_ok=True)
    conf = data_dir / "OrcaSlicer.conf"
    if conf.exists():
        return
    config = {
        "app": {
            "language": "en_US",
            "region": "Others",
            "show_splash_screen": "false",
            "show_daily_tips": "false",
            "stealth_mode": "true",  # no cloud, no update check
            "check_stable_update_only": "true",
            "single_instance": "false",
            "auto_calculate_flush": "disabled",
            "default_page": "1",
        },
        "firstguide": {"finish": "true", "privacyuse": "false"},
        "models": [FIXTURE_PRINTER],
    }
    conf.write_text(json.dumps(config, indent=4) + "\n", encoding="utf-8", newline="\n")


class OrcaApp:
    """One running test instance. Use as a context manager, or call start() / stop()."""

    def __init__(self, data_dir: Path, exe: Path | None = None, files: list[str] | None = None,
                 port: int = 0, token: str | None = None, debug_tools: bool = True,
                 extra_env: dict[str, str] | None = None, start_timeout: float = 120.0, detached: bool = False,
                 window: str = "minimized"):
        self.exe = Path(exe) if exe else find_app()
        self.data_dir = Path(data_dir)
        self.files = [str(f) for f in (files or [])]
        self.port = port
        self.token = token or secrets.token_urlsafe(24)
        self.debug_tools = debug_tools
        self.extra_env = dict(extra_env or {})
        self.start_timeout = start_timeout
        self.detached = detached  # keep running after this Python process exits (mcpctl)
        # "minimized" (default) or "normal"; $ORCA_MCP_WINDOW overrides.
        self.window = os.environ.get("ORCA_MCP_WINDOW", window)
        self.process: subprocess.Popen | None = None
        self.endpoint: dict | None = None

    # -- lifecycle -----------------------------------------------------------------------------
    def start(self) -> "OrcaApp":
        seed_data_dir(self.data_dir)
        endpoint_file = self.data_dir / "mcp" / "endpoint.json"
        if endpoint_file.exists():
            endpoint_file.unlink()

        env = dict(os.environ)
        env.update({
            "SLICER_LINUX_RUNTIME_ENABLED": "0",
            "ORCA_MCP_ENABLE": "1",
            "ORCA_MCP_PORT": str(self.port),  # 0 = any free port, reported in endpoint.json
            "ORCA_MCP_TOKEN": self.token,
            "ORCA_MCP_DEBUG_TOOLS": "1" if self.debug_tools else "0",
            # The server minimises the window once the application is up, so that a test run does
            # not get in the way of whoever is using the machine. Rendering works minimised.
            "ORCA_MCP_WINDOW": self.window,
        })
        env.update(self.extra_env)

        args = [str(self.exe), "--datadir", str(self.data_dir)] + self.files
        flags = 0
        if self.detached and sys.platform == "win32":
            flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
        self.process = subprocess.Popen(args, env=env, cwd=str(self.exe.parent), creationflags=flags,
                                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                        start_new_session=self.detached and sys.platform != "win32")
        deadline = time.monotonic() + self.start_timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"OrcaSlicer exited during start-up with code {self.process.returncode}; "
                                   f"see {self.data_dir / 'log'}")
            if endpoint_file.exists():
                try:
                    endpoint = json.loads(endpoint_file.read_text(encoding="utf-8"))
                except (OSError, ValueError):
                    endpoint = None
                if endpoint and self._owns(endpoint.get("pid")):
                    self.endpoint = endpoint
                    return self
            time.sleep(0.25)
        self.stop()
        raise TimeoutError(f"The MCP endpoint did not come up within {self.start_timeout:.0f} s; see {self.data_dir / 'log'}")

    def _owns(self, pid) -> bool:
        """The endpoint file belongs to the process we started, or to a child of it.

        On Windows orca-slicer.exe is a small launcher that loads OrcaSlicer.dll in-process, so the
        pid normally matches; the child check covers launchers that re-execute.
        """
        if pid == self.process.pid:
            return True
        try:
            import psutil
            return any(child.pid == pid for child in psutil.Process(self.process.pid).children(recursive=True))
        except Exception:
            return False

    def stop(self) -> None:
        """Ends the instance this object started (never any other process)."""
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=15)
        self.process = None
        self.endpoint = None

    def __enter__(self) -> "OrcaApp":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    # -- connection details ----------------------------------------------------------------------
    @property
    def url(self) -> str:
        return self.endpoint["url"]

    @property
    def bound_port(self) -> int:
        return int(self.endpoint["port"])

    def newest_log(self) -> Path | None:
        logs = sorted((self.data_dir / "log").glob("*.log"), key=lambda p: p.stat().st_mtime)
        return logs[-1] if logs else None


def fresh_data_dir(base: Path, name: str) -> Path:
    """An empty directory ``base/name`` for one test instance."""
    path = Path(base) / name
    if path.exists():
        shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True)
    return path
