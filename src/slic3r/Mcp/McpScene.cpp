#include "McpScene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <wx/string.h>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "McpIds.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

const double RAD_TO_DEG = 180. / 3.14159265358979323846;

std::string utf8(const wxString& text) { return std::string(text.utf8_str().data()); }

json scalar_from_text(ConfigOptionType type, const std::string& text)
{
    switch (type) {
    case coFloat: return std::strtod(text.c_str(), nullptr);
    case coInt: return std::atoi(text.c_str());
    case coBool: return text == "1" || text == "true";
    case coFloatOrPercent:
        // "15%" stays a string; an absolute value becomes a number.
        if (!text.empty() && text.back() == '%')
            return text;
        return std::strtod(text.c_str(), nullptr);
    default: return text; // strings, percents ("15%"), enums, points
    }
}

json bbox_json(const BoundingBoxf3& box)
{
    json out = json::object();
    out["min"] = Scene::vec3(box.min.x(), box.min.y(), box.min.z());
    out["max"] = Scene::vec3(box.max.x(), box.max.y(), box.max.z());
    return out;
}

json matrix_json(const Transform3d& transform)
{
    json rows = json::array();
    for (int r = 0; r < 4; ++r) {
        json row = json::array();
        for (int c = 0; c < 4; ++c)
            row.push_back(Scene::round4(transform.matrix()(r, c)));
        rows.push_back(std::move(row));
    }
    return rows;
}

Plater& plater() { return *wxGetApp().plater(); }

Vec3d plate_origin(int plate_idx)
{
    PartPlateList& plates = plater().get_partplate_list();
    if (plate_idx < 0 || plate_idx >= plates.get_plate_count())
        return Vec3d::Zero();
    return plates.get_plate(plate_idx)->get_origin();
}

int object_filament(const ModelObject& object)
{
    return object.config.has("extruder") ? object.config.extruder() : 1;
}

json paint_json(const ModelObject& object)
{
    bool support = false, seam = false, color = false, fuzzy = false;
    for (const ModelVolume* volume : object.volumes) {
        support = support || !volume->supported_facets.empty();
        seam = seam || !volume->seam_facets.empty();
        color = color || !volume->mmu_segmentation_facets.empty();
        fuzzy = fuzzy || !volume->fuzzy_skin_facets.empty();
    }
    json out = json::object();
    out["support"] = support;
    out["seam"] = seam;
    out["color"] = color;
    out["fuzzy_skin"] = fuzzy;
    return out;
}

} // namespace

static json point_json(const Vec2d& point)
{
    return json::array({Scene::round4(point.x()), Scene::round4(point.y())});
}

json config_value_json(const ConfigOption& option)
{
    // Strings and points are taken from the option itself: their serialised form escapes line
    // breaks and writes points as "XxY", which is for files, not for readers.
    switch (option.type()) {
    case coString: return static_cast<const ConfigOptionString&>(option).value;
    case coStrings: return static_cast<const ConfigOptionStrings&>(option).values;
    case coPoint: return point_json(static_cast<const ConfigOptionPoint&>(option).value);
    case coPoints: {
        json out = json::array();
        for (const Vec2d& point : static_cast<const ConfigOptionPoints&>(option).values)
            out.push_back(point_json(point));
        return out;
    }
    default: break;
    }
    if (option.is_vector()) {
        const auto&    vector_option = static_cast<const ConfigOptionVectorBase&>(option);
        const auto     scalar_type = static_cast<ConfigOptionType>(static_cast<int>(option.type()) & ~static_cast<int>(coVectorType));
        if (option.type() == coPointsGroups || option.type() == coIntsGroups)
            return option.serialize();
        json out = json::array();
        const std::vector<std::string> values = vector_option.vserialize();
        for (size_t i = 0; i < values.size(); ++i) {
            if (vector_option.is_nil(i))
                out.push_back(nullptr);
            else
                out.push_back(scalar_from_text(scalar_type, values[i]));
        }
        return out;
    }
    return scalar_from_text(option.type(), option.serialize());
}

json config_overrides_json(const ConfigBase& config, const char* skip_key)
{
    json out = json::object();
    for (const std::string& key : config.keys()) {
        if (skip_key != nullptr && key == skip_key)
            continue;
        if (const ConfigOption* option = config.option(key); option != nullptr)
            out[key] = config_value_json(*option);
    }
    return out;
}

