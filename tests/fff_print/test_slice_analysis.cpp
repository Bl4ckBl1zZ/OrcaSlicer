#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SliceAnalysis.hpp"
#include "libslic3r/SliceAnalysisJson.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <nlohmann/json.hpp>

#include "test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using namespace Slic3r::SliceAnalysis;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// Settings every test here states rather than inherits: 0.2 mm layers, no skirt, brim, support or elephant foot
// compensation, two walls, 15 % infill, density 1.24 g/cm3 and cost 20 per kg.
DynamicPrintConfig analysis_config(std::initializer_list<ConfigBase::SetDeserializeItem> items = {}, unsigned filaments = 1)
{
    DynamicPrintConfig config = filaments > 1 ? multifilament_config(filaments) : DynamicPrintConfig::full_print_config();
    // Density and cost per filament, so weights and costs are not zero.
    std::string density = "1.24", cost = "20";
    for (unsigned i = 1; i < filaments; ++i) {
        density += ",1.24";
        cost += ",20";
    }
    config.set_deserialize_strict({{"filament_density", density}, {"filament_cost", cost}});
    config.set_deserialize_strict({
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"elefant_foot_compensation", "0"},
        {"skirt_loops", "0"},
        {"brim_type", "no_brim"},
        {"enable_support", "0"},
        {"wall_loops", "2"},
        {"sparse_infill_density", "15%"},
        {"gcode_comments", "1"},
    });
    config.set_deserialize_strict(items);
    return config;
}

struct Sliced
{
    Model                model;
    Print                print;
    GCodeProcessorResult result;
    std::string          gcode;
    std::unique_ptr<MoveAnalysis> analysis;
    // World XY centre of the first object's first instance (PrintObject frames are centred on the object).
    Vec2d center() const { return unscale(print.objects().front()->instances().front().shift); }
};

// Slice meshes (auto-arranged), export the G-code with its processor result, and analyse it. `edit` may add
// modifiers to the model before the final apply.
std::unique_ptr<Sliced> slice_and_analyse(std::vector<TriangleMesh> meshes, const DynamicPrintConfig &config,
                                          const std::function<void(Model &)> &edit = {})
{
    auto s = std::make_unique<Sliced>();
    init_print(std::move(meshes), s->print, s->model, config);
    if (edit) {
        edit(s->model);
        DynamicPrintConfig full = DynamicPrintConfig::full_print_config();
        full.apply(config);
        s->print.apply(s->model, full);
    }
    s->gcode    = gcode(s->print, &s->result);
    s->analysis = std::make_unique<MoveAnalysis>(s->result, &s->print);
    return s;
}

// A table: a 10 x 10 mm leg 10 mm tall under a 40 x 40 x 2 mm top.
TriangleMesh table_mesh()
{
    TriangleMesh leg = make_cube(10., 10., 10.);
    leg.translate(15.f, 15.f, 0.f);
    TriangleMesh top = make_cube(40., 40., 2.);
    top.translate(0.f, 0.f, 10.f);
    leg.merge(top);
    return leg;
}

// A cube on the bed and, beside it, a second cube that starts 6 mm above the bed: one object.
TriangleMesh floating_mesh()
{
    TriangleMesh base = make_cube(10., 10., 12.);
    TriangleMesh fly  = make_cube(8., 8., 6.);
    fly.translate(20.f, 0.f, 6.f);
    base.merge(fly);
    return base;
}

double sum_layers(const MoveAnalysis &a, double LayerRow::*field)
{
    double s = 0.;
    for (const LayerRow &r : a.layers()) s += r.*field;
    return s;
}

const RoleStats *find_role(const Summary &s, const std::string &key)
{
    for (const RoleStats &r : s.roles)
        if (r.role == key) return &r;
    return nullptr;
}

} // namespace

