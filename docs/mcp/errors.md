# MCP error codes

A tool failure the agent can act on is a tool result with `isError: true` whose text is

```json
{"error": {"code": "stale_id",
           "message": "`targets[0]`: o57 does not exist (any more).",
           "hint": "Ids change when a project is loaded and disappear when the item is deleted. Call scene_get for the current ids.",
           "details": {"id": "o57"}}}
```

`message` says what is wrong, `hint` what to do next, `details` carries data to act on. A failed
call may also carry `dialogs`, `notes`, and, when it failed after changing something, `undo`,
`rev` and `journal`.

Add a code to this page when a tool introduces one. Prefer an existing code.

## Core

| Code | Meaning | What the agent should do |
|---|---|---|
| `invalid_argument` | The arguments violate the schema or contradict each other. Also: a name or message (object, part, plate, preset, project name, pause message) contains a control character (below 0x20, or 0x7F); then `details` is `{argument, position}` (byte offset). | Fix the named argument; the message quotes what is accepted. Names are one line of printable text. |
| `stale_id` | An id (object, instance, volume, plate, job, journal entry) does not exist. | Call `scene_get` (or `job_status`, `history`) for current ids. |
| `ui_busy` | The window cannot be interrupted now: a dialog is open, a drag or UI job is in progress, or the application is not responding. `details.reason` says which. | Retry shortly; `app_status` shows what is busy. |
| `dialog_blocked` | The application raised a dialog the call did not expect. `details.dialogs` quotes it; it was dismissed with its non-destructive button. | Check the state; repeat the call with the argument that answers the question, if there is one. |
| `conflict` | `if_rev` did not match the scene revision, or the target changed under a long operation. | Re-read the scene and retry. |
| `timeout` | The call did not finish in time; it may still be running. | Check the state before repeating. |
| `cancelled` | The client stopped waiting, or the server is shutting down. | - |
| `too_large` | The result would be too large to return. | Narrow the query (`limit`, `offset`, fewer ids, lower detail). |
| `internal` | A bug in the server. The application log has the details. | Report it. |
| `nothing_to_undo`, `nothing_to_redo` | The undo or redo list is empty, or the application refused. | `history` lists the steps. |
| `already_reverted`, `not_revertible` | Journal entry state. | - |
| `not_running`, `not_cancellable` | Job state. | `job_status` shows the job. |
| `path_denied` | The path is refused. Malformed paths, checked before any disk access, carry `details.reason`: `unc` (network or device path `\\server\share`, `\\?\...`, `\\.\...`), `control_character`, `relative` (only absolute local paths are accepted), `stream` (`:` after the drive letter, alternate data streams), `trailing_dot_or_space` (a name ending in `.` or a space), `device_name` (`CON`, `NUL`, `COM1`, `LPT1` ... as a name, whatever the extension). Otherwise: outside the allowed folders (`details.allowed`), a file type that is not accepted (`details.extensions`), a write into a folder that does not exist, through a symbolic link, onto a folder, or into the application's data or installation folder. `reason: hard_link`: the file to write over (or just written) has more than one hard link. `reason: changed_after_check`: the file opened for reading, or the file just written, resolves outside the allowed folders (a link or folder was swapped in after the check). | Give an absolute path on a local drive inside an allowed folder (`app_status` lists them), in an existing folder, not a link. See [README](README.md#file-paths). |
| `not_found` | The file to read does not exist, or its path does not resolve to a regular file (a dangling link, a folder). | Check the path. |
| `exists` | The file to write already exists. | Pass `overwrite: true` or choose another name. |

## Settings