namespace Scene {

double round4(double value)
{
    const double rounded = std::round(value * 10000.) / 10000.;
    return rounded == 0. ? 0. : rounded; // no "-0.0"
}

json vec3(double x, double y, double z)
{
    return json::array({round4(x), round4(y), round4(z)});
}

Detail detail_from(const std::string& name)
{
    if (name == "summary") return Detail::Summary;
    if (name == "full") return Detail::Full;
    return Detail::Normal;
}

size_t rev()
{
    return plater().undo_redo_stack_main().active_snapshot_time();
}

std::string volume_type_name(const ModelVolume& volume)
{
    switch (volume.type()) {
    case ModelVolumeType::MODEL_PART: return "part";
    case ModelVolumeType::NEGATIVE_VOLUME: return "negative_part";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER: return "support_blocker";
    case ModelVolumeType::SUPPORT_ENFORCER: return "support_enforcer";
    case ModelVolumeType::PRECISE_SEAM_CENTER: return "precise_seam_center";
    case ModelVolumeType::PRECISE_SEAM_LEFT: return "precise_seam_left";
    case ModelVolumeType::PRECISE_SEAM_RIGHT: return "precise_seam_right";
    case ModelVolumeType::PRECISE_SEAM_ENFORCED: return "precise_seam_enforced";
    case ModelVolumeType::PRECISE_SEAM_BLOCKED: return "precise_seam_blocked";
    case ModelVolumeType::PRECISE_SEAM_NEUTRAL: return "precise_seam_neutral";
    case ModelVolumeType::INVALID: break;
    }
    return "invalid";
}

json volume_json(const ModelObject& object, int vol_idx, Detail detail)
{
    const ModelVolume& volume = *object.volumes[static_cast<size_t>(vol_idx)];
    json out = json::object();
    out["id"] = Ids::of(volume);
    out["type"] = volume_type_name(volume);
    out["name"] = volume.name;
    // 0 = inherit the object's filament.
    out["filament"] = volume.config.has("extruder") ? volume.config.extruder() : 0;
    out["facets"] = volume.mesh().facets_count();

    // Placement in the object's frame; for bodies other than parts also relative to the object's
    // box, which is how they are usually described ("top centre").
    const BoundingBoxf3 box = volume.mesh().transformed_bounding_box(volume.get_matrix());
    const Vec3d         center = box.center();
    const Vec3d         size = box.size();
    out["center"] = vec3(center.x(), center.y(), center.z());
    out["size"] = vec3(size.x(), size.y(), size.z());
    if (!volume.is_model_part()) {
        const BoundingBoxf3& object_box = object.raw_mesh_bounding_box();
        const Vec3d          object_size = object_box.size();
        auto rel = [](double value, double low, double extent) { return extent > 1e-9 ? (value - low) / extent : 0.5; };
        out["center_rel"] = vec3(rel(center.x(), object_box.min.x(), object_size.x()), rel(center.y(), object_box.min.y(), object_size.y()),
                                 rel(center.z(), object_box.min.z(), object_size.z()));
    }
    out["overrides"] = config_overrides_json(volume.config.get(), "extruder");

    if (detail == Detail::Full) {
        const Vec3d rotation = volume.get_rotation();
        const Vec3d scale = volume.get_scaling_factor();
        const Vec3d offset = volume.get_offset();
        out["offset"] = vec3(offset.x(), offset.y(), offset.z());
        out["rotation"] = vec3(rotation.x() * RAD_TO_DEG, rotation.y() * RAD_TO_DEG, rotation.z() * RAD_TO_DEG);
        out["scale"] = vec3(scale.x(), scale.y(), scale.z());
        out["matrix"] = matrix_json(volume.get_matrix());
        out["bbox"] = bbox_json(box);
        const TriangleMeshStats& stats = volume.mesh().stats();
        json mesh = json::object();
        mesh["facets"] = stats.number_of_facets;
        mesh["volume_mm3"] = round4(stats.volume);
        mesh["open_edges"] = stats.open_edges;
        mesh["parts"] = stats.number_of_parts;
        out["mesh"] = std::move(mesh);
        json paint = json::object();
        paint["support"] = !volume.supported_facets.empty();
        paint["seam"] = !volume.seam_facets.empty();
        paint["color"] = !volume.mmu_segmentation_facets.empty();
        paint["fuzzy_skin"] = !volume.fuzzy_skin_facets.empty();
        out["paint"] = std::move(paint);
    }
    return out;
}

json instance_json(int obj_idx, int inst_idx, Detail detail)
{
    const Model&         model = plater().model();
    const ModelObject&   object = *model.objects[static_cast<size_t>(obj_idx)];
    const ModelInstance& instance = *object.instances[static_cast<size_t>(inst_idx)];
    const int            plate_idx = Ids::plate_of_instance(obj_idx, inst_idx);
    const Vec3d          origin = plate_origin(plate_idx);

    BoundingBoxf3 box = object.instance_bounding_box(static_cast<size_t>(inst_idx));
    box.translate(-origin);
    const Vec3d center = box.center();
    const Vec3d rotation = instance.get_rotation();
    const Vec3d scale = instance.get_scaling_factor();
    const Vec3d mirror = instance.get_mirror();

    json out = json::object();
    out["id"] = Ids::of(instance);
    if (plate_idx >= 0)
        out["plate"] = plate_idx + 1;
    else
        out["plate"] = nullptr;
    out["center"] = json::array({round4(center.x()), round4(center.y())});
    out["z_min"] = round4(box.min.z());
    out["rotation"] = vec3(rotation.x() * RAD_TO_DEG, rotation.y() * RAD_TO_DEG, rotation.z() * RAD_TO_DEG);
    out["scale"] = vec3(scale.x(), scale.y(), scale.z());
    if (mirror.x() < 0 || mirror.y() < 0 || mirror.z() < 0)
        out["mirror"] = vec3(mirror.x(), mirror.y(), mirror.z());
    out["bbox"] = bbox_json(box);
    if (!instance.printable)
        out["printable"] = false;
    out["outside_plate"] = instance.print_volume_state != ModelInstancePVS_Inside;
    if (detail == Detail::Full) {
        const Vec3d offset = instance.get_offset();
        out["offset_world"] = vec3(offset.x(), offset.y(), offset.z());
        out["matrix_world"] = matrix_json(instance.get_matrix());
    }
    return out;
}

json object_json(int obj_idx, Detail detail, int plate_idx)
{
    const Model&       model = plater().model();
    const ModelObject& object = *model.objects[static_cast<size_t>(obj_idx)];

    json out = json::object();
    out["id"] = Ids::of(object);
    out["name"] = object.name;
    out["printable"] = object.printable;
    out["filament"] = object_filament(object);
    if (!object.instances.empty()) {
        const Vec3d size = object.instance_bounding_box(0).size();
        out["size"] = vec3(size.x(), size.y(), size.z());
    }

    std::vector<int> instances;
    for (int i = 0; i < static_cast<int>(object.instances.size()); ++i) {
        const int on_plate = Ids::plate_of_instance(obj_idx, i);
        if (plate_idx == -1 || (plate_idx == -2 && on_plate < 0) || on_plate == plate_idx)
            instances.push_back(i);
    }

    if (detail == Detail::Summary) {
        out["instances"] = instances.size();
        int parts = 0, others = 0;
        for (const ModelVolume* volume : object.volumes)
            (volume->is_model_part() ? parts : others)++;
        out["parts"] = parts;
        out["modifiers"] = others;
        out["overrides"] = object.config.get().keys().size() - (object.config.has("extruder") ? 1 : 0);
        out["layer_ranges"] = object.layer_config_ranges.size();
        return out;
    }

    out["overrides"] = config_overrides_json(object.config.get(), "extruder");
    out["paint"] = paint_json(object);

    json instances_out = json::array();
    for (int i : instances)
        instances_out.push_back(instance_json(obj_idx, i, detail));
    out["instances"] = std::move(instances_out);

    json volumes = json::array();
    for (int v = 0; v < static_cast<int>(object.volumes.size()); ++v)
        volumes.push_back(volume_json(object, v, detail));
    out["volumes"] = std::move(volumes);

    json ranges = json::array();
    for (const auto& [range, config] : object.layer_config_ranges) {
        json item = json::object();
        item["z_min"] = round4(range.first);
        item["z_max"] = round4(range.second);
        item["overrides"] = config_overrides_json(config.get());
        ranges.push_back(std::move(item));
    }
    out["layer_ranges"] = std::move(ranges);
    return out;
}

json plate_json(int plate_idx)
{
    PartPlateList& plates = plater().get_partplate_list();
    PartPlate*     plate = plates.get_plate(plate_idx);
    json out = json::object();
    out["plate"] = plate_idx + 1;
    out["id"] = Ids::of(*plate);
    out["name"] = plate->get_plate_name();
    out["current"] = plates.get_curr_plate_index() == plate_idx;
    out["locked"] = plate->is_locked();
    out["sliced"] = plate->is_slice_result_valid();
    out["overrides"] = plate->config() != nullptr ? config_overrides_json(*plate->config()) : json(json::object());
    return out;
}

json project_json()
{
    json out = json::object();
    out["name"] = utf8(plater().get_project_name());
    const std::string path = utf8(plater().get_project_filename(".3mf"));
    if (path.empty())
        out["path"] = nullptr;
    else
        out["path"] = path;
    out["dirty"] = plater().is_project_dirty();
    out["rev"] = rev();
    return out;
}

json printer_json()
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    const Preset& preset = bundle.printers.get_edited_preset();
    json out = json::object();
    out["preset"] = preset.name;
    PartPlateList& plates = plater().get_partplate_list();
    if (plates.get_plate_count() > 0) {
        const BoundingBoxf3 volume = plates.get_plate(0)->get_build_volume();
        const Vec3d         size = volume.size();
        // The build volume is padded by a scene epsilon; report the nominal size.
        auto nominal = [](double v) { return std::round(v * 100.) / 100.; };
        out["bed"] = json::array({nominal(size.x()), nominal(size.y())});
        out["height"] = nominal(size.z());
    }
    if (const auto* nozzles = preset.config.option<ConfigOptionFloats>("nozzle_diameter"); nozzles != nullptr) {
        out["extruders"] = nozzles->values.size();
        json diameters = json::array();
        for (double d : nozzles->values)
            diameters.push_back(round4(d));
        out["nozzle"] = std::move(diameters);
    }
    return out;
}

