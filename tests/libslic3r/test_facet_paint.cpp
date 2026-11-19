#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/FacetPaint.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::FacetPaint;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A 20 mm cube made of 12 large triangles, corner at the origin: a region boundary can only be
// sharp if the triangles are cut.
const TriangleMesh &cube20()
{
    static const TriangleMesh mesh(its_make_cube(20., 20., 20.));
    return mesh;
}

Result paint(const TriangleMesh &mesh, const Selector &selector, double resolution = 0.1, const Transforms &trafo = {},
             const TriangleSelector::TriangleSplittingData &existing = {}, EnforcerBlockerType state = EnforcerBlockerType::ENFORCER)
{
    Request request;
    request.selector   = selector;
    request.state      = state;
    request.resolution = resolution;
    Result result      = apply(mesh, existing, trafo, request);
    REQUIRE(result.status == Status::Ok);
    return result;
}

double enforced(const Result &r) { return r.after.area(EnforcerBlockerType::ENFORCER); }

Transform3d scale(const Vec3d &s) { return Transform3d(Eigen::Scaling(s)); }
Transform3d rotation(double deg, const Vec3d &axis) { return Transform3d(Eigen::AngleAxisd(Geometry::deg2rad(deg), axis.normalized())); }

} // namespace

TEST_CASE("A z band cuts through the large triangles of a cube", "[FacetPaint]")
{
    // Band 5.3..12.7 on four 20 mm side faces; the band edges are not on any subdivision grid line.
    const double resolution = GENERATE(0.3, 0.1, 0.05);
    CAPTURE(resolution);
    const Result r = paint(cube20(), Selector::z_band(5.3, 12.7), resolution);
    const double expected = 4 * 20. * (12.7 - 5.3);
    // Whole triangles would be off by hundreds of mm2. Cut ones are off by at most a strip a
    // quarter of the resolution wide along the 160 mm of boundary (0.3 mm: 12 mm2 = 2 %).
    const double tolerance = 0.25 * resolution * 160.;
    INFO("painted " << enforced(r) << " mm2, expected " << expected << ", leaves " << r.after.leaf_triangles);
    CHECK(r.before.leaf_triangles == 12);
    CHECK(r.after.leaf_triangles > 12);
    CHECK_THAT(enforced(r), WithinAbs(expected, tolerance));
    CHECK_THAT(r.after.total_area, WithinRel(6 * 400., 1e-9));
    const StateStats *s = r.after.find(EnforcerBlockerType::ENFORCER);
    REQUIRE(s != nullptr);
    CHECK_THAT(s->fraction, WithinAbs(expected / 2400., tolerance / 2400.));
    CHECK(s->bbox_world.min.z() > 5.3 - resolution);
    CHECK(s->bbox_world.max.z() < 12.7 + resolution);
    CHECK_THAT(s->bbox_world.min.x(), WithinAbs(0., 1e-6));
    CHECK_THAT(s->bbox_world.max.y(), WithinAbs(20., 1e-6));
}

