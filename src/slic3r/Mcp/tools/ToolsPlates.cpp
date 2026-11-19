// Plate tools: plate_manage (add, delete, duplicate, rename, lock, unlock, select, move).
//
// Per-plate settings (bed type, print sequence, spiral mode ...) are settings_set with scope
// {"plate": n} (settings group); moving objects between plates is object_transform move_to.plate.

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../McpIds.hpp"
#include "../McpRefresh.hpp"
#include "../McpScene.hpp"
#include "../McpSchema.hpp"
#include "../McpTool.hpp"
#include "../McpUtil.hpp"
#include "edit/EditCommon.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

using namespace Edit;

json plates_json()
{
    json out = json::array();
    PartPlateList& list = plates();
    for (int p = 0; p < list.get_plate_count(); ++p) {
        json plate = Scene::plate_json(p);
        int  count = 0;
        const Model& m = model();
        for (int o = 0; o < static_cast<int>(m.objects.size()); ++o)
            for (int i = 0; i < static_cast<int>(m.objects[static_cast<size_t>(o)]->instances.size()); ++i)
                if (Ids::plate_of_instance(o, i) == p)
                    ++count;
        plate["instances"] = count;
        out.push_back(std::move(plate));
    }
    return out;
}

json plate_manage(ToolContext& ctx)
{
    if (ctx.has("name"))
        check_single_line(ctx.arg("name", ""), "name");
    Plater&           p = plater();
    PartPlateList&    list = plates();
    const std::string action = ctx.arg("action", "");
    const bool        has_plate = ctx.has("plate");
    const int         plate_idx = Ids::plate(has_plate ? ctx.args()["plate"] : json(), "plate");
    const int         previous = list.get_curr_plate_index();
    json              out = json::object();
    out["ok"] = true;

    auto restore_current = [&](int index) {
        if (index >= 0 && index < list.get_plate_count() && list.get_curr_plate_index() != index)
            p.select_plate(index);
    };

    if (action == "add") {
        const int count = ctx.arg<int>("count", 1);
        if (!p.can_add_plate())
            fail("limit", "No more plates can be added (" + std::to_string(list.get_plate_count()) + " is the maximum).", "Delete a plate first.");
        ctx.snapshot("add plate");
        json added = json::array();
        for (int k = 0; k < count; ++k) {
            if (!p.can_add_plate()) {
                ctx.note("Stopped after " + std::to_string(k) + " plate(s): the maximum number of plates was reached.");
                break;
            }
            const int index = p.add_plate();
            if (index < 0)
                break;
            added.push_back(index + 1);
        }
        restore_current(previous);
        out["added"] = added;
    } else if (action == "delete") {
        if (list.get_plate_count() <= 1)
            fail("last_plate", "The project has only one plate; it cannot be deleted.", "Delete its objects instead (delete).");
        // Objects on the plate are deleted with it, as the GUI's plate delete button does.
        std::vector<int> objects_on;
        const Model& m = model();
        for (int o = 0; o < static_cast<int>(m.objects.size()); ++o)
            for (int i = 0; i < static_cast<int>(m.objects[static_cast<size_t>(o)]->instances.size()); ++i)
                if (Ids::plate_of_instance(o, i) == plate_idx)
                    objects_on.push_back(o);
        const std::string keep = ctx.arg("objects", "delete");
        if (!objects_on.empty() && keep == "fail")
            fail("invalid_argument", "Plate " + std::to_string(plate_idx + 1) + " has objects.", "Pass objects: \"delete\" to delete them with the plate.");
        ctx.snapshot("delete plate " + std::to_string(plate_idx + 1));
        // Remove the plate's instances first (whole objects when all their instances are there).
        json removed = json::array();
        Model& mm = model();
        for (int o = static_cast<int>(mm.objects.size()) - 1; o >= 0; --o) {
            ModelObject* object = mm.objects[static_cast<size_t>(o)];
            std::vector<int> on;
            for (int i = 0; i < static_cast<int>(object->instances.size()); ++i)
                if (Ids::plate_of_instance(o, i) == plate_idx)
                    on.push_back(i);
            if (on.empty())
                continue;
            if (on.size() == object->instances.size()) {
                removed.push_back(Ids::of(*object));
                mm.delete_object(static_cast<size_t>(o));
            } else {
                for (auto it = on.rbegin(); it != on.rend(); ++it) {
                    removed.push_back(Ids::of(*object->instances[static_cast<size_t>(*it)]));
                    object->delete_instance(static_cast<size_t>(*it));
                }
                object->invalidate_bounding_box();
            }
        }
        if (!removed.empty())
            refresh_structure();
        if (p.delete_plate(plate_idx) < 0)
            fail("internal", "The application refused to delete plate " + std::to_string(plate_idx + 1) + ".");
        refresh_structure();
        out["removed_objects"] = removed;
        out["deleted"] = plate_idx + 1;
    } else if (action == "duplicate") {
        if (!p.can_add_plate())
            fail("limit", "No more plates can be added.", "Delete a plate first.");
        ctx.snapshot("duplicate plate " + std::to_string(plate_idx + 1));
        const std::set<size_t> before = object_id_set();
        const int index = p.duplicate_plate(plate_idx);
        if (index < 0)
            fail("internal", "The application could not duplicate plate " + std::to_string(plate_idx + 1) + ".");
        refresh_structure();
        restore_current(previous);
        out["created"] = list.get_plate_count();
        out["created_objects"] = objects_json(new_object_indices(before));
    } else if (action == "rename") {
        if (!ctx.has("name"))
            fail("invalid_argument", "`name` is required for action \"rename\".", "Pass name (an empty string clears it).");
        ctx.snapshot("rename plate " + std::to_string(plate_idx + 1));
        p.rename_plate(plate_idx, ctx.arg("name", ""));
    } else if (action == "lock" || action == "unlock") {
        ctx.snapshot(action + " plate " + std::to_string(plate_idx + 1));
        list.lock_plate(plate_idx, action == "lock");
        p.update();
    } else if (action == "select") {
        if (!has_plate)
            fail("invalid_argument", "`plate` is required for action \"select\".");
        ctx.snapshot("select plate " + std::to_string(plate_idx + 1));
        p.select_plate(plate_idx);
        ctx.note("The window now shows plate " + std::to_string(plate_idx + 1) + ".");
    } else if (action == "move") {
        if (!ctx.has("to"))
            fail("invalid_argument", "`to` (1-based position) is required for action \"move\".");
        const int to = ctx.arg<int>("to", 1) - 1;
        if (to < 0 || to >= list.get_plate_count())
            fail("invalid_argument", "`to` must be between 1 and " + std::to_string(list.get_plate_count()) + ".");
        if (to == plate_idx)
            fail("invalid_argument", "Plate " + std::to_string(plate_idx + 1) + " is already at position " + std::to_string(to + 1) + ".");
        ctx.snapshot("move plate " + std::to_string(plate_idx + 1) + " to " + std::to_string(to + 1));
        if (list.move_plate_to_index(plate_idx, to) < 0)
            fail("internal", "The application refused to move the plate.");
        // As the GUI's "move plate to the front": plates, slice context, object list, then the scene.
        list.update_plates();
        list.update_slice_context_to_current_plate(p.background_process());
        refresh_structure();
        // move_plate_to_index() makes the moved plate current; keep the user's current plate.
        const int now_current = previous == plate_idx ? to : (previous >= std::min(plate_idx, to) && previous <= std::max(plate_idx, to) ? previous + (plate_idx < to ? -1 : 1) : previous);
        {
            Plater::SuppressSnapshots suppress(&p);
            p.select_plate(now_current);
        }
    } else
        fail("invalid_argument", "Unknown `action` \"" + action + "\".", "Use add, delete, duplicate, rename, lock, unlock, select or move.");

    ctx.set_slice_invalidated();
    out["plates"] = plates_json();
    return out;
}

} // namespace

