#!/usr/bin/env python3
"""Drive a private OrcaSlicer test instance by hand.

    python mcpctl.py start [--name dev] [--port 0] [files...]   launch an instance and leave it running
    python mcpctl.py tools                                      list the tools it offers
    python mcpctl.py call <tool> ['<json arguments>'] [--era legacy] [--images DIR]
    python mcpctl.py stop                                       end the instance started by `start`

The instance uses its own data directory under $ORCA_MCP_RUN_DIR (default: ./run next to this
file) and never touches another OrcaSlicer. `stop` only ends the process `start` recorded.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from orca_mcp import McpClient, OrcaApp, fresh_data_dir  # noqa: E402


def run_dir() -> Path:
    return Path(os.environ.get("ORCA_MCP_RUN_DIR") or Path(__file__).resolve().parent / "run")


def state_file(name: str) -> Path:
    return run_dir() / f"{name}.instance.json"


def load_state(name: str) -> dict:
    path = state_file(name)
    if not path.exists():
        sys.exit(f"No instance named {name!r} is recorded in {path}. Run: mcpctl.py start --name {name}")
    return json.loads(path.read_text(encoding="utf-8"))


def cmd_start(args) -> None:
    if state_file(args.name).exists():
        sys.exit(f"Instance {args.name!r} is already recorded; run `mcpctl.py stop --name {args.name}` first.")
    app = OrcaApp(fresh_data_dir(run_dir(), args.name), files=[str(Path(f).resolve()) for f in args.files],
                  port=args.port, detached=True, debug_tools=not args.no_debug_tools)
    app.start()
    state = {"pid": app.process.pid, "exe": str(app.exe), "url": app.url, "token": app.token, "data_dir": str(app.data_dir)}
    state_file(args.name).write_text(json.dumps(state, indent=2), encoding="utf-8")
    print(json.dumps({k: state[k] for k in ("pid", "url", "data_dir")}, indent=2))


def cmd_stop(args) -> None:
    import psutil
    state = load_state(args.name)
    try:
        process = psutil.Process(state["pid"])
        # Only the process we started: same pid AND same executable.
        if Path(process.exe()).resolve() == Path(state["exe"]).resolve():
            process.terminate()
            try:
                process.wait(15)
            except psutil.TimeoutExpired:
                process.kill()
            print(f"stopped pid {state['pid']}")
        else:
            print(f"pid {state['pid']} is now a different program; left alone")
    except psutil.NoSuchProcess:
        print("already gone")
    state_file(args.name).unlink()


def client_for(args) -> McpClient:
    state = load_state(args.name)
    return McpClient(state["url"], state["token"], era=args.era, timeout=args.timeout).connect()


def cmd_tools(args) -> None:
    for tool in client_for(args).list_tools():
        flags = "".join(flag for flag, key in (("R", "readOnlyHint"), ("D", "destructiveHint"), ("I", "idempotentHint")) if tool["annotations"].get(key))
        print(f"{tool['name']:24} [{flags:3}] {tool['description'][:110]}")


def cmd_call(args) -> None:
    arguments = json.loads(args.arguments) if args.arguments else {}
    result = client_for(args).call(args.tool, arguments, progress=args.progress)
    for params in result.progress:
        print("progress:", json.dumps(params), file=sys.stderr)
    print(json.dumps(result.data, indent=None if args.compact else 2, ensure_ascii=False))
    for index, (mime, data) in enumerate(result.images):
        folder = Path(args.images or ".")
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / f"{args.tool}_{index}.{'png' if mime == 'image/png' else 'jpg'}"
        path.write_bytes(data)
        print(f"image: {path} ({len(data)} bytes)", file=sys.stderr)
    if result.is_error:
        sys.exit(2)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", default="dev", help="instance name (default: dev)")
    parser.add_argument("--era", default="modern", choices=["modern", "legacy"])
    parser.add_argument("--timeout", type=float, default=300.0)
    sub = parser.add_subparsers(dest="command", required=True)
    start = sub.add_parser("start")
    start.add_argument("--port", type=int, default=0, help="0 = any free port")
    start.add_argument("--no-debug-tools", action="store_true")
    start.add_argument("files", nargs="*")
    start.set_defaults(run=cmd_start)
    sub.add_parser("stop").set_defaults(run=cmd_stop)
    sub.add_parser("tools").set_defaults(run=cmd_tools)
    call = sub.add_parser("call")
    call.add_argument("tool")
    call.add_argument("arguments", nargs="?")
    call.add_argument("--images", help="folder to save image results in")
    call.add_argument("--progress", action="store_true", help="ask for progress notifications")
    call.add_argument("--compact", action="store_true")
    call.set_defaults(run=cmd_call)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
