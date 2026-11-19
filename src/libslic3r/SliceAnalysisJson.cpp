#include "SliceAnalysisJson.hpp"

#include <nlohmann/json.hpp>

#include <limits>

namespace Slic3r { namespace SliceAnalysis {

// Serialise the listed members under their own names.
#define SA_TO_JSON(Type, ...)                                                                      \
    void to_json(nlohmann::json &nlohmann_json_j, const Type &nlohmann_json_t)                     \
    {                                                                                              \
        nlohmann_json_j = nlohmann::json::object();                                                \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, __VA_ARGS__))                   \
    }

void to_json(nlohmann::json &j, const Box2 &v)
{
    if (!v.defined()) {
        j = nullptr;
        return;
    }
    j = nlohmann::json{{"min", {v.min_x, v.min_y}}, {"max", {v.max_x, v.max_y}}};
}

void to_json(nlohmann::json &j, const Box3 &v)
{
    to_json(j, v.xy);
    if (j.is_null())
        j = nlohmann::json::object();
    if (v.min_z > std::numeric_limits<double>::lowest())
        j["min_z"] = v.min_z;
    if (v.max_z < std::numeric_limits<double>::max())
        j["max_z"] = v.max_z;
}

SA_TO_JSON(Location, layer, z, x, y, object, role)
SA_TO_JSON(Verdict, verdict, message, source, exactness)
SA_TO_JSON(RoleStats, role, time_s, length_mm, volume_mm3, filament_mm, weight_g, cost, moves, max_speed, mean_speed, max_flow, mean_flow,
           slicer_filament_m, slicer_weight_g)
SA_TO_JSON(FilamentStats, filament, model_mm3, support_mm3, wipe_tower_mm3, flush_mm3, total_mm3, length_mm, weight_g, cost, moves_mm3, moves_support_mm3, moves_tower_mm3,
           diameter_mm, density, cost_per_kg)
SA_TO_JSON(ObjectStats, label_id, name, instance, time_s, volume_mm3, support_mm3, mass_g, support_mass_g, box, min_z, max_z, first_layer,
           last_layer)
SA_TO_JSON(Summary, time_s, silent_time_s, has_silent_mode, prepare_time_s, first_layer_time_s, moves_time_s, extrusion_time_s, travel_time_s,
           other_time_s, start_gcode_time_s, end_gcode_time_s, roles, filaments, total_mm3, model_mm3, support_mm3, wipe_tower_mm3, flush_mm3,
           total_length_mm, total_weight_g, total_cost, moves_volume_mm3, layer_count, first_layer_height, layer_height, min_layer_height,
           max_layer_height, max_z, tool_changes, filament_changes, extruder_changes, tool_change_time_s, travel_distance_mm, travel_moves,
           retractions, seams, wipe_tower_x, wipe_tower_y, wipe_tower_box, objects, move_count, plate_id, warnings)
SA_TO_JSON(LayerRoleStats, role, length_mm, volume_mm3, time_s, max_speed, mean_speed)
SA_TO_JSON(LayerFilamentStats, filament, length_mm, volume_mm3)
SA_TO_JSON(LayerRow, index, z, height, time_s, length_mm, volume_mm3, roles, filaments, max_flow, mean_flow, max_flow_at, max_speed, fan_min,
           fan_max, temp_min, temp_max, tool_changes, has_support, has_bridge, has_overhang_wall, has_ironing, has_gap_fill)
SA_TO_JSON(Cluster, first_layer, last_layer, min_z, max_z, box, length_mm, volume_mm3, longest_segment_mm, object)
SA_TO_JSON(FlowFilament, filament, limit, max_flow, max_actual_flow, ratio, max_at, at_limit_length_mm, at_limit_time_s, over_limit_length_mm,
           over_limit_time_s, share_at_limit, first_layer_at_limit, last_layer_at_limit, layers_at_limit, at_limit_length_by_role, worst)
SA_TO_JSON(FlowReport, verdict, tolerance, filaments)
SA_TO_JSON(SpeedRole, role, configured_speed, max_speed, mean_speed, max_actual_speed, over_configured_length_mm, below_half_length_mm,
           configured_acceleration, max_acceleration, max_speed_at)
SA_TO_JSON(SpeedReport, verdict, roles, first_layer_max_speed, initial_layer_speed, travel_max_speed, travel_speed, max_acceleration,
           machine_max_acceleration, max_speed_x, max_speed_y, machine_max_speed_x, machine_max_speed_y)