json process_json()
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    json out = json::object();
    out["preset"] = bundle.prints.get_edited_preset().name;
    out["modified"] = bundle.prints.current_dirty_options();
    return out;
}

json filaments_json()
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    const auto*   colours = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    json out = json::array();
    for (size_t i = 0; i < bundle.filament_presets.size(); ++i) {
        json item = json::object();
        item["slot"] = i + 1;
        item["preset"] = bundle.filament_presets[i];
        if (const Preset* preset = bundle.filaments.find_preset(bundle.filament_presets[i]); preset != nullptr)
            if (const auto* type = preset->config.option<ConfigOptionStrings>("filament_type"); type != nullptr && !type->values.empty())
                item["type"] = type->values.front();
        if (colours != nullptr && i < colours->values.size())
            item["color"] = colours->values[i];
        out.push_back(std::move(item));
    }
    return out;
}

json filament_used_by(int slot)
{
    json objects = json::array(), parts = json::array(), ranges = json::array(), paint = json::array();
    for (const ModelObject* object : plater().model().objects) {
        const int object_filament = object->config.has("extruder") ? object->config.extruder() : 1;
        bool      inherits = false;
        for (const ModelVolume* volume : object->volumes) {
            const int own = volume->config.has("extruder") ? volume->config.extruder() : 0;
            if (own == slot && (volume->is_model_part() || volume->is_modifier()))
                parts.push_back(Ids::of(*volume));
            if (own == 0 && volume->is_model_part())
                inherits = true;
            if (!volume->mmu_segmentation_facets.empty()) {
                const std::vector<size_t> painted = volume->get_extruders_from_multi_material_painting();
                if (std::find(painted.begin(), painted.end(), static_cast<size_t>(slot - 1)) != painted.end())
                    paint.push_back(Ids::of(*volume));
            }
        }
        if (inherits && object_filament == slot)
            objects.push_back(Ids::of(*object));
        for (const auto& [range, config] : object->layer_config_ranges)
            if (config.has("extruder") && config.get().opt_int("extruder") == slot)
                ranges.push_back({{"object", Ids::of(*object)}, {"z_min", round4(range.first)}, {"z_max", round4(range.second)}});
    }
    json out = json::object();
    out["objects"] = std::move(objects);
    out["parts"] = std::move(parts);
    out["layer_ranges"] = std::move(ranges);
    out["paint"] = std::move(paint);
    return out;
}

bool filament_in_use(const json& used_by)
{
    for (const char* key : {"objects", "parts", "layer_ranges", "paint"})
        if (used_by.contains(key) && !used_by[key].empty())
            return true;
    return false;
}

json summary_json()
{
    const Model&   model = plater().model();
    PartPlateList& plates = plater().get_partplate_list();
    json out = json::object();
    out["project"] = project_json();
    out["printer"] = printer_json()["preset"];
    out["process"] = process_json()["preset"];
    out["filaments"] = wxGetApp().preset_bundle->filament_presets.size();
    out["plates"] = plates.get_plate_count();
    json objects = json::array();
    for (int i = 0; i < static_cast<int>(model.objects.size()); ++i)
        objects.push_back(object_json(i, Detail::Summary));
    out["objects"] = std::move(objects);
    return out;
}

} // namespace Scene

}}} // namespace Slic3r::GUI::Mcp
