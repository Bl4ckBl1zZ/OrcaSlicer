# Tools: Paint

Source: `src/slic3r/Mcp/tools/ToolsPaint.cpp`, helpers in `src/slic3r/Mcp/tools/paint/`
(`PaintSelector` - the selector language, `PaintStats` - statistics and regions, `LayerEvents` -
`plate_layer_events`, `PaintOverlay` - screenshot overlays, `PaintDebug` - test-only tools).
Geometry: `src/libslic3r/FacetPaint.{hpp,cpp}` and the `select_region()` addition to
`TriangleSelector`. Tests: `tests/mcp/test_paint.py`, `tests/mcp/test_paint_effects.py`.

Painting is done by rule instead of with a brush. The result is the same data the paint gizmos
store (`FacetsAnnotation` of each model part), so it renders, saves to 3MF, slices and opens in the
gizmos exactly like hand-painted data.

## Frames

Positions and directions in selectors and seeds take `"frame"`:

| Frame | Meaning |
|---|---|
| `plate` (default) | mm from the origin of the plate the instance is on: the frame `scene_get` reports instance boxes in. |
| `world` | the scene's world coordinates (the same on every plate; screenshot cameras use them). |
| `object` | the object's own coordinates, before the instance's position, rotation, scale and mirror. `paint_get` gives each part's `box_object`. |

Paint is stored per part in the mesh and is shared by all instances of an object. For `plate` and
`world` the first instance is used, or the instance whose id is given as target. Lengths
(`resolution_mm`, areas) are in world mm.

## paint_apply

| Argument | Type | Default | |
|---|---|---|---|
| `targets` | id[] | required | Objects (all model parts), instances (same, with that instance as frame), parts. |
| `channel` | `support` \| `seam` \| `color` \| `fuzzy_skin` | required | |
| `state` | `"enforce"` \| `"block"` \| `"fuzzy"` \| `"none"` \| `{"filament": n}` | required for set/replace | support/seam: enforce, block; fuzzy_skin: fuzzy; color: a 1-based filament slot. `none` erases. |
| `selector` | Selector | required except `clear` | What to paint (below). |
| `op` | `set` \| `erase` \| `replace` \| `clear` | `set` | `erase` removes paint inside the selection; `replace` clears the channel on the targets first; `clear` removes all paint of the channel. |
| `only_where` | state | | Only repaint surface currently in this state. |
| `mode` | `precise` \| `facets` | `precise` | `facets`: whole original triangles whose centroid is inside. |
| `resolution_mm` | number 0.05..50 | 0.3 | Longest triangle side along a region boundary. |
| `max_new_triangles` | integer | 300000 | Budget of triangles one call may add per part. |
| `coarsen` | boolean | true | Over budget: double the resolution until it fits (reported), instead of failing with `too_fine`. |
| `limit_to_overhang_deg` | number | | Shorthand for AND-ing `{"overhang": {"threshold_deg": x}}`. |
| `dry_run` | boolean | false | Compute and report, change nothing. |

### Selector grammar

Every selector is an object with exactly one key.

