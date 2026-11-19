# Tools: Project

Source: `src/slic3r/Mcp/tools/ToolsProject.cpp` (helpers in `tools/edit/`). Tests: `tests/mcp/test_project.py`.

Every question the GUI asks on these paths is an argument here. A question no argument covers is
answered with its non-destructive button and the call fails with `dialog_blocked`, quoting it.
All paths go through the file policy (`app_status.read_roots` / `write_roots`).

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
| `on_unsaved`, `save_path`, `overwrite` | | | As in `project_new`. |
| `trust_embedded_gcode` | boolean | false | Accept presets in the file that carry custom G-code. Without it the "Modified G-code" / "Customized Preset" question fails the call with `dialog_blocked`. |

Questions answered: "unsaved changes - save?" (after `on_unsaved` was applied: No), "Newer 3MF
version" and "Load 3MF" notices (informational, returned in `dialogs`), the old infill rotation
template fix (Yes), "STEP names not UTF-8" (OK).

The application's 3MF reader keeps only one instance per object and plate (its plate map is keyed
by object), so further instances of an object would come back on no plate; `project_open` puts
such instances on the plate they lie on. (A project opened in the GUI shows the same upstream
problem.)

Result: `{"ok": true, "changed": <scene summary>}`.

Errors: `path_denied`, `not_found`, `unsaved_changes`, `no_path`, `load_failed` (with the
application's error text), `dialog_blocked`.

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
- `gcode` copies the plate's sliced G-code. The user's post-processing scripts are not run for
  this export (the GUI runs them on its own export path, which needs a file dialog).
- `gcode_3mf` writes what File > Export > Export plate sliced file writes.

Result: `{"ok": true, "files": [{"path", "bytes"}], "path"?, "bytes"?}`.

Errors: `path_denied`, `exists`, `not_sliced`, `busy_slicing`, `export_failed`, `invalid_argument`.
