# Filaments

Tool: `filament_edit` (source `src/slic3r/Mcp/tools/ToolsFilaments.cpp`, tests
`tests/mcp/test_filaments.py`). Listing the slots and where they are used is `filament_list` (core).
The preset of a slot is chosen with `preset_select` (type filament,
`slot`); objects, parts and modifiers are assigned to a slot with `object_update` / `modifier_add` /
`modifier_update` (`filament`, 1-based; 0 on a volume = the object's), height ranges with
`settings_set` (`extruder`). Filament slots and colours are outside the undo stack: every change is
recorded in the journal.

## filament_edit

| `action` | Arguments | What happens |
|---|---|---|
| `add` | `color?` `#RRGGBB` (default: the next colour the app picks), `preset?` | `Sidebar::add_custom_filament`: the slot is inserted after the last physical slot (before mixed slots, which move up; a note says so). Limit 64 (`limit`). History revert removes it again while it is unused. |
| `remove` | `slot`, `replace_with?` | `Sidebar::delete_filament`: renumbers object, part and paint assignments, plate sequences and the flush matrix. A slot in use (objects, parts, height ranges, colour paint) needs `replace_with`, else `in_use` with `details.used_by`. The last filament cannot go (`last_filament`). Slots above move down by one (note). Not revertible. If removing would switch a filament tab that holds unsaved edits: `unsaved_changes`. |
| `set_color` | `slot`, `color` | The body of the colour picker (`sync_colour_config`) through the slot's own selector: project colours, dirty flag, selector swatch, 3D view. History revert restores the old colour (including a multi-colour gradient). |
| `auto_flush` | - | `Sidebar::auto_calc_flushing_volumes(-1)`. Returns the matrix; history revert restores the old one. |

Mixed (blended) slots of this fork are listed but read-only (`invalid_argument`).

Result: the action's own entry (`added`, `removed` + `replaced_by`, `changed`, `flush_volumes_matrix`)
and `filaments`: every slot with `preset`, `type`, `vendor`, `nozzle_temperature`, `system`, `color`
(and `colors` for a multi-colour slot), `extruder` (filament map of multi-extruder printers, read-only),
`mixed`, `used_by: {objects, parts, layer_ranges, paint}`.

Errors: `invalid_argument`, `unknown_preset`, `incompatible`, `limit`, `last_filament`, `in_use`,
`unsaved_changes`.
