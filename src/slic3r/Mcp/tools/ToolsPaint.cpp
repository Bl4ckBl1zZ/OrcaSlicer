// Paint tools: paint_apply (support / seam enforcers and blockers, multi-material colour, fuzzy skin,
// selected by geometric rules instead of a brush) and paint_get (statistics, painted regions,
// facet ids). Colour / pause / custom G-code by layer (plate_layer_events) lives in
// paint/LayerEvents.cpp, the screenshot overlays in paint/PaintOverlay.cpp.
//
// The geometry is libslic3r/FacetPaint.hpp. This file does what a paint gizmo does around it:
// resolve the parts, commit to the FacetsAnnotation, refresh the object list, schedule the
// background process. The dispatcher closes an open gizmo before any mutating call, so a paint
// gizmo never holds stale selectors (McpDispatcher.cpp).

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Exception.hpp"
#include "libslic3r/FacetPaint.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../McpIds.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"
#include "paint/LayerEvents.hpp"
#include "paint/PaintDebug.hpp"
#include "paint/PaintOverlay.hpp"
#include "paint/PaintSelector.hpp"
#include "paint/PaintStats.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

using namespace Paint;

double r3(double v) { return std::round(v * 1000.) / 1000.; }

// Parts addressed by a `targets` argument: an object (all its model parts, first instance), an
// instance (the object's model parts seen through that instance) or a part.
std::vector<PaintTarget> resolve_targets(const json& ids)
{
    Plater*                  plater = wxGetApp().plater();
    PartPlateList&           plates = plater->get_partplate_list();
    const std::vector<ItemRef> items = Ids::items(ids, "targets");
    std::vector<PaintTarget> out;
    std::set<const ModelVolume*> seen;
    for (size_t i = 0; i < items.size(); ++i) {
        const ItemRef& ref = items[i];
        const int      inst_idx = ref.kind == IdKind::Instance ? ref.inst_idx : 0;
        auto add = [&](ModelVolume* volume, int vol_idx) {
            if (!seen.insert(volume).second)
                return;
            PaintTarget t;
            t.object = ref.object;
            t.obj_idx = ref.obj_idx;
            t.volume = volume;
            t.vol_idx = vol_idx;
            t.inst_idx = inst_idx;
            if (inst_idx < static_cast<int>(ref.object->instances.size()))
                t.instance_world = ref.object->instances[static_cast<size_t>(inst_idx)]->get_matrix();
            t.plate_idx = Ids::plate_of_instance(ref.obj_idx, inst_idx);
            if (t.plate_idx >= 0 && t.plate_idx < plates.get_plate_count())
                t.plate_origin = plates.get_plate(t.plate_idx)->get_origin();
            out.push_back(t);
        };
        const std::string label = "targets[" + std::to_string(i) + "]";
        if (ref.kind == IdKind::Volume) {
            if (!ref.volume->is_model_part())
                fail("not_a_part",
                     "`" + label + "` = " + Ids::of(*ref.volume) + " is a " + (ref.volume->is_modifier() ? std::string("modifier") :
                                                                             ref.volume->is_support_enforcer() ? std::string("support enforcer") :
                                                                             ref.volume->is_support_blocker() ? std::string("support blocker") :
                                                                             ref.volume->is_negative_volume() ? std::string("negative part") : std::string("non-printing body")) +
                         "; only model parts carry paint.",
                     "Paint the object or one of its parts (scene_get lists them with type \"part\"). A modifier or enforcer body already acts on its "
                     "whole volume.");
            add(ref.volume, ref.vol_idx);
        } else if (ref.kind == IdKind::Object || ref.kind == IdKind::Instance) {
            for (size_t v = 0; v < ref.object->volumes.size(); ++v)
                if (ref.object->volumes[v]->is_model_part())
                    add(ref.object->volumes[v], static_cast<int>(v));
        } else
            fail("invalid_argument", "`" + label + "` must be an object, instance or part id.");
    }
    if (out.empty())
        fail("not_a_part", "The targets have no model parts to paint.");
    return out;
}

