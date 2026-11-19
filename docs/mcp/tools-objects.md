# Tools: Objects

Source: `src/slic3r/Mcp/tools/ToolsObjects.cpp`, jobs in `tools/edit/EditJobs.cpp`. Tests: `tests/mcp/test_objects.py`.

Edits are made on the `Model` and followed by the refresh recipes, the way the object manipulation
panel works; the user's selection is not touched, except by `object_cut` (the new objects are
selected, as after the GUI's Cut). Every mutating call is one undo step named
`MCP: <what> <ids>`; long operations (arrange, orient, simplify, repair) take their undo step when
they start, so one undo returns to the state before the job.

Frames: instance positions are in the plate frame (`center` = bounding-box centre in XY, `z_min` =
lowest point); part (volume) placement is in the object's frame.

## object_add_primitive

| Argument | Type | Default | |
|---|---|---|---|
| `shape` | `cube` \| `cylinder` \| `sphere` \| `cone` \| `disc` \| `torus` \| `handy_model` (required) | | |
| `size` | number[] | a tenth of the bed | cube `[x, y, z]`; cylinder, cone, disc `[diameter, height]` (disc height 0.2 by default); sphere `[diameter]`; torus `[inner diameter, tube diameter]`. |
| `model` | string | | `handy_model` key: `orca_cube`, `orcasliced_combo`, `orca_badge`, `orca_tolerance_test`, `3dbenchy`, `cali_cat`, `autodesk_fdm_test`, `voron_cube`, `stanford_bunny`, `orca_string_hell`. |
| `name` | string | the shape | |
| `plate` | plate ref | current | |
| `center` | `[x, y]` | plate centre | Plate frame. |

Meshes are built as the GUI builds them (facets within 0.002 mm of the round surface). Result:
`{"ok": true, "created": [objects]}`. Errors: `invalid_argument`, `stale_id`, `plate_locked`, `load_failed`.

## object_transform

Transform objects (all their instances), instances, or parts. The steps run in this fixed order:
`reset`, `set_scale`, `set_size`, `scale`, `set_rotation`, `rotate`, `mirror`, `lay_flat`,
`translate`, `move_to`, then the drop to the bed.

| Argument | Type | |
|---|---|---|
| `targets` | id[] (required) | `o...` (every instance), `i...`, `v...` (part, in its object's frame). |
| `frame` | `plate` (default) \| `world` \| `local` | Axes for `rotate`, `mirror`, `translate`: the bed's, or the item's own. `world` also makes `move_to.center` world coordinates. |
| `translate` | `[dx, dy, dz]` | Relative move. |
| `move_to` | `{center?: [x, y], z_min?: number, plate?: plate ref}` | Absolute placement in the plate frame. `plate` alone moves to that plate keeping the position on the plate. `z_min > 0` switches the object's auto-drop off (noted). |
| `rotate` | `{axis: x\|y\|z\|[vector], angle_deg, about: center\|origin}` | Relative rotation about the bounding-box centre (default) or the instance origin. |
| `set_rotation` | `[x, y, z]` degrees | Absolute Euler angles, centre kept. |
| `scale` | number or `[x, y, z]` | Relative factors in the item's own axes. |
| `set_scale` | number or `[x, y, z]` | Absolute factors, centre kept. |
| `set_size` | `{size: [x\|null, y\|null, z\|null], uniform: true}` | Absolute size in mm (instances). Uniform: the first given axis sets the factor. |
| `mirror` | `x\|y\|z\|[vector]` | |
| `reset` | `["rotation", "scale", "mirror"]` | |
| `lay_flat` | `{facet, volume?}` \| `{normal, frame: world\|object}` \| `{face}` \| `{auto: true, rank?}` | Put a face on the bed (instances): a mesh facet, the direction that should face down, a face number from `mesh_query` `flat_faces` (same list, same order), or the largest flat face of the convex hull (`rank` 2 = second largest). |
| `drop_to_bed` | boolean, default true | Lowest point to z = 0 after the steps (unless `move_to.z_min` is given). |

Result: `{"ok": true, "changed": [objects with new boxes and plate assignment], "details"?}`;
`details[].lay_flat` has `normal_world_before`; for `face` and `auto` also `face`, `face_area_mm2`,
`rank` and the five largest `candidates`. Errors: `stale_id`, `invalid_argument`, `plate_locked`, `mesh_error`.

## object_duplicate

| Argument | Type | Default | |
|---|---|---|---|
| `targets` | id[] (required) | | Objects (each instance is copied) or instances. |
| `copies` | 1..200 | 1 | |
| `as` | `instances` \| `objects` | `instances` | Extra instances share mesh and settings; objects are independent copies. |
| `spacing` | mm | 5 | Copies are laid out in rows next to the source, wrapping at the plate edge. |

Copies that do not fit stay outside the plate; call `object_arrange`. Result: `created` (instance
ids, or new objects) and `changed`. Errors: `invalid_argument`, `limit` (more than 500 copies).

## delete

Delete objects, instances or parts / modifiers. Deleting every instance of an object deletes the
object; the last solid part of an object cannot be deleted (`last_part`); single parts of a cut
object with connectors are refused. Result: `removed` ids and the scene summary.

## object_split

`{target, into: objects|parts, keep_z}`. Objects: one object per separate shell, done on the
Model as `Plater::priv::split_object` does (the public `Plater::split_object(int, bool)` is declared
but not defined); `keep_z: true` keeps floating pieces at their height (auto-drop off), what the
GUI's "Disable Auto-Drop?" question does. The selection is not changed. Parts: the target part (or the object's only part) becomes one part per shell.
Errors: `not_splittable`, `invalid_argument`, `stale_id`.

