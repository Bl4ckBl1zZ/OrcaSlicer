// Scene tools: scene_get (read-only) and object_update (the small mutating tool that serves as
// the worked example of the mutating pattern: validate, snapshot, change the Model, refresh,
// return the changed subtree).

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../McpIds.hpp"
#include "../McpRefresh.hpp"
#include "../McpScene.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"
#include "../McpUtil.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// Results are kept well under what a client moves to a file (Claude Code: 50 000 characters).
const size_t SCENE_BUDGET_CHARS = 18000;

json scene_get(ToolContext& ctx)
{
    Plater*             plater = wxGetApp().plater();
    const Model&        model = plater->model();
    PartPlateList&      plates = plater->get_partplate_list();
    const Scene::Detail detail = Scene::detail_from(ctx.arg("detail", "normal"));
    const int           only_plate = ctx.has("plate") ? Ids::plate(ctx.args()["plate"]) : -1;
    const size_t        offset = ctx.arg<size_t>("offset", 0);
    const size_t        limit = ctx.arg<size_t>("limit", 50);

    // Optional filter by object id; instance and volume ids select their object.
    std::set<int> wanted;
    const bool    filtered = ctx.has("objects");
    if (filtered)
        for (const ItemRef& ref : Ids::items(ctx.args()["objects"], "objects"))
            wanted.insert(ref.obj_idx);

    json out = json::object();
    out["project"] = Scene::project_json();
    out["printer"] = Scene::printer_json();
    out["process"] = Scene::process_json();
    out["filaments"] = Scene::filaments_json();

    // Entries are (plate, object) pairs in plate order, then the objects with instances on no plate.
    size_t index = 0;     // position in the full sequence
    size_t emitted = 0;
    size_t used_chars = 0;
    bool   truncated = false;
    auto   take = [&](int obj_idx, int plate_idx, json& list) {
        if (truncated)
            return;
        const size_t position = index++;
        if (position < offset)
            return;
        if (emitted >= limit) {
            truncated = true;
            out["next"] = position;
            return;
        }
        json         entry = Scene::object_json(obj_idx, detail, plate_idx);
        const size_t size = dump_json(entry).size();
        if (emitted > 0 && used_chars + size > SCENE_BUDGET_CHARS) {
            truncated = true;
            out["next"] = position;
            return;
        }
        used_chars += size;
        ++emitted;
        list.push_back(std::move(entry));
    };

    json plates_out = json::array();
    for (int p = 0; p < plates.get_plate_count(); ++p) {
        if (only_plate >= 0 && p != only_plate)
            continue;
        json plate = Scene::plate_json(p);
        json objects = json::array();
        for (int o = 0; o < static_cast<int>(model.objects.size()); ++o) {
            if (filtered && wanted.count(o) == 0)
                continue;
            bool on_plate = false;
            for (int i = 0; i < static_cast<int>(model.objects[static_cast<size_t>(o)]->instances.size()) && !on_plate; ++i)
                on_plate = Ids::plate_of_instance(o, i) == p;
            if (on_plate)
                take(o, p, objects);
        }
        plate["objects"] = std::move(objects);
        plates_out.push_back(std::move(plate));
    }
    out["plates"] = std::move(plates_out);

    if (only_plate < 0) {
        json off_plate = json::array();
        for (int o = 0; o < static_cast<int>(model.objects.size()); ++o) {
            if (filtered && wanted.count(o) == 0)
                continue;
            bool off = false;
            for (int i = 0; i < static_cast<int>(model.objects[static_cast<size_t>(o)]->instances.size()) && !off; ++i)
                off = Ids::plate_of_instance(o, i) < 0;
            if (off)
                take(o, -2, off_plate);
        }
        out["off_plate"] = std::move(off_plate);
    }

    out["object_count"] = model.objects.size();
    if (truncated) {
        out["truncated"] = true;
        ctx.note("More objects follow: call scene_get again with offset = next, or use detail \"summary\", a plate, or a list of object ids.");
    }
    return out;
}

