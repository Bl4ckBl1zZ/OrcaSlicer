#ifndef slic3r_SliceAnalysis_hpp_
#define slic3r_SliceAnalysis_hpp_

// Slice analysis: turns a finished slice into plain, numerically checkable facts (no GUI types).
//
// Two sources, used separately or together:
//  - GCodeProcessorResult (after G-code export): every move with its feature, filament, feed rate,
//    width, height, mm3/mm, fan, temperature, time, layer and object label. Timing, filament, flow,
//    speed, cooling, seams, multi-material and the per-layer table come from here.
//  - Print (after Print::process(), i.e. after slicing, before or after export): layer islands
//    (lslices), extrusion entities per region and support layers. Contact areas, unsupported
//    regions, overhang degree bands and cross-section queries come from here.
//
// Coordinates are millimetres in the frame of the G-code moves: bed coordinates plus the plate
// offset (GCodeProcessor adds the plate offset back to every move; PrintInstance::shift contains
// the same offset), so Print-based and move-based locations can be compared directly.
// Layer indices: move-based results use the G-code layer index (MoveVertex::layer_id, 0 = first
// printed layer of the plate, including raft/support-only layers); Print-based results use the
// PrintObject layer index and always also give z.
//
// Everything is plain structs with std::string/std::vector members; SliceAnalysisJson.hpp has
// nlohmann::json conversions for all of them.

#include "libslic3r/ExtrusionEntity.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class Print;
class PrintObject;
class DynamicPrintConfig;
struct GCodeProcessorResult;

namespace SliceAnalysis {

// ------------------------------------------------------------------------------------------------
// Common types
// ------------------------------------------------------------------------------------------------

struct Box2
{
    double min_x = std::numeric_limits<double>::max();
    double min_y = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double max_y = std::numeric_limits<double>::lowest();

