#include "McpCamera.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

const double DEG_TO_RAD = 3.14159265358979323846 / 180.;

double round6(double value)
{
    const double rounded = std::round(value * 1e6) / 1e6;
    return rounded == 0. ? 0. : rounded;
}

json vec_json(const Vec3d& v)
{
    return json::array({round6(v.x()), round6(v.y()), round6(v.z())});
}

json matrix_json(const Transform3d& transform)
{
    json rows = json::array();
    for (int r = 0; r < 4; ++r) {
        json row = json::array();
        for (int c = 0; c < 4; ++c)
            row.push_back(round6(transform.matrix()(r, c)));
        rows.push_back(std::move(row));
    }
    return rows;
}

bool read_vec(const json& value, Vec3d& out)
{
    if (!value.is_array() || value.size() != 3 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number())
        return false;
    out = Vec3d(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
    return true;
}

} // namespace

std::vector<Vec3d> box_corners(const Vec3d& min, const Vec3d& max)
{
    std::vector<Vec3d> corners;
    corners.reserve(8);
    for (int i = 0; i < 8; ++i)
        corners.emplace_back((i & 1) ? max.x() : min.x(), (i & 2) ? max.y() : min.y(), (i & 4) ? max.z() : min.z());
    return corners;
}

void CameraView::update()
{
    const double az = azimuth_deg * DEG_TO_RAD;
    const double el = std::max(-90., std::min(90., elevation_deg)) * DEG_TO_RAD;
    // Unit vector from the target towards the eye.
    const Vec3d back(std::sin(az) * std::cos(el), -std::cos(az) * std::cos(el), std::sin(el));
    eye = target + distance * back;

    const Vec3d forward = -back;
    // "Up" is world +Z; looking straight down or up, it is the horizontal direction the camera
    // would tilt towards, which keeps the picture continuous as the elevation reaches +-90.
    Vec3d up_hint = Vec3d::UnitZ();
    if (std::abs(forward.z()) > 0.9999)
        up_hint = (forward.z() < 0. ? 1. : -1.) * Vec3d(-std::sin(az), std::cos(az), 0.);
    const Vec3d side = forward.cross(up_hint).normalized();
    up = side.cross(forward);

    Matrix4d v = Matrix4d::Identity();
    v.block<1, 3>(0, 0) = side.transpose();
    v.block<1, 3>(1, 0) = up.transpose();
    v.block<1, 3>(2, 0) = -forward.transpose();
    v(0, 3) = -side.dot(eye);
    v(1, 3) = -up.dot(eye);
    v(2, 3) = forward.dot(eye);
    view.matrix() = v;

    const double aspect = static_cast<double>(std::max(1, width)) / static_cast<double>(std::max(1, height));
    Matrix4d     p = Matrix4d::Zero();
    if (perspective) {
        const double f = 1. / std::tan(0.5 * fov_y_deg * DEG_TO_RAD);
        p(0, 0) = f / aspect;
        p(1, 1) = f;
        p(2, 2) = (far_z + near_z) / (near_z - far_z);
        p(2, 3) = 2. * far_z * near_z / (near_z - far_z);
        p(3, 2) = -1.;
    } else {
        const double half_h = 0.5 * ortho_height;
        const double half_w = half_h * aspect;
        p(0, 0) = 1. / half_w;
        p(1, 1) = 1. / half_h;
        p(2, 2) = -2. / (far_z - near_z);
        p(2, 3) = -(far_z + near_z) / (far_z - near_z);
        p(3, 3) = 1.;
    }
    projection.matrix() = p;
}

void CameraView::fit(const std::vector<Vec3d>& points, double zoom, double fill)
{
    if (points.empty())
        return;
    update();
    const double aspect = static_cast<double>(std::max(1, width)) / static_cast<double>(std::max(1, height));
    const Matrix3d rotation = view.matrix().block<3, 3>(0, 0);
    zoom = std::max(1e-3, zoom);
    if (perspective) {
        const double tan_v = std::tan(0.5 * fov_y_deg * DEG_TO_RAD) * fill;
        const double tan_h = tan_v * aspect;
        double       needed = 1.;
        for (const Vec3d& point : points) {
            const Vec3d c = rotation * (point - target); // x right, y up, z towards the eye
            // The point is at depth (distance - c.z) in front of the eye.
            needed = std::max(needed, std::abs(c.x()) / tan_h + c.z());
            needed = std::max(needed, std::abs(c.y()) / tan_v + c.z());
        }
        distance = needed / zoom;
    } else {
        double half_h = 0.5;
        for (const Vec3d& point : points) {
            const Vec3d c = rotation * (point - target);
            half_h = std::max(half_h, std::abs(c.y()) / fill);
            half_h = std::max(half_h, std::abs(c.x()) / (fill * aspect));
        }
        ortho_height = 2. * half_h / zoom;
    }
    update();
}