SA_TO_JSON(CoolingLayer, layer, z, time_s, slow_down_layer_time, fan_cooling_layer_time, min_speed, slow_down_min_speed, fan_max,
           below_slow_down_time, at_min_speed)
SA_TO_JSON(CoolingReport, verdict, layers_below_slow_down_time, layers_below_fan_time, min_layer_time_s, min_layer_time_layer, layers)
SA_TO_JSON(SupportLayerRow, layer, z, length_mm, volume_mm3, box)
SA_TO_JSON(SupportObject, object_index, name, support_layers, contact_area_mm2, support_area_mm2, first_contact_layer, last_contact_layer,
           contact_box)
SA_TO_JSON(SupportReport, verdict, length_mm, volume_mm3, time_s, interface_volume_mm3, slicer_support_mm3, layers_with_support, first_layer,
           last_layer, box, layers, clusters, objects)
SA_TO_JSON(BoxQuery, length_mm, volume_mm3, time_s, layers, first_layer, last_layer, box, length_by_role, length_by_filament)
SA_TO_JSON(OverhangBand, band, length_mm, example)
SA_TO_JSON(OverhangReport, verdict, bridge_length_mm, bridge_volume_mm3, internal_bridge_length_mm, overhang_wall_length_mm,
           overhang_wall_volume_mm3, bridge_runs, overhang_wall_runs, layers_with_bridge, layers_with_overhang_wall, longest_bridge_mm,
           longest_bridge_at, bridge_clusters, overhang_wall_clusters, bands_computed, wall_bands)
SA_TO_JSON(UnsupportedRegion, object_index, layer, z, area_mm2, box, cx, cy, floating, max_distance_mm)
SA_TO_JSON(UnsupportedReport, verdict, tolerance_mm, floating_islands, floating_area_mm2, unsupported_area_mm2, max_distance_mm, regions)
SA_TO_JSON(FirstLayerObject, object_index, name, instance, contact_area_mm2, object_first_layer_area_mm2, footprint, height_mm, aspect_ratio,
           slenderness, islands, raft, raft_layers, brim_length_mm)
SA_TO_JSON(FirstLayerReport, verdict, first_layer_height, first_layer_time_s, has_brim, has_skirt, has_raft, brim_length_mm, skirt_length_mm,
           brim_box, skirt_box, objects)
SA_TO_JSON(SeamObject, object, count, cx, cy, box, mean_angle_deg, concentration, angular_spread_deg, rear_share, front_share, left_share,
           right_share, median_step_mm, aligned_share, pattern, min_y, max_y, mean_y, mean_x, samples)
SA_TO_JSON(SeamReport, verdict, from_seam_moves, count, objects)
SA_TO_JSON(ThinFeatureReport, verdict, gap_fill_length_mm, gap_fill_volume_mm3, layers_with_gap_fill, gap_fill_clusters, narrow_width_mm,
           narrow_length_mm, narrow_examples, uncovered_computed, uncovered_area_mm2, uncovered)
SA_TO_JSON(Transition, layer, z, from, to, tower_volume_mm3)
SA_TO_JSON(FilamentRange, filament, first_layer, last_layer, layers, ranges)
SA_TO_JSON(MultiMaterialReport, verdict, tool_changes, max_changes_per_layer, changes_per_layer, transitions, filaments, transition_counts,
           flush_per_filament, tower_volume_mm3, flush_mm3, has_wipe_tower, wipe_tower_box, wipe_tower_height, tower_width, tower_depth,
           tower_brim_width)
SA_TO_JSON(SectionRole, role, length_mm, volume_mm3, mean_width)
SA_TO_JSON(SectionFromMoves, layer, z, query, roles, filaments, area_mm2, extruded_area_mm2, infill_area_mm2)
SA_TO_JSON(SectionRegion, object_index, region_id, infill_pattern, infill_density, wall_loops, slice_area_mm2)
SA_TO_JSON(SectionFromPrint, layer, z, query, island_area_mm2, islands, perimeter_count, wall_width_mm, wall_thickness_mm, sparse_infill_area_mm2,
           solid_infill_area_mm2, sparse_infill_fill_ratio, roles, regions)
SA_TO_JSON(DiffEntry, key, before, after, delta, relative, added, removed)

#undef SA_TO_JSON

} } // namespace Slic3r::SliceAnalysis