TEST_CASE("Feature and layer sums of a cube add up to the slicer's own totals", "[SliceAnalysis]")
{
    // 20 mm cube at 0.2 mm layers = 100 layers.
    auto s = slice_and_analyse({cube(20.)}, analysis_config());
    const Summary &sum = s->analysis->summary();
    const GCodeProcessorResult &r = s->result;

    CHECK(sum.layer_count == 100);
    CHECK_THAT(sum.first_layer_height, WithinAbs(0.2, 1e-3));
    CHECK_THAT(sum.layer_height, WithinAbs(0.2, 1e-3));
    CHECK_THAT(sum.max_z, WithinAbs(20., 1e-3));

    // Time: the per-move times add up to the processor's total; features + travel + other = all moves;
    // layers + start + end G-code = all moves.
    CHECK_THAT(sum.moves_time_s, WithinRel(sum.time_s, 1e-3));
    CHECK_THAT(sum.extrusion_time_s + sum.travel_time_s + sum.other_time_s, WithinRel(sum.moves_time_s, 1e-9));
    CHECK_THAT(sum_layers(*s->analysis, &LayerRow::time_s) + sum.start_gcode_time_s + sum.end_gcode_time_s, WithinRel(sum.moves_time_s, 1e-9));
    double role_time = 0.;
    for (const RoleStats &rs : sum.roles) role_time += rs.time_s;
    CHECK_THAT(role_time, WithinRel(sum.extrusion_time_s, 1e-9));
    // The first layer as the slicer measures it.
    CHECK_THAT(s->analysis->layers().front().time_s, WithinRel(double(r.initial_layer_time), 0.05));

    // Volume: per feature = per move = the slicer's model + support + prime tower volumes.
    double role_volume = 0., role_weight = 0., slicer_role_weight = 0.;
    for (const RoleStats &rs : sum.roles) {
        role_volume += rs.volume_mm3;
        role_weight += rs.weight_g;
        slicer_role_weight += rs.slicer_weight_g;
        INFO(rs.role);
        CHECK_THAT(rs.weight_g, WithinRel(rs.slicer_weight_g, 1e-3));
    }
    CHECK_THAT(role_volume, WithinRel(sum.moves_volume_mm3, 1e-9));
    CHECK_THAT(sum.moves_volume_mm3, WithinRel(sum.model_mm3 + sum.support_mm3 + sum.wipe_tower_mm3, 1e-4));
    CHECK_THAT(role_weight, WithinRel(slicer_role_weight, 1e-3));
    double custom_volume = 0.;
    if (const RoleStats *c = find_role(sum, "custom")) custom_volume = c->volume_mm3;
    CHECK_THAT(sum_layers(*s->analysis, &LayerRow::volume_mm3), WithinRel(sum.moves_volume_mm3 - custom_volume, 1e-6));

    // Filament totals = Print::print_statistics() after export.
    const PrintStatistics &ps = s->print.print_statistics();
    CHECK_THAT(sum.total_weight_g, WithinRel(ps.total_weight, 1e-6));
    CHECK_THAT(sum.total_length_mm, WithinRel(ps.total_used_filament, 1e-6));
    CHECK_THAT(sum.total_mm3, WithinRel(ps.total_extruded_volume, 1e-6));
    REQUIRE(sum.filaments.size() == 1);
    CHECK_THAT(sum.filaments[0].moves_mm3, WithinRel(sum.filaments[0].model_mm3, 1e-4));

    // Travel and retractions as counted by the processor.
    CHECK(sum.travel_moves == r.print_statistics.total_travel_moves);
    CHECK(sum.retractions > 0);
    CHECK(sum.tool_changes <= 1); // at most the initial tool selection

    // The object: one instance, attributed by its footprint (generic printer: no object labels in the moves).
    CHECK(s->analysis->object_source() == "print_footprint");
    const auto obj = std::find_if(sum.objects.begin(), sum.objects.end(), [](const ObjectStats &o) { return o.label_id >= 0; });
    REQUIRE(obj != sum.objects.end());
    CHECK_THAT(obj->box.size_x(), WithinAbs(20., 0.5));
    CHECK(obj->first_layer == 0);
    CHECK(obj->last_layer == 99);

    // The table is pageable and serialisable.
    const auto page = s->analysis->layer_page(10, 5, 2);
    REQUIRE(page.size() == 5);
    CHECK(page[1].index == 12);
    nlohmann::json j = sum;
    CHECK(j["layer_count"].get<int>() == 100);
    j = page;
    CHECK(j.size() == 5);
}

TEST_CASE("First layer contact area of a cube is its base", "[SliceAnalysis]")
{
    auto s = slice_and_analyse({cube(20.)}, analysis_config());
    const FirstLayerReport rep = check_first_layer(s->print, s->analysis.get());
    REQUIRE(rep.objects.size() == 1);
    CHECK_THAT(rep.objects[0].contact_area_mm2, WithinRel(400., 0.005));
    CHECK_THAT(rep.objects[0].aspect_ratio, WithinRel(1., 0.01));
    CHECK_THAT(rep.objects[0].height_mm, WithinAbs(20., 1e-3));
    CHECK(rep.objects[0].islands == 1);
    CHECK_FALSE(rep.has_brim);
    CHECK(rep.verdict.verdict == "ok");
}

TEST_CASE("A tall thin object without brim is flagged and a brim clears it", "[SliceAnalysis]")
{
    // 5 x 5 x 30 mm: height / width = 6.
    const TriangleMesh pillar = make_cube(5., 5., 30.);
    auto bare = slice_and_analyse({pillar}, analysis_config());
    FirstLayerReport rep = check_first_layer(bare->print, bare->analysis.get());
    REQUIRE(rep.objects.size() == 1);
    CHECK_THAT(rep.objects[0].aspect_ratio, WithinRel(6., 0.01));
    CHECK_THAT(rep.objects[0].contact_area_mm2, WithinRel(25., 0.01));
    CHECK(rep.verdict.verdict == "warning");

    auto brim = slice_and_analyse({pillar}, analysis_config({{"brim_type", "outer_only"}, {"brim_width", "5"}}));
    rep = check_first_layer(brim->print, brim->analysis.get());
    CHECK(rep.has_brim);
    CHECK(rep.objects[0].brim_length_mm > 0.);
    CHECK(rep.verdict.verdict == "ok");
}

