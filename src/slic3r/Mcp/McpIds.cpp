#include "McpIds.hpp"

#include <cstdlib>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "McpError.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace Ids {

namespace {

const char* kind_name(IdKind kind)
{
    switch (kind) {
    case IdKind::Object: return "an object";
    case IdKind::Instance: return "an instance";
    case IdKind::Volume: return "a volume (part or modifier)";
    case IdKind::Plate: return "a plate";
    }
    return "an item";
}

struct Parsed
{
    IdKind kind;
    size_t value;
};

Parsed parse(const std::string& id, const char* what)
{
    Parsed      out { IdKind::Object, 0 };
    size_t      prefix = 1;
    if (id.rfind("pl", 0) == 0) {
        out.kind = IdKind::Plate;
        prefix = 2;
    } else if (!id.empty() && id[0] == 'o')
        out.kind = IdKind::Object;
    else if (!id.empty() && id[0] == 'i')
        out.kind = IdKind::Instance;
    else if (!id.empty() && id[0] == 'v')
        out.kind = IdKind::Volume;
    else
        prefix = 0;
    bool digits = prefix > 0 && id.size() > prefix && id.size() < prefix + 19;
    for (size_t i = prefix; digits && i < id.size(); ++i)
        digits = id[i] >= '0' && id[i] <= '9';
    if (!digits)
        fail("invalid_argument", std::string("`") + what + "`: \"" + id + "\" is not an id.",
             "Ids look like o57 (object), i58 (instance), v59 (volume) or pl41 (plate); take them from scene_get.");
    out.value = static_cast<size_t>(std::strtoull(id.c_str() + prefix, nullptr, 10));
    return out;
}

[[noreturn]] void stale(const std::string& id, const char* what)
{
    fail("stale_id", std::string("`") + what + "`: " + id + " does not exist (any more).",
         "Ids change when a project is loaded and disappear when the item is deleted. Call scene_get for the current ids.",
         json::object({{"id", id}}));
}

[[noreturn]] void wrong_kind(const std::string& id, IdKind actual, const char* what, const char* expected)
{
    fail("invalid_argument", std::string("`") + what + "`: " + id + " is " + kind_name(actual) + "; this argument needs " + expected + ".",
         "o = object, i = instance, v = volume (part or modifier), pl = plate.");
}

Model& model() { return wxGetApp().plater()->model(); }

// Finds the item with ObjectBase id `value` among objects, instances and volumes.
bool find(size_t value, ItemRef& out)
{
    Model& m = model();
    for (size_t oi = 0; oi < m.objects.size(); ++oi) {
        ModelObject* object = m.objects[oi];
        if (object->id().id == value) {
            out.kind = IdKind::Object;
            out.object = object;
            out.obj_idx = static_cast<int>(oi);
            return true;
        }
        for (size_t ii = 0; ii < object->instances.size(); ++ii)
            if (object->instances[ii]->id().id == value) {
                out.kind = IdKind::Instance;
                out.object = object;
                out.obj_idx = static_cast<int>(oi);
                out.instance = object->instances[ii];
                out.inst_idx = static_cast<int>(ii);
                return true;
            }
        for (size_t vi = 0; vi < object->volumes.size(); ++vi)
            if (object->volumes[vi]->id().id == value) {
                out.kind = IdKind::Volume;
                out.object = object;
                out.obj_idx = static_cast<int>(oi);
                out.volume = object->volumes[vi];
                out.vol_idx = static_cast<int>(vi);
                return true;
            }
    }
    return false;
}

int find_plate(size_t value)
{
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        if (plates.get_plate(i)->id().id == value)
            return i;
    return -1;
}

} // namespace

std::string of(const ModelObject& object) { return "o" + std::to_string(object.id().id); }
std::string of(const ModelInstance& instance) { return "i" + std::to_string(instance.id().id); }
std::string of(const ModelVolume& volume) { return "v" + std::to_string(volume.id().id); }
std::string of(PartPlate& plate) { return "pl" + std::to_string(plate.id().id); }