    Box2() = default;
    Box2(double x0, double y0, double x1, double y1) : min_x(x0), min_y(y0), max_x(x1), max_y(y1) {}
    bool   defined() const { return min_x <= max_x && min_y <= max_y; }
    void   merge(double x, double y);
    void   merge(const Box2 &b);
    bool   contains(double x, double y) const { return x >= min_x && x <= max_x && y >= min_y && y <= max_y; }
    bool   intersects(const Box2 &b) const;
    Box2   inflated(double d) const { return defined() ? Box2(min_x - d, min_y - d, max_x + d, max_y + d) : *this; }
    double size_x() const { return defined() ? max_x - min_x : 0.; }
    double size_y() const { return defined() ? max_y - min_y : 0.; }
    double area() const { return size_x() * size_y(); }
    double center_x() const { return 0.5 * (min_x + max_x); }
    double center_y() const { return 0.5 * (min_y + max_y); }
};

// An XY box with a z range; z range [-inf, +inf] when not given.
struct Box3
{
    Box2   xy;
    double min_z = std::numeric_limits<double>::lowest();
    double max_z = std::numeric_limits<double>::max();
    Box3() = default;
    Box3(const Box2 &xy, double z0 = std::numeric_limits<double>::lowest(), double z1 = std::numeric_limits<double>::max()) : xy(xy), min_z(z0), max_z(z1) {}
};

// Where something is. layer: see the note on layer indices at the top. object: the G-code object
// label id (ModelInstance::get_labeled_id()), -1 when not inside an object (skirt, wipe tower).
struct Location
{
    int         layer  = -1;
    double      z      = 0.;
    double      x      = 0.;
    double      y      = 0.;
    int         object = -1;
    std::string role;
};

// Stable snake_case key for a feature (extrusion role): "outer_wall", "inner_wall", "overhang_wall",
// "sparse_infill", "internal_solid_infill", "top_surface", "bottom_surface", "ironing", "bridge",
// "internal_bridge", "gap_fill", "skirt", "brim", "support", "support_interface", "support_transition",
// "prime_tower", "custom", "mixed", "none".
const char   *role_key(ExtrusionRole role);
// Inverse of role_key(); erNone for an unknown key.
ExtrusionRole role_from_key(const std::string &key);
bool          is_support_role(ExtrusionRole role);
bool          is_bridge_role(ExtrusionRole role);

// "ok" / "warning" / "problem" / "not_applicable". Each check sets one with a one-line message.
struct Verdict
{
    std::string verdict = "ok";
    std::string message;
    // Which data was used and how exact the numbers are.
    std::string source;
    std::string exactness;
    void raise(const char *level, const std::string &msg);
};

// ------------------------------------------------------------------------------------------------
// Summary and per-layer table (source: GCodeProcessorResult; Print optional for object names)
// ------------------------------------------------------------------------------------------------

struct Options
{
    // Time mode used for per-feature/per-layer/per-object times: 0 normal, 1 silent (stealth).
    int time_mode = 0;
};

struct RoleStats
{
    std::string role;
    double      time_s      = 0.; // time of the extrusion moves of this feature
    double      length_mm   = 0.; // XY(Z) path length extruded
    double      volume_mm3  = 0.; // filament volume pushed (E * filament cross-section)
    double      filament_mm = 0.; // filament length (E)
    double      weight_g    = 0.;
    double      cost        = 0.;
    size_t      moves       = 0;
    double      max_speed   = 0.; // mm/s commanded feed rate
    double      mean_speed  = 0.; // length / (sum of length / feed rate)
    double      max_flow    = 0.; // mm3/s commanded volumetric rate (feed rate * mm3/mm)
    double      mean_flow   = 0.; // volume / commanded extrusion time
    // The slicer's own figures for this feature (PrintEstimatedStatistics::used_filaments_per_role).
    double      slicer_filament_m = 0.;
    double      slicer_weight_g   = 0.;
};

struct FilamentStats
{
    int    filament = 0; // 0-based filament index
    // The slicer's own volumes (PrintEstimatedStatistics::*_volumes_per_extruder, flush_per_filament), mm3.
    double model_mm3      = 0.;
    double support_mm3    = 0.;
    double wipe_tower_mm3 = 0.;
    double flush_mm3      = 0.;
    double total_mm3      = 0.;
    // From total_mm3 with the filament's diameter, density and cost.
    double length_mm = 0.;
    double weight_g  = 0.;
    double cost      = 0.;
    // Sum over the extrusion moves of this filament (excludes flush, which is not an extrusion move), and its split
    // into support and prime tower features. Model + support match the slicer's per filament; the slicer books
    // prime tower volume extruded just before a tool change on the next filament (its tower cache is flushed at the
    // next feature change, after the switch), so per-filament tower figures differ by that amount, totals do not.
    double moves_mm3 = 0.;
    double moves_support_mm3 = 0.;
    double moves_tower_mm3 = 0.;
    double diameter_mm  = 0.;
    double density      = 0.; // g/cm3
    double cost_per_kg  = 0.;
};

struct ObjectStats
{
    int         label_id = -1; // G-code object label (ModelInstance::get_labeled_id())
    std::string name;          // from the result's object masses or the Print, when known
    int         instance = -1;
    double      time_s     = 0.; // time of the moves inside this object's label block
    double      volume_mm3 = 0.; // extruded volume of those moves (incl. its supports and brim when labelled)
    double      support_mm3 = 0.;
    double      mass_g         = 0.; // slicer's object_masses (model + its brim), when present
    double      support_mass_g = 0.; // slicer's support_masses, when present
    Box2        box;                 // XY extent of its extrusions
    double      min_z = 0., max_z = 0.;
    int         first_layer = -1, last_layer = -1;
};

struct Summary
{
    // Totals as the slicer reports them.
    double time_s          = 0.; // normal mode
    double silent_time_s   = 0.; // 0 when the printer has no silent mode
    bool   has_silent_mode = false;
    double prepare_time_s  = 0.; // start G-code
    double first_layer_time_s = 0.; // GCodeProcessorResult::initial_layer_time (normal mode)
    // Sums over moves (Options::time_mode).
    double moves_time_s      = 0.; // all moves; equals time_s up to rounding (cross-check)
    double extrusion_time_s  = 0.;
    double travel_time_s     = 0.;
    double other_time_s      = 0.; // retract/unretract/wipe/tool change/custom moves outside extrusion
    double start_gcode_time_s = 0.; // custom-role moves before the first print move
    double end_gcode_time_s   = 0.; // custom-role moves after the last extrusion

