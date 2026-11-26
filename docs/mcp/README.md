# MCP server

OrcaSlicer contains a [Model Context Protocol](https://modelcontextprotocol.io) server. When it is
switched on, an AI agent running on the same computer (Claude Code, for example) can inspect the
open project and change it through tools. The agent works in the window the user is looking at:
changes appear live, and every change is one step in the undo history.

The server is **off by default**, listens on this computer only, and requires an access token.
Printers are reached only through the four [printer tools](tools-printers.md), only after the
user switched them on in Preferences, and every print job sent and every print stopped needs the
user's click in a confirmation window. There is no tool for cloud login, the account or the
network plug-in.

- [Tool reference](#tools)
- [Adding tools](adding-tools.md) (for developers)
- [Error codes](errors.md)

## Switching it on

Preferences > General > **MCP server (AI agent control)**:

| Control | Meaning |
|---|---|
| Enable MCP server | Starts or stops the server at once; no restart needed. |
| Port | TCP port on `127.0.0.1`, default `13720`. If the port is taken the server does not start and the status line says so; it never picks another port, because client configurations hold the address. |
| Status | `Listening on http://127.0.0.1:13720/mcp`, `Stopped`, or the reason it is not running. |
| Allow the agent to edit G-code settings | Off by default. G-code text settings can be read but not changed unless this is on (`mcp_allow_gcode_edit`). |
| Allow AI agents to read printer status | Off by default. Lets `printer_list` and `printer_status` list the printers and read their state, job, temperatures and filament trays (`mcp_printer_read`). See [tools-printers.md](tools-printers.md). |
| Allow AI agents to send print jobs | Off by default. Lets `print_send` and `printer_control` ask to send a sliced plate, and pause, resume or stop a print (`mcp_printer_send`). Every send and every stop still needs the user's click in a confirmation window, unless the next switch is on. |
| Send print jobs without asking me | Off by default. With sending allowed, sends and stops go out without the confirmation window, so prints can be started while the user is away from the computer (`mcp_printer_send_unattended`). |
| Connect Claude Code > Copy command | Copies the `claude mcp add ...` command, including the token. |
| Client configuration > Copy JSON | Copies a `.mcp.json` entry, including the token. |
| Access token > Copy token | Copies the token alone. |
| New access token > Regenerate | Replaces the token; clients need the new one. |

The settings are stored in the application configuration as `mcp_enabled`, `mcp_port`,
`mcp_allow_gcode_edit`, `mcp_printer_read`, `mcp_printer_send` and `mcp_printer_send_unattended`; no tool can change them. The
token is stored in `<data folder>/mcp/token`. While the server runs,
`<data folder>/mcp/endpoint.json` holds
`{"schema": 1, "url", "port", "token", "pid", "protocolVersions", "app", "appVersion"}` for local
helpers such as the stdio bridge; it is removed when the server stops.

## Connecting a client

Claude Code, from a terminal (the command the Preferences button copies):

```
claude mcp add --transport http orcaslicer http://127.0.0.1:13720/mcp --header "Authorization: Bearer <token>"
```

or as a project file `.mcp.json`:

```json
{
  "mcpServers": {
    "orcaslicer": {
      "type": "http",
      "url": "http://127.0.0.1:13720/mcp",
      "headers": { "Authorization": "Bearer ${ORCA_MCP_TOKEN}" }
    }
  }
}
```

Any MCP client that speaks Streamable HTTP can connect the same way: `POST` to the URL with the
`Authorization: Bearer <token>` header.

## Protocol

One endpoint, `POST /mcp`, answering in both protocol eras:

| Era | Versions | How a client starts |
|---|---|---|
| modern | `2026-07-28` | No handshake. Every request carries its protocol version and client capabilities in `params._meta` and mirrors them in the `MCP-Protocol-Version`, `Mcp-Method` and (for `tools/call`) `Mcp-Name` headers. `server/discover` describes the server. |
| legacy | `2025-11-25`, `2025-06-18`, `2025-03-26` | `initialize`, then `notifications/initialized`. No session id is issued. |

Methods: `server/discover`, `subscriptions/listen` (modern); `initialize`, `ping`,
`notifications/*` (legacy); `tools/list` and `tools/call` (both). The server keeps no
per-connection state in either era; ids, job handles and the scene revision are passed as tool
arguments.

A tool call is answered with one JSON object, or, when the request carries a `progressToken`,
with an event stream of `notifications/progress` followed by the result. Closing the stream (or
`notifications/cancelled` in the legacy era) ends the wait; it does not stop the operation the
tool started.

Tool results are a text block holding one JSON document, plus image blocks for pictures. A
failure the agent can act on is a result with `isError: true` and
`{"error": {"code", "message", "hint", "details"}}`; see [errors.md](errors.md). JSON-RPC errors
are used only for malformed requests, unknown methods and tools, and bad protocol headers.

## Security

| Control | Behaviour |
|---|---|
| Off by default | Nothing listens until the user enables the server. |
| Loopback only | Bound to `127.0.0.1`. On Windows the port is claimed with `SO_EXCLUSIVEADDRUSE`, so no other process can bind the same address and port (not even with `SO_REUSEADDR`) to take over or share it; if the exclusive claim fails the server does not start and the status line gives the reason. |
| Access token | 32 random bytes from OpenSSL's secure random generator (`RAND_bytes`), URL-safe base64 (43 characters). There is no fallback to a weaker generator: if it fails, the server does not start (and Regenerate keeps the old token). A token shorter than 32 characters is never used: a short stored token is replaced by a new one, and a short `ORCA_MCP_TOKEN` is ignored (logged) in favour of the stored token. Required on every request as `Authorization: Bearer`; compared in constant time; a wrong or missing token gets `401` with no detail. |
| `Origin` | Any request carrying an `Origin` header is rejected with `403`: native clients send none, web pages always do. No CORS headers are ever sent and preflight requests are not answered. |
| `Host` | Must be `127.0.0.1:<port>` or `localhost:<port>`, which defeats DNS rebinding. |
| Method and type | `POST` with `Content-Type: application/json` only. |
| Limits | 8 MB request body, 200 requests per second, 32 connections. A body that nests arrays or objects deeper than 64 levels is refused before it is parsed (JSON-RPC `-32700`, HTTP 400). |
| Files | Tools read only from the configured read folders (default: Documents, Downloads, Desktop and the workspace) and write only into the write folders (default: the workspace `<data folder>/mcp/workspace`), never over an existing file unless asked to. Configuration keys `mcp_read_roots`, `mcp_write_roots` (`;`-separated). See [File paths](#file-paths). |
| Printer secrets | `printhost_apikey`, `printhost_password`, `printhost_user`, `printhost_port`, `printhost_cafile`, `print_host`, `print_host_webui` and `flashforge_serial_number` are never returned: every read gives `"<redacted>"` when set (`""` when empty), including `preset_diff`, the unsaved edits listed by `unsaved_changes` / `discarded`, and the old and new values `settings_set` reports. They, `host_type` and the other printer-host keys cannot be written (`forbidden_key`). |
| Projects | `project_open` never brings custom G-code without the user's G-code permission (`untrusted_gcode`, see [tools-project.md](tools-project.md#project_open)): the G-code in effect after the load and plate custom G-code are compared with the user's own presets, so neither the file's own list of changed settings nor a "published" project gets around it. Post-processing scripts, slicing plug-ins and printer connection settings (`post_process`, `slicing_pipeline_plugin`, `printer_agent`, `host_type`, ...) a project would bring are always put back to the user's values, also in the presets the file brought. `model_import` of a 3MF drops all of these from the imported objects' settings. |
| Scripts | `slice` and `export_file` (G-code) are refused (`untrusted_scripts`) while the process preset runs post-processing scripts or slicing plug-ins the user has not saved in their own preset (an unsaved edit, or a preset that came with a project file). |
| Printer access | `printer_list` / `printer_status` and `print_send` / `printer_control` work only while the user's Preferences switches `mcp_printer_read` / `mcp_printer_send` are on (both off by default; otherwise `printer_access_disabled`). No tool can change the switches or any other `mcp_*` setting. Printer tool calls, refusals, the user's answers and transfer results are written to the journal `history` lists ([journal entries](tools-printers.md#journal-entries)). |
| Print confirmation | Every `print_send` and every `printer_control` stop waits for the user's click in a confirmation window (plate picture, printer, filament-to-tray mapping, time, weight, options); Send is disabled for the first second, Cancel is the default, Escape or closing declines, no answer in 5 minutes declines. The window is modeless and out of the server's reach: no tool can see, click or close it. One window at a time, at most three requests waiting (`rate_limited`). |
| Printer data | Printer tools return names, models, states, job progress, temperatures and trays; never access codes, serial numbers, IP addresses or host names, API keys, passwords or cloud account data. Printers are named by opaque salted ids (`prn_...`, `host_...`). |
| Never exposed | Cloud login and the account, the network plug-in, the plug-in system, preferences other than this section. |

The model is: other users of the computer and web pages in the user's browser must not be able to
use the server. Software already running as the user can read the token file and is out of scope.

### File paths

Every path a tool reads or writes (`Files::check_read` / `check_write` in `McpFiles.cpp`) is
checked in three steps, and a refusal is `path_denied` (or `not_found`, `exists`):

1. **The text alone, before anything touches the disk** (`check_path_text`). Only absolute paths
   on a local drive are accepted (`C:\...` on Windows, `/...` elsewhere). Refused with
   `details.reason`:

   | `reason` | Refused |
   |---|---|
   | `unc` | Network and device paths: `\\server\share`, `\\?\...`, `\\.\...` (Windows would contact the server as soon as the path is looked at). |
   | `control_character` | A character below 0x20 or 0x7F (DEL). |
   | `relative` | A relative path. |
   | `stream` | Windows: a `:` other than after the drive letter (alternate data streams). |
   | `trailing_dot_or_space` | Windows: a folder or file name ending in `.` or a space, which Windows silently drops. |
   | `device_name` | Windows: a reserved device name as a folder or file name, whatever the extension: `CON`, `PRN`, `AUX`, `NUL`, `COM0`-`COM9`, `LPT0`-`LPT9` (also with superscript 1-3), `CONIN$`, `CONOUT$` (`nul.3mf` is refused). |

2. **Lexically inside an allowed folder** (`.` and `..` resolved without the file system), with an
   accepted extension. Otherwise `path_denied` with `details.allowed` (the folders) or
   `details.extensions`.
3. **Resolved** (links and junctions followed) and still inside an allowed folder:
   - read: the file must exist and be a regular file, else `not_found`;
   - write: the parent folder must exist (`path_denied`, "the folder ... does not exist"); the
     target must not be a symbolic link (the agent never writes through a link) or a folder; an
     existing file needs `overwrite: true` (`exists`); an existing file with more than one hard
     link is refused (`reason: hard_link`: writing would change the file under its other names);
     and whatever the configured folders say, nothing is written into the application's data
     folder or installation folder (the workspace excepted).
4. **Read: the file is opened, then its final path checked.** The tool opens the file and asks the
   system where the open file really is (Windows `GetFinalPathNameByHandle`, macOS `F_GETPATH`,
   Linux `/proc/self/fd`); outside the allowed folders is `path_denied` with
   `reason: changed_after_check` (a link or folder was swapped in after step 3). On Windows the
   file stays open without delete sharing until the tool has loaded it, so it cannot be renamed,
   replaced or moved away (nor can its folders be renamed) in between.
5. **Write: checked again after writing.** The written file (no symbolic link, one hard link) must
   resolve inside a write folder and outside the application's folders; otherwise
   `changed_after_check` / `hard_link`. The application writes through its own exporters (temporary
   file, then rename), which cannot be handed an open file, so this detects a redirection but
   cannot prevent it.

Containment compares folder **identities**, not spellings: a path is inside a folder when the folder
itself or one of the path's parents is the same file-system object (device and file id), so a
case-insensitive volume (NTFS, APFS and HFS+ by default) accepts the folder written in another case
or Unicode normalisation, and a case-sensitive one does not mix up `Work` and `work`. Step 2 is
only a pre-filter: it folds ASCII case on Windows and macOS, so a path whose letters differ only in
non-ASCII case can be refused there although the volume would accept it.

What remains (time of check to time of use): on macOS and Linux a file cannot be pinned, so a
symbolic link swapped in between the open in step 4 and the application's own open could still
redirect a read (only to a file of the user's that the agent then reads; writes are caught by step
5). Hard links to files outside the folders can be read (the agent cannot create links; the user
or another program would have had to). A program running as the user can always change files
between calls; that is outside the threat model above.

## Working with the user

Calls run on the application's main thread, one at a time, between the user's own actions.

- A tool that changes something waits while the user is in the middle of something: a modal
  dialog, a mouse drag in the 3D view, an arrange or orient job, a project loading. If that lasts
  longer than the tool's queue time (30 s) the call fails with `ui_busy`. Reading tools still run
  while a dialog is open.
- Dialogs the application raises during a call are answered by the tool's policy and returned to
  the agent as `dialogs`; nothing is left on screen waiting for a click. The one exception is the
  print confirmation window of `print_send` and `printer_control` stop, which only the user answers
  ([tools-printers.md](tools-printers.md#the-confirmation-window)).
- Each changing call is one undo step named `MCP: ...`. The user can undo it like their own, and
  the `history` tool can too. Changes outside the undo stack (presets, filaments) are kept in a
  journal that `history` lists and reverts.
- The agent does not use the user's selection, camera or current tab.

## Conventions

- Millimetres, degrees, seconds, grams. Colours are `#RRGGBB`. Z is up; the bed is the plane z = 0.
- Ids: `o<N>` object, `i<N>` instance, `v<N>` volume (part, modifier, blocker ...), `pl<N>` plate.
  They are stable for the session, survive undo and redo, and change when a project is loaded.
- Numbering is 1-based everywhere: plates (also addressed by `pl<N>` or `"current"`), filament
  slots and layers (as the preview's layer slider counts them). A filament slot argument is always
  named `filament`; 0 on a part or modifier means "the object's filament".
- Objects and instances are positioned in the frame of their plate (world coordinates minus the
  plate origin). `center` is the centre of the bounding box in XY, `z_min` its lowest point.
- Volumes are positioned in the frame of their object; `center_rel` is 0..1 across the object's
  bounding box.
- `rev` is the scene revision: it changes with every undoable change, whoever made it. Changing
  tools accept `if_rev` and fail with `conflict` if the scene moved on.
- Camera: azimuth 0 looks from the front (from -Y towards +Y) and increases counter-clockwise seen
  from above; elevation 0 is horizontal, 90 looks straight down.
- Argument names: `targets` is a list of several items a tool acts on, `target` exactly one item,
  and `object` the parent of something new (the object a modifier or height range is added to).
  `action` selects the sub-command of a tool that has several (`history`, `mesh_edit`,
  `paint_apply`, `layer_range`, `filament_edit`, `plate_manage` ...); `kind` selects what a query
  or export produces (`mesh_query`, `export_file`).
- Names and messages the agent gives must be one line of printable text: a control character
  (below 0x20, or 0x7F) fails with `invalid_argument`, `details: {argument, position}` (the byte
  offset), because these texts end up in G-code comments, file headers and the UI. This applies to
  `name` of `object_update`, `object_add_primitive`, `object_merge`, `modifier_add`,
  `modifier_update`, `plate_manage`, `project_new` and `preset_save`; `save_as` of
  `preset_select`; `new_name` of `preset_rename`; `message` of `plate_layer_events`.
- Paths are absolute paths on a local drive inside the allowed folders ([File paths](#file-paths)).

## Tools

52 tools, one reference page per tool group:

| Group | Tools | Page |
|---|---|---|
| Core | `app_status`, `history`, `job_status`, `job_cancel` | [tools-core.md](tools-core.md) |
| Scene | `scene_get`, `object_update` | [tools-scene.md](tools-scene.md) |
| Project | `project_new`, `project_open`, `project_save`, `model_import`, `export_file` | [tools-project.md](tools-project.md) |
| Objects | `object_add_primitive`, `object_transform`, `object_duplicate`, `delete`, `object_split`, `object_merge`, `object_cut`, `object_arrange`, `object_orient`, `mesh_edit` | [tools-objects.md](tools-objects.md) |
| Modifiers | `modifier_add`, `modifier_update`, `layer_range` | [tools-modifiers.md](tools-modifiers.md) |
| Plates | `plate_manage` | [tools-plates.md](tools-plates.md) |
| Mesh | `mesh_query` | [tools-mesh.md](tools-mesh.md) |
| Settings | `settings_search`, `settings_get`, `preset_list`, `filament_list` | [tools-settings.md](tools-settings.md) |
| Setting edits | `settings_set` | [tools-settings-edit.md](tools-settings-edit.md) |
| Presets | `preset_select`, `preset_save`, `preset_delete`, `preset_rename`, `preset_diff` | [tools-presets.md](tools-presets.md) |
| Filaments | `filament_edit` | [tools-filaments.md](tools-filaments.md) |
| Paint | `paint_apply`, `paint_get`, `plate_layer_events` | [tools-paint.md](tools-paint.md) |
| Slice | `plate_validate`, `slice`, `slice_status`, `slice_cancel`, `slice_result` | [tools-slice.md](tools-slice.md) |
| Analysis | `slice_layers`, `analyze` | [tools-analysis.md](tools-analysis.md) |
| View | `screenshot` (views `editor`, `preview`; overlays) | [tools-view.md](tools-view.md) |
| Printers | `printer_list`, `printer_status`, `print_send`, `printer_control` (off until the user allows them in Preferences) | [tools-printers.md](tools-printers.md) |

There is one tool per verb across groups: everything is deleted with `delete` (objects, instances,
parts, modifiers and other bodies) and every file other than the project is written with
`export_file` (STL, OBJ, 3MF copy, G-code, `.gcode.3mf`).

With `ORCA_MCP_DEBUG_TOOLS=1` test builds also register `debug_*` tools for the test-suite (never
needed by an agent): `debug_echo`, `debug_sleep`, `debug_job`, `debug_dialog`, `debug_ui`,
`debug_stats`, `debug_touch`, `debug_window`, `debug_viewer_state` (read-only: the window's G-code
preview state), `debug_settings_ui`, `debug_paint_gizmo`, `debug_paint_3mf`. They inspect the
window; tests set up their scenes with the real tools above.

[tools.md](tools.md) is a reference generated from the registry (`tests/mcp/gen_tool_reference.py`
against a running build; regenerate it when tools change, as the copy in the tree may lag);
`tools/list` is the authority on what a build offers.

## Build and environment

The server is built by default; `-DSLIC3R_MCP=OFF` leaves it out together with its hooks.

For automated test launches, environment variables override the preferences (read once at
start-up, nothing is saved):

| Variable | Effect |
|---|---|
| `ORCA_MCP_ENABLE=1` / `0` | Force the server on or off. |
| `ORCA_MCP_PORT=<n>` | Port; `0` picks any free port, reported in `endpoint.json`. |
| `ORCA_MCP_TOKEN=<token>` | Use this token instead of the stored one. It must have at least 32 characters; a shorter one is ignored (with a warning in the log) and the stored token is used. |
| `ORCA_MCP_ROOTS=<a;b>` | Extra folders tools may read from and write to. |
| `ORCA_MCP_ENDPOINT_FILE=<path>` | Write a second copy of `endpoint.json` there. |
| `ORCA_MCP_DEBUG_TOOLS=1` | Also register the `debug_*` tools the test-suite uses. |
| `ORCA_MCP_WINDOW=minimized` | Minimise the main window once the application is up (test runs on a desktop in use). |

The client test-suite lives in `tests/mcp`; see [adding-tools.md](adding-tools.md#tests).