TEST_CASE("Box, sphere, cylinder and half-space regions match their analytic areas on a cube", "[FacetPaint]")
{
    const double tol = 0.01;
    SECTION("box cutting the cube") {
        // y <= 7.3: face y = 0 whole, a 7.3 mm strip of four other faces.
        CHECK_THAT(enforced(paint(cube20(), Selector::box({-1., -1., -1.}, {21., 7.3, 21.}))), WithinRel(400. + 4 * 20. * 7.3, tol));
    }
    SECTION("box inside one triangle of the top face") {
        // No vertex or edge of the mesh is inside: only the separating-axis test finds the overlap.
        CHECK_THAT(enforced(paint(cube20(), Selector::box({3.3, 4.1, 19.}, {8.9, 15.7, 21.}), 0.05)), WithinRel(5.6 * 11.6, tol));
    }
    SECTION("oriented box") {
        // A box rotated 45 degrees about Z, centred on the top face: a square of side 6 on it.
        CHECK_THAT(enforced(paint(cube20(), Selector::oriented_box({10.3, 9.7, 20.}, rotation(45., Vec3d::UnitZ()).linear(), {3., 3., 1.}), 0.05)),
                   WithinRel(36., tol));
    }
    SECTION("sphere on the top face") {
        CHECK_THAT(enforced(paint(cube20(), Selector::sphere({10.3, 9.7, 20.}, 4.), 0.05)), WithinRel(PI * 16., tol));
    }
    SECTION("vertical cylinder through the cube") {
        CHECK_THAT(enforced(paint(cube20(), Selector::cylinder({10.2, 9.6, -5.}, Vec3d::UnitZ(), 30., 3.), 0.05)), WithinRel(2. * PI * 9., tol));
    }
    SECTION("capsule lying on the top face") {
        // Rectangle 9.4 x 4 plus a disk of radius 2.
        CHECK_THAT(enforced(paint(cube20(), Selector::capsule({5.3, 5.1, 20.}, {14.7, 5.1, 20.}, 2.), 0.05)), WithinRel(9.4 * 4. + PI * 4., tol));
    }
    SECTION("half-space above a plane") {
        CHECK_THAT(enforced(paint(cube20(), Selector::halfspace({0., 0., 7.7}, Vec3d::UnitZ()))), WithinRel(400. + 4 * 20. * 12.3, tol));
    }
    SECTION("oblique slab") {
        // 3 <= x + y <= 9 (normal not normalized: the planes stay where they are given).
        // On the bottom and top faces the band is the area between two diagonals: (81 - 9) / 2 each;
        // on the faces x = 0 and y = 0 it is a 6 mm wide vertical strip.
        CHECK_THAT(enforced(paint(cube20(), Selector::slab({1., 1., 0.}, 3., 9.))), WithinRel(2 * 36. + 2 * 6. * 20., tol));
    }
}

TEST_CASE("Normal selectors pick top faces and overhangs", "[FacetPaint]")
{
    SECTION("top face of a cube") {
        CHECK_THAT(enforced(paint(cube20(), Selector::normal(Vec3d::UnitZ(), 1.))), WithinRel(400., 1e-9));
    }
    SECTION("faces looking +X") {
        const Result r = paint(cube20(), Selector::normal(Vec3d::UnitX(), 10.));
        CHECK_THAT(enforced(r), WithinRel(400., 1e-9));
        CHECK_THAT(r.after.find(EnforcerBlockerType::ENFORCER)->bbox_world.min.x(), WithinAbs(20., 1e-6));
    }
    SECTION("overhang cap of a sphere") {
        // Facets within the threshold of straight down form a spherical cap of area 2 pi R^2 (1 - cos t).
        const double       threshold = GENERATE(30., 45., 60.);
        const TriangleMesh sphere    = make_sphere(10., 2. * PI / 360.);
        const double       expected  = 2. * PI * 100. * (1. - std::cos(Geometry::deg2rad(threshold)));
        const Result       r         = paint(sphere, Selector::overhang(threshold));
        INFO("threshold " << threshold << ": " << enforced(r) << " mm2 vs analytic " << expected);
        CHECK_THAT(enforced(r), WithinRel(expected, 0.02));
        CHECK(r.after.find(EnforcerBlockerType::ENFORCER)->bbox_world.max.z() < 0.);
    }
}

