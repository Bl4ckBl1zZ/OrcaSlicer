// Analysis tools on a sliced plate: slice_layers (per-layer table, cross-section of one layer) and
// analyze (printability checks, region queries, before/after diff). Backed by the slice-analysis
// core (libslic3r/SliceAnalysis.hpp) through tools/slice/SliceAnalysisGlue.hpp.

#include <chrono>
#include <string>
#include <vector>

#include "../McpIds.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"
#include "slice/SliceAnalysisGlue.hpp"
#include "slice/SliceData.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

using namespace Slice;

int plate_arg(const ToolContext& ctx) { return Ids::plate(ctx.has("plate") ? ctx.args()["plate"] : json(nullptr)); }

void require_sliced(int plate_idx)
{
    if (plate_busy(plate_idx))
        fail("busy_slicing", "The plate is being sliced right now.", "Wait with slice_status, then ask again.");
    if (!plate_has_result(plate_idx)) {
        json state = plate_state(plate_idx);
        fail("not_sliced", "Plate " + std::to_string(plate_idx + 1) + " has no valid slice result (state: " + state.value("state", "") + ").",
             state.value("state", "") == "invalidated" ? "The result was invalidated by a change; call slice again." : "Call slice first.", state);
    }
}

json slice_layers(ToolContext& ctx)
{
    Analysis::require();
    const int plate_idx = plate_arg(ctx);
    require_sliced(plate_idx);
    const auto   start = std::chrono::steady_clock::now();
    const double computed_ms = Analysis::ensure(plate_idx);
    json out;
    if (ctx.arg("detail", "table") == "section") {
        if (!ctx.has("layer") && !ctx.has("z"))
            fail("invalid_argument", "detail \"section\" needs `layer` or `z`.");
        out = Analysis::section(plate_idx, ctx.arg<int>("layer", 1), ctx.arg<double>("z", 0.), ctx.has("z") && !ctx.has("layer"),
                                ctx.has("box") ? ctx.args()["box"] : json(nullptr));
    } else {
        std::vector<std::string> columns = ctx.arg<std::vector<std::string>>("columns", {});
        int from = ctx.arg<int>("from", 1), to = ctx.arg<int>("to", 0);
        if (ctx.has("layer"))
            from = to = ctx.arg<int>("layer", 1);
        out = Analysis::layers(plate_idx, from, to, ctx.arg<int>("every", 1), columns, ctx.arg<int>("limit", 50));
        if (out.value("truncated", false))
            ctx.note("More layers follow: call again with from = next, or use `every` to sample.");
    }
    out["analysis_ms"] = round_to(computed_ms > 0. ? computed_ms : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), 1);
    return out;
}

json analyze(ToolContext& ctx)
{
    Analysis::require();
    const int plate_idx = plate_arg(ctx);
    require_sliced(plate_idx);
    const auto   start = std::chrono::steady_clock::now();
    const double computed_ms = Analysis::ensure(plate_idx);
    const int    max_items = ctx.arg<int>("max_items", 5);
    const json   params = ctx.has("params") ? ctx.args()["params"] : json::object();
    json out = json::object();
    out["plate"] = plate_idx + 1;
    out["frame"] = "plate";
    json verdicts = json::object();
    for (const json& c : ctx.args()["checks"]) {
        const std::string name = c.get<std::string>();
        const json p = params.contains(name) ? params[name] : json::object();
        json result;
        if (name == "summary")
            result = Analysis::summary(plate_idx);
        else if (name == "diff")
            result = Analysis::diff(plate_idx, std::max(max_items, 10), p.value("rel_tolerance", 0.001));
        else if (name == "region") {
            if (!p.contains("box"))
                fail("invalid_argument", "check \"region\" needs params.region.box = {min: [x, y(, z)], max: [x, y(, z)]} in plate coordinates.");
            result = Analysis::box_query(plate_idx, p["box"], p.value("features", std::vector<std::string>{}), p.value("support_only", false));
        } else
            result = Analysis::check(plate_idx, name, p, max_items);
        if (result.is_object() && result.contains("verdict") && result["verdict"].is_object())
            verdicts[name] = result["verdict"].value("verdict", "");
        out[name] = std::move(result);
    }
    if (!verdicts.empty())
        out["verdicts"] = std::move(verdicts);
    out["analysis_ms"] = round_to(computed_ms, 1);
    out["elapsed_ms"] = round_to(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), 1);
    return out;
}

} // namespace

