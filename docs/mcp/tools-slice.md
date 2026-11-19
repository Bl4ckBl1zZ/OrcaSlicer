# Slice tools

`plate_validate`, `slice`, `slice_status`, `slice_cancel`, `slice_result`, and the
`preview` view of `screenshot`. Writing the G-code is `export_file` with `kind` `gcode` or
`gcode_3mf` (below). Source: `src/slic3r/Mcp/tools/ToolsSlice.cpp`, helpers in
`src/slic3r/Mcp/tools/slice/`. Error codes: [errors.md](errors.md#slice).

Plates are numbered from 1 (or `pl<N>` ids, or `"current"`). Layers are numbered from 1, as the
preview's layer slider shows them. Positions are in the frame of the plate.

## How slicing is driven

`slice` posts the same event the Slice button posts (`EVT_GLTOOLBAR_SLICE_PLATE`, or
`EVT_GLTOOLBAR_SLICE_ALL` for `"all"`), so the application runs its own path
(`on_action_slice_plate` → `Plater::reslice` → `BackgroundSlicingProcess`) and the window switches
to the Preview tab, as after a click. Because the event bypasses the button's enabled state, the tool
first runs the checks that disable the button and refuses with `validation_failed` instead.

Progress and outcome are followed without hooks in upstream code: a `wxEventFilter` sees the
`SlicingStatusEvent` (percent, text) and `SlicingProcessCompletedEvent` (finished, cancelled, error
with its message) events before the Plater handles them, and a 100 ms timer ends the agent's job
once the Plater reports it is no longer slicing (after its own completion handler ran). Error boxes
a failed slice raises are answered by the job's dialog rules, so none is left open.

Plate states (`slice_status`, `plate_validate.state`, `details.state` of `not_sliced`):

| State | Meaning |
|---|---|
| `empty` | No printable object on the plate. |
| `not_sliced` | Never sliced in this session. |
| `slicing` | Being sliced now (`percent`). |
| `sliced` | Valid result. |
| `invalidated` | A result existed, then the scene or settings changed. |
| `invalid` | The plate fails validation (`plate_validate` lists why). |
| `failed` | The last slice ended with an error (`reason` quotes it). |
| `cancelled` | The last slice was stopped (`slice_cancel`, the user, or a change while slicing). |

A change to the scene or settings while a slice runs makes the application stop the slice; the job
ends `cancelled` with `cancel_reason: "scene_changed_or_user"`. Slice again.

## plate_validate

Pre-slice check without slicing. Arguments: `plate?`. Returns
`{plate, ok, errors: [...], warnings: [...], printable_instances, state}`; each error and warning has
`message`, `kind` (`outside_plate`, `empty_plate`, `filament_not_printable`, `setting`,
`object_collision`, `object_collision_sequential`, `filament_bed_type`, `filament_temperatures`,
`layer_height_limit`, `object`, `print`), and where known `object: {id, name}`, `key` (the setting)
and `fix`. The current plate is validated by the slicer (`Print::validate`); another plate gets the
geometric checks and its print's last applied state.

## slice

Arguments: `plate?` (PlateRef or `"all"`), `wait_s?` (0-280, default 100). Returns the slice job:
`{job, kind: "slice", status, percent, text, elapsed_s, result: {outcome, plates: [...]}}` where each
plate entry has the state plus `time_s`, `time`, `filament_g`, `filament_m`, `cost`, `layers`,
`layer_height`, `height_mm`, `filament_changes`, `warning_count`, `warnings` (first five messages),
`outcome`, `duration_s`. When still running after `wait_s`: `status: "running"` and a `hint`; poll
with `slice_status`. A plate that already has a valid result is not re-sliced (`reused: true`).
Errors: `validation_failed`, `nothing_to_slice`, `busy_slicing`.

## slice_status

Arguments: `wait_s?` (default 0). Returns the latest slice job (as above) plus `plates` (state of
every plate); with `wait_s` it long-polls until a running slice ends, streaming progress.

## slice_cancel

No arguments. Stops the running slice (what the slicing notification's Cancel button does).
Errors: `not_running`.

## slice_result

Arguments: `plate?`, `sections?` (`summary`, `filaments`, `features`, `objects`, `warnings`; default
summary and warnings), `time_mode?` (`normal` | `silent`). All figures come from the slicer's own
result (`GCodeProcessorResult`, `PrintEstimatedStatistics`), with per-feature times summed over the
moves. Errors: `not_sliced`, `busy_slicing`.

Slice warnings (`slice_result` section `warnings`, and the first five in each plate summary of
`slice`) carry the text the GUI shows for them: the G-code processor stores some warnings as keys
(`bed_temperature_too_high_than_filament` ...), which the GUI turns into a sentence
(`Plater::get_slice_warning_string`); the same sentence is `message` here. `code` holds the
processor's error code when it has one (e.g. `1000C001`), otherwise the raw key when it differs
from the text. Warnings the GUI does not show (e.g. the timelapse notice it leaves empty) are left
out.

## G-code export: export_file

There is no separate slice export tool: G-code is written by `export_file`
([tools-project.md](tools-project.md#export_file)) with

- `kind: "gcode"`: copies the plate's sliced G-code (the file the preview reads) to `path`
  (`.gcode`). One plate per file (`plate: "all"` is refused). Post-processing scripts are not run
  from an agent call (a note says so when the print preset has any).
- `kind: "gcode_3mf"`: writes a sliced project with `Plater::export_3mf` (the strategy of File >
  Export > Export plate sliced file) to a `.gcode.3mf`; `plate` may be `"all"` (every plate with a
  printable object). Statistics are included when the application setting `export_statistics` is on.

Other arguments: `plate?` (default the current plate), `overwrite?`. Every plate exported must have a
current slice result: otherwise `not_sliced` (with the plate's `state` in `details`), or
`busy_slicing` while it is being sliced. Returns `{ok, kind, path, bytes, files: [{path, bytes}]}`
plus, for `gcode`, `plate`, `header` (the statistics comments of the file) and `summary` (as in the
`slice` plate summary), and for `gcode_3mf` `plates` (one summary per exported plate). Paths must be
in `app_status.write_roots`. Errors: `not_sliced`, `busy_slicing`, `path_denied`, `exists`,
`export_failed`, `invalid_argument`. Nothing is sent to a printer.

## screenshot view "preview"

`screenshot {"view": "preview", "plate", "camera", "size", "preview": {...}}` draws the plate's
sliced toolpaths offscreen (the G-code preview renderer, a second `libvgcode::Viewer`): the Preview
tab's view type, layer slider, camera and toggles are not touched. The camera follows the editor
view's conventions exactly (same presets, azimuth 90 = from the right, zoom 1 = fit), so an editor
and a preview picture with the same camera line up pixel for pixel.

`preview` options: `color_by` (`feature`, `filament`, `color_print`, `speed`, `actual_speed`,
`layer_height`, `line_width`, `flow`, `actual_flow`, `layer_time`, `layer_time_log`, `fan_speed`,
`temperature`, `pressure_advance`, `acceleration`, `jerk`), `layers: {from, to}`, `z_range: {min,
max}`, `section` (only the top layer of the range: a cross-section), `dim_lower_layers`, `show`
(`travel`, `seams`, `retractions`, `deretractions`, `wipes`, `tool_changes`, `color_changes`,
`pauses`, `custom_gcode`), `hide_features` (keys or names), `show_supports`, `show_wipe_tower`,
`show_bed`, `show_shells`, `time_mode`.

The result carries `camera` (as the editor view returns it: matrices, eye, target), `legend`
(`kind: categories` with each feature's colour, time and share, or `kind: range` with the value range
and colour steps), `objects_in_view` (pixel position and box of each object), and `render_ms`.
Errors: `not_sliced`, `busy_slicing`, `invalid_argument`, `gl_unavailable`.

## debug_viewer_state (test builds only)

Registered only with debug tools on (`ORCA_MCP_DEBUG_TOOLS=1`). Read-only: the layer range and tab
of the window's own G-code preview, and the current plate, so the tests can check that the
offscreen preview leaves the window's viewer alone. The slice tests set up their scenes with the
real tools (`model_import`, `plate_manage`, `object_transform`, `settings_set`, `slice`,
`export_file`), not with fixtures.

## Examples (20 mm cube, generic printer; real output, abbreviated)

```
slice {"wait_s": 100}
→ {"job": "job1", "kind": "slice", "status": "finished", "percent": 100.0, "elapsed_s": 0.8,
   "result": {"outcome": "finished", "plates": [{"plate": 1, "id": "pl11", "state": "sliced",
     "time_s": 578.7, "time": "9m 39s", "filament_g": 4.01, "filament_m": 1.343, "cost": 0.08,
     "layers": 100, "layer_height": 0.2, "height_mm": 20.0, "filament_changes": 0,
     "warning_count": 1, "warnings": ["The current heatbed temperature is relatively high. The nozzle may clog ..."],
     "outcome": "finished"}]}}

slice_result {}
→ {"plate": 1, "summary": {"time_s": 578.7, "prepare_time_s": 0.8, "first_layer_time_s": 23.4,
     "filament_g": 4.01, "layers": 100, "first_layer_height": 0.2, "height_mm": 20.0,
     "travel_m": 3.48, "retractions": 201, "time_split_s": {"extrusion": 522.5, "travel": 37.6, "other": 15.8},
     "toolpath_box": {"min": [115.2, 115.2, 0.2], "max": [134.8, 134.8, 20.0]}},
   "warnings": [{"level": "error", "message": "The current heatbed temperature is relatively high. The nozzle may clog when
                 printing this filament in a closed environment. Please open the front door and/or remove the upper glass.",
                 "code": "1000C001", "step": "gcode"}]}

slice {}            (cube moved half off the plate)
→ error validation_failed: "Plate 1 cannot be sliced: Object \"20mm_cube.obj\" is partly outside the plate."
   details.errors[0] = {"kind": "outside_plate", "object": {"id": "o52", "name": "20mm_cube.obj"}, "instance": "i70",
                        "box": {...}, "fix": "Move it fully onto the plate or fully off it (object_transform), or scale it down."}

export_file {"kind": "gcode", "path": ".../out/benchy.gcode"}
→ {"ok": true, "kind": "gcode", "path": ".../benchy.gcode", "plate": 1, "bytes": 4990978,
   "header": {"estimated printing time (normal mode)": "44m 30s", ...},
   "summary": {"time_s": 2670.7, "filament_g": 13.27, "layers": 240, ...},
   "files": [{"path": ".../benchy.gcode", "bytes": 4990978}]}
```

Ground truth: on Benchy the reported time (2670.7 s) and weight (13.27 g) equal the G-code's own
comments (`44m 30s`, `13.27 g`); the tests compare them on every run.