TEST_CASE("Unsupported table top is reported without support and covered with support", "[SliceAnalysis]")
{
    SECTION("no support")
    {
        auto s = slice_and_analyse({table_mesh()}, analysis_config());
        const UnsupportedReport rep = check_unsupported(s->print);
        CHECK(rep.floating_islands == 0);
        // The top's first layer overhangs the leg: 40 x 40 - 10 x 10 = 1500 mm2 less the tolerance band around the leg.
        CHECK(rep.unsupported_area_mm2 > 1400.);
        CHECK(rep.unsupported_area_mm2 < 1500.);
        // Farthest corner: (20, 20) from the leg's grown corner (5 + tol, 5 + tol), plus the tolerance.
        CHECK(rep.max_distance_mm > 19.);
        CHECK(rep.verdict.verdict == "warning");
        REQUIRE_FALSE(rep.regions.empty());
        CHECK_THAT(rep.regions.front().z, WithinAbs(10.2, 1e-3));

        const SupportReport sup = check_supports(*s->analysis, &s->print);
        CHECK(sup.volume_mm3 == 0.);
        CHECK(sup.objects.front().contact_area_mm2 == 0.);

        const OverhangReport oh = check_overhangs(*s->analysis, &s->print);
        CHECK(oh.bridge_length_mm + oh.overhang_wall_length_mm > 0.);
        REQUIRE(oh.bands_computed);
        CHECK(oh.wall_bands.back().band == "100");
        CHECK(oh.wall_bands.back().length_mm > 100.); // the top's outer walls over air: 4 x 40 mm, two walls
        CHECK_THAT(oh.wall_bands.back().example.z, WithinAbs(10.2, 1e-3));
    }
    SECTION("support")
    {
        auto s = slice_and_analyse({table_mesh()}, analysis_config({{"enable_support", "1"}, {"support_type", "normal(auto)"}}));
        const SupportReport sup = check_supports(*s->analysis, &s->print);
        CHECK(sup.volume_mm3 > 0.);
        CHECK_THAT(sup.volume_mm3, WithinRel(s->analysis->summary().support_mm3, 1e-4));
        REQUIRE(sup.objects.size() == 1);
        // The model rests on support over most of the overhang.
        CHECK(sup.objects[0].contact_area_mm2 > 1000.);
        CHECK(sup.objects[0].contact_area_mm2 < 1500.);
        // Support under the overhang, none above the top or far away.
        const Vec2d c = s->center();
        const Box3 under(Box2(c.x() + 10., c.y() - 15., c.x() + 19., c.y() + 15.), 0., 10.);
        const Box3 above(Box2(c.x() - 20., c.y() - 20., c.x() + 20., c.y() + 20.), 12.5, 100.);
        const Box3 far(Box2(c.x() + 60., c.y() + 60., c.x() + 80., c.y() + 80.));
        CHECK(support_in_box(*s->analysis, under).length_mm > 100.);
        CHECK(support_in_box(*s->analysis, above).length_mm == 0.);
        CHECK(support_in_box(*s->analysis, far).length_mm == 0.);
        // Nothing above the underside of the top (z 10).
        for (const SupportLayerRow &row : sup.layers)
            CHECK(row.z <= 10. + 1e-3);

        // With support, far less is left unsupported than without.
        const UnsupportedReport rep = check_unsupported(s->print);
        CHECK(rep.floating_islands == 0);
        CHECK(rep.unsupported_area_mm2 < 300.);
    }
}

TEST_CASE("An island that starts in mid-air is floating unless support holds it", "[SliceAnalysis]")
{
    auto bare = slice_and_analyse({floating_mesh()}, analysis_config());
    UnsupportedReport rep = check_unsupported(bare->print);
    CHECK(rep.floating_islands == 1);
    CHECK(rep.verdict.verdict == "problem");
    REQUIRE_FALSE(rep.regions.empty());
    CHECK(rep.regions.front().floating);
    CHECK_THAT(rep.regions.front().area_mm2, WithinRel(64., 0.01));
    CHECK_THAT(rep.regions.front().z, WithinAbs(6.2, 1e-3));

    auto supported = slice_and_analyse({floating_mesh()}, analysis_config({{"enable_support", "1"}, {"support_type", "normal(auto)"}}));
    rep = check_unsupported(supported->print);
    CHECK(rep.floating_islands == 0);
}

