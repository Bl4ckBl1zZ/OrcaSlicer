# Analysis tools

`slice_layers` and `analyze`: facts about a sliced plate, computed by the slice-analysis core
(`src/libslic3r/SliceAnalysis.hpp`) from the plate's `GCodeProcessorResult` (every move) and its
`Print` (layer islands, extrusion entities, support layers). Source:
`src/slic3r/Mcp/tools/ToolsAnalysis.cpp`, glue in `tools/slice/SliceAnalysisGlue.cpp`.

Conventions in every result: layers numbered from 1 (as the preview slider); XY and boxes in the
plate's frame (the core works in the frame of the moves, which adds the plate's offset; the glue
converts); `object_id` (`o<N>`) is added next to the core's object label or Print index. Lists are
capped at `max_items` with `<name>_total` giving the full count.

The analysis of a plate is computed once per result and cached (`analysis_ms` says how long it
took; 0 when cached). When a new result of the plate replaces an analysed one, the old summary is
kept, so `analyze {checks: ["diff"]}` answers "what did my change do" in one call. Every finished
`slice` job also analyses its plates, so the diff is available after any change + re-slice.

Errors: `not_sliced`, `busy_slicing`, `invalid_argument`, `not_available` (core not built in).

## slice_layers

Arguments: `plate?`, `from?`, `to?`, `every?`, `limit?` (max 200), `columns?`, `layer?`, `z?`,
`detail?` (`table` | `section`), `box?` ({min: [x, y], max: [x, y]}).

- `table`: `{plate, layer_count, rows: [...], truncated?, next?}`. Default columns: `index`, `z`,
  `height`, `time_s`, `volume_mm3`, `max_flow`, `max_speed`, `fan_max`, `temp_max`,
  `tool_changes`, `has_support`, `has_bridge`, `has_overhang_wall`. Others: `length_mm`,
  `mean_flow`, `max_flow_at`, `fan_min`, `temp_min`, `roles`, `filaments`, `has_ironing`,
  `has_gap_fill`.
- `section` (needs `layer` or `z`): `{layer, z, moves: {roles, filaments, extruded_area_mm2, ...},
  geometry: {islands, island_area_mm2, perimeter_count, wall_thickness_mm,
  sparse_infill_fill_ratio, regions: [...]}}`, optionally inside `box`.

## analyze

Arguments: `plate?`, `checks` (any of the list below), `params?` (per check), `max_items?`
(default 5). Returns one member per check plus `verdicts` ({check: ok | warning | problem}).

| Check | What it reports | Params |
|---|---|---|
| `flow` | Highest volumetric flow per filament vs `filament_max_volumetric_speed`, where it peaks, how long the print runs at the limit | `tolerance` (0.02) |
| `speeds` | Commanded speed and acceleration per feature vs configured | `tolerance` |
| `cooling` | Layers below the slow-down / fan layer-time thresholds, shortest layer | - |
| `supports` | Support volume, time, layers, clusters, contact area per object | - |
| `overhangs` | Bridges and overhang walls: lengths, longest bridge, clusters, overhang bands | - |
| `unsupported` | Islands with nothing below (floating starts) and cantilevers | `tolerance_mm`, `min_area_mm2`, `report_distance_mm` |
| `first_layer` | Contact area, footprint, aspect ratio, brim / raft per object | `aspect_warning` (3) |
| `seams` | Seam count, mean direction, concentration, alignment per object | `align_distance_mm` (1) |
| `thin_features` | Gap fill, narrow walls, island areas no extrusion covers | `narrow_width_mm`, `min_area_mm2` |
| `multi_material` | Tool changes per layer, transitions, purge and prime tower | - |
| `summary` | The core's totals, per feature, per filament, per object | - |
| `region` | Extrusions inside `params.region.box` ({min: [x, y(, z)], max: [...]}, plate frame) | `box`, `features`, `support_only` |
| `diff` | Changes since the plate's previous result, largest relative change first | `rel_tolerance` (0.001) |

Filament slots are 1-based here too (the core's 0-based indices are shifted). Numbers are rounded to
three decimals.

## Example (20 mm cube)

```
slice_layers {"detail": "section", "z": 10}
→ {"layer": 50, "z": 10.0,
   "geometry": {"islands": 1, "island_area_mm2": 400.0, "perimeter_count": 3, "wall_thickness_mm": 1.192,
                "sparse_infill_fill_ratio": 0.207,
                "regions": [{"object_id": "o52", "infill_pattern": "crosshatch", "infill_density": 15.0, "wall_loops": 3}]},
   "moves": {"roles": [{"role": "inner_wall", "length_mm": 147.16}, {"role": "outer_wall", "length_mm": 78.36},
                       {"role": "sparse_infill", "length_mm": 142.801}]}}

analyze {"checks": ["flow"], "max_items": 1}
→ {"flow": {"filaments": [{"filament": 1, "limit": 12.0, "max_flow": 12.108, "ratio": 1.009, "share_at_limit": 0.069,
              "max_at": {"layer": 2, "z": 0.4, "x": 116.381, "y": 116.361, "role": "internal_solid_infill", "object_id": "o52"}}],
            "verdict": {"verdict": "warning", "message": "filament 1 is limited by its max volumetric speed on 6 layers (7% of its extrusion time)"}},
   "verdicts": {"flow": "warning"}}
```

Performance (Benchy, 240 layers, this machine): the analysis is computed when the slice job ends
(well under the 0.3 s between the slicer's completion event and the job's end); all ten checks in one
call take about 40 ms and return about 19 KB.
