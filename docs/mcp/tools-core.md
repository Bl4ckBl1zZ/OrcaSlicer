# Tools: Core

Source: `src/slic3r/Mcp/tools/ToolsCore.cpp`. Tests: `tests/mcp/test_core_tools.py`,
`tests/mcp/test_core_dispatch.py`.

## app_status

Status of the window the server drives. Read-only. Call it first in a session, and again when a
call fails with `ui_busy`.

No arguments. Result:

```json
{"app": "OrcaSlicer", "version": "2.5.1", "technology": "FFF",
 "project": {"name": "Untitled", "path": null, "dirty": true, "rev": 1},
 "printer": {"preset": "MyKlipper 0.4 nozzle", "bed": [250.0, 250.0], "height": 250.0, "extruders": 1, "nozzle": [0.4]},
 "process": {"preset": "0.20mm Standard @MyKlipper", "modified": []},
 "filaments": [{"slot": 1, "preset": "Generic PLA @System", "type": "PLA", "color": "#26A69A"}],
 "busy": {"slicing": false, "jobs": [], "modal_dialog": null, "gizmo": null, "blocks_changes": null},
 "undo": "MCP: update o52", "redo": null,
 "read_roots": ["C:/Users/me/Documents", "..."], "write_roots": [".../mcp/workspace"],
 "conventions": {"units": "...", "axes": "...", "positions": "...", "volumes": "...", "ids": "...", "camera": "..."},
 "printers": {"read_allowed": false, "send_allowed": false, "sends_in_flight": 0},
 "server": {"tools": 6, "debug_tools": false}}
```

`busy.modal_dialog` is the title of a dialog the user has open; `busy.blocks_changes` is the
reason a changing call would have to wait right now, or `null`. `printers` shows the user's
printer switches (no tool can change them) and the print requests waiting for the user or being
transferred; see [tools-printers.md](tools-printers.md).

## history

Undo, redo and the change journal. Marked destructive: undo can discard work, including the
user's own steps when they are on top of the list.

| Argument | Type | Default | |
|---|---|---|---|
| `action` | `list` \| `undo` \| `redo` \| `revert` | `list` | |
| `steps` | integer 1..50 | 1 | How many steps to undo or redo. |
| `journal_seq` | integer[] | | For `revert`: journal entries to revert. |

Result (every action): `undo` (names, most recent first), `redo`, `journal` (newest first,
`{seq, time, tool, scope, key, before, after, reverted, revertible}`), `rev`. After `undo` /
`redo`: `undone` / `redone` (the names) and a `scene` summary. After `revert`: `reverted`.

Errors: `nothing_to_undo`, `nothing_to_redo`, `stale_id`, `already_reverted`, `not_revertible`,
`ui_busy`.

```
history {"action": "undo"}
  -> {"ok": true, "undone": ["MCP: update o52"], "scene": {...}, "undo": [], "redo": ["MCP: update o52"], "journal": [], "rev": 1}
```

## job_status

Progress and result of a long-running operation started by another tool. Read-only.

| Argument | Type | Default | |
|---|---|---|---|
| `job` | `job<N>` | most recent job | |
| `kind` | string | | Without `job`: only jobs of this kind (`slice`, `arrange` ...). |
| `wait_s` | number 0..280 | 0 | Wait up to this long for the job to end. |

Result: `{"job", "kind", "status": "running" | "finished" | "failed" | "cancelled", "percent",
"text", "elapsed_s", "result"?, "dialogs"?}`, or `{"status": "idle", "jobs": []}` when there has
been no job. Errors: `stale_id`.

## job_cancel

Stops a running operation. The operation stops shortly after; `job_status` shows the final state.
Work already applied to the scene is not rolled back.

| Argument | Type | Default |
|---|---|---|
| `job` | `job<N>` | most recent running job |
| `kind` | string | |

Errors: `not_running`, `not_cancellable`, `stale_id`.