| Selector | Meaning | Boundary |
|---|---|---|
| `{"all": true}` | every facet | |
| `{"facets": [ids]}` or `{"facets": {"ids": [...], "part": "v12"}}` | original facets by index | whole facets |
| `{"normal": {"direction": [0,0,1], "max_angle_deg": 10, "frame"?}}` | facets facing a direction | whole facets |
| `{"overhang": {"threshold_deg": 45, "up"?: [0,0,1]}}` | downward facets within the threshold of straight down (the support gizmo's "by angle"; threshold from the horizontal like `support_threshold_angle`) | whole facets |
| `{"z_range": {"min": 2, "max": 5, "frame"?}}` | horizontal band | sharp |
| `{"slab": {"normal": [..], "min": a, "max": b, "frame"?}}` | between two planes `a <= p . unit(normal) <= b` | sharp |
| `{"halfspace": {"point": [..], "normal": [..], "frame"?}}` | the side the normal points to | sharp |
| `{"box": {"min": [..], "max": [..], "frame"?}}` | axis-aligned box | sharp |
| `{"box": {"center": [..], "size": [..], "rotation_deg"?: [x,y,z], "frame"?}}` | oriented box (rotated about X, then Y, then Z) | sharp |
| `{"sphere": {"center": [..], "radius": r, "frame"?}}` | ball | sharp |
| `{"cylinder": {"base": [..], "axis": [..], "radius": r, "height"?: h, "frame"?}}` | finite cylinder; height defaults to the axis length | sharp |
| `{"capsule": {"a": [..], "b": [..], "radius": r, "frame"?}}` | points within r of a segment | sharp |
| `{"stroke": {"path": [[x,y,z] or Seed, ...], "radius": r, "frame"?, "connected_only"?: true}}` | a brush stroke along surface points | sharp |
| `{"connected": {"seed": Seed, "angle_deg"?: 30}}` | smart fill: the smooth surface around the seed | whole facets |
| `{"same_state": {"seed": Seed, "angle_deg"?}}` | bucket fill: the connected region with the seed's current state; only at the top level or in a top-level `and` | follows existing paint |
| `{"and": [...]}`, `{"or": [...]}`, `{"not": {...}}` | combinations; a region AND a whole-facet leaf has the region's sharp boundary | |

Seeds:

| Seed | |
|---|---|
| `{"point": [x,y,z], "frame"?}` | snapped to the closest surface point of the targets |
| `{"ray": {"origin": [..], "direction": [..]}, "frame"?}` | first hit |
| `{"facet": n, "part"?: "v12"}` | centroid of an original facet |
| `{"pixel": [u, v], "camera": {...}}` | first hit of the ray through pixel (u right, v down from the top-left) of a screenshot; `camera` is the `camera` object the screenshot returned (`view_matrix`, `projection_matrix`, `width`, `height` are used) |

A seed is resolved once against all painted parts and paints only the part it lands on. A world
ray (pixel, or `frame: "world"`/`"plate"`) is tested against every instance of the object, since
the picture shows them all. The result's `seeds` list says where each landed (part, facet, point,
normal, and for point seeds the snap distance).

Examples:

```json
{"targets": ["o3"], "channel": "support", "state": "enforce",
 "selector": {"and": [{"box": {"min": [-30,-15,20], "max": [-5,15,26], "frame": "object"}},
                      {"overhang": {"threshold_deg": 45}}]}}

{"targets": ["o3"], "channel": "color", "state": {"filament": 2},
 "selector": {"connected": {"seed": {"pixel": [520, 300], "camera": {"...": "from screenshot"}}, "angle_deg": 20}}}
```

### Result

```json
{"ok": true, "changed": true, "channel": "support", "op": "set", "state": "enforce", "mode": "precise",
 "resolution_mm": 0.2, "resolution_used_mm": 0.2, "coarsened": false, "facets_selected": 2, "triangles_added": 0,
 "area_mm2": {"before": {}, "after": {"enforce": 150.0}},
 "parts": [{"part": "v12", "object": "o10", "name": "t_shape.obj", "instance": "i11", "plate": 1, "changed": true,
            "facets_selected": 2, "triangles_added": 0, "resolution_used_mm": 0.2, "surface_mm2": 2700.0,
            "before": {}, "after": {"enforce": {"area_mm2": 150.0, "fraction": 0.055556, "triangles": 2,
                                                "box": {"min": [...], "max": [...]}, "box_object": {"min": [...], "max": [...]}}},
            "delta_mm2": {"enforce": 150.0}}],
 "rev": 7, "undo": "MCP: paint support enforce on o10", "slice_invalidated": true}
```

`box` is in the plate frame, `box_object` in the object frame. With nothing changed (already in that
state), `changed` is false and no undo step is taken, so repeating a call is harmless.

### GUI behaviour

- An open gizmo is closed before the call (the core does this for every mutating tool; the result
  notes it). The paint gizmos commit each stroke on mouse-up, so nothing is lost.
- One undo step per call, named `MCP: paint <channel> <state|op> on <objects>`.
- After a change: the object list's paint rows, the plate's filament use (colour), the 3D view and the
  background process are refreshed, as the gizmos do after a stroke; the plate must be re-sliced.
- `notes` say when the settings make new paint ineffective, as the GUI user would find out: support
  enforcers with `enable_support` off (enforcers need it) or with an automatic `support_type`
  (enforcers then only add to automatic supports; `normal(manual)` / `tree(manual)` give support only
  where enforced), fuzzy-skin paint with `fuzzy_skin` = `disabled_fuzzy` (the default; `none`, labelled
  "Painted only", or any other mode makes painted fuzzy skin print).
