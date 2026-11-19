# Presets

Tools: `preset_select`, `preset_save`, `preset_delete`, `preset_rename`, `preset_diff` (source
`src/slic3r/Mcp/tools/ToolsPresets.cpp`, helpers `tools/settings/PresetOps.*`, tests
`tests/mcp/test_presets.py`). Listing presets is `preset_list` (core). Preset changes are outside the
undo stack; each is recorded in the journal (`history`). System presets are never modified.

## preset_select

`type` (`process`, `printer`, `filament`), `name`, `filament` (slot number, type filament), `on_unsaved`, `save_as`.

The selection goes through the GUI's own path: the process tab's `select_preset`, or the sidebar's
printer / filament-slot dropdown (the tool sets the dropdown and runs its selection handlers), so the
app does everything it does for a hand selection: compatibility updates, object re-centring when the
bed changes, the slot's preset mapping, flushing volumes; picking a filament preset also sets the
slot's colour to the preset's default colour, as in the GUI. Names are resolved as the selector shows
them (label or internal name). A preset that exists but is not offered fails with `incompatible`; an
unknown one with `unknown_preset`; both list `details.offered`.

The "unsaved changes" question is never shown; `on_unsaved` decides before anything changes:

| `on_unsaved` | Edits of the preset being switched | Edits of a dependent preset made incompatible (a filament for a new process; process and filament for a new printer) |
|---|---|---|
| `fail` (default) | `unsaved_changes`, `details.<type>.modified` lists them | same |
| `discard` | dropped, returned in `discarded` | dropped, with a note |
| `carry_over` | re-applied to the new preset one key at a time: `carried_over`, refused keys in `rejected_keys` (not for printers, nor for a filament of another type, as in the GUI) | dropped, with a note |
| `save` | saved first under `save_as` (required for a system preset), returned in `saved_as` | `unsaved_changes` |

With several filament slots the sidebar does not switch the filament tab, so a slot selection never
touches unsaved filament edits.

Result: `before`, `selected`, `changed`, `discarded`, `carried_over`, `rejected_keys`, `saved_as`,
`presets` (selected and modified state of all three), `filaments`, `scene` (summary). History revert
selects the previous preset again (with `fail` semantics); discarded edits are in the journal entry's
`before`.

## preset_save

`type`, `action` `save` (default) or `discard`, `name`, `overwrite`, `filament`.

- `save` without `name` overwrites the selected user preset; a system preset needs `name`
  (`system_preset`); an existing name fails with `name_exists` unless `overwrite: true` (never for a
  system preset); nothing to save fails with `nothing_modified`. Saving selects the saved preset (the
  app's behaviour). Returns `name`, `saved_keys`, `replaced`.
- `discard` drops the unsaved edits (`nothing_modified` when none) and returns them; history revert
  re-applies them.
- `filament` (slot number): checks that the filament tab edits that slot's preset.

## preset_delete

`type`, `name`, `on_unsaved` (`fail` | `discard`). User presets only (`system_preset`). Refused when
other presets inherit from it or a filament slot uses it. Deleting the selected preset uses the tab's
delete (its confirmation is answered) and selects the parent preset, as the GUI does. Not revertible.

## preset_rename

`type`, `name`, `new_name`. Saves the preset under the new name (filament slots that referred to it
follow), removes the old file and restores the previous selection. Refused for system presets, existing
names, inherited presets and while unsaved edits exist. History revert renames it back.

## preset_diff

`type`, `a`, `b` (preset names, or `@edited` = the tab's preset with unsaved edits, `@saved` = the
selected preset as saved; default `@edited` vs `@saved`), `limit`. Returns `count` and
`differences: [{key, a, b}]` (`null` when a side lacks the key). Read-only.

Secrets: values of the printer's address and credentials (`printhost_apikey`, `printhost_password`,
`printhost_user`, `printhost_port`, `printhost_cafile`, `print_host`, `print_host_webui`,
`flashforge_serial_number`) are `"<redacted>"` (`""` when empty) wherever this group returns
values: `preset_diff` `a` / `b`, the `unsaved_changes` details, `discarded` and the `preset_save`
`discard` result. A redacted key still shows that it differs. These keys, `host_type` and the other
printer-host keys cannot be set by the agent (`forbidden_key`, see
[tools-settings-edit.md](tools-settings-edit.md)).

Names (`preset_select` `save_as`, `preset_save` `name`, `preset_rename` `new_name`) must be one line
of printable text: a control character (below 0x20, or 0x7F) fails with `invalid_argument`,
`details: {argument, position}`.

Errors: `unknown_preset`, `incompatible`, `unsaved_changes`, `system_preset`, `name_exists`,
`nothing_modified`, `invalid_argument`.
