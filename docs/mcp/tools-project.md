# Tools: Project

Source: `src/slic3r/Mcp/tools/ToolsProject.cpp` (helpers in `tools/edit/`). Tests: `tests/mcp/test_project.py`.

Every question the GUI asks on these paths is an argument here. A question no argument covers is
answered with its non-destructive button and the call fails with `dialog_blocked`, quoting it.
All paths go through the file policy (`app_status.read_roots` / `write_roots`): absolute paths on a
local drive only; UNC, device, alternate-stream and reserved-name paths are refused before any disk
access (`path_denied` with `details.reason`), writes need an existing folder and never go through a
symbolic link. See [README](README.md#file-paths). Names (`project_new` `name`) must not contain
control characters (`invalid_argument`, `details: {argument, position}`).

## project_new

Start an empty project. The printer, process and filament presets stay selected; objects, plates
and per-plate settings are cleared. Not undoable (a new project starts a new undo history).

| Argument | Type | Default | |
|---|---|---|---|
| `on_unsaved` | `fail` \| `discard` \| `save` | `fail` | What to do with unsaved changes of the current project. |
| `save_path` | string | the project's file | Where `save` writes when the project has no file yet. |
| `overwrite` | boolean | false | Allow `save_path` to replace a file. |
| `name` | string | "Untitled" | Name of the new project. |

Result: `{"ok": true, "changed": <scene summary>}`; `notes` says when the old project was saved.

Errors: `unsaved_changes` (with `on_unsaved: "fail"`), `no_path`, `path_denied`, `exists`, `export_failed`.

## project_open

Open a 3MF project, replacing the current one (objects, plates, plate settings, presets stored in
the file). Ids are new afterwards. Not undoable.

| Argument | Type | Default | |
|---|---|---|---|
| `path` | string (required) | | `.3mf` inside a read root. |
| `on_unsaved`, `save_path`, `overwrite` | | | As in `project_new`; `on_unsaved` also decides about unsaved preset edits (below). |
| `trust_embedded_gcode` | boolean | false | Accept presets in the file that carry custom G-code. Needs the user's "Allow the agent to edit G-code settings" preference (`mcp_allow_gcode_edit`); without it `true` fails at once with `forbidden_key`. |

Unsaved preset edits (process, filament, printer): opening a project replaces them, and the GUI would
ask "Load project: unsaved changes". Here `on_unsaved` decides before anything is loaded: `fail`
(default) fails with `unsaved_changes`, `details.presets` listing the edits per preset type (secrets
redacted, as everywhere); `discard` and `save` drop them, with a note listing what was dropped (with
`save` the project file just written holds them). To keep them as presets, `preset_save` first.

Embedded custom G-code: without `trust_embedded_gcode` the G-code question ("Modified G-code" /
"Customized Preset") is shown to the agent even if the user ticked "don't show again". That question
relies on the list of changed settings the file itself carries and is skipped for "published"
projects, so after the load the G-code in effect is also compared with the user's own presets: every
G-code option (`machine_start_gcode`, `filament_start_gcode`, ... any `*_gcode` or code option) of
the selected process, printer and filament presets against the user's preset of that name, or for a
preset that came with the file its nearest ancestor (`inherits`) in the user's library (the defaults
when there is none); the G-code options of the project's own settings; and the plates' custom G-code
(`plate_layer_events` of type `custom`). If the question came up or anything differs, the
application has already loaded the presets, so the load is undone: the call fails with
`untrusted_gcode`, `details: {path, settings, questions}` (`settings`: `{scope, key, kind, name?,
slot? / plate?}` for each difference; `questions`: the application's question quoted, if any), and
an **empty project** remains. Check the G-code, then repeat with `trust_embedded_gcode: true` (which
needs the preference above).

Scripts, plug-ins and the printer's connection: a project opened through MCP never brings them,
trusted G-code or not. Post-processing scripts (`post_process`), plug-in backed options
(`slicing_pipeline_plugin`, `printer_agent`, the `plugins` manifest, `*_plugin_config_overrides`)
and network settings (`host_type`, `print_host`, `printhost_*`, `print_host_webui`,
`bbl_use_printhost`, `flashforge_serial_number`) that differ from the user's presets (compared as
above) are put back to the user's values: in the values in effect (where the project's preset is
selected this shows as an unsaved edit) and in the presets the file brought, so discarding the edits
does not bring them back. A note lists the keys. The same options are removed from the per-object,
per-part and layer-range settings of the project (G-code options there too, unless
`trust_embedded_gcode`). If this fails, the load is undone (empty project) and the call fails with
`internal`. (The application itself already ignores `print_host` and the credentials of a project.)

Questions answered: "unsaved changes - save?" (after `on_unsaved` was applied: No), "Newer 3MF
version" and "Load 3MF" notices (informational, returned in `dialogs`), the old infill rotation
template fix (Yes), "STEP names not UTF-8" (OK).

The application's 3MF reader keeps only one instance per object and plate (its plate map is keyed
by object), so further instances of an object would come back on no plate; `project_open` puts
such instances on the plate they lie on. (A project opened in the GUI shows the same upstream
problem.)

Result: `{"ok": true, "changed": <scene summary>}`.