ItemRef item(const std::string& id, const char* what, bool allow_plate)
{
    const Parsed parsed = parse(id, what);
    ItemRef      out;
    if (parsed.kind == IdKind::Plate) {
        if (!allow_plate)
            wrong_kind(id, IdKind::Plate, what, "an object, instance or volume");
        out.kind = IdKind::Plate;
        out.plate_idx = find_plate(parsed.value);
        if (out.plate_idx < 0)
            stale(id, what);
        return out;
    }
    if (!find(parsed.value, out))
        stale(id, what);
    // The prefix is redundant with the number; a mismatch means the caller mixed something up.
    if (out.kind != parsed.kind)
        fail("invalid_argument", std::string("`") + what + "`: " + id + " has the wrong prefix; the item with that number is " + kind_name(out.kind) + ".",
             "Use the ids exactly as scene_get returns them.");
    return out;
}

std::vector<ItemRef> items(const json& ids, const char* what, bool allow_plate)
{
    std::vector<ItemRef> out;
    if (!ids.is_array())
        fail("invalid_argument", std::string("`") + what + "` must be an array of ids.");
    for (size_t i = 0; i < ids.size(); ++i) {
        const std::string label = std::string(what) + "[" + std::to_string(i) + "]";
        if (!ids[i].is_string())
            fail("invalid_argument", "`" + label + "` must be an id string.");
        out.push_back(item(ids[i].get<std::string>(), label.c_str(), allow_plate));
    }
    return out;
}

ObjectRef object(const std::string& id, const char* what)
{
    const ItemRef ref = item(id, what, true);
    if (ref.kind != IdKind::Object)
        wrong_kind(id, ref.kind, what, "an object (o...)");
    return ObjectRef { ref.object, ref.obj_idx };
}

InstanceRef instance(const std::string& id, const char* what)
{
    const ItemRef ref = item(id, what, true);
    if (ref.kind != IdKind::Instance)
        wrong_kind(id, ref.kind, what, "an instance (i...)");
    return InstanceRef { ref.object, ref.obj_idx, ref.instance, ref.inst_idx };
}

VolumeRef volume(const std::string& id, const char* what)
{
    const ItemRef ref = item(id, what, true);
    if (ref.kind != IdKind::Volume)
        wrong_kind(id, ref.kind, what, "a volume (v...)");
    return VolumeRef { ref.object, ref.obj_idx, ref.volume, ref.vol_idx };
}

int plate(const json& ref, const char* what)
{
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    if (ref.is_null())
        return plates.get_curr_plate_index();
    if (ref.is_number()) {
        const long long number = ref.get<long long>();
        if (number < 1 || number > plates.get_plate_count())
            fail("stale_id", std::string("`") + what + "`: there is no plate " + std::to_string(number) + ".",
                 "Plates are numbered 1 to " + std::to_string(plates.get_plate_count()) + ".", json::object({{"plates", plates.get_plate_count()}}));
        return static_cast<int>(number - 1);
    }
    if (ref.is_string()) {
        const std::string text = ref.get<std::string>();
        if (text == "current")
            return plates.get_curr_plate_index();
        const ItemRef found = item(text, what, true);
        if (found.kind != IdKind::Plate)
            wrong_kind(text, found.kind, what, "a plate (number, pl..., or \"current\")");
        return found.plate_idx;
    }
    fail("invalid_argument", std::string("`") + what + "` must be a plate number, a pl<N> id or \"current\".");
}

int plate_of_instance(int obj_idx, int inst_idx)
{
    return wxGetApp().plater()->get_partplate_list().find_instance(obj_idx, inst_idx);
}

int filament_count()
{
    return static_cast<int>(wxGetApp().preset_bundle->filament_presets.size());
}

int filament_slot(const json& slot, const char* what)
{
    if (!slot.is_number_integer())
        fail("invalid_argument", std::string("`") + what + "` must be a filament slot number (1-based).");
    const long long number = slot.get<long long>();
    const int       count = filament_count();
    if (number < 1 || number > count)
        fail("invalid_argument", std::string("`") + what + "`: there is no filament slot " + std::to_string(number) + ".",
             "The project has " + std::to_string(count) + " filament slot(s), numbered from 1. filament_list shows them.",
             json::object({{"filaments", count}}));
    return static_cast<int>(number - 1);
}

} // namespace Ids

}}} // namespace Slic3r::GUI::Mcp