| Code | Meaning | What the agent should do |
|---|---|---|
| `unknown_key` | There is no such setting. `details.nearest` lists similar keys. | Use `settings_search`. |
| `key_not_in_scope` | The setting cannot live at that scope. `details.scopes` lists where it can. | Use one of those scopes, or `effective: true` to read the value in force. |
| `invalid_value` | The value has the wrong type, is not one of the allowed values, or a list has the wrong length. | The message says what is accepted. |
| `out_of_range` | A number is outside the setting's range (`details.min`, `details.max`). | - |
| `forbidden_key` | The agent may not write this setting: `post_process`, the printer host and credentials (`print_host`, `print_host_webui`, `printhost_*`, `host_type`, `bbl_use_printhost`, `printer_agent`, `flashforge_serial_number`), plug-in backed options, G-code text unless the user allowed it. Also `project_open` with `trust_embedded_gcode: true` while G-code edits are not allowed. | Ask the user to change it (or to allow G-code edits). |
| `unknown_preset` | No preset of that type has this name. `details.offered` lists what the selector offers. | Pick from `details.offered` or `preset_list`. |
| `incompatible` | The preset exists but is not offered for the current printer (or print). | Pick a compatible one, or select the printer it belongs to first. |
| `unsaved_changes` | The operation would drop unsaved preset edits. `details` lists them (secrets redacted). | Repeat with `on_unsaved` (`discard`, `carry_over`, `save`), or `preset_save` first. |
| `system_preset` | System presets cannot be overwritten, renamed or deleted. | Save under a new `name`. |
| `name_exists` | A preset of that name exists. | Choose another name, or `overwrite: true` for a user preset. |
| `nothing_modified` | There are no unsaved edits to save or discard. | - |
| `overlap` | A height range would overlap another range of the object. `details.conflicts_with`. | Change the bounds; ranges may touch. |
| `last_part` | `modifier_update` would change the type of an object's last solid part. | Keep a part, or delete the object instead. |
| `last_filament` | The project needs at least one filament. | - |
| `in_use` | The filament slot is used by objects, parts, height ranges or paint. `details.used_by`. | Give `replace_with`. |
| `limit` | The maximum number of filament slots is reached. | Remove a slot first. |
| `load_failed` | A mesh file could not be read. | Check the file. |

## View

| Code | Meaning | What the agent should do |
|---|---|---|
| `gl_unavailable` | No usable OpenGL context: the 3D view was never shown since start-up. | Ask the user to bring the OrcaSlicer window on screen once. |

## Protocol errors

JSON-RPC errors, not tool results:

| Code | HTTP | Meaning |
|---|---|---|
| `-32700` | 400 | The body is not valid JSON, or nests arrays / objects deeper than 64 levels (refused before parsing). |
| `-32600` | 400 | Not a JSON-RPC 2.0 request (also: a batch, a forbidden `Host` or `Origin` with 403, too many requests with 429). |
| `-32601` | 404 (modern), 200 (legacy) | Unknown method. |
| `-32602` | 400 (modern), 200 (legacy) | Invalid params: unknown tool, arguments not an object, missing `_meta` fields. |
| `-32020` | 400 | Header mismatch: a mirrored header is missing or differs from the body. |
| `-32022` | 400 | Unsupported protocol version; `data.supported` lists the versions. |

## Edit

Projects, import/export, objects, plates (`tools-project.md`, `tools-objects.md`, `tools-plates.md`).