TEST_CASE("Bridges of the arch are counted with their span", "[SliceAnalysis]")
{
    // TestMesh::bridge: legs at x 75..80 and 120..125, deck from z 5 to 8: a 40 mm span.
    auto s = slice_and_analyse({mesh(TestMesh::bridge)}, analysis_config());
    const OverhangReport oh = check_overhangs(*s->analysis, &s->print);
    CHECK(oh.bridge_runs > 0);
    CHECK(oh.layers_with_bridge >= 1);
    CHECK(oh.longest_bridge_mm >= 40.);
    CHECK(oh.longest_bridge_mm < 52.);
    CHECK_THAT(oh.longest_bridge_at.z, WithinAbs(5.2, 1e-3));
    REQUIRE_FALSE(oh.bridge_clusters.empty());
    CHECK(oh.bridge_clusters.front().box.size_x() >= 40.);
    // The layer table marks the bridge layer.
    const int l = s->analysis->layer_at_z(5.2);
    REQUIRE(l >= 0);
    CHECK(s->analysis->layers()[l].has_bridge);
}

TEST_CASE("A modifier's infill and walls show in the cross-section inside it and not outside", "[SliceAnalysis]")
{
    // 20 mm cube, 10 % infill and 2 walls; a centred 8 x 8 mm modifier through the whole height with 50 % and 4 walls.
    auto s = slice_and_analyse({cube(20.)}, analysis_config({{"sparse_infill_density", "10%"}, {"sparse_infill_pattern", "rectilinear"}}),
                               [](Model &model) {
                                   DynamicPrintConfig mod;
                                   mod.set_deserialize_strict({{"sparse_infill_density", "50%"}, {"wall_loops", "4"}});
                                   ModelObject *object = model.objects.front();
                                   ModelVolume *v = object->add_volume(make_cube(8., 8., 30.), ModelVolumeType::PARAMETER_MODIFIER);
                                   v->set_offset(Vec3d(10., 10., 10.));
                                   v->config.apply(mod);
                               });
    const Vec2d c = s->center();
    const double z = 10.;
    const Box2 inside(c.x() - 2., c.y() - 2., c.x() + 2., c.y() + 2.);
    const Box2 outside(c.x() + 5.5, c.y() - 2., c.x() + 8.5, c.y() + 2.);
    const SectionFromPrint in  = section_from_print(s->print, z, inside);
    const SectionFromPrint out = section_from_print(s->print, z, outside);
    REQUIRE(in.regions.size() == 1);
    REQUIRE(out.regions.size() == 1);
    CHECK_THAT(in.regions[0].infill_density, WithinAbs(50., 1e-6));
    CHECK(in.regions[0].wall_loops == 4);
    CHECK_THAT(out.regions[0].infill_density, WithinAbs(10., 1e-6));
    CHECK(out.regions[0].wall_loops == 2);
    CHECK(in.sparse_infill_fill_ratio > 3. * out.sparse_infill_fill_ratio);
    CHECK(out.sparse_infill_fill_ratio > 0.03);

    // Walls: across the object's outer wall (outside the modifier) 2 insets; across the modifier boundary 4.
    const Box2 outer_wall(c.x() + 8., c.y() - 1., c.x() + 11., c.y() + 1.);
    const Box2 mod_wall(c.x() + 1.5, c.y() - 1., c.x() + 4.5, c.y() + 1.);
    const SectionFromPrint ow = section_from_print(s->print, z, outer_wall);
    const SectionFromPrint mw = section_from_print(s->print, z, mod_wall);
    CHECK(ow.perimeter_count == 2);
    CHECK(mw.perimeter_count == 4);
    CHECK(mw.wall_thickness_mm > ow.wall_thickness_mm);

    // The same comparison from the G-code moves of that layer.
    const int l = s->analysis->layer_at_z(z);
    const SectionFromMoves min = section_from_moves(*s->analysis, l, inside);
    const SectionFromMoves mout = section_from_moves(*s->analysis, l, outside);
    CHECK(min.infill_area_mm2 / min.area_mm2 > 3. * mout.infill_area_mm2 / mout.area_mm2);
}

