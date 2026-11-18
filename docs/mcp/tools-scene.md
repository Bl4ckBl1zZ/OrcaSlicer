# Tools: Scene

Source: `src/slic3r/Mcp/tools/ToolsScene.cpp`. Tests: `tests/mcp/test_core_tools.py`.

## scene_get

The scene tree of the open project. Read-only. The ids it returns are what every other tool takes.

| Argument | Type | Default | |
|---|---|---|---|
| `detail` | `summary` \| `normal` \| `full` | `normal` | `summary`: counts only. `full`: adds matrices, per-volume boxes, mesh statistics. |
| `plate` | plate number, `pl<N>` or `"current"` | all plates | |
| `objects` | id[] | all | Instance and volume ids select their object. |
| `offset`, `limit` | integer | 0, 50 | Paging over objects. |

Result at `normal`:

```json
{"project": {"name": "Untitled", "path": null, "dirty": true, "rev": 1},
 "printer": {"preset": "MyKlipper 0.4 nozzle", "bed": [250.0, 250.0], "height": 250.0, "extruders": 1, "nozzle": [0.4]},
 "process": {"preset": "0.20mm Standard @MyKlipper", "modified": []},
 "filaments": [{"slot": 1, "preset": "Generic PLA @System", "type": "PLA", "color": "#26A69A"}],
 "plates": [{"plate": 1, "id": "pl11", "name": "", "current": true, "locked": false, "sliced": false, "overrides": {},
   "objects": [{"id": "o52", "name": "20mm_cube.obj", "printable": true, "filament": 1, "size": [20.0, 20.0, 20.0],
     "overrides": {}, "paint": {"support": false, "seam": false, "color": false, "fuzzy_skin": false},
     "instances": [{"id": "i84", "plate": 1, "center": [125.0, 125.0], "z_min": 0.0, "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0], "bbox": {"min": [115.0, 115.0, 0.0], "max": [135.0, 135.0, 20.0]}, "outside_plate": false}],
     "volumes": [{"id": "v55", "type": "part", "name": "20mm_cube.obj", "filament": 1, "facets": 12,
                  "center": [0.0, 0.0, 10.0], "size": [20.0, 20.0, 20.0], "overrides": {}}],
     "layer_ranges": []}]}],
 "off_plate": [], "object_count": 1}
```

- An object with instances on several plates appears under each plate with the instances on it;
  objects with an instance on no plate are listed under `off_plate`.
- Instance `center`, `z_min` and `bbox` are in the frame of the plate; volume `center` and `size`
  are in the frame of the object. Bodies other than parts also have `center_rel` (0..1 across the
  object's bounding box).
- `filament` on a volume is 0 when it inherits the object's filament.
- `overrides` are the settings stored on the item itself, not effective values.
- When the result would exceed about 18 000 characters or `limit` objects, it ends early with
  `"truncated": true` and `"next"`: pass that as `offset`.

Errors: `stale_id`.

## object_update

Rename an object or part, set objects or instances printable or not, assign a filament slot to
objects, parts or modifiers. One undo step. Idempotent.

| Argument | Type | |
|---|---|---|
| `targets` | id[] (required) | Objects (`o...`), instances (`i...`, `printable` only), volumes (`v...`, `name` and `filament` only). |
| `name` | string | One target only. Renaming an object with a single part renames the part too, as the GUI does. |
| `printable` | boolean | On an object: all its instances. |
| `filament` | integer | 1-based slot; 0 on a part or modifier means "inherit from the object". |

Result: `{"ok": true, "changed": [objects in scene_get shape], "rev", "undo", "slice_invalidated": true}`.

Errors: `invalid_argument` (nothing to change, a kind of target that does not take the argument,
no such filament slot), `stale_id`, `ui_busy`, `conflict` (`if_rev`).

```
object_update {"targets": ["o52"], "name": "calibration cube"}
  -> {"ok": true, "changed": [{"id": "o52", "name": "calibration cube", ...}], "rev": 2, "undo": "MCP: update o52", "slice_invalidated": true}
```