std::set<int> objects_of(const std::vector<PaintTarget>& targets)
{
    std::set<int> out;
    for (const PaintTarget& t : targets)
        out.insert(t.obj_idx);
    return out;
}

void instance_notes(ToolContext& ctx, const std::vector<PaintTarget>& targets)
{
    std::set<int> noted;
    for (const PaintTarget& t : targets) {
        if (!noted.insert(t.obj_idx).second)
            continue;
        if (t.object->instances.size() > 1)
            ctx.note(Ids::of(*t.object) + " has " + std::to_string(t.object->instances.size()) +
                     " instances; paint is shared by all of them. Plate and world coordinates refer to " +
                     Ids::of(*t.object->instances[static_cast<size_t>(t.inst_idx)]) + " (target an instance id to use another).");
        if (t.plate_idx < 0)
            ctx.note(Ids::of(*t.object) + " is on no plate; its plate frame is the world frame.");
    }
}

json part_header(const PaintTarget& t)
{
    json e = json::object();
    e["part"] = t.id();
    e["object"] = Ids::of(*t.object);
    e["name"] = t.volume->name;
    if (!t.object->instances.empty())
        e["instance"] = Ids::of(*t.object->instances[static_cast<size_t>(t.inst_idx)]);
    e["plate"] = t.plate_idx >= 0 ? json(t.plate_idx + 1) : json(nullptr);
    return e;
}

std::map<std::string, double> areas(FacetPaint::Channel channel, const FacetPaint::Stats& stats)
{
    std::map<std::string, double> out;
    for (const FacetPaint::StateStats& s : stats.states)
        out[state_name(channel, s.state)] += s.area;
    return out;
}

// What the paint gizmos do after committing a stroke (GLGizmoFdmSupports::update_model_object and
// siblings): refresh the object list's info rows, recompute plate filament use for colour, and
// schedule the background process; plus a repaint so colour paint shows at once.
void after_commit(const std::set<int>& objects, FacetPaint::Channel channel)
{
    Plater*     plater = wxGetApp().plater();
    ObjectList* list = wxGetApp().obj_list();
    for (int obj_idx : objects) {
        list->update_info_items(static_cast<size_t>(obj_idx));
        if (channel == FacetPaint::Channel::Color)
            plater->get_partplate_list().notify_instance_update(obj_idx, 0);
    }
    plater->schedule_background_process();
    if (GLCanvas3D* canvas = plater->get_view3D_canvas3D(); canvas != nullptr) {
        canvas->set_as_dirty();
        canvas->request_extra_frame();
    }
}

// The value of a setting for a part as slicing will see it: part, then object, then the process.
const ConfigOption* effective_option(const PaintTarget& t, const char* key)
{
    if (t.volume->config.has(key))
        return t.volume->config.get().option(key);
    if (t.object->config.has(key))
        return t.object->config.get().option(key);
    return wxGetApp().preset_bundle->prints.get_edited_preset().config.option(key);
}

// Tell the agent when the settings make the new paint ineffective, as a user would see in the GUI.
void setting_notes(ToolContext& ctx, const std::vector<PaintTarget>& targets, FacetPaint::Channel channel, EnforcerBlockerType state)
{
    std::set<std::string> said;
    for (const PaintTarget& t : targets) {
        const std::string obj = Ids::of(*t.object);
        if (channel == FacetPaint::Channel::Support && state == EnforcerBlockerType::ENFORCER) {
            const ConfigOption* enable = effective_option(t, "enable_support");
            if (enable != nullptr && !enable->getBool() && said.insert("support_off " + obj).second)
                ctx.note("Supports are off for " + obj + " (enable_support): enforcers take effect only with enable_support on.");
            const ConfigOption* type = effective_option(t, "support_type");
            if (type != nullptr && type->serialize().find("auto") != std::string::npos && said.insert("support_auto " + obj).second)
                ctx.note("support_type of " + obj + " is " + type->serialize() +
                         ": enforcers add to the automatic supports. For support only where enforced use normal(manual) or tree(manual).");
        }
        if (channel == FacetPaint::Channel::FuzzySkin && state == EnforcerBlockerType::FUZZY_SKIN) {
            const ConfigOption* fuzzy = effective_option(t, "fuzzy_skin");
            if (fuzzy != nullptr && fuzzy->serialize() == "disabled_fuzzy" && said.insert("fuzzy " + obj).second)
                ctx.note("fuzzy_skin of " + obj + " is disabled (\"disabled_fuzzy\"): painted fuzzy skin takes effect when fuzzy_skin is \"none\" "
                         "(Painted only) or another mode.");
        }
    }
}

