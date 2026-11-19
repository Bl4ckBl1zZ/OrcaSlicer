# Tools: Settings (read-only)

Source: `src/slic3r/Mcp/tools/ToolsSettings.cpp`, shared helpers `McpSettings.{hpp,cpp}`.
Tests: `tests/mcp/test_settings.py`.

One API covers every option the application defines. A setting has a **key** (the name used in
profile files), an **owner** (the preset type that holds it: `process`, `filament`, `printer`, or
the `project`), and a list of **scopes** where it can be overridden: `plate`, `object`, `part`,
`modifier`, `layer_range`. Narrower scopes override wider ones.

Scope arguments:

| Scope | Meaning |
|---|---|
| `"process"`, `"printer"` | The edited process / printer preset. |
| `{"filament": 2}` | The filament preset of slot 2. |
| `"project"` | Project-level values (filament colours, flush volumes ...). |
| `{"plate": 1}` | Per-plate settings. |
| `{"object": "o57"}` | Overrides stored on an object. |
| `{"volume": "v77"}` | Overrides stored on a part or a modifier. |
| `{"layer_range": {"object": "o57", "z_min": 0, "z_max": 5}}` | Overrides of a height range. |

Values on the wire: booleans, numbers and strings as JSON natives; percentages as `"15%"`;
float-or-percent options as a number (absolute) or a `"50%"` string; enums as their profile
string; per-filament and per-extruder options as lists, with `null` for an unset entry of a
nullable option; points as `[x, y]`. The printer's address and credentials (`printhost_apikey`,
`printhost_password`, `printhost_user`, `printhost_port`, `printhost_cafile`, `print_host`,
`print_host_webui`, `flashforge_serial_number`) are returned as `"<redacted>"` (`""` when not set),
here and in every other tool that returns setting values (`settings_set` old/new values,
`preset_diff`, unsaved-edit lists in `unsaved_changes` details and `discarded` results); their
`default` in `settings_describe` is `""`.

## settings_search

Find settings by keyword, category or owner. Read-only.

| Argument | Type | Default | |
|---|---|---|---|
| `query` | string | | Words matched against key, label, category and description; all must match. |
| `category` | string | | Category contains this (`Strength`, `Support`, `Speed` ...). |
| `owner` | `process` \| `filament` \| `printer` \| `project` | | |
| `scope` | Scope | | Only settings that can be set at this scope. |
| `mode` | `simple` \| `advanced` \| `expert` | | Only settings of this user mode or a simpler one. |
| `limit`, `offset` | integer | 20, 0 | |

Without arguments: `{"categories": {owner: {category: count}}, "total"}`. Otherwise:

```json
{"settings": [{"key": "sparse_infill_density", "label": "Sparse infill density", "category": "Strength",
               "type": "percent", "unit": "%", "min": 0.0, "max": 100.0, "default": "20%", "mode": "simple",
               "owner": "process", "scopes": ["process", "object", "part", "modifier", "layer_range"],
               "tooltip": "Density of internal sparse infill ..."}],
 "matches": 12, "truncated": true, "next": 20}
```

Other members when they apply: `values` and `labels` (enums), `vector` (`filament`, `extruder` or
`list`), `nullable`, `percent_of` (the key a percentage refers to), `read_only`, and `write`
(`"refused: ..."` for keys the agent may not set: post-processing scripts, the printer's address
and credentials (`print_host`, `print_host_webui`, `printhost_*`, `host_type`, `bbl_use_printhost`,
`printer_agent`, `flashforge_serial_number`), plug-in backed options, and G-code text unless the
user allowed it). Labels and
descriptions are the English source strings. Hits are ordered by how well they match, then by
user mode, then by key.

## settings_get

Read values at a scope. Read-only.

| Argument | Type | Default | |
|---|---|---|---|
| `scope` | Scope (required) | | |
| `keys` | string[] | | Without it: what is modified at the scope. |
| `effective` | boolean | false | Return `{value, source}`: the value in force and where it comes from. |
| `modified_only` | boolean | false | Of the given keys, only those modified at the scope. |

```
settings_get {"scope": "process", "keys": ["layer_height", "sparse_infill_density"]}
  -> {"scope": "process", "preset": "0.20mm Standard @MyKlipper", "modified": [],
      "values": {"layer_height": 0.2, "sparse_infill_density": "15%"}}

settings_get {"scope": {"volume": "v77"}, "keys": ["sparse_infill_density", "nozzle_diameter"], "effective": true}
  -> {"scope": "volume:v77",
      "values": {"sparse_infill_density": {"value": "100%", "source": "volume:v77"},
                 "nozzle_diameter": {"value": [0.4], "source": "printer"}}}
```

- Stored values (`effective` false): a key the scope does not set is `null`. A key that cannot
  live at the scope at all is the error `key_not_in_scope`, which lists where it can.
- Effective values are layered as slicing layers them: the global configuration (process,
  filaments, printer, project) <- plate (per-plate keys) <- object <- volume or layer range.
  Sources: `process`, `filament`, `printer`, `project`, `plate:1`, `object:o57`, `volume:v77`,
  `layer_range:0-5`. They are computed before slicing; limits the slicer applies afterwards are
  not reflected.
- For a preset scope the result also has `preset` (its name) and `modified` (keys that differ
  from the saved preset).

Errors: `unknown_key` (with `details.nearest`), `key_not_in_scope` (with `details.scopes`),
`stale_id`, `invalid_argument`.

## preset_list

List printer, process or filament presets. Read-only.

| Argument | Type | Default |
|---|---|---|
| `type` | `printer` \| `process` \| `filament` (required) | |
| `query` | string (name contains) | |
| `compatible_only` | boolean | true |
| `limit`, `offset` | integer | 50, 0 |

Result: `{"type", "selected", "presets": [{"name", "system", "compatible", "selected"?,
"modified_keys"?, "inherits"?, "slots"? (filament), "filament_type"? (filament)}], "total",
"truncated"?, "next"?}`.

## filament_list

The project's filament slots. Read-only, no arguments.

```json
{"filaments": [{"slot": 1, "preset": "Generic PLA @System", "type": "PLA", "color": "#26A69A",
                "used_by": {"objects": ["o52"], "parts": [], "paint": []}}]}
```

`used_by.objects` are objects whose filament is this slot (and that have a part inheriting it),
`parts` are parts or modifiers set to it explicitly, `paint` are parts where it appears only
through colour painting.

## For tool groups: McpSettings.hpp

Everything the settings tools share lives in `McpSettings.hpp`; a group that writes settings uses
it instead of its own conversion:

- `Settings::find_def`, `owner_of`, `scopes_of`, `allowed_on_object / volume / layer_range /
  plate`, `vector_kind`, `nearest_keys`, `describe`.
- `Settings::parse_scope(json)` -> `Scope`; `key_allowed(scope, key, &why, &where)`;
  `stored_config(scope)`, `preset_of(scope)`, `modified_keys(scope)`,
  `effective_value(scope, key, full_config)`.
- `Settings::value_json` / `value_json_for(config, key)` (option -> wire, with redaction) and
  `option_from_json(key, value, vector_size)` (wire -> a new `ConfigOption`, validated: type,
  enum membership, range, vector length, with a scalar repeated for every entry). It throws
  `unknown_key`, `invalid_value` or `out_of_range` with the accepted values in the message.
- `Settings::write_refusal(key)` and `is_secret(key)`: the access rules of the security section.
  Every tool that writes a setting must refuse a key with a non-empty `write_refusal`
  (`forbidden_key`).