TEST_CASE("Transformations map selectors between world, object and mesh frames", "[FacetPaint]")
{
    SECTION("rotation: a world z band selects a mesh y band") {
        Transforms t;
        t.instance     = rotation(90., Vec3d::UnitX()); // mesh y -> world z
        const Result r = paint(cube20(), Selector::z_band(5.3, 12.7), 0.1, t);
        CHECK_THAT(enforced(r), WithinRel(4 * 20. * 7.4, 0.005));
        const BoundingBoxf3 &box = r.after.find(EnforcerBlockerType::ENFORCER)->bbox_object;
        CHECK(box.min.y() > 5.2);
        CHECK(box.max.y() < 12.8);
        CHECK_THAT(box.max.z(), WithinAbs(20., 1e-6));
        // The same band in the mesh frame selects mesh z.
        const Result m = paint(cube20(), Selector::z_band(5.3, 12.7, Frame::Mesh), 0.1, t);
        CHECK(m.after.find(EnforcerBlockerType::ENFORCER)->bbox_object.min.z() > 5.2);
    }
    SECTION("non-uniform scale: band and resolution in world millimetres") {
        Transforms t;
        t.volume       = scale({2., 1., 0.5}); // a 40 x 20 x 10 box
        const Result r = paint(cube20(), Selector::z_band(2.3, 6.1), 0.1, t);
        CHECK_THAT(r.after.total_area, WithinRel(2 * (40. * 20. + 40. * 10. + 20. * 10.), 1e-9));
        CHECK_THAT(enforced(r), WithinRel(120. * 3.8, 0.005));
        // In the object frame the band is the same, the instance is identity.
        CHECK(r.after.find(EnforcerBlockerType::ENFORCER)->bbox_object.min.z() > 2.2);
    }
    SECTION("non-uniform scale after a rotation shears normals") {
        // Rotating 45 degrees about Y, then stretching z by 2, tips the former top face to 63.4
        // degrees from +Z. Transforming normals by the matrix itself instead of its inverse
        // transpose would give 26.6 degrees.
        Transforms t;
        t.volume   = rotation(45., Vec3d::UnitY());
        t.instance = scale({1., 1., 2.});
        CHECK(enforced(paint(cube20(), Selector::normal(Vec3d::UnitZ(), 60.), 0.1, t)) == 0.);
        const Result r = paint(cube20(), Selector::normal(Vec3d::UnitZ(), 65.), 0.1, t);
        // Two faces of the cube qualify: the former top and x = 0 faces.
        CHECK(r.after.find(EnforcerBlockerType::ENFORCER)->triangles == 4);
    }
    SECTION("mirror") {
        Transforms t;
        t.instance = scale({-1., 1., 1.}); // world x = -mesh x
        // The mesh face x = 0 looks +X after mirroring.
        const Result n = paint(cube20(), Selector::normal(Vec3d::UnitX(), 1.), 0.1, t);
        CHECK_THAT(enforced(n), WithinRel(400., 1e-9));
        CHECK_THAT(n.after.find(EnforcerBlockerType::ENFORCER)->bbox_object.max.x(), WithinAbs(0., 1e-6));
        // World x >= -5.3 is mesh x <= 5.3.
        const Result b = paint(cube20(), Selector::box({-5.3, -1., -1.}, {1., 21., 21.}), 0.1, t);
        CHECK_THAT(enforced(b), WithinRel(400. + 4 * 20. * 5.3, 0.005));
        CHECK(b.after.find(EnforcerBlockerType::ENFORCER)->bbox_object.max.x() < 5.4);
    }
    SECTION("translation of the instance") {
        Transforms t;
        t.instance.translation() = Vec3d(100., 50., 3.);
        CHECK_THAT(enforced(paint(cube20(), Selector::z_band(8.3, 15.7), 0.1, t)), WithinRel(4 * 20. * 7.4, 0.005));
    }
}

TEST_CASE("Smart fill stops at edges sharper than its angle", "[FacetPaint]")
{
    // A 72-sided cylinder: neighbouring side facets differ by 5 degrees, the caps by 90.
    const int          sides = 72;
    // its_make_cylinder() takes ceil(2 pi / fa) steps; keep the quotient just under 72.
    const TriangleMesh cyl(its_make_cylinder(10., 20., 2. * PI / (sides - 0.01)));
    REQUIRE(cyl.its.indices.size() == size_t(4 * sides));
    const double       quad = 2. * 10. * std::sin(PI / sides) * 20.;
    const Seed         seed = Seed::along_ray({30., 0.3, 10.}, {-1., 0., 0.});

    CHECK_THAT(enforced(paint(cyl, Selector::connected(seed, 3.))), WithinRel(quad, 1e-6));
    CHECK_THAT(enforced(paint(cyl, Selector::connected(seed, 30.))), WithinRel(sides * quad, 1e-6));
    const Result all = paint(cyl, Selector::connected(seed, 95.));
    CHECK_THAT(enforced(all), WithinRel(all.after.total_area, 1e-9));

    SECTION("seed by point and by facet") {
        CHECK_THAT(enforced(paint(cube20(), Selector::connected(Seed::at_point({10., 10., 25.}), 30.))), WithinRel(400., 1e-9));
        CHECK_THAT(enforced(paint(cube20(), Selector::connected(Seed::on_facet(0), 89.))), WithinRel(400., 1e-9));
        CHECK_THAT(enforced(paint(cube20(), Selector::connected(Seed::on_facet(0), 90.))), WithinRel(2400., 1e-9));
    }
}

