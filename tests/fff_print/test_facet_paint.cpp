#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/FacetPaint.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Paint a world-frame selection onto the only part of the only object, as an agent would.
FacetPaint::Result paint_object(Model &model, FacetPaint::Channel channel, const FacetPaint::Selector &selector, EnforcerBlockerType state,
                                FacetPaint::Mode mode = FacetPaint::Mode::Precise)
{
    ModelObject *object = model.objects.front();
    REQUIRE(object->volumes.size() == 1);
    FacetPaint::Request request;
    request.selector   = selector;
    request.state      = state;
    request.mode       = mode;
    request.resolution = 0.1;
    const FacetPaint::Result r = FacetPaint::apply_to_volume(*object->volumes.front(), object->instances.front()->get_matrix(), channel, request);
    REQUIRE(r.status == FacetPaint::Status::Ok);
    REQUIRE(r.changed);
    return r;
}

// Layers (by slice z) where the painting segmentation assigns anything to the second filament.
std::vector<double> layers_with_second_filament(PrintObject &object)
{
    const std::vector<std::vector<ExPolygons>> segmentation = multi_material_segmentation_by_painting(object, [] {});
    REQUIRE(segmentation.size() == object.layers().size());
    std::vector<double> out;
    for (size_t i = 0; i < segmentation.size(); ++i)
        if (segmentation[i].size() > 1 && ! segmentation[i][1].empty())
            out.push_back(object.layers()[i]->slice_z);
    return out;
}

} // namespace

TEST_CASE("A precisely painted colour band selects the second filament only in its layers", "[FacetPaint]")
{
    // A 10 mm cube is 12 triangles; a band 3.2..6.6 mm cuts through every side triangle, so whole
    // triangles could not produce it (their centroids are at z = 3.33 and 6.67). 0.2 mm layers: slice z = 0.1, 0.3, ... 9.9.
    DynamicPrintConfig config = multifilament_config(2, {
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"gcode_comments", "1"}, // As init_print sets it, so the second apply only changes the model.
    });
    Print print;
    Model model;
    init_print({cube(10)}, print, model, config);
    const double z0 = model.objects.front()->instance_bounding_box(0).min.z();
    REQUIRE(std::abs(z0) < EPSILON);

    const FacetPaint::Mode mode = GENERATE(FacetPaint::Mode::Precise, FacetPaint::Mode::WholeFacets);
    CAPTURE(int(mode));
    const FacetPaint::Result painted = paint_object(model, FacetPaint::Channel::Color, FacetPaint::Selector::z_band(3.2, 6.6), EnforcerBlockerType::Extruder2, mode);
    print.apply(model, config);
    PrintObject &object = *print.get_object(0);
    object.slice();
    REQUIRE(object.layers().size() == 50);

    const std::vector<double> layers = layers_with_second_filament(object);
    REQUIRE_FALSE(layers.empty());
    INFO("filament 2 from slice z " << layers.front() << " to " << layers.back() << " in " << layers.size() << " layers");
    if (mode == FacetPaint::Mode::Precise) {
        // Within a strip a quarter of the resolution wide along the 80 mm of boundary.
        CHECK_THAT(painted.after.area(EnforcerBlockerType::Extruder2), Catch::Matchers::WithinAbs(40. * 3.4, 0.25 * 0.1 * 80.));
        // Exactly the 17 layers whose slice plane is inside the band (3.3 .. 6.5).
        CHECK(layers.size() == 17);
        CHECK(layers.front() > 3.2);
        CHECK(layers.back() < 6.6);
    } else {
        // The side triangles with their centroid at z = 3.33 reach down to the bed.
        CHECK(layers.front() < 1.);
        CHECK(layers.back() > 9.);
    }
}

TEST_CASE("Support enforcers painted on half an overhang generate support under that half only", "[FacetPaint]")
{
    // A 40 x 40 x 2 mm cap on an 8 x 8 mm stem 12 mm up. Manual support: only enforcers make support.
    TriangleMesh mesh = make_cube(8, 8, 14);
    mesh.translate(16, 16, 0);
    TriangleMesh cap = make_cube(40, 40, 2);
    cap.translate(0, 0, 12);
    mesh.merge(cap);

    const DynamicPrintConfig config = [] {
        DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
        c.set_deserialize_strict({{"enable_support", "1"}, {"support_type", "normal(manual)"}, {"layer_height", "0.2"}, {"gcode_comments", "1"}});
        return c;
    }();

    auto support_x_range = [](const std::string &gcode) {
        double      min_x = std::numeric_limits<double>::max(), max_x = std::numeric_limits<double>::lowest();
        GCodeReader reader;
        reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            if (line.extruding(self) && std::string_view(line.comment()).find("support") != std::string_view::npos) {
                for (double x : {double(self.x()), double(line.new_X(self))}) {
                    min_x = std::min(min_x, x);
                    max_x = std::max(max_x, x);
                }
            }
        });
        return std::make_pair(min_x, max_x);
    };

    SECTION("no paint: no support") {
        Print print;
        Model model;
        init_print({TriangleMesh(mesh)}, print, model, config);
        const auto [min_x, max_x] = support_x_range(Test::gcode(print));
        CHECK(min_x > max_x);
    }
    // Paint enforcers by a world-frame selector, slice, and return the x range of support extrusions.
    auto support_with = [&](auto &&make_selector, double &center, double &area) {
        Print print;
        Model model;
        init_print({TriangleMesh(mesh)}, print, model, config);
        center = model.objects.front()->instance_bounding_box(0).center().x();
        area   = paint_object(model, FacetPaint::Channel::Support, make_selector(center), EnforcerBlockerType::ENFORCER).after.area(EnforcerBlockerType::ENFORCER);
        print.apply(model, config);
        return support_x_range(Test::gcode(print));
    };
    double center = 0., area = 0.;

    SECTION("enforcers on the overhang right of the centre") {
        const auto range = support_with([](double c) { return FacetPaint::Selector::overhang(45.) && FacetPaint::Selector::halfspace({c + 4., 0., 0.}, Vec3d::UnitX()); },
                                        center, area);
        // The cap's underside right of x = center + 4: 16 x 40 mm.
        CHECK_THAT(area, Catch::Matchers::WithinRel(16. * 40., 0.01));
        INFO("support x " << range.first << " .. " << range.second << ", painted from x = " << center + 4.);
        REQUIRE(range.first < range.second);
        // The support spreads a few mm past the painted edge (it also overhangs the cap's outer
        // edge by about 2 mm), but stays out of the left half, which reaches to center - 20.
        CHECK(range.first > center - 2.);
        CHECK(range.second > center + 18.);
    }
    SECTION("enforcers on the whole overhang") {
        const auto range = support_with([](double) { return FacetPaint::Selector::overhang(45.); }, center, area);
        // The cap's whole underside (the stem is a separate shell inside it) and the stem's bottom face on the bed.
        CHECK_THAT(area, Catch::Matchers::WithinRel(40. * 40. + 8. * 8., 0.01));
        INFO("support x " << range.first << " .. " << range.second);
        CHECK(range.first < center - 18.);
        CHECK(range.second > center + 18.);
    }
}