json paint_apply(ToolContext& ctx)
{
    const json& args = ctx.args();
    FacetPaint::Channel channel;
    if (!parse_channel(args["channel"].get<std::string>(), channel))
        fail("invalid_argument", "`channel` must be support, seam, color or fuzzy_skin.");
    const std::string op = ctx.arg("op", "set");
    const int         filaments = Ids::filament_count();

    // --- validate --------------------------------------------------------------------------
    EnforcerBlockerType state = EnforcerBlockerType::NONE;
    if (op == "set" || op == "replace") {
        if (!ctx.has("state"))
            fail("invalid_argument", "`state` is required for op \"" + op + "\".",
                 channel == FacetPaint::Channel::Color ? "color: {\"filament\": n} or \"none\"." :
                 channel == FacetPaint::Channel::FuzzySkin ? "fuzzy_skin: \"fuzzy\" or \"none\"." : "support / seam: \"enforce\", \"block\" or \"none\".");
        state = parse_state(channel, args["state"], "state", filaments);
    } else if (ctx.has("state")) {
        if (parse_state(channel, args["state"], "state", std::max(filaments, int(EnforcerBlockerType::ExtruderMax))) != EnforcerBlockerType::NONE)
            fail("invalid_argument", "op \"" + op + "\" removes paint; drop `state` or give \"none\".",
                 op == "erase" ? "To paint a state use op \"set\"." : "To clear and repaint use op \"replace\".");
    }
    if (op == "clear") {
        if (ctx.has("selector"))
            fail("invalid_argument", "op \"clear\" removes all paint of the channel and takes no `selector`.",
                 "To remove paint only inside a region use op \"erase\" with a selector.");
    } else if (!ctx.has("selector"))
        fail("invalid_argument", "`selector` is required for op \"" + op + "\".", "{\"all\": true} selects the whole surface.");

    std::optional<EnforcerBlockerType> only_where;
    if (ctx.has("only_where"))
        only_where = parse_state(channel, args["only_where"], "only_where", std::max(filaments, int(EnforcerBlockerType::ExtruderMax)));

    const std::string mode = ctx.arg("mode", "precise");
    const double      resolution = ctx.arg<double>("resolution_mm", 0.3);
    const size_t      max_new = ctx.arg<size_t>("max_new_triangles", 300000);
    const bool        coarsen = ctx.arg<bool>("coarsen", true);
    const bool        dry_run = ctx.arg<bool>("dry_run", false);

    std::vector<PaintTarget> targets = resolve_targets(args["targets"]);

    SelectorSpec spec;
    if (op != "clear") {
        spec = parse_selector(args["selector"]);
        if (ctx.has("limit_to_overhang_deg")) {
            SelectorSpec overhang;
            overhang.kind = SelectorSpec::Kind::Overhang;
            overhang.path = "limit_to_overhang_deg";
            overhang.x = ctx.arg<double>("limit_to_overhang_deg", 45.);
            overhang.a = Vec3d::UnitZ();
            SelectorSpec both;
            both.kind = SelectorSpec::Kind::And;
            both.path = "selector";
            both.children.push_back(std::move(spec));
            both.children.push_back(std::move(overhang));
            spec = std::move(both);
        }
        resolve_seeds(spec, targets);
    }

    // --- compute everything before changing anything ------------------------------------------
    std::vector<FacetPaint::Result> results;
    results.reserve(targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        const PaintTarget&  t = targets[i];
        FacetPaint::Request req;
        req.selector = op == "clear" ? FacetPaint::Selector::none() : compile(spec, targets, i);
        req.state = state;
        req.only_where = only_where;
        req.clear_first = op == "replace" || op == "clear";
        req.mode = mode == "facets" ? FacetPaint::Mode::WholeFacets : FacetPaint::Mode::Precise;
        req.resolution = resolution;
        req.max_new_triangles = max_new;
        req.coarsen_to_fit = coarsen;
        req.max_state = FacetPaint::max_state(channel);
        try {
            results.push_back(FacetPaint::apply(t.volume->mesh(), FacetPaint::annotation(*t.volume, channel).get_data(), t.transforms(), req));
        } catch (const Slic3r::InvalidArgument& e) {
            fail("invalid_argument", std::string("`selector`: ") + e.what() + " (part " + t.id() + ").");
        }
        const FacetPaint::Result& r = results.back();
        if (r.status == FacetPaint::Status::TooFine)
            fail("too_fine",
                 "Painting " + t.id() + " at resolution " + std::to_string(resolution) + " mm would add more than " + std::to_string(max_new) +
                     " triangles.",
                 "Use a coarser resolution_mm, mode \"facets\", a smaller region, raise max_new_triangles, or allow coarsen (the default).",
                 json::object({ { "part", t.id() }, { "resolution_mm", resolution }, { "max_new_triangles", max_new } }));
    }

    bool any_changed = false;
    for (const FacetPaint::Result& r : results)
        any_changed = any_changed || r.changed;

    // --- commit -------------------------------------------------------------------------------
    if (any_changed && !dry_run) {
        std::string summary = std::string("paint ") + channel_name(channel) + " ";
        summary += op == "set" || op == "replace" ? (op == "replace" ? "replace with " : "") + state_name(channel, state) : op;
        std::set<std::string> objs;
        for (const PaintTarget& t : targets)
            objs.insert(Ids::of(*t.object));
        summary += " on";
        size_t n = 0;
        for (const std::string& o : objs) {
            if (n++ == 3) {
                summary += " ...";
                break;
            }
            summary += " " + o;
        }
        ctx.snapshot(summary);
        std::set<int> changed_objects;
        for (size_t i = 0; i < targets.size(); ++i) {
            if (!results[i].changed)
                continue;
            TriangleSelector::TriangleSplittingData data = results[i].data;
            FacetPaint::annotation(*targets[i].volume, channel).set_data(std::move(data));
            changed_objects.insert(targets[i].obj_idx);
        }
        after_commit(changed_objects, channel);
        ctx.set_slice_invalidated();
    }

    // --- result -------------------------------------------------------------------------------
    json   parts = json::array();
    size_t added = 0, selected = 0;
    double used = resolution;
    std::map<std::string, double> before_total, after_total;
    for (size_t i = 0; i < targets.size(); ++i) {
        const FacetPaint::Result& r = results[i];
        json e = part_header(targets[i]);
        e["changed"] = r.changed;
        e["facets_selected"] = r.facets_selected;
        e["triangles_added"] = r.triangles_added;
        e["resolution_used_mm"] = r3(r.resolution_used);
        e["surface_mm2"] = r3(r.after.total_area);
        e["before"] = states_json(channel, r.before);
        e["after"] = states_json(channel, r.after);
        json delta = json::object();
        std::map<std::string, double> b = areas(channel, r.before), a = areas(channel, r.after);
        for (const auto& [k, v] : a)
            delta[k] = r3(v - (b.count(k) ? b[k] : 0.));
        for (const auto& [k, v] : b)
            if (!a.count(k))
                delta[k] = r3(-v);
        e["delta_mm2"] = std::move(delta);
        for (const auto& [k, v] : b)
            before_total[k] += v;
        for (const auto& [k, v] : a)
            after_total[k] += v;
        added += r.triangles_added;
        selected += r.facets_selected;
        used = std::max(used, r.resolution_used);
        parts.push_back(std::move(e));
    }
    auto totals = [](const std::map<std::string, double>& m) {
        json o = json::object();
        for (const auto& [k, v] : m)
            o[k] = r3(v);
        return o;
    };

    json out = json::object();
    out["ok"] = true;
    out["changed"] = any_changed && !dry_run;
    if (dry_run) {
        out["dry_run"] = true;
        out["would_change"] = any_changed;
    }
    out["channel"] = channel_name(channel);
    out["op"] = op;
    if (op == "set" || op == "replace")
        out["state"] = state_name(channel, state);
    out["mode"] = mode;
    out["resolution_mm"] = resolution;
    out["resolution_used_mm"] = r3(used);
    out["coarsened"] = used > resolution + 1e-12;
    out["facets_selected"] = selected;
    out["triangles_added"] = added;
    out["area_mm2"] = json::object({ { "before", totals(before_total) }, { "after", totals(after_total) } });
    out["parts"] = std::move(parts);
    if (op != "clear" && has_seeds(spec))
        out["seeds"] = seeds_json(spec, targets);
    if (used > resolution + 1e-12)
        ctx.note("The resolution was coarsened to " + std::to_string(r3(used)) + " mm to stay within " + std::to_string(max_new) +
                 " new triangles (max_new_triangles); boundaries are less sharp than asked.");
    if (!any_changed)
        ctx.note("Nothing changed: the selection is empty, or already in that state.");
    if (selected == 0 && op != "clear")
        ctx.note("The selector touched no facet of the targets. Check the frame (default \"plate\") and the coordinates against paint_get's "
                 "part boxes or scene_get.");
    if (any_changed && (op == "set" || op == "replace"))
        setting_notes(ctx, targets, channel, state);
    instance_notes(ctx, targets);
    return out;
}

