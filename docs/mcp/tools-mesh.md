# Tools: Mesh

Source: `src/slic3r/Mcp/tools/ToolsMesh.cpp`, geometry in `McpGeometry.{hpp,cpp}`.
Tests: `tests/mcp/test_mesh.py`.

## mesh_query

Measure and inspect geometry without changing it. Read-only. Positions and boxes are in plate
coordinates (mm), like `scene_get`.

| Argument | Used by | |
|---|---|---|
| `kind` (required) | | `stats`, `overhangs`, `flat_faces`, `raycast`, `distance` |
| `targets` | stats, overhangs, flat_faces (required); raycast (optional) | Object, instance or volume ids. An object id stands for its parts and its first instance. |
| `threshold_deg` | overhangs | Default 45. A facet counts when its normal is within this angle of straight down. |
| `limit` | overhangs | Regions listed, default 10. |
| `pixel`, `camera` | raycast | A pixel `[u, v]` of a screenshot and that screenshot's `camera` object, unchanged. |
| `origin`, `direction` | raycast | Or: a ray given directly, in plate coordinates. |
| `plate` | raycast | The plate whose coordinates `origin` uses (default: current). |
| `a`, `b` | distance | Two points. |

### stats

```
mesh_query {"kind": "stats", "targets": ["o52"]}
  -> {"stats": [{"id": "o52", "facets": 12, "volume_mm3": 8000.0, "area_mm2": 2400.0, "manifold": true,
                 "open_edges": 0, "shells": 1, "size": [20.0, 20.0, 20.0],
                 "bbox": {"min": [115.0, 115.0, 0.0], "max": [135.0, 135.0, 20.0]}}]}
```

Volume and area are those of the object as it stands (scaled by its instance). `manifold` is
false when the mesh has open edges; `shells` counts its connected pieces.

### overhangs

Where the model faces downward, before slicing: the same test the support painting gizmo's
"highlight overhangs" applies. Facets resting on the bed are not counted.

```
mesh_query {"kind": "overhangs", "targets": ["o57"], "threshold_deg": 45}
  -> {"overhangs": [{"id": "o57", "threshold_deg": 45.0, "area_mm2": 640.2, "facets": 212, "share_of_surface": 0.031,
                     "lowest_z": 22.0,
                     "clusters": [{"volume": "v59", "area_mm2": 590.1, "facets": 180,
                                   "box": {"min": [95.0, 110.0, 22.0], "max": [120.0, 140.0, 24.0]}}],
                     "cluster_count": 2}]}
```

A cluster is a connected region of overhanging facets; its box says where it is. The boxes are
what `paint_apply` region selectors and support blocker / enforcer bodies are placed with.

### flat_faces

The faces an object could be laid on: the planes of its convex hull, largest first, with their
outward normals as the object stands now. `on_bed` marks the face that already points down.

```
mesh_query {"kind": "flat_faces", "targets": ["o52"]}
  -> {"flat_faces": [{"id": "o52", "faces": [{"face": 0, "normal": [0.0, 0.0, -1.0], "area_mm2": 400.0, "on_bed": true}, ...]}]}
```

### raycast

What a ray hits: the nearest part along it.

```
screenshot {"camera": {"preset": "front", "target": "objects"}}
  -> image; {"camera": {...}, "objects_in_view": [{"id": "o52", "pixel": [512, 384], ...}]}
mesh_query {"kind": "raycast", "pixel": [512, 384], "camera": {...}}
  -> {"ray": {"origin": [...], "direction": [...]},
      "hit": {"object": "o52", "instance": "i70", "volume": "v55", "facet": 3,
              "point": [125.0, 115.0, 10.0], "point_object": [0.0, -10.0, 10.0],
              "normal": [0.0, -1.0, 0.0], "distance": 35.2}}
```

`point` is in plate coordinates, `point_object` in the object's own frame (what volume placement
and paint seeds use), `facet` is the index of the triangle in the part's mesh. `hit` is `null`
when the ray misses. Without `targets` every object is a candidate; modifiers and other bodies
that are not parts are only hit when named as a target.

### distance

`{"distance", "delta": [dx, dy, dz], "horizontal"}` between `a` and `b`.

Errors: `stale_id`, `invalid_argument`.