TEST_CASE("Two filaments: tool changes, per-layer filament use and purge", "[SliceAnalysis]")
{
    // Walls on filament 2, everything else on filament 1, with a prime tower: every layer uses both.
    auto s = slice_and_analyse({cube(20.)}, analysis_config({{"outer_wall_filament_id", "2"},
                                                             {"inner_wall_filament_id", "2"},
                                                             {"sparse_infill_filament_id", "1"},
                                                             {"internal_solid_filament_id", "1"},
                                                             {"top_surface_filament_id", "1"},
                                                             {"bottom_surface_filament_id", "1"},
                                                             {"enable_prime_tower", "1"},
                                                             {"prime_tower_width", "35"},
                                                             {"wipe_tower_x", "120"},
                                                             {"wipe_tower_y", "120"},
                                                             {"printable_area", "0x0,200x0,200x200,0x200"}},
                                                            2));
    const Summary &sum = s->analysis->summary();
    const MultiMaterialReport mm = check_multi_material(*s->analysis, &s->print);
    CHECK(mm.tool_changes >= 99); // at least one change per layer after the first
    CHECK(mm.max_changes_per_layer >= 1);
    CHECK(mm.has_wipe_tower);
    CHECK(mm.tower_volume_mm3 > 0.);
    REQUIRE(mm.filaments.size() == 2);
    for (const FilamentRange &f : mm.filaments) {
        INFO("filament " << f.filament);
        CHECK(f.layers == 100);
        CHECK(f.ranges.size() == 1);
    }
    for (const Transition &t : mm.transitions) {
        CHECK(t.from != t.to);
        CHECK(t.from >= 0);
        CHECK(t.from <= 1);
    }
    double purge = 0.;
    for (const Transition &t : mm.transitions) purge += t.tower_volume_mm3;
    CHECK(purge > 0.);
    CHECK(purge <= mm.tower_volume_mm3 * (1. + 1e-6));
    // Per-layer filament use.
    const LayerRow &mid = s->analysis->layers()[50];
    REQUIRE(mid.filaments.size() == 2);
    CHECK(mid.filaments[0].volume_mm3 > 0.);
    CHECK(mid.filaments[1].volume_mm3 > 0.);
    CHECK(mid.tool_changes >= 1);
    // Per filament: model and support from the moves = the slicer's; the prime tower total too, while the slicer
    // books tower volume extruded just before a change on the next filament (see FilamentStats).
    REQUIRE(sum.filaments.size() == 2);
    double tower_moves = 0., tower_slicer = 0.;
    for (const FilamentStats &f : sum.filaments) {
        INFO("filament " << f.filament);
        CHECK_THAT(f.moves_mm3 - f.moves_tower_mm3, WithinRel(f.model_mm3 + f.support_mm3, 1e-4));
        CHECK_THAT(f.moves_tower_mm3, WithinRel(f.wipe_tower_mm3, 0.2));
        tower_moves += f.moves_tower_mm3;
        tower_slicer += f.wipe_tower_mm3;
    }
    CHECK_THAT(tower_moves, WithinRel(tower_slicer, 1e-4));
    CHECK(sum.tool_changes >= unsigned(mm.tool_changes));
    nlohmann::json j = mm;
    CHECK(j["transitions"].size() == mm.transitions.size());
}

TEST_CASE("Rear and random seams give different seam summaries", "[SliceAnalysis]")
{
    const TriangleMesh cyl = make_cylinder(10., 10.);
    auto rear   = slice_and_analyse({cyl}, analysis_config({{"seam_position", "back"}}));
    auto random = slice_and_analyse({cyl}, analysis_config({{"seam_position", "random"}}));
    const SeamReport r = check_seams(*rear->analysis);
    const SeamReport x = check_seams(*random->analysis);
    REQUIRE(r.count >= 49);
    REQUIRE(x.count >= 49);
    const auto main_object = [](const SeamReport &rep) {
        return *std::max_element(rep.objects.begin(), rep.objects.end(), [](const SeamObject &a, const SeamObject &b) { return a.count < b.count; });
    };
    const SeamObject ro = main_object(r), xo = main_object(x);
    CHECK(ro.rear_share > 0.9);
    CHECK_THAT(ro.mean_angle_deg, WithinAbs(90., 15.));
    CHECK(ro.concentration > 0.95);
    CHECK(ro.pattern == "aligned");
    CHECK(ro.mean_y > ro.cy + 8.);
    CHECK(xo.rear_share < 0.6);
    CHECK(xo.concentration < 0.6);
    CHECK(xo.pattern != "aligned");
    CHECK(xo.median_step_mm > ro.median_step_mm);
}

TEST_CASE("Volumetric flow limit is reported where it caps the speed", "[SliceAnalysis]")
{
    const auto config = [](const char *limit) {
        return analysis_config({{"sparse_infill_speed", "250"},
                                {"internal_solid_infill_speed", "250"},
                                {"filament_max_volumetric_speed", limit},
                                {"sparse_infill_density", "20%"}});
    };
    auto limited = slice_and_analyse({cube(20.)}, config("4"));
    FlowReport rep = check_flow(*limited->analysis, limits_from_print(limited->print));
    REQUIRE(rep.filaments.size() == 1);
    CHECK_THAT(rep.filaments[0].limit, WithinAbs(4., 1e-9));
    CHECK(rep.filaments[0].at_limit_length_mm > 100.);
    CHECK(rep.filaments[0].over_limit_length_mm == 0.);
    CHECK(rep.filaments[0].max_flow <= 4. * 1.02);
    CHECK(rep.filaments[0].layers_at_limit > 50);
    CHECK(rep.verdict.verdict == "warning");
    REQUIRE_FALSE(rep.filaments[0].worst.empty());
    // The flow column of the layer table agrees.
    double max_flow = 0.;
    for (const LayerRow &row : limited->analysis->layers()) max_flow = std::max(max_flow, row.max_flow);
    CHECK_THAT(max_flow, WithinRel(rep.filaments[0].max_flow, 1e-6));

    auto unlimited = slice_and_analyse({cube(20.)}, config("100"));
    rep = check_flow(*unlimited->analysis, limits_from_print(unlimited->print));
    CHECK(rep.filaments[0].at_limit_length_mm == 0.);
    CHECK(rep.verdict.verdict == "ok");
    CHECK(rep.filaments[0].max_flow > 4.);
}

