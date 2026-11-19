# Modifier bodies and height ranges

Tools: `modifier_add`, `modifier_update`, `modifier_delete`, `layer_range`
(source `src/slic3r/Mcp/tools/ToolsModifiers.cpp`, tests `tests/mcp/test_modifiers.py`).

They cover the object list's context menu: Add Part, Add Negative Part, Add Modifier, Add Support
Blocker, Add Support Enforcer, Add Precise Seam (its six volume types), and Height Range Modifier.
Settings of a body are written with `settings_set` scope `{"volume": id}`; filament assignment with
the `filament` argument here or `object_update`.

## Frames and placement

| Frame | Meaning |
|---|---|
| `object` (default for `place.center`) | The `ModelObject`'s own frame, in which volume matrices are expressed. `scene_get` reports volume `center` in this frame. |
| `plate` | World minus the origin of the plate the object's first instance is on: the frame of `scene_get` instance positions. |
| `world` | The 3D scene's frame (all plates laid out on one grid). |

World and plate points are converted through the object's **first instance**. An object with several
instances gets the body in every instance (bodies belong to the object).

- `place.center_rel` is `0..1` across the box of the object's **parts** (not modifiers), in the
  object frame: `[0.5, 0.5, 1]` is the top centre. These are the same numbers `scene_get` returns as a
  volume's `center_rel`, so a value read there can be passed back.
- `place.center` is where the centre of the body's own bounding box goes.
- Without `center` or `center_rel` the body is centred on the object; a slab sits on the bed under the
  object's centre (as the menu does).
- The body is **aligned with the bed**, whatever the object's rotation or scale (the menu does the same:
  the volume transform is the inverse of the instance's rotation and scale). `place.rotation`
  `[rx, ry, rz]` (degrees) then rotates it about the bed's X, Y and Z axes, in that order, around its
  centre. Sizes are in mm in that bed-aligned frame.
- Every result carries `placement`: the body's bounding box in object, plate and world frames, its
  plate-frame centre, its world size, how it was placed (`placed_by`) and the shape size used. Use it to
  check the body is where it should be.

Unlike the menu, the tool does not bake the instance transform into every volume
(`apply_object_instance_transfrom_to_all_volumes`): the other volumes' transforms stay as they were.

## modifier_add

| Argument | |
|---|---|
| `object` | Object id. |
| `type` | `part`, `negative_part`, `modifier`, `support_blocker`, `support_enforcer`, `precise_seam_center`, `precise_seam_left`, `precise_seam_right`, `precise_seam_enforced`, `precise_seam_blocked`, `precise_seam_neutral`. |
| `shape` | `{primitive, size}` or `{file, scale}`. Primitives and sizes (mm): `cube` `[x,y,z]` or `[edge]`; `cylinder`, `cone`, `disc` `[diameter, height]`; `sphere` `[diameter]`; `torus` `[inner diameter, tube diameter]`; `slab` `[x,y,z]` (default 1.5x the object's footprint, half its height). Without `size` the menu's default size is used. Files: stl, obj, 3mf, amf, step, stp inside the read roots; the mesh keeps its orientation. |
| `place` | `center_rel` or `center` + `frame`, `rotation`, `fit: "object_xy"` (cube/slab: the object's footprint) with `margin`. |
| `name` | Default as the menu: `Generic-Cube` ...; the file name for a mesh. |
| `filament` | 1-based slot, 0 = the object's. Default: a part takes the object's filament, other bodies 0. |
| `settings` | Per-body overrides (part and modifier only), validated like `settings_set` at volume scope. |

One undo step (`MCP: add <type> to o<N>`). Result: `changed` (the object, `scene_get` shape), `volume`
(detail full), `placement`. Errors: `stale_id`, `invalid_argument`, `path_denied`, `load_failed`,
`unknown_key`, `key_not_in_scope`, `invalid_value`, `out_of_range`, `forbidden_key`.

A precise-seam body that does not touch the object's box gets a note: it must cross the outer wall to
have an effect.

```json
{"object": "o57", "type": "modifier", "shape": {"primitive": "cube", "size": [10, 10, 10]},
 "place": {"center_rel": [0.5, 0.5, 1.0]}, "name": "dense top",
 "settings": {"sparse_infill_density": "80%", "wall_loops": 4}}
→ {"ok": true, "volume": {"id": "v80", "type": "modifier", "size": [10, 10, 10], "center_rel": [0.5, 0.5, 1.0],
   "overrides": {"sparse_infill_density": "80%", "wall_loops": 4}, ...},
   "placement": {"plate": 1, "center_plate": [128, 128, 20], "bbox_plate": {"min": [123, 123, 15], "max": [133, 133, 25]}, ...},
   "undo": "MCP: add modifier to o57", "slice_invalidated": true}
```

## modifier_update

`target` (volume id) and any of: `type` (the last solid part cannot change type: `last_part`; text and
SVG volumes cannot become blockers/enforcers), `name`, `filament`, `place` (`center_rel` / `center` +
`frame` / `rotation`: absolute, about the bed axes), `size` `[x,y,z]` mm along the body's own axes, or
`scale` `[x,y,z]` relative to its mesh. What is not given is kept; a rotation-only change keeps the
centre. One undo step. Result as `modifier_add`.

## modifier_delete

`targets`: volume ids. An object must keep one solid part (`last_part`); volumes of a cut object are
refused (the GUI asks how to handle the cut information). One undo step. Result: `deleted`, `changed`.

## layer_range

| `action` | Arguments |
|---|---|
| `list` | `object` |
| `add` | `object`, `z_min`, `z_max`, `values?` |
| `update` | `object`, `z_min`, `z_max` (identify the range, within 0.0001 mm), `new_z_min?`, `new_z_max?`, `values?`, `reset?` |
| `delete` | `object`, `z_min`, `z_max` |

Heights are mm from the bed. A range always holds `layer_height` (default: the object's override, else
the process value) and `extruder` 0, as the GUI creates it; `layer_height` cannot be reset and must lie
within the printer's `min_layer_height`/`max_layer_height` (`out_of_range`). Keys: those of a part plus
`layer_height` (`key_not_in_scope` otherwise). Ranges of one object must not overlap (`overlap`, with
the conflicting range in `details`); they may touch. Range settings apply to the object's parts, not
to its modifiers (a note says so). One undo step per call. Result: `layer_ranges`, `changed`.

## Test-only tools

With debug tools enabled (`ORCA_MCP_DEBUG_TOOLS=1`, the test harness) the group also registers
`debug_settings_project` (save/load a 3MF) and `debug_settings_ui` (object list rows, selector texts,
filament colours), used by the tests to check round trips and what the window shows.
