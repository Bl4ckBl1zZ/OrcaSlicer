# Tools: View

Source: `src/slic3r/Mcp/tools/ToolsView.cpp`, `McpRenderer.{hpp,cpp}`, `McpCamera.{hpp,cpp}`.
Tests: `tests/mcp/test_view.py`.

## screenshot

A picture of the scene from any viewpoint. Read-only.

It is rendered offscreen into a framebuffer with a camera of its own. The user's camera, tab,
selection and view options are neither read nor changed, nothing flickers, and the window does not
have to be visible: it works minimised and while the Preview tab is shown.

| Argument | Type | Default | |
|---|---|---|---|
| `view` | `editor` (other views may be added by tool groups) | `editor` | |
| `plate` | plate number, `pl<N>`, `"current"` | current plate | The plate whose bed and objects are drawn. |
| `camera` | object | preset `iso`, target `plate`, zoom 1, perspective | See below. |
| `size` | `{width, height}`, 64..2048 | 1024 x 768 | |
| `editor` | object | | Options of the editor view, below. |
| `format` | `png` \| `jpeg` | `png` | A PNG over 900 kB is sent as JPEG. |

Camera:

| Member | |
|---|---|
| `preset` | `iso` (from the front right), `iso_left`, `iso_back`, `iso_back_left`, `top`, `bottom`, `front`, `back`, `left`, `right`. |
| `azimuth_deg`, `elevation_deg` | Override the preset. Azimuth 0 looks from the front (camera on the -Y side), 90 from the right (+X side), 180 from the back; elevation 0 is horizontal, 90 looks straight down, -90 straight up. |
| `target` | `"plate"` (the plate and what stands on it), `"objects"` (the objects only), a list of ids, or a point `[x, y, z]` in plate coordinates (the picture is centred on it, at the plate's scale). |
| `zoom` | 1 fits the target into the picture with a small border; 2 is twice as close. |
| `distance` | Perspective only: distance from the target in mm, instead of fitting. |
| `projection` | `perspective` (35 degree vertical field of view) or `orthographic`. |

Editor options:

| Member | Default | |
|---|---|---|
| `color_by` | `filament` | `filament`: parts in their filament colour with colour painting, as the editor shows them. `object`: one colour per object. `type`: parts grey, other bodies in a colour per type. |
| `show_modifiers` | true | Modifiers, negative parts, support blockers and enforcers, drawn translucent. |
| `show_bed` | true | |
| `only` | | Draw only these objects, instances or volumes. |
| `highlight` | | Draw these in orange. |
| `transparent` | | Draw these translucent. |
| `overlay` | `none` | Overlays are added by tool groups (paint, overhangs). |

The bed is drawn plainly so that a picture can be read and measured: the plate rectangle, grid
lines every 10 mm with a bold line every 50 mm, counted from the plate origin, and the axes at the
origin (X red, Y green, Z blue, a tenth of the plate long). Seen from below, the plate is not
filled, so the underside of the objects is visible.

Result: an image block, and as text

```json
{"size": [1024, 768], "format": "png", "plate": 1, "view": "editor",
 "camera": {"projection": "perspective", "azimuth_deg": 45.0, "elevation_deg": 35.0,
            "target": [125.0, 125.0, 10.0], "position": [429.24, -179.24, 311.27], "up": [-0.41, 0.41, 0.82],
            "distance": 525.25, "fov_y_deg": 35.0, "near": 359.63, "far": 691.45, "width": 1024, "height": 768,
            "view_matrix": [[...], [...], [...], [...]], "projection_matrix": [[...], [...], [...], [...]]},
 "objects_in_view": [{"id": "o52", "instance": "i70", "name": "20mm_cube.obj",
                      "pixel": [512, 384], "pixel_box": [478, 347, 546, 422], "in_picture": true}],
 "legend": {"color_by": "filament", "colors": {"filament 1": "#26A69A"}, "background": "#EDF0F2",
            "bed": {"fill": "#C7CCD1", "grid_mm": 10, "bold_grid_mm": 50, "axes": "X red, Y green, Z blue, from the plate origin"}}}
```

- `camera` is the camera actually used, in world coordinates, with row-major 4x4 matrices
  (OpenGL conventions: the eye looks down -Z, clip space -1..1). A world point `p` lands on pixel
  `u = (x/w * 0.5 + 0.5) * width`, `v = (1 - (y/w * 0.5 + 0.5)) * height` where
  `(x, y, z, w) = projection_matrix * view_matrix * (p, 1)`. Pixels count from the top-left
  corner, u to the right, v down. Pass the camera back to `mesh_query` (raycast) to turn a pixel
  into a point on the model.
- `objects_in_view` says where each object is in the picture, so no labels are drawn into it.
- `legend` says which colour means what. Surfaces are shaded, so a pixel of an object is a darker
  or lighter variant of its legend colour.

Errors: `stale_id`, `invalid_argument`, `gl_unavailable` (the 3D view was never shown since the
application started; bring the window on screen once).

```
screenshot {"camera": {"preset": "top", "projection": "orthographic"}, "size": {"width": 800, "height": 800}}
screenshot {"camera": {"azimuth_deg": 120, "elevation_deg": 20, "target": ["o52"], "zoom": 0.8}, "editor": {"color_by": "type"}}
```

## Adding a view or an overlay (for tool groups)

`McpRenderer.hpp` has two extension points, so that other groups add to `screenshot` without
editing it. Register from your group's `MCP_TOOL_GROUP` function; the group's name must sort
before `View`.

```cpp
RenderPlugins::add_view({"preview", "Sliced toolpaths of the plate ...", options_schema,
    [](ToolContext& ctx, const ViewRequest& request) -> Picture {
        return Renderer::render_custom(request.plate_idx, request.width, request.height, request.camera,
                                       toolpaths_box, /*show_bed*/ true, request.format,
                                       [&](const Renderer::RenderTarget& target) {
                                           viewer.render(target.camera->view, target.camera->projection);
                                           (*target.legend)["features"] = ...;
                                       });
    }});

RenderPlugins::add_overlay({"support_paint", "Support paint: enforcers blue, blockers red.",
    [](const Renderer::RenderTarget& target, const json& editor_options) { /* draw with target.camera */ }});
```

A view's options are the tool argument named like the view (`"preview": {...}`); its schema is
merged into the tool's schema. `render_custom` makes the GL context current, creates the
framebuffer, fits the camera to the box you give, draws the bed, calls your pass with the
framebuffer bound, reads the picture back and encodes it.