TEST_CASE("Bucket fill repaints one connected region of a state", "[FacetPaint]")
{
    const TriangleMesh &mesh = cube20();
    // Two enforced bands that do not touch: top (z >= 15.3) and bottom (z <= 4.1).
    const Result top    = paint(mesh, Selector::z_band(15.3, 21.));
    const Result both   = paint(mesh, Selector::z_band(-1., 4.1), 0.1, {}, top.data);
    const double top_a  = 400. + 80. * 4.7;
    const double bot_a  = 400. + 80. * 4.1;
    REQUIRE_THAT(enforced(both), WithinRel(top_a + bot_a, 0.005));
    const double top_exact = enforced(top);

    SECTION("fill the top region") {
        const Result r = paint(mesh, Selector::same_state(Seed::at_point({10., 10., 30.})), 0.1, {}, both.data, EnforcerBlockerType::BLOCKER);
        CHECK_THAT(r.after.area(EnforcerBlockerType::BLOCKER), WithinRel(top_exact, 1e-9));
        CHECK_THAT(r.after.area(EnforcerBlockerType::ENFORCER), WithinRel(enforced(both) - top_exact, 1e-9));
    }
    SECTION("fill the unpainted region between them") {
        const Result r = paint(mesh, Selector::same_state(Seed::at_point({10., -5., 10.})), 0.1, {}, both.data, EnforcerBlockerType::BLOCKER);
        CHECK_THAT(r.after.area(EnforcerBlockerType::BLOCKER), WithinRel(2400. - enforced(both), 1e-9));
        CHECK_THAT(enforced(r), WithinRel(enforced(both), 1e-9));
    }
    SECTION("bucket fill restricted by a region") {
        const Result r = paint(mesh, Selector::same_state(Seed::at_point({10., 10., 30.})) && Selector::halfspace({10.3, 0., 0.}, Vec3d::UnitX()), 0.1, {},
                               both.data, EnforcerBlockerType::BLOCKER);
        // Top face right of x = 10.3, the strip of face x = 20, and the right parts of faces y = 0 and y = 20.
        CHECK_THAT(r.after.area(EnforcerBlockerType::BLOCKER), WithinRel(9.7 * 20. + 20. * 4.7 + 2 * 9.7 * 4.7, 0.01));
    }
    SECTION("bucket fill is only allowed at the top level") {
        CHECK_THROWS_AS(paint(mesh, Selector::same_state(Seed::on_facet(0)) || Selector::all()), InvalidArgument);
    }
}

TEST_CASE("A stroke paints like a brush", "[FacetPaint]")
{
    SECTION("polyline of rays on the top face") {
        const std::vector<Seed> path = {Seed::along_ray({5.3, 5.1, 30.}, -Vec3d::UnitZ()), Seed::along_ray({10., 5.1, 30.}, -Vec3d::UnitZ()),
                                        Seed::along_ray({14.7, 5.1, 30.}, -Vec3d::UnitZ())};
        CHECK_THAT(enforced(paint(cube20(), Selector::stroke(path, 2.), 0.05)), WithinRel(9.4 * 4. + PI * 4., 0.01));
    }
    SECTION("single point: a disk") {
        CHECK_THAT(enforced(paint(cube20(), Selector::stroke({Seed::at_point({10.3, 9.7, 20.5})}, 3.), 0.05)), WithinRel(PI * 9., 0.01));
    }
    SECTION("a thin wall's far side is only painted when asked") {
        // 1 mm thick wall; a 2 mm ball on its front face reaches the back face in a disk of radius sqrt(3).
        const TriangleMesh wall(its_make_cube(20., 1., 20.));
        const std::vector<Seed> path = {Seed::at_point({10.1, -1., 10.2})};
        CHECK_THAT(enforced(paint(wall, Selector::stroke(path, 2., true), 0.05)), WithinRel(PI * 4., 0.01));
        CHECK_THAT(enforced(paint(wall, Selector::stroke(path, 2., false), 0.05)), WithinRel(PI * 4. + PI * 3., 0.01));
    }
}

