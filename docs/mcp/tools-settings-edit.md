# Setting edits

Tool: `settings_set` (source `src/slic3r/Mcp/tools/ToolsSettingsEdit.cpp`, helpers
`tools/settings/SettingsCommon.*`, tests `tests/mcp/test_settings_edit.py`). Reading settings is
`settings_get` / `settings_search` (core).

## settings_set

| Argument | |
|---|---|
| `scope` | `"process"`, `"printer"`, `{"filament": slot}`, `"project"`, `{"plate": n}`, `{"object": id}`, `{"volume": id}`, `{"layer_range": {"object", "z_min", "z_max"}}` |
| `values` | `{key: value}`. Booleans, numbers, strings; enums as their value string (the GUI label is also accepted); percentages `"15%"` or the number; float-or-percent as a number (mm) or `"50%"`; per-filament / per-extruder options as an array of the current length, or one value for every entry. |
| `reset` | Keys to reset: at preset scopes back to the saved preset value, elsewhere the override is removed. |
| `auto_adjust` | Default true: answer Yes to the app's "adjust automatically?" questions (dependent options), No when false. The answered dialogs are returned in `dialogs`. |

The whole batch is validated before anything is written:

| Check | Error |
|---|---|
| The key exists | `unknown_key`, `details.nearest` |
| Not a blocked key: `post_process`, `print_host*`, `printhost_*`, `bbl_use_printhost`, `printer_agent`, plugin-backed and read-only options; G-code text options unless the user enabled the MCP preference `mcp_allow_gcode_edit` | `forbidden_key` |
| The key belongs to the scope | `key_not_in_scope`, `details.scopes` lists where it can be set |
| Type, enum, vector length | `invalid_value`, `details.values` / `labels` for enums |
| min / max of the definition | `out_of_range`, `details.min` / `max` |

Which keys a scope takes follows the GUI: process/printer/filament the preset type's option list;
object = process options of `PrintObjectConfig` + `PrintRegionConfig`; part and modifier =
`PrintRegionConfig`; layer range = those plus `layer_height`; plate = `curr_bed_type`,
`print_sequence`, `spiral_mode`, `skirt_start_angle`, `first_layer_sequence_choice`,
`other_layers_sequence_choice` (only `auto`: `customize` opens the plate dialog, and the custom order
keys are not writable yet); project = keys of the project config except the per-slot colour, map and
mixed-filament arrays (use `filament_edit`).

### How each scope is written

| Scope | Path | Undo |
|---|---|---|
| process, printer | `Tab::apply_preset_config_values` on the tab: the path a hand edit takes (dependent options, dirty marker "(modified)", plater update). The preset is modified, not saved (`preset_save`). | journal |
| filament slot | The filament tab is first pointed at the slot's preset (as the sidebar's edit button does); fails with `unsaved_changes` if the tab holds unsaved edits of another filament preset. Several slots can share one preset: `slots_sharing_preset` lists them, and all change. | journal |
| project | `project_config`, then `update_project_dirty_from_presets`, `export_selections`, `on_config_change` (the sequence the colour picker runs). | journal |
| plate | The `PartPlate` setters the plate tab uses. | undo stack |
| object, part, modifier, layer range | The item's `ModelConfig` (`apply_only` / `erase`), then the object list's settings rows (`object_config_options_changed`), the per-object settings tabs (`update_model_config`), `ParamsPanel::notify_object_config_changed` and the targeted refresh. Layer ranges rebuild the object list. | undo stack |

Only parts and modifiers carry settings; other volume types fail with `invalid_argument`. A height
range keeps its `layer_height` and `extruder` (cannot be reset).

### Result

```json
{"ok": true, "scope": "process", "preset": "0.20mm Standard @Generic", "modified": true,
 "changed": [{"key": "wall_loops", "old": 2, "new": 3}],
 "side_effects": [{"key": "...", "old": ..., "new": ...}],
 "journal": [12], "slice_invalidated": true}
```

`side_effects` are keys of the edited preset that changed without being asked for (the app's
dependency rules). Model scopes return `object` (the object in `scene_get` shape) and `undo`; the plate
scope returns `plate`. Journal entries revert through `history` with action `revert`: the old values of
every changed key (including side effects) are applied again through the same path; it fails with
`conflict` if another preset is selected by then.

Errors: `unknown_key`, `key_not_in_scope`, `invalid_value`, `out_of_range`, `forbidden_key`,
`stale_id`, `invalid_argument`, `unsaved_changes`.