Errors: `path_denied`, `not_found`, `unsaved_changes`, `no_path`, `forbidden_key`,
`untrusted_gcode`, `load_failed` (with the application's error text), `dialog_blocked`, `internal`.

## project_save

Save the project as 3MF, as File > Save / Save As does without its file dialog.

| Argument | Type | Default | |
|---|---|---|---|
| `path` | string | the project's file | `.3mf` inside a write root. With a path the project now uses that file (Save As). |
| `copy` | boolean | false | Write a copy; the project's file name and unsaved state stay. Needs `path`. |
| `overwrite` | boolean | false | Replace an existing file other than the project's own. |

Result: `{"ok": true, "path", "bytes", "copy", "project": {"name", "path", "dirty", "rev"}}`.

Errors: `no_path`, `path_denied`, `exists`, `export_failed`, `invalid_argument`.

## model_import

Import model files into the current project as new objects. One undo step.

| Argument | Type | Default | |
|---|---|---|---|
| `paths` | string[] (1..50, required) | | STL, OBJ, 3MF (geometry only), STEP/STP, AMF, DRC, SVG, OLTP inside a read root. |
| `as` | `objects` \| `single_object` | `objects` | `single_object`: everything imported becomes one multi-part object, relative positions kept. |
| `units` | `auto` \| `mm` \| `inch` \| `m` | `auto` | `auto`: a model so small it looks like metres or inches is scaled up (the GUI's "Object too small" question, answered Yes). `mm`: never. `inch`/`m`: convert every file. |
| `multipart` | `auto` \| `merge` \| `keep_separate` | `auto` | A file whose objects sit at several heights ("Multi-part object detected"): `auto`/`merge` = one object with parts, `keep_separate` = separate objects. |
| `too_large` | `scale_down` \| `keep` | `scale_down` | "Object too large" question. |
| `plate` | plate ref | current | Plate the objects are placed on. The window's current plate is restored afterwards. |
| `step` | `{linear_deflection, angle_deflection, split_compound}` | app settings | STEP tessellation; the mesh-quality dialog is never shown. |

Each file is loaded on its own (`Plater::load_files`, one file per call), so the multi-file "load
as a single object?" question never comes up; `as: "single_object"` assembles afterwards. Objects
are placed as the GUI places them (bed centre or the nearest free spot); within one call several
objects can land on the same spot (a note says so), so call `object_arrange` to pack them.

A multi-colour file (a 3MF with several filaments) adds filament slots to the project, as in the
GUI. That change is outside the undo stack: it is recorded in the journal (`history`, not
revertible there) and a note says how many slots were added.

A 3MF's per-object settings never bring G-code, post-processing scripts, plug-in or printer
connection options (see `project_open`): they are removed from the imported objects, their parts
and layer ranges, and a note lists them.

Result: `{"ok": true, "imported": [objects in scene_get shape], "failed": [{path, error}]?, "dialogs": [...]}`.

Errors: `path_denied` (outside the roots or an unsupported type), `not_found`, `plate_locked`,
`load_failed` (nothing imported; the application's error text), `dialog_blocked`.

## export_file

| Argument | Type | Default | |
|---|---|---|---|
| `kind` | `stl` \| `obj` \| `3mf` \| `gcode` \| `gcode_3mf` (required) | | |
| `path` | string (required) | | Inside a write root, extension matching the kind (`.gcode.3mf` for `gcode_3mf`). |
| `objects` | id[] | all | `stl` / `obj`: which objects (every instance, placed as on the plates). |
| `merge` | boolean | true | `stl` / `obj`: one file; false = one file per object, `<stem>_<object name>.<ext>`. |
| `plate` | plate ref or `"all"` | current | `gcode` / `gcode_3mf`; `"all"` only for `gcode_3mf`. |
| `overwrite` | boolean | false | |

- `3mf` is a copy of the project (`project_save` with `copy: true`).
- `gcode` copies the plate's sliced G-code (one plate per file; `plate: "all"` is refused). The
  user's post-processing scripts are not run for this export (the GUI runs them on its own export
  path, which needs a file dialog); a note says so when the print preset has any.
- `gcode_3mf` writes what File > Export > Export plate sliced file writes; `plate: "all"` exports
  every plate with a printable object. Statistics are included when the application setting
  `export_statistics` is on.
- The G-code kinds are the slice group's writer (`Slice::export_gcode` in `ToolsSlice.cpp`): every
  exported plate must have a current slice result, else `not_sliced` (`details.state` says why, e.g.
  `invalidated` after a change) or `busy_slicing`. See [tools-slice.md](tools-slice.md).

Result: `{"ok": true, "files": [{"path", "bytes"}], "path"?, "bytes"?}` (`path` / `bytes` when one
file was written). The G-code kinds always write one file and add `kind`, and for `gcode` also
`plate`, `header` (the statistics comments of the G-code) and `summary` (time, filament, layers,
warnings of the plate, as in the `slice` result), for `gcode_3mf` `plates` (one such summary per
exported plate).

Errors: `path_denied`, `exists`, `not_sliced`, `busy_slicing`, `export_failed`, `invalid_argument`,
`untrusted_scripts` (kinds `gcode` / `gcode_3mf`; as for `slice`).