json object_update(ToolContext& ctx)
{
    if (ctx.has("name"))
        check_single_line(ctx.arg("name", ""), "name");
    Plater*      plater = wxGetApp().plater();
    const bool   has_name = ctx.has("name");
    const bool   has_printable = ctx.has("printable");
    const bool   has_filament = ctx.has("filament");
    if (!has_name && !has_printable && !has_filament)
        fail("invalid_argument", "Nothing to change: give at least one of `name`, `printable`, `filament`.");

    // 1. Validate everything before touching anything.
    const std::vector<ItemRef> targets = Ids::items(ctx.args()["targets"], "targets");
    const std::string          name = ctx.arg("name", "");
    const bool                 printable = ctx.arg<bool>("printable", true);
    const int                  filament = ctx.arg<int>("filament", 0);
    if (has_name && name.empty())
        fail("invalid_argument", "`name` must not be empty.");
    if (has_name && targets.size() > 1)
        fail("invalid_argument", "`name` can be set on one item at a time (got " + std::to_string(targets.size()) + " targets).");
    if (has_filament && filament > Ids::filament_count())
        fail("invalid_argument", "`filament`: there is no filament slot " + std::to_string(filament) + ".",
             "The project has " + std::to_string(Ids::filament_count()) + " filament slot(s). filament_list shows them.");
    for (size_t i = 0; i < targets.size(); ++i) {
        const ItemRef&    ref = targets[i];
        const std::string label = "targets[" + std::to_string(i) + "]";
        if (ref.kind == IdKind::Instance && (has_name || has_filament))
            fail("invalid_argument", "`" + label + "` is an instance; instances have no name or filament of their own.",
                 "Use the object id (o...) for name and filament; instances only take `printable`.");
        if (ref.kind == IdKind::Volume && has_printable)
            fail("invalid_argument", "`" + label + "` is a volume; `printable` applies to objects and instances.");
        if (ref.kind == IdKind::Object && has_filament && filament < 1)
            fail("invalid_argument", "`filament` must be 1 or more for an object.", "0 (inherit from the object) is only meaningful for a part or modifier.");
    }

    // 2. One undo step for the whole call, taken right before the first change.
    std::string summary = "update";
    for (size_t i = 0; i < targets.size() && i < 3; ++i) {
        const ItemRef& ref = targets[i];
        summary += " " + (ref.kind == IdKind::Object ? Ids::of(*ref.object) : ref.kind == IdKind::Instance ? Ids::of(*ref.instance) : Ids::of(*ref.volume));
    }
    if (targets.size() > 3)
        summary += " ...";
    ctx.snapshot(summary);

    // 3. Change the Model.
    std::set<int> changed;
    for (const ItemRef& ref : targets) {
        changed.insert(ref.obj_idx);
        if (ref.kind == IdKind::Object) {
            if (has_name) {
                ref.object->name = name;
                // The GUI renames the only volume along with its object.
                if (ref.object->volumes.size() == 1)
                    ref.object->volumes.front()->name = name;
            }
            if (has_printable)
                for (ModelInstance* instance : ref.object->instances)
                    instance->printable = printable;
            if (has_filament)
                ref.object->config.set_key_value("extruder", new ConfigOptionInt(filament));
        } else if (ref.kind == IdKind::Instance) {
            ref.instance->printable = printable;
        } else {
            if (has_name)
                ref.volume->name = name;
            if (has_filament) {
                if (filament == 0)
                    ref.volume->config.erase("extruder");
                else
                    ref.volume->config.set_key_value("extruder", new ConfigOptionInt(filament));
            }
        }
    }

    // 4. Bring the window, object list, plates and background slicer in sync.
    Refresh::full();
    ctx.set_slice_invalidated();

    // 5. Return what changed, in scene_get shape.
    json items = json::array();
    for (int obj_idx : changed)
        items.push_back(Scene::object_json(obj_idx, Scene::Detail::Normal));
    json out = json::object();
    out["ok"] = true;
    out["changed"] = std::move(items);
    (void) plater;
    return out;
}

} // namespace

MCP_TOOL_GROUP(Scene)
{
    {
        ToolDef tool;
        tool.name = "scene_get";
        tool.title = "Scene";
        tool.description =
            "Scene tree of the open project: plates with their objects, instances (position, rotation, scale, bounding box, whether outside the "
            "plate), parts and modifiers (type, filament, size, per-part setting overrides), height ranges, paint summary, plus the project, "
            "printer, process preset and filaments. Positions are in mm in the frame of the object's plate. detail \"summary\" gives counts "
            "only, \"normal\" the full tree, \"full\" adds matrices, per-volume boxes and mesh statistics. Restrict with `plate` or `objects`; "
            "large scenes are paged (`truncated`, `next` -> pass as `offset`). The ids returned here are what every other tool takes.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "detail": {"enum": ["summary", "normal", "full"], "default": "normal"},
                "plate": {"$ref": "#/$defs/PlateRef"},
                "objects": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "description": "Only these objects (instance and volume ids select their object)."},
                "offset": {"type": "integer", "minimum": 0, "default": 0},
                "limit": {"type": "integer", "minimum": 1, "maximum": 500, "default": 50, "description": "Maximum number of objects returned."}
            },
            "additionalProperties": false})JSON", "scene_get");
        tool.read_only = true;
        tool.idempotent = true;
        tool.handler = scene_get;
        registry.add(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "object_update";
        tool.title = "Rename, set printable, assign filament";
        tool.description =
            "Rename an object or part, mark objects or instances printable / not printable, or assign a filament slot to objects, parts or "
            "modifiers. `filament` is a 1-based slot from filament_list; 0 on a part or modifier means \"use the object's filament\". "
            "One undo step. Returns the changed objects in scene_get shape. Transforms are object_transform; settings are settings_set.";
        tool.input_schema = Schema::parse(R"JSON({
            "type": "object",
            "properties": {
                "targets": {"type": "array", "items": {"$ref": "#/$defs/Id"}, "minItems": 1,
                            "description": "Object ids (o...), instance ids (i..., printable only) or volume ids (v..., name and filament only)."},
                "name": {"type": "string", "minLength": 1, "maxLength": 200, "description": "New name; one target only."},
                "printable": {"type": "boolean"},
                "filament": {"type": "integer", "minimum": 0, "description": "1-based filament slot; 0 on a part or modifier = inherit from the object."}
            },
            "required": ["targets"],
            "additionalProperties": false})JSON", "object_update");
        tool.idempotent = true;
        tool.handler = object_update;
        registry.add(std::move(tool));
    }
}

}}} // namespace Slic3r::GUI::Mcp