    std::vector<RoleStats>     roles;     // only features that were printed
    std::vector<FilamentStats> filaments; // only filaments with a non-zero volume

    double total_mm3        = 0.; // sum of FilamentStats::total_mm3 (incl. flush)
    double model_mm3        = 0.;
    double support_mm3      = 0.;
    double wipe_tower_mm3   = 0.;
    double flush_mm3        = 0.;
    double total_length_mm  = 0.;
    double total_weight_g   = 0.;
    double total_cost       = 0.;
    double moves_volume_mm3 = 0.; // sum over extrusion moves; = model + support + wipe tower

    size_t layer_count        = 0;
    double first_layer_height = 0.;
    double layer_height       = 0.; // most common height of the layers above the first
    double min_layer_height   = 0.;
    double max_layer_height   = 0.;
    double max_z              = 0.; // highest extrusion z

    unsigned tool_changes      = 0; // Tool_change moves
    unsigned filament_changes  = 0; // slicer's total_filament_changes
    unsigned extruder_changes  = 0; // slicer's total_extruder_changes
    double   tool_change_time_s = 0.;
    double   travel_distance_mm = 0.; // slicer's total_travel_distance
    unsigned travel_moves       = 0;
    unsigned retractions        = 0;
    unsigned seams              = 0;
    double   wipe_tower_x = 0., wipe_tower_y = 0.; // centre of the prime tower extrusions
    Box2     wipe_tower_box;

    std::vector<ObjectStats> objects; // per object instance (label); -1 = outside any object
    size_t   move_count = 0;
    unsigned plate_id   = 0; // GCodeProcessorResult::id
    std::vector<std::string> warnings; // the slicer's warning enum strings
};

struct LayerRoleStats
{
    std::string role;
    double      length_mm  = 0.;
    double      volume_mm3 = 0.;
    double      time_s     = 0.;
    double      max_speed  = 0.;
    double      mean_speed = 0.;
};

struct LayerFilamentStats
{
    int    filament   = 0;
    double length_mm  = 0.; // path length
    double volume_mm3 = 0.;
};

struct LayerRow
{
    int    index  = 0;
    double z      = 0.; // highest extrusion z of the layer
    double height = 0.; // most used extrusion height
    double time_s = 0.; // all moves of the layer (start/end G-code excluded)
    double length_mm  = 0.;
    double volume_mm3 = 0.;
    std::vector<LayerRoleStats>     roles;
    std::vector<LayerFilamentStats> filaments;
    double   max_flow  = 0.; // mm3/s
    double   mean_flow = 0.;
    Location max_flow_at;
    double   max_speed = 0.;
    double   fan_min = 0., fan_max = 0.;   // % over extrusion moves
    double   temp_min = 0., temp_max = 0.; // C over extrusion moves
    int      tool_changes = 0;
    bool     has_support = false, has_bridge = false, has_overhang_wall = false, has_ironing = false, has_gap_fill = false;
    size_t   first_move = 0, last_move = 0; // indices into GCodeProcessorResult::moves
};

// One pass over the moves. Keeps a pointer to the result: the result must outlive this object.
class MoveAnalysis
{
public:
    MoveAnalysis() = default;
    // result: after G-code export (Print::export_gcode(path, &result, ...) or the GUI's slice result).
    // print: optional, after process(); only used for object names when the result has none.
    MoveAnalysis(const GCodeProcessorResult &result, const Print *print = nullptr, const Options &options = {});