TEST_CASE("Selectors combine with and, or and not", "[FacetPaint]")
{
    const Selector box    = Selector::box({3.3, 4.1, 19.}, {8.9, 15.7, 21.});
    const Selector sphere = Selector::sphere({6.1, 9.9, 20.}, 2.);
    CHECK_THAT(enforced(paint(cube20(), box && !sphere, 0.05)), WithinRel(5.6 * 11.6 - PI * 4., 0.01));
    CHECK_THAT(enforced(paint(cube20(), Selector::sphere({4., 4., 20.}, 2.) || Selector::sphere({15., 15., 20.}, 3.), 0.05)),
               WithinRel(PI * 13., 0.01));
    // Whole-facet and region leaves together: the top face only, within a band that also crosses the sides.
    CHECK_THAT(enforced(paint(cube20(), Selector::normal(Vec3d::UnitZ(), 1.) && Selector::box({-1., -1., -1.}, {21., 7.3, 21.}))),
               WithinRel(20. * 7.3, 0.005));
    // Everything but a band.
    CHECK_THAT(enforced(paint(cube20(), !Selector::z_band(5.3, 12.7))), WithinRel(2400. - 80. * 7.4, 0.005));
    // Overhangs inside a box ("supports only on overhangs steeper than 50 degrees in this region").
    const TriangleMesh sphere_mesh = make_sphere(10., 2. * PI / 180.);
    const Result       r           = paint(sphere_mesh, Selector::overhang(50.) && Selector::halfspace(Vec3d::Zero(), Vec3d::UnitX()));
    CHECK_THAT(enforced(r), WithinRel(0.5 * 2. * PI * 100. * (1. - std::cos(Geometry::deg2rad(50.))), 0.02));
}

TEST_CASE("Erase, replace and clear change only what they are asked to", "[FacetPaint]")
{
    const TriangleMesh &mesh = cube20();
    const Result        all  = paint(mesh, Selector::all());
    REQUIRE_THAT(enforced(all), WithinRel(2400., 1e-9));
    CHECK(all.after.leaf_triangles == 12);

    // Erase a band.
    const Result erased = paint(mesh, Selector::z_band(5.3, 12.7), 0.1, {}, all.data, EnforcerBlockerType::NONE);
    CHECK_THAT(enforced(erased), WithinRel(2400. - 80. * 7.4, 0.005));

    // Replace enforcer by blocker in a box that covers painted and unpainted surface.
    Request replace;
    replace.selector   = Selector::box({-1., -1., -1.}, {21., 7.3, 21.});
    replace.state      = EnforcerBlockerType::BLOCKER;
    replace.only_where = EnforcerBlockerType::ENFORCER;
    replace.resolution = 0.1;
    const Result r     = apply(mesh, erased.data, {}, replace);
    const double box_a = 400. + 4 * 20. * 7.3;
    // The box covers the band only on the faces x = 0, x = 20 (7.3 x 7.4 each) and y = 0 (20 x 7.4).
    const double box_band = 2 * 7.3 * 7.4 + 20. * 7.4;
    CHECK_THAT(r.after.area(EnforcerBlockerType::BLOCKER), WithinRel(box_a - box_band, 0.01));
    CHECK_THAT(r.after.area(EnforcerBlockerType::BLOCKER) + enforced(r), WithinRel(enforced(erased), 1e-6));

    // Clear everything.
    Request clear;
    clear.selector    = Selector::none();
    clear.clear_first = true;
    const Result c    = apply(mesh, r.data, {}, clear);
    CHECK(c.changed);
    CHECK(c.after.states.empty());
    CHECK(c.data.triangles_to_split.empty());
    CHECK(c.after.live_triangles == 12);

    // Erasing what was painted merges every subdivision away again.
    const Result band  = paint(mesh, Selector::z_band(5.3, 12.7));
    const Result unset = paint(mesh, Selector::z_band(5.3, 12.7), 0.1, {}, band.data, EnforcerBlockerType::NONE);
    CHECK(unset.after.states.empty());
    CHECK(unset.after.live_triangles == 12);
    CHECK(unset.data.triangles_to_split.empty());
}

TEST_CASE("Whole-facet mode paints original facets by their centroid", "[FacetPaint]")
{
    Request request;
    request.selector = Selector::z_band(5.3, 12.7);
    request.mode     = Mode::WholeFacets;
    const Result r   = apply(cube20(), {}, {}, request);
    // One triangle of each side face has its centroid at z = 20/3.
    CHECK_THAT(enforced(r), WithinRel(800., 1e-9));
    CHECK(r.after.leaf_triangles == 12);
}