MCP_TOOL_GROUP(Analysis)
{
    {
        ToolDef tool;
        tool.name = "slice_layers";
        tool.title = "Per-layer table or one layer's cross-section";
        tool.description =
            "Per-layer data of a sliced plate (layers 1-based as the preview slider shows): z, height, time, extruded volume, highest "
            "volumetric flow and speed, fan and temperature, tool changes, whether the layer has support / bridges / overhang walls; "
            "`columns` adds roles (per feature) and filaments. Paged: `from`, `to`, `every`, `limit` (max 200; `next` when more). With "
            "detail \"section\" and `layer` or `z` (optionally an XY `box` in plate coordinates): the cross-section of one layer from the "
            "moves (features, lengths, widths, filled area) and from the sliced geometry (islands, area, wall count and thickness, measured "
            "infill density, regions with their infill settings). Fails with not_sliced or busy_slicing.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "plate": {"$ref": "#/$defs/PlateRef"},
                "from": {"type": "integer", "minimum": 1, "default": 1},
                "to": {"type": "integer", "minimum": 1},
                "every": {"type": "integer", "minimum": 1, "default": 1},
                "limit": {"type": "integer", "minimum": 1, "maximum": 200, "default": 50},
                "columns": {"type": "array", "items": {"type": "string"}, "description": "Row fields to return (default: index, z, height, time_s, volume_mm3, max_flow, max_speed, fan_max, temp_max, tool_changes, has_support, has_bridge, has_overhang_wall). Others: length_mm, mean_flow, max_flow_at, fan_min, temp_min, roles, filaments, has_ironing, has_gap_fill."},
                "layer": {"type": "integer", "minimum": 1},
                "z": {"type": "number"},
                "detail": {"enum": ["table", "section"], "default": "table"},
                "box": {"type": "object", "properties": {"min": {"$ref": "#/$defs/Vec2"}, "max": {"$ref": "#/$defs/Vec2"}}, "required": ["min", "max"], "additionalProperties": false}
            },
            "additionalProperties": false})JSON", "slice_layers");
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = slice_layers;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "analyze";
        tool.title = "Printability checks of a sliced plate";
        tool.description =
            "Checks on a sliced plate, each with a verdict (ok / warning / problem) and the worst places (layer, z, x, y in plate "
            "coordinates, object id): flow (volumetric flow vs filament_max_volumetric_speed), speeds (commanded vs configured), cooling "
            "(layer times vs slow-down / fan thresholds), supports (amount, layers, clusters, contact area per object), overhangs (bridges, "
            "overhang walls, longest bridge, overhang bands), unsupported (islands with nothing below: floating starts, cantilevers), "
            "first_layer (contact area, footprint, aspect ratio, brim/raft), seams (count, direction, alignment per object), thin_features "
            "(gap fill, narrow walls, dropped features), multi_material (tool changes, purge, prime tower), summary (totals, per feature, "
            "per object), region (what is printed inside params.region.box; support_only), diff (what changed since this plate's previous "
            "slice: largest relative changes first). `params` per check, e.g. {\"flow\": {\"tolerance\": 0.05}}. Lists are capped at "
            "`max_items` (with <name>_total).";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "plate": {"$ref": "#/$defs/PlateRef"},
                "checks": {"type": "array", "minItems": 1, "items": {"enum": ["flow", "speeds", "cooling", "supports", "overhangs", "unsupported", "first_layer", "seams", "thin_features", "multi_material", "summary", "region", "diff"]}},
                "params": {"type": "object", "description": "Per-check parameters keyed by check name."},
                "max_items": {"type": "integer", "minimum": 1, "maximum": 50, "default": 5}
            },
            "required": ["checks"],
            "additionalProperties": false})JSON", "analyze");
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = analyze;
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::Mcp