json paint_get(ToolContext& ctx)
{
    const json& args = ctx.args();
    std::vector<FacetPaint::Channel> channels;
    if (ctx.has("channel")) {
        FacetPaint::Channel c;
        if (!parse_channel(args["channel"].get<std::string>(), c))
            fail("invalid_argument", "`channel` must be support, seam, color or fuzzy_skin.");
        channels.push_back(c);
    } else
        channels = { FacetPaint::Channel::Support, FacetPaint::Channel::Seam, FacetPaint::Channel::Color, FacetPaint::Channel::FuzzySkin };
    const bool   components = ctx.arg<bool>("components", false);
    const bool   facets = ctx.arg<bool>("facets", false);
    const size_t limit = ctx.arg<size_t>("limit", 20);

    const std::vector<PaintTarget> targets = resolve_targets(args["targets"]);
    json parts = json::array();
    for (const PaintTarget& t : targets) {
        json e = part_header(t);
        const indexed_triangle_set& its = t.volume->mesh().its;
        e["facets"] = its.indices.size();
        const Transform3d world = t.instance_plate() * t.volume->get_matrix();
        const Transform3d object = t.volume->get_matrix();
        BoundingBoxf3 box, box_object;
        for (const stl_vertex& v : its.vertices) {
            box.merge(Vec3d(world * v.cast<double>()));
            box_object.merge(Vec3d(object * v.cast<double>()));
        }
        e["box"] = box_json(box);
        e["box_object"] = box_json(box_object);
        json channels_out = json::object();
        double surface = 0.;
        for (FacetPaint::Channel c : channels) {
            const FacetsAnnotation& annotation = FacetPaint::annotation(*t.volume, c);
            if (annotation.empty() && channels.size() > 1)
                continue;
            const FacetPaint::Stats stats = FacetPaint::statistics(t.volume->mesh(), annotation.get_data(), t.transforms(), FacetPaint::max_state(c));
            surface = stats.total_area;
            json ch = json::object();
            ch["states"] = states_json(c, stats);
            ch["triangles"] = stats.live_triangles;
            if (components) {
                size_t total = 0;
                ch["regions"] = components_json(t, c, limit, total);
                ch["regions_total"] = total;
            }
            if (facets)
                ch["facet_ids"] = facets_json(t, c, std::max<size_t>(limit, 200));
            channels_out[channel_name(c)] = std::move(ch);
        }
        if (surface == 0.) {
            const FacetPaint::Stats stats = FacetPaint::statistics(t.volume->mesh(), TriangleSelector::TriangleSplittingData{}, t.transforms());
            surface = stats.total_area;
        }
        e["surface_mm2"] = r3(surface);
        e["channels"] = std::move(channels_out);
        parts.push_back(std::move(e));
    }
    json out = json::object();
    out["parts"] = std::move(parts);
    out["frames"] = "box / centroid / mean_normal: plate frame (mm from the plate origin, as scene_get); box_object: the object's own frame.";
    instance_notes(ctx, targets);
    return out;
}

} // namespace