TEST_CASE("Small fast layers are reported against the minimum layer time", "[SliceAnalysis]")
{
    // A 5 mm cube cannot take 20 s per layer even at 10 mm/s.
    const auto config = [](const char *layer_time) {
        return analysis_config({{"slow_down_for_layer_cooling", "1"},
                                {"slow_down_layer_time", layer_time},
                                {"slow_down_min_speed", "10"},
                                {"fan_cooling_layer_time", "0"}});
    };
    auto fast = slice_and_analyse({cube(5.)}, config("20"));
    CoolingReport rep = check_cooling(*fast->analysis, limits_from_print(fast->print));
    CHECK(rep.layers_below_slow_down_time > 20);
    CHECK(rep.verdict.verdict == "warning");
    REQUIRE_FALSE(rep.layers.empty());
    const CoolingLayer &mid = rep.layers[rep.layers.size() / 2];
    CHECK(mid.below_slow_down_time);
    CHECK(mid.at_min_speed);
    CHECK(mid.time_s < 20.);

    auto relaxed = slice_and_analyse({cube(5.)}, config("1"));
    rep = check_cooling(*relaxed->analysis, limits_from_print(relaxed->print));
    CHECK(rep.layers_below_slow_down_time == 0);
    CHECK(rep.verdict.verdict == "ok");
}

TEST_CASE("Speeds stay within the configured feature speeds", "[SliceAnalysis]")
{
    auto s = slice_and_analyse({cube(20.)}, analysis_config({{"sparse_infill_speed", "120"}, {"outer_wall_speed", "60"}}));
    const SpeedReport rep = check_speeds(*s->analysis, limits_from_print(s->print));
    const auto role = [&rep](const std::string &k) {
        return *std::find_if(rep.roles.begin(), rep.roles.end(), [&k](const SpeedRole &r) { return r.role == k; });
    };
    CHECK_THAT(role("sparse_infill").configured_speed, WithinAbs(120., 1e-9));
    CHECK(role("sparse_infill").max_speed <= 120. * 1.001);
    CHECK(role("sparse_infill").over_configured_length_mm == 0.);
    CHECK(role("outer_wall").max_speed <= 60. * 1.001);
    CHECK(role("outer_wall").max_acceleration > 0.);
    CHECK(rep.verdict.verdict == "ok");
}

TEST_CASE("A fin below the minimum feature size is reported as not printed", "[SliceAnalysis]")
{
    // A 0.08 mm fin 4 mm beside a 10 mm cube; nothing narrower than 0.2 mm is printed.
    TriangleMesh m = make_cube(10., 10., 5.);
    TriangleMesh fin = make_cube(0.08, 8., 5.);
    fin.translate(14.f, 1.f, 0.f);
    m.merge(fin);
    auto s = slice_and_analyse({m}, analysis_config({{"min_feature_size", "50%"}, {"wall_generator", "arachne"}}));
    const ThinFeatureReport rep = check_thin_features(*s->analysis, &s->print);
    REQUIRE(rep.uncovered_computed);
    REQUIRE_FALSE(rep.uncovered.empty());
    // The largest uncovered region is the fin: 0.08 x 8 mm.
    const UnsupportedRegion &fin_region = rep.uncovered.front();
    CHECK_THAT(fin_region.area_mm2, WithinRel(0.64, 0.1));
    CHECK_THAT(fin_region.box.size_y(), WithinAbs(8., 0.1));
    CHECK(rep.verdict.verdict == "warning");

    auto plain = slice_and_analyse({make_cube(10., 10., 5.)}, analysis_config({{"min_feature_size", "50%"}, {"wall_generator", "arachne"}}));
    const ThinFeatureReport ok = check_thin_features(*plain->analysis, &plain->print);
    CHECK(ok.uncovered_area_mm2 < 0.1 * rep.uncovered_area_mm2);
}

TEST_CASE("Gap fill between walls is measured", "[SliceAnalysis]")
{
    // A 1.3 mm wall with two classic loops leaves a gap between the inner loops.
    auto s = slice_and_analyse({make_cube(20., 1.3, 5.)}, analysis_config({{"wall_generator", "classic"}, {"gap_infill_speed", "30"}}));
    const ThinFeatureReport rep = check_thin_features(*s->analysis, &s->print);
    const RoleStats *gf = find_role(s->analysis->summary(), "gap_fill");
    REQUIRE(gf != nullptr);
    CHECK(rep.gap_fill_length_mm > 0.);
    CHECK_THAT(rep.gap_fill_length_mm, WithinRel(gf->length_mm, 1e-9));
    CHECK(rep.layers_with_gap_fill > 0);
}