| Code | Meaning | What the agent should do |
|---|---|---|
| `unsaved_changes` | The project has unsaved changes and the call would replace it; for `project_open` also unsaved preset edits (`details.presets`, secrets redacted). | Pass `on_unsaved` (`save` with `save_path`, or `discard`; both drop unsaved preset edits, with a note), or `project_save` / `preset_save` first. |
| `no_path` | Saving needs a file name and the project has none. | Pass `path` / `save_path` inside a write root. |
| `load_failed` | A file could not be loaded; the message quotes the application's error. | Check the file; for STEP try other `step` values. |
| `untrusted_gcode` | `project_open` without `trust_embedded_gcode`: the project brings custom G-code the user's presets do not have (in the presets in effect after the load, the project's settings or a plate's custom G-code), or the application's "Modified G-code" / "Customized Preset" question fired. The load was undone; an empty project remains. `details.path` is the project, `details.settings` lists where the G-code is (`{scope, key, kind, name?, slot? / plate?}`), `details.questions` quotes the application's question(s), if any. | Check the G-code; repeat with `trust_embedded_gcode: true`, which needs the user's `mcp_allow_gcode_edit` preference (otherwise `forbidden_key`). |
| `untrusted_scripts` | `slice` or `export_file` (G-code): the process preset runs post-processing scripts (`post_process`) or slicing plug-ins (`slicing_pipeline_plugin`) the user has not saved in their own preset: an unsaved edit, or a preset that came with a project file. `details.settings` lists `{key, preset, reason}`. | Ask the user to check them, then save the process preset in the window or remove them. |
| `export_failed` | Writing a file failed. | Check the folder and that no other program holds the file. |
| `not_sliced` | `export_file` kind `gcode` / `gcode_3mf` needs a valid slice of the plate. | Slice first. |
| `busy_slicing` | Slicing is running; the result is not final. | Wait (`slice_status`), then export. |
| `plate_locked` | The plate is locked; nothing is arranged, oriented, imported or moved onto it. | `plate_manage` action `unlock`, or choose another plate. |
| `nothing_to_arrange` | The plate (or project) has no objects to arrange or orient. | Choose another plate or pass `targets`. |
| `not_splittable` | The mesh is one connected shell. | - |
| `last_part` | `delete` would leave an object without a solid part (parts, modifiers and other bodies are all deleted with `delete`). | Delete the object instead, or keep a part. |
| `last_plate` | The only plate cannot be deleted. | Delete its objects instead. |
| `limit` | Too many copies or plates. | Use fewer. |
| `mesh_error` | A geometry operation produced nothing usable (no flat face, empty cut). | Check the arguments. |

## Slice

| Code | Meaning | What the agent should do |
|---|---|---|
| `not_sliced` | The plate has no valid slice result. `details.state` says why: `not_sliced`, `invalidated` (changed after the last slice), `invalid`, `failed`, `cancelled`, `empty`. | Call `slice` (fix the problems `plate_validate` lists first). |
| `busy_slicing` | The plate (or, for `slice`, any plate) is being sliced right now. | Wait with `slice_status` (`wait_s`), or stop it with `slice_cancel`. |
| `validation_failed` | The plate cannot be sliced; `details.errors` lists each problem with the `object` or setting `key` concerned and a `fix`. | Apply the fixes, then slice again. |
| `nothing_to_slice` | No printable object on the plate (or on any plate for `"all"`). | Put an object on the plate or mark objects printable. |
| `export_failed` | `export_file` could not write the G-code or `.gcode.3mf`. | Check the folder; slice again if the sliced G-code is gone. |
| `not_available` | The slice analysis module is not built into this version. | Use `slice_result`. |
| `gl_unavailable` | No usable GL context for a preview picture. | Bring the OrcaSlicer window on screen and retry. |

## Paint

Codes of `paint_apply`, `paint_get` and `plate_layer_events` (besides `invalid_argument` and `stale_id`):

| Code | Meaning | What the agent should do |
|---|---|---|
| `not_a_part` | A target is a modifier, negative part, support enforcer/blocker or another body that carries no paint, or the targets have no model part. | Target the object or one of its parts (`scene_get` lists them with type `part`). |
| `too_fine` | Painting at `resolution_mm` would add more than `max_new_triangles` triangles and `coarsen` is false. `details` has the part, resolution and budget. | Use a coarser `resolution_mm`, `mode: "facets"`, a smaller region, a higher budget, or leave `coarsen` on. |
| `seed_missed` | A ray or pixel seed hits none of the painted parts. | Check the pixel against the screenshot the camera came from, or aim the ray at the part; or use a point seed. |
| `forbidden_key` | `plate_layer_events` type `custom` while the user has not allowed G-code edits (`mcp_allow_gcode_edit`). | Ask the user to allow it in Preferences, or use `pause` / `filament_change` / `color_change`. |