MCP_TOOL_GROUP(Paint)
{
    {
        json schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "targets": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 1,
                            "description": "Objects (all their model parts), instances (same, with that instance as the plate/world frame) or parts (v...). Modifiers and other bodies cannot be painted."},
                "channel": {"enum": ["support", "seam", "color", "fuzzy_skin"]},
                "state": {"description": "support, seam: \"enforce\" | \"block\" | \"none\". color: {\"filament\": n} (1-based slot) | \"none\" (the part's own filament). fuzzy_skin: \"fuzzy\" | \"none\". Required for op set / replace.",
                          "oneOf": [{"enum": ["enforce", "block", "fuzzy", "none"]},
                                    {"type": "object", "properties": {"filament": {"type": "integer", "minimum": 1}}, "required": ["filament"], "additionalProperties": false}]},
                "selector": {"$ref": "#/$defs/Selector"},
                "op": {"enum": ["set", "erase", "replace", "clear"], "default": "set",
                       "description": "set: paint `state` inside the selection. erase: remove paint inside it. replace: clear the channel on the targets, then paint. clear: remove all paint of the channel (no selector)."},
                "only_where": {"description": "Only repaint surface currently in this state (e.g. recolour what is filament 2, or turn blockers into enforcers).",
                               "oneOf": [{"enum": ["enforce", "block", "fuzzy", "none"]},
                                         {"type": "object", "properties": {"filament": {"type": "integer", "minimum": 1}}, "required": ["filament"], "additionalProperties": false}]},
                "mode": {"enum": ["precise", "facets"], "default": "precise",
                         "description": "precise: region boundaries cut through triangles (subdivided down to resolution_mm). facets: whole original triangles whose centroid is inside."},
                "resolution_mm": {"type": "number", "minimum": 0.05, "maximum": 50, "default": 0.3, "description": "Longest triangle side along a region boundary, in mm (world)."},
                "max_new_triangles": {"type": "integer", "minimum": 1000, "maximum": 5000000, "default": 300000},
                "coarsen": {"type": "boolean", "default": true, "description": "When the triangle budget would be exceeded, double the resolution until it fits (reported as coarsened) instead of failing with too_fine."},
                "limit_to_overhang_deg": {"type": "number", "minimum": 0, "maximum": 90, "description": "Shorthand: AND the selector with {\"overhang\": {\"threshold_deg\": x}}."},
                "dry_run": {"type": "boolean", "default": false, "description": "Compute and report the result without changing anything."}
            },
            "required": ["targets", "channel"],
            "additionalProperties": false})JSON", "paint_apply");
        schema["$defs"] = Schema::parse(selector_defs_json(), "paint selector $defs");

        ToolDef tool;
        tool.name = "paint_apply";
        tool.title = "Paint supports, seams, colours, fuzzy skin";
        tool.description =
            "Paint model surfaces like the paint gizmos, but by rule instead of a brush: support enforcers/blockers, seam enforcers/blockers, "
            "multi-material colour (filament slot) and fuzzy skin. One undo step; the object list, 3D view and plate are refreshed and the plate "
            "must be re-sliced. Region boundaries are sharp: triangles crossing them are subdivided to resolution_mm.\n"
            "Selector grammar (exactly one key per object): {\"all\":true}; {\"facets\":[ids]}; {\"normal\":{\"direction\":[0,0,1],\"max_angle_deg\":10}}; "
            "{\"overhang\":{\"threshold_deg\":45}} (normal within 45 deg of straight down, as support_threshold_angle); {\"z_range\":{\"min\":2,\"max\":5}}; "
            "{\"slab\":{\"normal\":[1,1,0],\"min\":0,\"max\":3}}; {\"halfspace\":{\"point\":[x,y,z],\"normal\":[0,0,-1]}}; {\"box\":{\"min\":[..],\"max\":[..]}} or "
            "{\"box\":{\"center\":[..],\"size\":[..],\"rotation_deg\":[0,0,30]}}; {\"sphere\":{\"center\":[..],\"radius\":4}}; "
            "{\"cylinder\":{\"base\":[..],\"axis\":[0,0,1],\"radius\":3,\"height\":10}}; {\"capsule\":{\"a\":[..],\"b\":[..],\"radius\":2}}; "
            "{\"stroke\":{\"path\":[[x,y,z],...],\"radius\":1.5}}; {\"connected\":{\"seed\":SEED,\"angle_deg\":30}} (smart fill); "
            "{\"same_state\":{\"seed\":SEED}} (bucket fill); {\"and\":[...]}, {\"or\":[...]}, {\"not\":{...}}. Positions take \"frame\": "
            "\"plate\" (default; mm from the plate origin as scene_get reports), \"world\" or \"object\" (the object's own coordinates; paint_get gives "
            "box_object). SEED: {\"point\":[x,y,z]} (snapped to the surface), {\"ray\":{\"origin\":..,\"direction\":..}}, {\"facet\":n,\"part\":\"v..\"} "
            "or {\"pixel\":[u,v],\"camera\":<camera object returned by screenshot>} - look at a picture, point at a pixel, fill that surface.\n"
            "Example, supports only under an arm: {\"targets\":[\"o3\"],\"channel\":\"support\",\"state\":\"enforce\",\"selector\":{\"and\":[{\"box\":{\"min\":[-30,-15,20],"
            "\"max\":[-5,15,26],\"frame\":\"object\"}},{\"overhang\":{\"threshold_deg\":45}}]}}. Logo in white: channel color, state {\"filament\":2}, "
            "selector {\"connected\":{\"seed\":{\"pixel\":[520,300],\"camera\":{...}},\"angle_deg\":20}}.\n"
            "Returns per part and state the area in mm2, share of the surface, triangle count and box (plate frame) before and after, "
            "delta_mm2, triangles added, the resolution actually used, where each seed landed, and changed. dry_run previews. "
            "Supports need enable_support and a manual support_type to use only enforcers; colour needs the filament slot to exist (filament_list). "
            "A change at a height is cheaper than painting a band: plate_layer_events.";
        tool.input_schema = std::move(schema);
        tool.handler = paint_apply;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "paint_get";
        tool.title = "Paint statistics";
        tool.description =
            "What is painted where: for each model part of the targets and each paint channel (support, seam, color, fuzzy_skin) the painted "
            "states with area in mm2, share of the surface, triangle count and bounding box (plate frame and object frame), plus the part's own "
            "box in both frames and its surface area - the coordinates to write selectors in. components: true lists the connected painted regions "
            "(largest first: state, area, box, centroid, mean normal, flatness) so you can tell e.g. a support patch under an arm from a stray one. "
            "facets: true lists the original facet ids per state. Without `channel`, only painted channels are listed. Read-only.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "targets": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 1, "description": "Objects, instances or parts."},
                "channel": {"enum": ["support", "seam", "color", "fuzzy_skin"]},
                "components": {"type": "boolean", "default": false},
                "facets": {"type": "boolean", "default": false},
                "limit": {"type": "integer", "minimum": 1, "maximum": 1000, "default": 20, "description": "Maximum regions per channel and part (facet ids: max(limit, 200) per state)."}
            },
            "required": ["targets"],
            "additionalProperties": false})JSON", "paint_get");
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = paint_get;
        registry.add(std::move(tool));
    }
    Paint::register_layer_events(registry);
    Paint::register_overlays();
    Paint::register_debug_tools(registry);
}

}}} // namespace Slic3r::GUI::Mcp