- Colour paint shows in the normal editor view; support, seam and fuzzy paint show in their gizmos and
  in screenshot overlays.

### Errors

`stale_id`, `invalid_argument` (bad selector, with the path of the offending member, e.g.
`selector.and[1].box.min`; missing filament slot - the hint names `filament_edit`), `not_a_part`,
`too_fine`, `seed_missed`. See [errors.md](errors.md#paint).

## paint_get

Read-only. Per model part of the targets: `facets`, `surface_mm2`, the part's `box` (plate frame) and
`box_object`, and per channel the painted states (as in `paint_apply`) and `triangles` (live
triangles of the paint data). Without `channel` only painted channels are listed.

| Argument | Type | Default | |
|---|---|---|---|
| `targets` | id[] | required | |
| `channel` | channel | all painted | |
| `components` | boolean | false | `regions`: connected painted regions, largest first: `state`, `area_mm2`, `triangles`, `box`, `box_object`, `centroid`, `mean_normal` (area-weighted), `flatness` (1 for a plane, ~0 for a closed band); `regions_total`. |
| `facets` | boolean | false | `facet_ids` per state: original facets that carry the state (wholly or partly), capped. |
| `limit` | integer | 20 | Regions per channel and part (facet ids: max(limit, 200)). |

## plate_layer_events

Events at a layer of a plate, as the layer slider's menu adds them. They live in
`Model::plates_custom_gcodes[plate]`, are saved in the 3MF, and are not on the undo stack (like the
slider): each change is journaled, `history` `revert` undoes it.

| Argument | Type | Default | |
|---|---|---|---|
| `action` | `list` \| `add` \| `remove` \| `clear` | `list` | |
| `plate` | PlateRef | current | |
| `at` | `{"z": mm}` \| `{"layer": n}` | | z is snapped to the layer it falls in; layers are 1-based, exact once the plate is sliced, nominal (first layer height + n x layer height) before. |
| `type` | `filament_change` \| `color_change` \| `pause` \| `template` \| `custom` | | |
| `filament` | integer | | filament_change: the slot to switch to. color_change: the extruder (1). |
| `color` | `#RRGGBB` | | color_change only. |
| `message` | string | | pause only. |
| `gcode` | string | | custom only. |

Rules taken from the slider: one event per layer; filament changes need two or more slots and a plate
printed with one filament (the slider removes them otherwise); none on a plate printed object by
object or in spiral vase mode; `template` needs the printer's `template_custom_gcode`; `custom` needs
the preference `mcp_allow_gcode_edit` (else `forbidden_key`). Empty `color_change_gcode` /
`machine_pause_gcode` are reported in `notes`.

Result: `{"plate", "mode", "layers": {"source", "count", "first_z", "top_z"}, "events": [{"index", "z",
"layer", "type", "filament"?, "color"?, "message"?, "gcode"?}]}`, plus `ok`, `journal` and
`slice_invalidated` after a change.

## Screenshot overlays

Registered with the core's `RenderPlugins` (`screenshot` `editor.overlay`): `support_paint`,
`seam_paint`, `color_paint`, `fuzzy_paint` draw the painted states flat over the parts (support
enforcers green, blockers red, seam enforcers blue, fuzzy green, colour in the filament colours) and
return `legend.overlay = {"paint_channel", "states": {state: "#RRGGBB"}}`; `overhang` draws faces
within `editor.overhang_threshold_deg` (default 45) of facing down in orange, with their area. Like the
`overhang` selector and the gizmo's "by angle", it includes faces resting on the bed (`mesh_query`
`overhangs` leaves those out). The screenshot schema does not yet carry `overhang_threshold_deg` (a
core request), so today the default 45 is what is drawn.

## Test-only tools

Registered only with `ORCA_MCP_DEBUG_TOOLS=1`: `debug_paint_gizmo` (open/inspect/close a paint gizmo
and compare what it loaded with the model), `debug_paint_3mf` (save a 3MF copy, load it into a
separate Model, compare paint and layer events), `debug_paint_slice` (slice a copy of a plate with the
app's configuration and write G-code), `debug_paint_fixture` (add a filament slot, transform an
instance, add an instance, a modifier or a part).