    const Summary               &summary() const { return m_summary; }
    const std::vector<LayerRow> &layers() const { return m_layers; }
    // Rows [from, from + count) taking every `every`-th row.
    std::vector<LayerRow>        layer_page(size_t from, size_t count, size_t every = 1) const;
    // Index of the layer whose z is nearest to z (-1 if none).
    int                          layer_at_z(double z) const;
    const GCodeProcessorResult  *result() const { return m_result; }
    const Options               &options() const { return m_options; }
    // Seam positions: Seam moves (GCodeProcessor detects a closed outer wall); falls back to the
    // start points of outer-wall runs when the result has no Seam moves.
    struct Seam { int layer; float x, y, z; int object; };
    const std::vector<Seam>     &seams() const { return m_seams; }
    bool                         seams_from_seam_moves() const { return m_seams_from_moves; }
    struct ToolChange { int layer; double z; int from, to; size_t move; };
    const std::vector<ToolChange> &tool_changes() const { return m_tool_changes; }
    // Object (label id) of a move: MoveVertex::object_label_id when the G-code carries object labels
    // (Bambu-style "; start printing object" comments). Otherwise, when a Print was given, the instance
    // whose sliced XY extent (grown by 1 mm) contains the move's end point; -1 outside every instance.
    int                          object_at(size_t move_index) const;
    // Time of a move in the chosen mode. The processor stores the time of the move that follows a closed outer wall on
    // the Seam marker pushed just before it (the time block is created before the marker); that time is moved here to
    // the next real move, so every second of the total lands on a real move.
    double                       move_time(size_t move_index) const;
    // "gcode_labels", "print_footprint" or "none".
    const std::string           &object_source() const { return m_object_source; }

private:
    const GCodeProcessorResult *m_result = nullptr;
    Options                     m_options;
    Summary                     m_summary;
    std::vector<LayerRow>       m_layers;
    std::vector<Seam>           m_seams;
    bool                        m_seams_from_moves = true;
    std::vector<ToolChange>     m_tool_changes;
    std::vector<int>            m_move_object; // only when objects are attributed by footprint
    std::map<size_t, double>    m_carried_time; // move index -> time carried from preceding Seam markers
    std::string                 m_object_source = "none";
};

// Calls fn(move_index, from_x, from_y, from_z, move) for every move that has a start point: moves the
// G-code processor inserted only for rendering (internal_only) and Seam markers are skipped, so
// lengths and sums are not double-counted.
template<class Fn> void for_each_move(const GCodeProcessorResult &result, Fn &&fn);

// ------------------------------------------------------------------------------------------------
// Limits used by the checks (from the configuration)
// ------------------------------------------------------------------------------------------------

struct Limits
{
    std::vector<double> max_volumetric_speed;   // per filament, mm3/s (filament_max_volumetric_speed)
    std::vector<double> slow_down_layer_time;   // per filament, s
    std::vector<double> slow_down_min_speed;    // per filament, mm/s
    std::vector<double> fan_cooling_layer_time; // per filament, s
    std::vector<bool>   slow_down_for_layer_cooling;
    // Configured speed and acceleration per feature (role key), first nozzle variant.
    std::map<std::string, double> role_speed;
    std::map<std::string, double> role_acceleration;
    double initial_layer_speed        = 0.;
    double initial_layer_acceleration = 0.;
    double travel_speed               = 0.;
    double travel_acceleration        = 0.;
    double default_acceleration       = 0.;
    double machine_max_speed_x = 0., machine_max_speed_y = 0.;
    double machine_max_acceleration_extruding = 0., machine_max_acceleration_travel = 0.;
    double nozzle_diameter = 0.4;
    double support_top_z_distance = 0.2;
    double get(const std::vector<double> &v, int filament, double dflt = 0.) const;
};
Limits limits_from_config(const DynamicPrintConfig &config);
// print.full_print_config() (global values; per-object/per-region overrides are not reflected).
Limits limits_from_print(const Print &print);

// ------------------------------------------------------------------------------------------------
// Checks
// ------------------------------------------------------------------------------------------------

// Volumetric flow against filament_max_volumetric_speed.
struct FlowFilament
{
    int      filament = 0;
    double   limit = 0.;            // mm3/s, 0 = no limit configured
    double   max_flow = 0.;         // highest commanded flow of an extrusion move
    double   max_actual_flow = 0.;  // highest flow at the planner's actual (cruise) speed
    double   ratio = 0.;            // max_flow / limit
    Location max_at;
    double   at_limit_length_mm = 0.; // moves with flow >= (1 - tolerance) * limit (speed capped by the limit)
    double   at_limit_time_s    = 0.;
    double   over_limit_length_mm = 0.; // flow > (1 + tolerance) * limit
    double   over_limit_time_s    = 0.;
    double   share_at_limit = 0.;    // at_limit_time / extrusion time of the filament
    int      first_layer_at_limit = -1, last_layer_at_limit = -1, layers_at_limit = 0;
    std::vector<std::pair<std::string, double>> at_limit_length_by_role;
    std::vector<Location> worst;     // up to max_locations, highest flow first
};
struct FlowReport
{
    Verdict                   verdict;
    double                    tolerance = 0.02;
    std::vector<FlowFilament> filaments;
};
FlowReport check_flow(const MoveAnalysis &analysis, const Limits &limits, double tolerance = 0.02, size_t max_locations = 5);

// Speeds and accelerations actually commanded vs configured.
struct SpeedRole
{
    std::string role;
    double configured_speed = 0.;      // 0 = not known
    double max_speed = 0., mean_speed = 0.;
    double max_actual_speed = 0.;      // planner cruise speed
    double over_configured_length_mm = 0.; // feed rate > configured * (1 + tolerance), first layer excluded
    double below_half_length_mm = 0.;  // feed rate < configured / 2 (overhang/cooling/flow slow-downs)
    double configured_acceleration = 0.;
    double max_acceleration = 0.;
    Location max_speed_at;
};
struct SpeedReport
{
    Verdict                verdict;
    std::vector<SpeedRole> roles;
    double first_layer_max_speed = 0., initial_layer_speed = 0.;
    double travel_max_speed = 0., travel_speed = 0.;
    double max_acceleration = 0., machine_max_acceleration = 0.;
    double max_speed_x = 0., max_speed_y = 0., machine_max_speed_x = 0., machine_max_speed_y = 0.;
};
SpeedReport check_speeds(const MoveAnalysis &analysis, const Limits &limits, double tolerance = 0.02);

// Layer times vs the cooling thresholds.
struct CoolingLayer
{
    int    layer = 0;
    double z = 0., time_s = 0.;
    double slow_down_layer_time = 0.; // threshold of the filaments printed on the layer (max)
    double fan_cooling_layer_time = 0.;
    double min_speed = 0.;            // lowest extrusion feed rate on the layer
    double slow_down_min_speed = 0.;
    double fan_max = 0.;
    bool   below_slow_down_time = false; // still faster than the minimum layer time after the slow-down
    bool   at_min_speed = false;         // the slow-down hit slow_down_min_speed
};
struct CoolingReport
{
    Verdict verdict;
    int     layers_below_slow_down_time = 0;
    int     layers_below_fan_time       = 0;
    double  min_layer_time_s = 0.;
    int     min_layer_time_layer = -1;
    std::vector<CoolingLayer> layers; // only layers below one of the thresholds
};
CoolingReport check_cooling(const MoveAnalysis &analysis, const Limits &limits);

// Clusters of extrusions of a feature (connected within a gap in XY, across consecutive layers).
struct Cluster
{
    int    first_layer = -1, last_layer = -1;
    double min_z = 0., max_z = 0.;
    Box2   box;
    double length_mm = 0., volume_mm3 = 0.;
    double longest_segment_mm = 0.;
    int    object = -1;
};

// Support material.
struct SupportLayerRow
{
    int    layer = 0;
    double z = 0., length_mm = 0., volume_mm3 = 0.;
    Box2   box;
};
struct SupportObject
{
    int         object_index = -1; // index in Print::objects()
    std::string name;
    int         support_layers = 0;
    double      contact_area_mm2 = 0.;   // model area resting on support (Print geometry)
    double      support_area_mm2 = 0.;   // sum over support layers of the support extrusions' covered area
    int         first_contact_layer = -1, last_contact_layer = -1;
    Box2        contact_box;
};
struct SupportReport
{
    Verdict verdict;
    double  length_mm = 0., volume_mm3 = 0., time_s = 0.;
    double  interface_volume_mm3 = 0.;
    double  slicer_support_mm3 = 0.;
    int     layers_with_support = 0;
    int     first_layer = -1, last_layer = -1;
    Box2    box;
    std::vector<SupportLayerRow> layers;
    std::vector<Cluster>         clusters; // largest first
    std::vector<SupportObject>   objects;  // when a Print was given
};
SupportReport check_supports(const MoveAnalysis &analysis, const Print *print = nullptr, size_t max_clusters = 10);
// Support extrusions (all support features) inside a box: length and volume clipped to the XY box,
// z range applied to the move z.
struct BoxQuery
{
    double length_mm = 0., volume_mm3 = 0., time_s = 0.;
    int    layers = 0, first_layer = -1, last_layer = -1;
    Box2   box; // extent of what was found
    std::vector<std::pair<std::string, double>> length_by_role;
    std::vector<std::pair<int, double>>         length_by_filament;
};
BoxQuery support_in_box(const MoveAnalysis &analysis, const Box3 &box);
// Any features (role keys; empty = all extrusions) inside a box.
BoxQuery extrusions_in_box(const MoveAnalysis &analysis, const Box3 &box, const std::vector<std::string> &roles = {});

// Bridges and overhang walls.
struct OverhangBand
{
    std::string band; // "0", "0-25", "25-50", "50-75", "75-100", "100"
    double      length_mm = 0.;
    Location    example; // a point in the band (highest overhang of the band)
};
struct OverhangReport
{
    Verdict verdict;
    // From the moves.
    double bridge_length_mm = 0., bridge_volume_mm3 = 0.;
    double internal_bridge_length_mm = 0.;
    double overhang_wall_length_mm = 0., overhang_wall_volume_mm3 = 0.;
    int    bridge_runs = 0, overhang_wall_runs = 0, layers_with_bridge = 0, layers_with_overhang_wall = 0;
    double   longest_bridge_mm = 0.; // longest straight bridge segment = span
    Location longest_bridge_at;
    std::vector<Cluster> bridge_clusters;
    std::vector<Cluster> overhang_wall_clusters;
    // From the Print (perimeters vs the layer below), when given.
    bool   bands_computed = false;
    std::vector<OverhangBand> wall_bands; // length of wall per overhang band, share of the line width unsupported
};
OverhangReport check_overhangs(const MoveAnalysis &analysis, const Print *print = nullptr, size_t max_clusters = 10);

// Unsupported regions ("will this fall").
struct UnsupportedRegion
{
    int    object_index = -1;
    int    layer = -1; // PrintObject layer index
    double z = 0.;
    double area_mm2 = 0.;
    Box2   box;
    double cx = 0., cy = 0.;
    bool   floating = false;        // the whole island has nothing underneath (no object, no support)
    double max_distance_mm = 0.;    // farthest region vertex from the supported area (lower bound of the cantilever)
};
struct UnsupportedReport
{
    Verdict verdict;
    double  tolerance_mm = 0.;
    int     floating_islands = 0;
    double  floating_area_mm2 = 0.;
    double  unsupported_area_mm2 = 0.; // not floating: overhanging/bridged parts with nothing underneath
    double  max_distance_mm = 0.;
    std::vector<UnsupportedRegion> regions; // floating first, then by area
};
struct UnsupportedOptions
{
    // Distance by which the lower layer/support footprint is grown before testing what is supported
    // (an extrusion supported over part of its width still sticks). <= 0: half the nozzle diameter.
    double tolerance_mm = -1.;
    // Regions smaller than this are ignored.
    double min_area_mm2 = 0.05;
    // Support layers whose top is within this distance below a layer's bottom count as support under it
    // (<= 0: support_top_z_distance + the layer height + 0.05).
    double support_gap_mm = -1.;
    // Overhanging regions whose farthest vertex is at most this far from supported area are not reported.
    double report_distance_mm = 1.;
    size_t max_regions = 50;
};
UnsupportedReport check_unsupported(const Print &print, const UnsupportedOptions &options = {});

// First layer, adhesion.
struct FirstLayerObject
{
    int         object_index = -1;
    std::string name;
    int         instance = 0;
    double      contact_area_mm2 = 0.;   // area of the first layer's islands (the raft's when there is a raft)
    double      object_first_layer_area_mm2 = 0.;
    Box2        footprint;               // world XY
    double      height_mm = 0.;
    double      aspect_ratio = 0.;       // height / min(footprint width, depth)
    double      slenderness = 0.;        // height / sqrt(contact area)
    int         islands = 0;
    bool        raft = false;
    int         raft_layers = 0;
    double      brim_length_mm = 0.;     // brim extrusions within the object's footprint grown by 20 mm
};
struct FirstLayerReport
{
    Verdict verdict;
    double  first_layer_height = 0., first_layer_time_s = 0.;
    bool    has_brim = false, has_skirt = false, has_raft = false;
    double  brim_length_mm = 0., skirt_length_mm = 0.;
    Box2    brim_box, skirt_box;
    std::vector<FirstLayerObject> objects;
};
// aspect_warning: verdict warning when an object without brim/raft has aspect_ratio above it.
FirstLayerReport check_first_layer(const Print &print, const MoveAnalysis *analysis = nullptr, double aspect_warning = 3.);

// Seams.
struct SeamObject
{
    int    object = -1; // label id
    int    count = 0;
    double cx = 0., cy = 0.; // centre of the object's outer walls (XY box centre)
    Box2   box;
    double mean_angle_deg = 0.;     // circular mean of the seam angle around the centre; 0 = +X (right), 90 = +Y (rear), 180 = left, 270 = front
    double concentration = 0.;      // resultant length of the unit angle vectors: 1 = all in one direction, ~0 = spread
    double angular_spread_deg = 0.; // circular standard deviation
    double rear_share = 0., front_share = 0., left_share = 0., right_share = 0.; // angle quadrants centred on 90/270/180/0
    double median_step_mm = 0.;     // median XY distance from a seam to the nearest seam of the layer below
    double aligned_share = 0.;      // share of steps <= the alignment distance
    std::string pattern;            // "aligned", "scattered" or "mixed"
    double min_y = 0., max_y = 0., mean_y = 0., mean_x = 0.;
    std::vector<Location> samples;  // up to max_samples, evenly spread over the layers
};
struct SeamReport
{
    Verdict verdict;
    bool    from_seam_moves = true;
    int     count = 0;
    std::vector<SeamObject> objects;
};
SeamReport check_seams(const MoveAnalysis &analysis, double align_distance_mm = 1.0, size_t max_samples = 20);

// Thin features.
struct ThinFeatureReport
{
    Verdict verdict;
    double gap_fill_length_mm = 0., gap_fill_volume_mm3 = 0.;
    int    layers_with_gap_fill = 0;
    std::vector<Cluster> gap_fill_clusters;
    double narrow_width_mm = 0.;      // threshold used
    double narrow_length_mm = 0.;     // walls narrower than narrow_width_mm
    std::vector<Location> narrow_examples;
    // From the Print: parts of the layer islands no extrusion covers (dropped features).
    bool   uncovered_computed = false;
    double uncovered_area_mm2 = 0.;
    std::vector<UnsupportedRegion> uncovered; // reuses the region struct; floating = false
};
// narrow_width_mm <= 0: 0.75 * nozzle diameter of limits (0.3 for a 0.4 nozzle).
// With a print, layer islands are compared with the area the extrusions cover; regions narrower than
// what a perimeter can print show up as uncovered (min_area_mm2 filters slivers).
ThinFeatureReport check_thin_features(const MoveAnalysis &analysis, const Print *print = nullptr, double narrow_width_mm = -1.,
                                      double min_area_mm2 = 0.1, size_t max_items = 20);

// Multi-material.
struct Transition
{
    int    layer = 0;
    double z = 0.;
    int    from = -1, to = -1;
    double tower_volume_mm3 = 0.; // prime tower extrusion after this change until the next print move
};
struct FilamentRange
{
    int filament = 0;
    int first_layer = -1, last_layer = -1, layers = 0;
    std::vector<std::pair<int, int>> ranges; // inclusive layer ranges
};
struct MultiMaterialReport
{
    Verdict verdict;
    int     tool_changes = 0;
    int     max_changes_per_layer = 0;
    std::vector<std::pair<int, int>> changes_per_layer; // (layer, count), layers with changes only
    std::vector<Transition>    transitions;
    std::vector<FilamentRange> filaments;
    std::vector<std::pair<std::pair<int, int>, int>> transition_counts; // slicer's filament_change_count_map
    std::vector<std::pair<int, double>> flush_per_filament;             // slicer's flush volumes
    double tower_volume_mm3 = 0.;
    double flush_mm3 = 0.;
    bool   has_wipe_tower = false;
    Box2   wipe_tower_box;               // of the prime tower extrusions
    double wipe_tower_height = 0.;
    // From the Print's wipe tower data when given.
    double tower_width = 0., tower_depth = 0., tower_brim_width = 0.;
};
MultiMaterialReport check_multi_material(const MoveAnalysis &analysis, const Print *print = nullptr);

// Cross-section at one layer inside an XY box (empty box = whole layer).
struct SectionRole
{
    std::string role;
    double length_mm = 0., volume_mm3 = 0., mean_width = 0.;
};
struct SectionFromMoves
{
    int    layer = -1;
    double z = 0.;
    Box2   query;
    std::vector<SectionRole> roles;
    std::vector<std::pair<int, double>> filaments; // filament, length
    double area_mm2 = 0.;          // query box area
    double extruded_area_mm2 = 0.; // sum of length * width
    double infill_area_mm2 = 0.;   // sparse infill length * width
};
SectionFromMoves section_from_moves(const MoveAnalysis &analysis, int layer, const Box2 &box = Box2());
struct SectionRegion
{
    int         object_index = -1;
    int         region_id = -1;
    std::string infill_pattern;
    double      infill_density = 0.; // configured, %
    int         wall_loops = 0;      // configured
    double      slice_area_mm2 = 0.; // of this region inside the box
};
struct SectionFromPrint
{
    int    layer = -1; // object layer index (first object hit)
    double z = 0.;
    Box2   query;
    double island_area_mm2 = 0.;      // layer islands inside the box
    int    islands = 0;               // islands touching the box
    int    perimeter_count = 0;       // number of distinct wall insets present in the box
    double wall_width_mm = 0.;        // mean wall line width
    double wall_thickness_mm = 0.;    // perimeter_count lines: (n - 1) * spacing + width
    double sparse_infill_area_mm2 = 0.; // covered by sparse infill lines (length * width)
    double solid_infill_area_mm2 = 0.;
    double sparse_infill_fill_ratio = 0.; // sparse infill covered area / (island area - wall area) in the box: measured density
    std::vector<SectionRole>   roles;
    std::vector<SectionRegion> regions; // regions with slices in the box (modifiers create their own)
};
// z: the layer of each object whose print_z is nearest (within half a layer). box in world XY.
SectionFromPrint section_from_print(const Print &print, double z, const Box2 &box = Box2());

// ------------------------------------------------------------------------------------------------
// Diff of two summaries
// ------------------------------------------------------------------------------------------------

// Flattened numeric view of a summary: "time_s", "roles.sparse_infill.time_s", "filaments.0.weight_g", ...
std::vector<std::pair<std::string, double>> flatten(const Summary &summary);
struct DiffEntry
{
    std::string key;
    double      before = 0., after = 0., delta = 0.;
    double      relative = 0.; // delta / |before|, 0 when before is 0
    bool        added = false, removed = false;
};
// Entries whose |delta| > abs_tolerance and (before == 0 or |relative| > rel_tolerance), largest relative change first.
std::vector<DiffEntry> diff(const Summary &before, const Summary &after, double rel_tolerance = 0.001, double abs_tolerance = 1e-6);

} // namespace SliceAnalysis
} // namespace Slic3r

#include "libslic3r/GCode/GCodeProcessor.hpp"

namespace Slic3r { namespace SliceAnalysis {

template<class Fn> void for_each_move(const GCodeProcessorResult &result, Fn &&fn)
{
    const auto &moves = result.moves;
    if (moves.empty())
        return;
    Vec3f last = moves.front().position;
    for (size_t i = 0; i < moves.size(); ++i) {
        const GCodeProcessorResult::MoveVertex &m = moves[i];
        if (m.internal_only || m.type == EMoveType::Seam)
            continue;
        fn(i, last.x(), last.y(), last.z(), m);
        last = m.position;
    }
}

} } // namespace Slic3r::SliceAnalysis

#endif // slic3r_SliceAnalysis_hpp_