TEST_CASE("Painted data survives serialization and the 3MF string form", "[FacetPaint]")
{
    const TriangleMesh &mesh = cube20();
    const Result        r    = paint(mesh, Selector::sphere({10.3, 9.7, 20.}, 4.) || Selector::z_band(2.3, 3.9), 0.05);
    const Result        c    = paint(mesh, Selector::box({-1., -1., 10.1}, {21., 21., 21.}), 0.1, {}, r.data, EnforcerBlockerType::Extruder5);

    // TriangleSelector round trip.
    TriangleSelector selector(mesh);
    selector.deserialize(c.data, false);
    CHECK(selector.serialize() == c.data);
    const Stats s = statistics(mesh, c.data, {});
    CHECK_THAT(s.area(EnforcerBlockerType::Extruder5), WithinRel(c.after.area(EnforcerBlockerType::Extruder5), 1e-12));
    CHECK(s.leaf_triangles == c.after.leaf_triangles);

    // The per-triangle hex strings written to and read from 3MF.
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *source = object->add_volume(TriangleMesh(mesh), ModelVolumeType::MODEL_PART, false);
    ModelVolume *target = object->add_volume(TriangleMesh(mesh), ModelVolumeType::MODEL_PART, false);
    source->mmu_segmentation_facets.set_data(TriangleSelector::TriangleSplittingData(c.data));
    for (int i = 0; i < int(mesh.its.indices.size()); ++i)
        if (std::string str = source->mmu_segmentation_facets.get_triangle_as_string(i); ! str.empty())
            target->mmu_segmentation_facets.set_triangle_from_string(i, str);
    target->mmu_segmentation_facets.shrink_to_fit();
    CHECK(target->mmu_segmentation_facets.get_data() == c.data);
    CHECK(target->mmu_segmentation_facets.has_facets(*target, EnforcerBlockerType::Extruder5));
    // What the slicer reads.
    CHECK_FALSE(target->mmu_segmentation_facets.get_facets_strict(*target, EnforcerBlockerType::Extruder5).indices.empty());
}

TEST_CASE("Painting the same selection twice changes nothing", "[FacetPaint]")
{
    const Selector selector = GENERATE(Selector::z_band(5.3, 12.7), Selector::sphere({10.3, 9.7, 20.}, 4.),
                                       Selector::box({3.3, 4.1, 19.}, {8.9, 15.7, 21.}) && !Selector::sphere({6.1, 9.9, 20.}, 2.));
    const Result first  = paint(cube20(), selector, 0.1);
    const Result second = paint(cube20(), selector, 0.1, {}, first.data);
    CHECK(first.changed);
    CHECK_FALSE(second.changed);
    CHECK(second.data == first.data);
    CHECK(second.triangles_added == 0);
}

TEST_CASE("The triangle limit coarsens or refuses a too fine resolution", "[FacetPaint]")
{
    Request request;
    request.selector          = Selector::sphere({10.3, 9.7, 20.}, 6.) || Selector::z_band(5.3, 12.7);
    request.resolution        = 0.001;
    request.max_new_triangles = 5000;

    const Result coarse = apply(cube20(), {}, {}, request);
    CHECK(coarse.status == Status::Ok);
    CHECK(coarse.resolution_used > request.resolution);
    CHECK(coarse.triangles_added <= request.max_new_triangles);
    CHECK_THAT(enforced(coarse), WithinRel(PI * 36. + 80. * 7.4, 0.05));

    request.coarsen_to_fit = false;
    const Result refused   = apply(cube20(), {}, {}, request);
    CHECK(refused.status == Status::TooFine);
    CHECK_FALSE(refused.changed);
    CHECK(refused.data.triangles_to_split.empty());
}