## object_merge

`{targets (2+ objects or instances), name = "Assembly"}`. Builds one multi-part object keeping every
part where it is, merges per-object settings, keeps per-part filaments (as the GUI's Assemble), and
removes the sources. Result: `created`, `removed`.

## object_cut

| Argument | Type | Default | |
|---|---|---|---|
| `target` | object or instance id (required) | | An object is cut through its first instance's placement. |
| `plane` | `{z}` \| `{point, normal, frame: plate\|world\|object}` (required) | | "Upper" is the side the normal points to. |
| `keep` | `both` \| `upper` \| `lower` | `both` | |
| `as_parts` | boolean | false | Both halves as parts of one object. |
| `flip_upper`, `flip_lower` | boolean | false | Turn a half upside down. |
| `place_on_cut_upper`, `place_on_cut_lower` | boolean | false | Lay a half on its cut face. |

Uses `Slic3r::Cut::perform_with_plane` and `Plater::apply_cut_object_to_model`, as the Cut gizmo.
The pieces stay where they were cut, so with `keep: both` they overlap on the plate (a note says
so); `object_arrange` separates them.
Connectors (dowels, plugs, grooves) are not available. Errors: `invalid_argument` (plane misses
the object, contradictory options), `mesh_error`.

## object_arrange (job)

| Argument | Type | Default | |
|---|---|---|---|
| `scope` | `plate` \| `all` | `plate` | |
| `plate` | plate ref | current | |
| `spacing` | mm | the GUI's setting | 0 = automatic. |
| `allow_rotation`, `multi_material_same_plate`, `avoid_calibration_region`, `align_to_y` | boolean | the GUI's settings | Set for this call only. |
| `wait_s` | 0..280 | 60 | |

Runs the application's ArrangeJob. Arranging one plate makes it the window's current plate while
the job runs; the previous current plate is restored at the end. The finished job's `result` has
`placements` (object, instance, plate, centre, rotation, outside_plate). Errors: `plate_locked`,
`nothing_to_arrange`, `ui_busy`, `stale_id`.

## object_orient (job)

`{targets? | plate?, goal: min_support|min_volume, wait_s}`. Runs `orientation::orient` on a
worker thread on the instances' current shape and applies the rotation on the main thread; if an
instance moved meanwhile, the job fails with `conflict` and applies nothing. Errors:
`plate_locked`, `nothing_to_arrange` (empty plate), `ui_busy`, `invalid_argument`.

## mesh_edit

| `op` | Arguments | |
|---|---|---|
| `simplify` (job) | `target_ratio` (0..1, default 0.5) \| `target_facets` \| `max_error_mm` | Quadric edge collapse on every volume of the targets (or the given parts). |
| `repair` (job) | | CGAL repair of solid parts: holes, orientation, self-intersections. Returns per-volume statistics. |
| `convert_units` | `from`: `inch` \| `cm` \| `m` \| `to_inch` \| `to_m` | Scales meshes (objects or parts). |
| `scale_to_fit` | `margin` mm | Uniform scale of instances to fit the plate, then centred on their plate. |

Simplify and repair replace meshes: the volumes get new ids (`volumes[].id`, `old_id`). Paint is
remapped when the `keep_painting` preference is on, otherwise removed (`paint: "removed"`). A volume
that changed while the job ran makes the job fail with `conflict`. Errors: `invalid_argument`,
`stale_id`, `conflict`.