MCP_TOOL_GROUP(Plates)
{
    ToolDef tool;
    tool.name = "plate_manage";
    tool.title = "Add, delete, duplicate, rename, lock, select or reorder plates";
    tool.description =
        "Manage plates. action \"add\" (`count`, default 1), \"delete\" (`plate`; its objects are deleted with it unless objects: "
        "\"fail\"; last_plate for the only plate), \"duplicate\" (copies the plate with its objects to a new last plate), \"rename\" "
        "(`name`), \"lock\" / \"unlock\" (arrange and orient leave locked plates alone; nothing can be imported or moved onto them), "
        "\"select\" (makes it the plate the window shows and the default for tools that take `plate`), \"move\" (`to`: new 1-based "
        "position; plate numbers shift, pl ids stay). `plate` is a number, a pl<N> id or \"current\" (default). Per-plate settings "
        "(bed type, print sequence, spiral mode ...) are settings_set with scope {plate: n}; moving objects between plates is "
        "object_transform move_to.plate. One undo step. Returns the plates with their instance counts.";
    tool.input_schema = Schema::parse(R"JSON({
        "type": "object",
        "properties": {
            "action": {"enum": ["add", "delete", "duplicate", "rename", "lock", "unlock", "select", "move"]},
            "plate": {"$ref": "#/$defs/PlateRef"},
            "name": {"type": "string", "maxLength": 200},
            "to": {"type": "integer", "minimum": 1},
            "count": {"type": "integer", "minimum": 1, "maximum": 36, "default": 1},
            "objects": {"enum": ["delete", "fail"], "default": "delete"}
        },
        "required": ["action"],
        "additionalProperties": false})JSON", "plate_manage");
    tool.destructive = true;
    tool.handler = plate_manage;
    registry.add(std::move(tool));
}

}}} // namespace Slic3r::GUI::Mcp