TEST_CASE("Requests are validated", "[FacetPaint]")
{
    Request request;
    request.state     = EnforcerBlockerType::Extruder3;
    request.max_state = EnforcerBlockerType::BLOCKER;
    CHECK_THROWS_AS(apply(cube20(), {}, {}, request), InvalidArgument);
    CHECK_THROWS_AS(Selector::sphere(Vec3d::Zero(), -1.), InvalidArgument);
    CHECK_THROWS_AS(Selector::normal(Vec3d::Zero(), 10.), InvalidArgument);
    CHECK_THROWS_AS(paint(cube20(), Selector::facets({12})), InvalidArgument);
    CHECK_THROWS_AS(paint(cube20(), Selector::connected(Seed::along_ray({50., 50., 50.}, Vec3d::UnitZ()))), InvalidArgument);
    CHECK_THAT(enforced(paint(cube20(), Selector::facets({0, 1}))), WithinRel(400., 1e-9));
}

TEST_CASE("Painting a model part commits to the channel's annotation", "[FacetPaint]")
{
    Model          model;
    ModelObject   *object   = model.add_object();
    ModelVolume   *part     = object->add_volume(TriangleMesh(cube20()), ModelVolumeType::MODEL_PART, false);
    ModelInstance *instance = object->add_instance();
    instance->set_offset(Vec3d(100., 100., 0.));
    part->set_offset(Vec3d(-10., -10., 0.));

    Request request;
    request.selector = Selector::z_band(5.3, 12.7);
    request.state    = EnforcerBlockerType::ENFORCER;
    const auto timestamp = part->supported_facets.timestamp();
    const Result r       = apply_to_volume(*part, instance->get_matrix(), Channel::Support, request);
    CHECK(r.changed);
    CHECK(part->supported_facets.timestamp() != timestamp);
    CHECK(part->supported_facets.has_facets(*part, EnforcerBlockerType::ENFORCER));
    CHECK(part->seam_facets.empty());
    CHECK_THAT(r.after.find(EnforcerBlockerType::ENFORCER)->bbox_world.min.x(), WithinAbs(90., 1e-6));

    // The same request again leaves the annotation alone.
    const auto stamp2 = part->supported_facets.timestamp();
    CHECK_FALSE(apply_to_volume(*part, instance->get_matrix(), Channel::Support, request).changed);
    CHECK(part->supported_facets.timestamp() == stamp2);

    // Channel limits.
    request.state = EnforcerBlockerType::Extruder3;
    CHECK_THROWS_AS(apply_to_volume(*part, instance->get_matrix(), Channel::Seam, request), InvalidArgument);
    CHECK_NOTHROW(apply_to_volume(*part, instance->get_matrix(), Channel::Color, request));
    CHECK(part->mmu_segmentation_facets.has_facets(*part, EnforcerBlockerType::Extruder3));
    request.state = EnforcerBlockerType::FUZZY_SKIN;
    CHECK(apply_to_volume(*part, instance->get_matrix(), Channel::FuzzySkin, request).changed);
    CHECK(part->fuzzy_skin_facets.has_facets(*part, EnforcerBlockerType::FUZZY_SKIN));

    ModelVolume *modifier = object->add_volume(TriangleMesh(cube20()), ModelVolumeType::PARAMETER_MODIFIER, false);
    CHECK_THROWS_AS(apply_to_volume(*modifier, instance->get_matrix(), Channel::Support, request), InvalidArgument);
}