TEST_CASE("Summary diff lists what a settings change changed", "[SliceAnalysis]")
{
    auto sparse = slice_and_analyse({cube(20.)}, analysis_config({{"sparse_infill_density", "10%"}}));
    auto dense  = slice_and_analyse({cube(20.)}, analysis_config({{"sparse_infill_density", "40%"}}));
    const std::vector<DiffEntry> d = diff(sparse->analysis->summary(), dense->analysis->summary());
    const auto find = [&d](const std::string &k) { return std::find_if(d.begin(), d.end(), [&k](const DiffEntry &e) { return e.key == k; }); };
    const auto infill = find("roles.sparse_infill.volume_mm3");
    REQUIRE(infill != d.end());
    CHECK(infill->delta > 0.);
    CHECK(infill->relative > 1.);
    REQUIRE(find("total_weight_g") != d.end());
    CHECK(find("total_weight_g")->delta > 0.);
    CHECK(find("layer_count") == d.end());
    CHECK(diff(sparse->analysis->summary(), sparse->analysis->summary()).empty());
}

TEST_CASE("Two objects get separate figures", "[SliceAnalysis]")
{
    auto s = slice_and_analyse({cube(10.), cube(10.)}, analysis_config());
    std::vector<ObjectStats> objects;
    for (const ObjectStats &o : s->analysis->summary().objects)
        if (o.label_id >= 0) objects.push_back(o);
    REQUIRE(objects.size() == 2);
    CHECK(objects[0].label_id != objects[1].label_id);
    CHECK_THAT(objects[0].volume_mm3, WithinRel(objects[1].volume_mm3, 0.02));
    CHECK_FALSE(objects[0].box.intersects(objects[1].box));
    CHECK_FALSE(objects[0].name.empty());
}

// Prints the JSON of every report for a supported table, for reading by people.
TEST_CASE("Analysis report of a supported table", "[.][SliceAnalysisReport]")
{
    auto s = slice_and_analyse({table_mesh()}, analysis_config({{"enable_support", "1"}}));
    const Limits lim = limits_from_print(s->print);
    nlohmann::json j;
    j["summary"]     = s->analysis->summary();
    j["layers_0_3"]  = s->analysis->layer_page(0, 3);
    j["flow"]        = check_flow(*s->analysis, lim);
    j["speeds"]      = check_speeds(*s->analysis, lim);
    j["cooling"]     = check_cooling(*s->analysis, lim);
    j["supports"]    = check_supports(*s->analysis, &s->print, 3);
    j["overhangs"]   = check_overhangs(*s->analysis, &s->print, 3);
    j["unsupported"] = check_unsupported(s->print);
    j["first_layer"] = check_first_layer(s->print, s->analysis.get());
    j["seams"]       = check_seams(*s->analysis, 1., 3);
    j["thin"]        = check_thin_features(*s->analysis, &s->print);
    j["mm"]          = check_multi_material(*s->analysis, &s->print);
    j["section"]     = section_from_print(s->print, 11.);
    std::cout << j.dump(1) << std::endl;
}

