# Tools: Plates

Source: `src/slic3r/Mcp/tools/ToolsPlates.cpp`. Tests: `tests/mcp/test_plates.py`.

## plate_manage

| Argument | Type | Default | |
|---|---|---|---|
| `action` | `add` \| `delete` \| `duplicate` \| `rename` \| `lock` \| `unlock` \| `select` \| `move` (required) | | |
| `plate` | plate number, `pl<N>` or `"current"` | current | The plate acted on. |
| `count` | 1..36 | 1 | `add`: how many plates. |
| `name` | string | | `rename` (empty string clears the name). |
| `to` | integer | | `move`: the new 1-based position. |
| `objects` | `delete` \| `fail` | `delete` | `delete`: what happens to the objects on the plate. |

- `add` appends plates; the window's current plate stays the same.
- `delete` deletes the plate and the instances on it (whole objects when all their instances are
  there). The only plate cannot be deleted (`last_plate`).
- `duplicate` copies the plate with its objects (new objects) to a new last plate.
- `lock` / `unlock`: arrange and orient leave locked plates alone; `model_import`,
  `object_add_primitive` and `object_transform move_to.plate` refuse a locked plate (`plate_locked`).
- `select` makes the plate the one the window shows and the default for tools that take `plate`.
- `move` reorders: plate numbers shift, `pl` ids and the objects stay with their plate.

Per-plate settings (bed type, print sequence, first-layer sequence, spiral mode, ...) are
`settings_set` with `scope: {"plate": n}`. Moving objects between plates is `object_transform`
with `move_to: {"plate": n}`.

One undo step. Result: `{"ok": true, "plates": [{plate, id, name, current, locked, sliced, overrides, instances}], ...}`
plus `added`, `deleted` / `removed_objects`, or `created` / `created_objects`.

Errors: `invalid_argument`, `stale_id`, `limit` (no more plates), `last_plate`.