// Prints computed against analytic areas. Hidden from the default run; run with
//   libslic3r_tests "[FacetPaintReport]"
TEST_CASE("Painted areas compared with analytic values", "[FacetPaint][FacetPaintReport][.]")
{
    struct Case { const char *name; Selector selector; double expected; };
    const std::vector<Case> cases = {
        {"cube z band 5.3..12.7", Selector::z_band(5.3, 12.7), 592.},
        {"cube box y<=7.3", Selector::box({-1., -1., -1.}, {21., 7.3, 21.}), 984.},
        {"cube box inside one triangle", Selector::box({3.3, 4.1, 19.}, {8.9, 15.7, 21.}), 5.6 * 11.6},
        {"cube sphere r=4 on top", Selector::sphere({10.3, 9.7, 20.}, 4.), PI * 16.},
        {"cube cylinder r=3 through", Selector::cylinder({10.2, 9.6, -5.}, Vec3d::UnitZ(), 30., 3.), 2. * PI * 9.},
        {"cube capsule on top", Selector::capsule({5.3, 5.1, 20.}, {14.7, 5.1, 20.}, 2.), 9.4 * 4. + PI * 4.},
        {"cube slab 3<=x+y<=9", Selector::slab({1., 1., 0.}, 3., 9.), 312.},
        {"cube box && !sphere", Selector::box({3.3, 4.1, 19.}, {8.9, 15.7, 21.}) && !Selector::sphere({6.1, 9.9, 20.}, 2.), 5.6 * 11.6 - PI * 4.},
    };
    std::cout << "\n";
    for (double resolution : {0.3, 0.1, 0.05})
        for (const Case &c : cases) {
            const Result r = paint(cube20(), c.selector, resolution);
            std::cout << c.name << " @" << resolution << ": " << enforced(r) << " vs " << c.expected << " mm2, error "
                      << 100. * (enforced(r) - c.expected) / c.expected << " %, " << r.after.leaf_triangles << " leaves\n";
        }
    for (double threshold : {30., 45., 60.}) {
        const TriangleMesh sphere   = make_sphere(10., 2. * PI / 360.);
        const double       expected = 2. * PI * 100. * (1. - std::cos(Geometry::deg2rad(threshold)));
        const Result       r        = paint(sphere, Selector::overhang(threshold));
        std::cout << "sphere R10 overhang " << threshold << ": " << enforced(r) << " vs " << expected << " mm2, error "
                  << 100. * (enforced(r) - expected) / expected << " % (facet discretization)\n";
    }
}

// Timing and memory on a large mesh. Hidden from the default run; run with
//   libslic3r_tests "[FacetPaintPerf]"
TEST_CASE("Painting a large mesh stays fast and bounded", "[FacetPaint][FacetPaintPerf][.]")
{
    using clock          = std::chrono::steady_clock;
    const int  divisions = GENERATE(700, 1000, 1400);
    const auto t0        = clock::now();
    // A 50 mm sphere with about 4 pi^2 / fa^2 facets.
    const TriangleMesh mesh = make_sphere(50., 2. * PI / divisions);
    const double       t_mesh = std::chrono::duration<double>(clock::now() - t0).count();
    std::cout << "\nmesh: " << mesh.its.indices.size() << " facets, built in " << t_mesh << " s\n";

    struct Case { const char *name; Selector selector; double resolution; };
    const std::vector<Case> cases = {
        {"z band 0.1mm", Selector::z_band(-10.3, 20.7), 0.1},
        {"z band 0.05mm", Selector::z_band(-10.3, 20.7), 0.05},
        {"box 0.3mm", Selector::box({-20., -20., -60.}, {20., 20., 0.}), 0.3},
        {"box 0.1mm", Selector::box({-20., -20., -60.}, {20., 20., 0.}), 0.1},
        {"box 0.05mm", Selector::box({-20., -20., -60.}, {20., 20., 0.}), 0.05},
        {"sphere 0.1mm", Selector::sphere({0., 0., 50.}, 20.), 0.1},
        {"overhang 45", Selector::overhang(45.), 0.1},
        {"overhang 45 and box", Selector::overhang(45.) && Selector::box({-20., -20., -60.}, {20., 20., 0.}), 0.1},
        {"smart fill 30", Selector::connected(Seed::at_point({0., 0., 60.}), 30.), 0.1},
        {"stroke r=2", Selector::stroke({Seed::at_point({-20., 0., 60.}), Seed::at_point({0., 0., 60.}), Seed::at_point({20., 0., 60.})}, 2.), 0.05},
    };
    for (const Case &c : cases) {
        Request request;
        request.selector   = c.selector;
        request.resolution = c.resolution;
        request.max_new_triangles = 3000000;
        const auto   t1 = clock::now();
        const Result r  = apply(mesh, {}, {}, request);
        const double dt = std::chrono::duration<double>(clock::now() - t1).count();
        TriangleSelector selector(mesh);
        selector.deserialize(r.data, false);
        std::cout << c.name << ": " << dt << " s, painted " << enforced(r) << " mm2, +" << r.triangles_added << " triangles, selector memory "
                  << selector.memory_usage() / (1024 * 1024) << " MiB, data " << (r.data.bitstream.size() / 8 + r.data.triangles_to_split.size() * 8) / 1024
                  << " KiB, resolution " << r.resolution_used << "\n";
        CHECK(r.status == Status::Ok);
        CHECK(enforced(r) > 0.);
    }
}