// Prints the totals cross-check of a cube and of a two-filament cube, for the notes.
TEST_CASE("Analysis totals cross-check figures", "[.][SliceAnalysisCrossCheck]")
{
    const auto print_check = [](const char *name, Sliced &s) {
        const Summary &sum = s.analysis->summary();
        double role_time = 0., role_volume = 0., role_weight = 0., slicer_role_weight = 0., layer_time = 0., layer_volume = 0.;
        for (const RoleStats &r : sum.roles) {
            role_time += r.time_s;
            role_volume += r.volume_mm3;
            role_weight += r.weight_g;
            slicer_role_weight += r.slicer_weight_g;
        }
        for (const LayerRow &r : s.analysis->layers()) {
            layer_time += r.time_s;
            layer_volume += r.volume_mm3;
        }
        const PrintStatistics &ps = s.print.print_statistics();
        std::cout.precision(10);
        std::cout << "== " << name << " (moves " << sum.move_count << ", layers " << sum.layer_count << ")\n"
                  << "time: slicer " << sum.time_s << " s, sum of moves " << sum.moves_time_s << ", features " << role_time << " + travel "
                  << sum.travel_time_s << " + other " << sum.other_time_s << ", layers " << layer_time << " + start " << sum.start_gcode_time_s
                  << " + end " << sum.end_gcode_time_s << "\n"
                  << "first layer: slicer " << sum.first_layer_time_s << " s, layer 0 row " << s.analysis->layers().front().time_s << "\n"
                  << "volume: slicer model+support+tower " << sum.model_mm3 + sum.support_mm3 + sum.wipe_tower_mm3 << " mm3, moves "
                  << sum.moves_volume_mm3 << ", features " << role_volume << ", layers " << layer_volume << ", flush " << sum.flush_mm3 << "\n"
                  << "weight: ours " << sum.total_weight_g << " g, PrintStatistics " << ps.total_weight << ", per feature ours " << role_weight
                  << " slicer " << slicer_role_weight << "\n"
                  << "filament: ours " << sum.total_length_mm << " mm, PrintStatistics " << ps.total_used_filament << ", volume ours "
                  << sum.total_mm3 << " PrintStatistics " << ps.total_extruded_volume << "\n";
        for (const FilamentStats &f : sum.filaments)
            std::cout << "filament " << f.filament << ": slicer model " << f.model_mm3 << " support " << f.support_mm3 << " tower "
                      << f.wipe_tower_mm3 << " flush " << f.flush_mm3 << "; moves " << f.moves_mm3 << " (support " << f.moves_support_mm3
                      << ", tower " << f.moves_tower_mm3 << ")\n";
        std::cout << std::flush;
    };
    auto cube_print = slice_and_analyse({cube(20.)}, analysis_config());
    print_check("20 mm cube", *cube_print);
    auto two = slice_and_analyse({cube(20.)}, analysis_config({{"outer_wall_filament_id", "2"},
                                                              {"inner_wall_filament_id", "2"},
                                                              {"enable_prime_tower", "1"},
                                                              {"prime_tower_width", "35"},
                                                              {"wipe_tower_x", "120"},
                                                              {"wipe_tower_y", "120"},
                                                              {"printable_area", "0x0,200x0,200x200,0x200"}},
                                                             2));
    print_check("20 mm cube, walls on filament 2, prime tower", *two);
    auto table = slice_and_analyse({table_mesh()}, analysis_config({{"enable_support", "1"}}));
    print_check("table with support", *table);
}

// Run time of the analysis on a large print.
TEST_CASE("Analysis run time on a large print", "[.][SliceAnalysisPerf]")
{
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    // One 50 mm sphere at 0.2 mm with rectilinear infill (~1 M moves), and four at 0.1 mm with gyroid and support (tens of millions).
    const auto [copies, layer_height, pattern] = GENERATE(table<int, const char *, const char *>({{1, "0.2", "rectilinear"}, {4, "0.1", "gyroid"}}));
    std::vector<TriangleMesh> meshes;
    for (int i = 0; i < copies; ++i)
        meshes.push_back(mesh(TestMesh::sphere_50mm));
    auto t0 = clock::now();
    auto s = std::make_unique<Sliced>();
    init_print(std::move(meshes), s->print, s->model,
               analysis_config({{"layer_height", layer_height}, {"sparse_infill_density", "25%"}, {"sparse_infill_pattern", pattern},
                                {"enable_support", "1"}, {"seam_position", "random"}}));
    s->gcode = gcode(s->print, &s->result);
    auto t1 = clock::now();
    MoveAnalysis a(s->result, &s->print);
    auto t2 = clock::now();
    const Limits lim = limits_from_print(s->print);
    std::vector<std::pair<std::string, double>> times;
    auto timed = [&](const char *name, auto &&fn) { auto a0 = clock::now(); fn(); times.emplace_back(name, ms(a0, clock::now())); };
    timed("flow", [&] { check_flow(a, lim); });
    timed("speeds", [&] { check_speeds(a, lim); });
    timed("cooling", [&] { check_cooling(a, lim); });
    timed("seams", [&] { check_seams(a); });
    timed("multi_material", [&] { check_multi_material(a, &s->print); });
    timed("supports(moves+print)", [&] { check_supports(a, &s->print); });
    timed("overhangs(moves+bands)", [&] { check_overhangs(a, &s->print); });
    timed("unsupported(print)", [&] { check_unsupported(s->print); });
    timed("thin_features(moves+print)", [&] { check_thin_features(a, &s->print); });
    timed("first_layer", [&] { check_first_layer(s->print, &a); });
    timed("section_from_print", [&] { section_from_print(s->print, 25.); });
    timed("section_from_moves", [&] { section_from_moves(a, a.layer_at_z(25.), Box2()); });
    timed("json(summary+all layers)", [&] { nlohmann::json j = a.summary(); j["layers"] = a.layers(); (void) j.dump(); });
    std::cout << "moves: " << s->result.moves.size() << ", layers: " << a.layers().size() << ", slice+export: " << ms(t0, t1)
              << " ms, MoveAnalysis: " << ms(t1, t2) << " ms" << std::endl;
    for (const auto &t : times)
        std::cout << "  " << t.first << ": " << t.second << " ms" << std::endl;
    CHECK(s->result.moves.size() > 400000);
    std::cout << std::flush;
}