void CameraView::set_depth_range(const std::vector<Vec3d>& points)
{
    update();
    const Matrix3d rotation = view.matrix().block<3, 3>(0, 0);
    double         min_c = std::numeric_limits<double>::max();    // c.z: towards the eye, relative to the target
    double         max_c = std::numeric_limits<double>::lowest();
    for (const Vec3d& point : points) {
        const double c = (rotation * (point - target)).z();
        min_c = std::min(min_c, c);
        max_c = std::max(max_c, c);
    }
    if (points.empty()) {
        min_c = -100.;
        max_c = 100.;
    }
    const double pad = std::max(1., 0.05 * (max_c - min_c));
    if (!perspective && distance < max_c + 2. * pad)
        distance = max_c + 2. * pad; // an orthographic eye may sit anywhere along its axis
    // Depth in front of the eye is (distance - c).
    near_z = std::max({distance - max_c - pad, 0.002 * (distance - min_c + pad), 0.05});
    far_z = std::max(distance - min_c + pad, near_z + 1.);
    update();
}

bool CameraView::project(const Vec3d& world, Vec2d& pixel) const
{
    const Eigen::Vector4d clip = projection.matrix() * view.matrix() * Eigen::Vector4d(world.x(), world.y(), world.z(), 1.);
    if (clip.w() <= 1e-9)
        return false;
    const double ndc_x = clip.x() / clip.w();
    const double ndc_y = clip.y() / clip.w();
    pixel = Vec2d((ndc_x * 0.5 + 0.5) * width, (1. - (ndc_y * 0.5 + 0.5)) * height);
    return true;
}

void CameraView::ray(const Vec2d& pixel, Vec3d& origin, Vec3d& direction) const
{
    const double ndc_x = 2. * pixel.x() / width - 1.;
    const double ndc_y = 1. - 2. * pixel.y() / height;
    const Matrix4d inverse = (projection.matrix() * view.matrix()).inverse();
    Eigen::Vector4d near_point = inverse * Eigen::Vector4d(ndc_x, ndc_y, -1., 1.);
    Eigen::Vector4d far_point = inverse * Eigen::Vector4d(ndc_x, ndc_y, 1., 1.);
    near_point /= near_point.w();
    far_point /= far_point.w();
    origin = near_point.head<3>();
    direction = (far_point.head<3>() - near_point.head<3>()).normalized();
}

json CameraView::to_json() const
{
    json out = json::object();
    out["projection"] = perspective ? "perspective" : "orthographic";
    out["azimuth_deg"] = round6(azimuth_deg);
    out["elevation_deg"] = round6(elevation_deg);
    out["target"] = vec_json(target);
    out["position"] = vec_json(eye);
    out["up"] = vec_json(up);
    out["distance"] = round6(distance);
    if (perspective)
        out["fov_y_deg"] = round6(fov_y_deg);
    else
        out["ortho_height"] = round6(ortho_height);
    out["near"] = round6(near_z);
    out["far"] = round6(far_z);
    out["width"] = width;
    out["height"] = height;
    out["view_matrix"] = matrix_json(view);
    out["projection_matrix"] = matrix_json(projection);
    return out;
}

bool CameraView::from_json(const json& value, CameraView& out)
{
    if (!value.is_object())
        return false;
    auto number = [&value](const char* key, double& target_value) {
        auto it = value.find(key);
        if (it == value.end() || !it->is_number())
            return false;
        target_value = it->get<double>();
        return true;
    };
    CameraView view;
    const std::string projection = value.value("projection", std::string());
    if (projection != "perspective" && projection != "orthographic")
        return false;
    view.perspective = projection == "perspective";
    double width = 0., height = 0.;
    if (!number("azimuth_deg", view.azimuth_deg) || !number("elevation_deg", view.elevation_deg) || !number("distance", view.distance) ||
        !number("near", view.near_z) || !number("far", view.far_z) || !number("width", width) || !number("height", height))
        return false;
    if (!value.contains("target") || !read_vec(value["target"], view.target))
        return false;
    if (view.perspective ? !number("fov_y_deg", view.fov_y_deg) : !number("ortho_height", view.ortho_height))
        return false;
    if (width < 1. || height < 1. || view.distance <= 0. || view.far_z <= view.near_z)
        return false;
    view.width = static_cast<int>(width);
    view.height = static_cast<int>(height);
    view.update();
    out = view;
    return true;
}

}}} // namespace Slic3r::GUI::Mcp
