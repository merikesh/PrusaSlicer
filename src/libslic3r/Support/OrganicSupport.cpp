#include "OrganicSupport.hpp"

#include "AABBTreeLines.hpp"
#include "ClipperUtils.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "MutablePolygon.hpp"
#include "TriangleMeshSlicer.hpp"

#include "Support/SupportCommon.hpp"

#include <cassert>

#include <tbb/parallel_for.h>

#define TREE_SUPPORT_ORGANIC_NUDGE_NEW 1

#ifndef TREE_SUPPORT_ORGANIC_NUDGE_NEW
    // Old version using OpenVDB, works but it is extremely slow for complex meshes.
    #include "OpenVDBUtilsLegacy.hpp"
    #include <openvdb/tools/VolumeToSpheres.h>
#endif // TREE_SUPPORT_ORGANIC_NUDGE_NEW

namespace Slic3r
{

namespace FFFTreeSupport
{

// Test whether two circles, each on its own plane in 3D intersect.
// Circles are considered intersecting, if the lowest point on one circle is below the other circle's plane.
// Assumption: The two planes are oriented the same way.
static bool circles_intersect(
    const Vec3d &p1, const Vec3d &n1, const double r1, 
    const Vec3d &p2, const Vec3d &n2, const double r2)
{
    assert(n1.dot(n2) >= 0);

    const Vec3d z = n1.cross(n2);
    const Vec3d dir1 = z.cross(n1);
    const Vec3d lowest_point1 = p1 + dir1 * (r1 / dir1.norm());
    assert(n2.dot(p1) >= n2.dot(lowest_point1));
    if (n2.dot(lowest_point1) <= 0)
        return true;
    const Vec3d dir2 = z.cross(n2);
    const Vec3d lowest_point2 = p2 + dir2 * (r2 / dir2.norm());
    assert(n1.dot(p2) >= n1.dot(lowest_point2));
    return n1.dot(lowest_point2) <= 0;
}

template<bool flip_normals>
void triangulate_fan(indexed_triangle_set &its, int ifan, int ibegin, int iend)
{
    // at least 3 vertices, increasing order.
    assert(ibegin + 3 <= iend);
    assert(ibegin >= 0 && iend <= its.vertices.size());
    assert(ifan >= 0 && ifan < its.vertices.size());
    int num_faces = iend - ibegin;
    its.indices.reserve(its.indices.size() + num_faces * 3);
    for (int v = ibegin, u = iend - 1; v < iend; u = v ++) {
        if (flip_normals)
            its.indices.push_back({ ifan, u, v });
        else
            its.indices.push_back({ ifan, v, u });
    }
}

static void triangulate_strip(indexed_triangle_set &its, int ibegin1, int iend1, int ibegin2, int iend2)
{
    // at least 3 vertices, increasing order.
    assert(ibegin1 + 3 <= iend1);
    assert(ibegin1 >= 0 && iend1 <= its.vertices.size());
    assert(ibegin2 + 3 <= iend2);
    assert(ibegin2 >= 0 && iend2 <= its.vertices.size());
    int n1 = iend1 - ibegin1;
    int n2 = iend2 - ibegin2;
    its.indices.reserve(its.indices.size() + (n1 + n2) * 3);

    // For the first vertex of 1st strip, find the closest vertex on the 2nd strip.
    int istart2 = ibegin2;
    {
        const Vec3f &p1    = its.vertices[ibegin1];
        auto         d2min = std::numeric_limits<float>::max();
        for (int i = ibegin2; i < iend2; ++ i) {
            const Vec3f &p2 = its.vertices[i];
            const float d2  = (p2 - p1).squaredNorm();
            if (d2 < d2min) {
                d2min = d2;
                istart2 = i;
            }
        }
    }

    // Now triangulate the strip zig-zag fashion taking always the shortest connection if possible.
    for (int u = ibegin1, v = istart2; n1 > 0 || n2 > 0;) {
        bool take_first;
        int u2, v2;
        auto update_u2 = [&u2, u, ibegin1, iend1]() {
            u2 = u;
            if (++ u2 == iend1)
                u2 = ibegin1;
        };
        auto update_v2 = [&v2, v, ibegin2, iend2]() {
            v2 = v;
            if (++ v2 == iend2)
                v2 = ibegin2;
        };
        if (n1 == 0) {
            take_first = false;
            update_v2();
        } else if (n2 == 0) {
            take_first = true;
            update_u2();
        } else {
            update_u2();
            update_v2();
            float l1 = (its.vertices[u2] - its.vertices[v]).squaredNorm();
            float l2 = (its.vertices[v2] - its.vertices[u]).squaredNorm();
            take_first = l1 < l2;
        }
        if (take_first) {
            its.indices.push_back({ u, u2, v });
            -- n1;
            u = u2;
        } else {
            its.indices.push_back({ u, v2, v });
            -- n2;
            v = v2;
        }
    }
}

// Discretize 3D circle, append to output vector, return ranges of indices of the points added.
static std::pair<int, int> discretize_circle(const Vec3f &center, const Vec3f &normal, const float radius, const float eps, std::vector<Vec3f> &pts)
{
    // Calculate discretization step and number of steps.
    float angle_step = 2. * acos(1. - eps / radius);
    auto  nsteps     = int(ceil(2 * M_PI / angle_step));
    angle_step = 2 * M_PI / nsteps;

    // Prepare coordinate system for the circle plane.
    Vec3f x = normal.cross(Vec3f(0.f, -1.f, 0.f)).normalized();
    Vec3f y = normal.cross(x).normalized();
    assert(std::abs(x.cross(y).dot(normal) - 1.f) < EPSILON);

    // Discretize the circle.
    int begin = int(pts.size());
    pts.reserve(pts.size() + nsteps);
    float angle = 0;
    x *= radius;
    y *= radius;
    for (int i = 0; i < nsteps; ++ i) {
        pts.emplace_back(center + x * cos(angle) + y * sin(angle));
        angle += angle_step;
    }
    return { begin, int(pts.size()) };
}

// Returns Z span of the generated mesh.
static std::pair<float, float> extrude_branch(
    const std::vector<const SupportElement*>    &path,
    const TreeSupportSettings                   &config,
    const SlicingParameters                     &slicing_params,
    const std::vector<SupportElements>          &move_bounds,
    indexed_triangle_set                        &result)
{
    Vec3d p1, p2, p3;
    Vec3d v1, v2;
    Vec3d nprev;
    Vec3d ncurrent;
    assert(path.size() >= 2);
    static constexpr const float eps = 0.015f;
    std::pair<int, int> prev_strip;

//    char fname[2048];
//    static int irun = 0;

    float zmin, zmax;

    for (size_t ipath = 1; ipath < path.size(); ++ ipath) {
        const SupportElement &prev    = *path[ipath - 1];
        const SupportElement &current = *path[ipath];
        assert(prev.state.layer_idx + 1 == current.state.layer_idx);
        p1 = to_3d(unscaled<double>(prev   .state.result_on_layer), layer_z(slicing_params, config, prev   .state.layer_idx));
        p2 = to_3d(unscaled<double>(current.state.result_on_layer), layer_z(slicing_params, config, current.state.layer_idx));
        v1 = (p2 - p1).normalized();
        if (ipath == 1) {
            nprev = v1;
            // Extrude the bottom half sphere.
            float radius     = unscaled<float>(getRadius(config, prev.state));
            float angle_step = 2. * acos(1. - eps / radius);
            auto  nsteps     = int(ceil(M_PI / (2. * angle_step)));
            angle_step       = M_PI / (2. * nsteps);
            int   ifan       = int(result.vertices.size());
            result.vertices.emplace_back((p1 - nprev * radius).cast<float>());
            zmin = result.vertices.back().z();
            float angle = angle_step;
            for (int i = 1; i < nsteps; ++ i, angle += angle_step) {
                std::pair<int, int> strip = discretize_circle((p1 - nprev * radius * cos(angle)).cast<float>(), nprev.cast<float>(), radius * sin(angle), eps, result.vertices);
                if (i == 1)
                    triangulate_fan<false>(result, ifan, strip.first, strip.second);
                else
                    triangulate_strip(result, prev_strip.first, prev_strip.second, strip.first, strip.second);
//                sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++ irun);
//                its_write_obj(result, fname);
                prev_strip = strip;
            }
        }
        if (ipath + 1 == path.size()) {
            // End of the tube.
            ncurrent = v1;
            // Extrude the top half sphere.
            float radius = unscaled<float>(getRadius(config, current.state));
            float angle_step = 2. * acos(1. - eps / radius);
            auto  nsteps = int(ceil(M_PI / (2. * angle_step)));
            angle_step = M_PI / (2. * nsteps);
            auto angle = float(M_PI / 2.);
            for (int i = 0; i < nsteps; ++ i, angle -= angle_step) {
                std::pair<int, int> strip = discretize_circle((p2 + ncurrent * radius * cos(angle)).cast<float>(), ncurrent.cast<float>(), radius * sin(angle), eps, result.vertices);
                triangulate_strip(result, prev_strip.first, prev_strip.second, strip.first, strip.second);
//                sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++ irun);
//                its_write_obj(result, fname);
                prev_strip = strip;
            }
            int ifan = int(result.vertices.size());
            result.vertices.emplace_back((p2 + ncurrent * radius).cast<float>());
            zmax = result.vertices.back().z();
            triangulate_fan<true>(result, ifan, prev_strip.first, prev_strip.second);
//            sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++ irun);
//            its_write_obj(result, fname);
        } else {
            const SupportElement &next = *path[ipath + 1];
            assert(current.state.layer_idx + 1 == next.state.layer_idx);
            p3 = to_3d(unscaled<double>(next.state.result_on_layer), layer_z(slicing_params, config, next.state.layer_idx));
            v2 = (p3 - p2).normalized();
            ncurrent = (v1 + v2).normalized();
            float radius = unscaled<float>(getRadius(config, current.state));
            std::pair<int, int> strip = discretize_circle(p2.cast<float>(), ncurrent.cast<float>(), radius, eps, result.vertices);
            triangulate_strip(result, prev_strip.first, prev_strip.second, strip.first, strip.second);
            prev_strip = strip;
//            sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++irun);
//            its_write_obj(result, fname);
        }
#if 0
        if (circles_intersect(p1, nprev, settings.getRadius(prev), p2, ncurrent, settings.getRadius(current))) {
            // Cannot connect previous and current slice using a simple zig-zag triangulation,
            // because the two circles intersect.

        } else {
            // Continue with chaining.

        }
#endif
    }

    return std::make_pair(zmin, zmax);
}

#ifdef TREE_SUPPORT_ORGANIC_NUDGE_NEW

// New version using per layer AABB trees of lines for nudging spheres away from an object.
static void organic_smooth_branches_avoid_collisions(
    const PrintObject                                   &print_object,
    const TreeModelVolumes                              &volumes,
    const TreeSupportSettings                           &config,
    std::vector<SupportElements>                        &move_bounds,
    const std::vector<std::pair<SupportElement*, int>>  &elements_with_link_down,
    const std::vector<size_t>                           &linear_data_layers,
    std::function<void()>                                throw_on_cancel)
{
    struct LayerCollisionCache {
        coord_t          min_element_radius{ std::numeric_limits<coord_t>::max() };
        bool             min_element_radius_known() const { return this->min_element_radius != std::numeric_limits<coord_t>::max(); }
        coord_t          collision_radius{ 0 };
        std::vector<Linef> lines;
        AABBTreeIndirect::Tree<2, double> aabbtree_lines;
        bool             empty() const { return this->lines.empty(); }
    };
    std::vector<LayerCollisionCache> layer_collision_cache;
    layer_collision_cache.reserve(1024);
    const SlicingParameters &slicing_params = print_object.slicing_parameters();
    for (const std::pair<SupportElement*, int>& element : elements_with_link_down) {
        LayerIndex layer_idx = element.first->state.layer_idx;
        if (size_t num_layers = layer_idx + 1; num_layers > layer_collision_cache.size()) {
            if (num_layers > layer_collision_cache.capacity())
                reserve_power_of_2(layer_collision_cache, num_layers);
            layer_collision_cache.resize(num_layers, {});
        }
        auto& l = layer_collision_cache[layer_idx];
        l.min_element_radius = std::min(l.min_element_radius, getRadius(config, element.first->state));
    }

    throw_on_cancel();

    for (LayerIndex layer_idx = 0; layer_idx < LayerIndex(layer_collision_cache.size()); ++layer_idx)
        if (LayerCollisionCache& l = layer_collision_cache[layer_idx]; !l.min_element_radius_known())
            l.min_element_radius = 0;
        else {
            //FIXME
            l.min_element_radius = 0;
            std::optional<std::pair<coord_t, std::reference_wrapper<const Polygons>>> res = volumes.get_collision_lower_bound_area(layer_idx, l.min_element_radius);
            assert(res.has_value());
            l.collision_radius = res->first;
            Lines alines = to_lines(res->second.get());
            l.lines.reserve(alines.size());
            for (const Line &line : alines)
                l.lines.push_back({ unscaled<double>(line.a), unscaled<double>(line.b) });
            l.aabbtree_lines = AABBTreeLines::build_aabb_tree_over_indexed_lines(l.lines);
            throw_on_cancel();
        }

    struct CollisionSphere {
        const SupportElement& element;
        int                   element_below_id;
        const bool            locked;
        float                 radius;
        // Current position, when nudged away from the collision.
        Vec3f                 position;
        // Previous position, for Laplacian smoothing.
        Vec3f                 prev_position;
        // 
        Vec3f                 last_collision;
        double                last_collision_depth;
        // Minimum Z for which the sphere collision will be evaluated.
        // Limited by the minimum sloping angle and by the bottom of the tree.
        float                 min_z{ -std::numeric_limits<float>::max() };
        // Maximum Z for which the sphere collision will be evaluated.
        // Limited by the minimum sloping angle and by the tip of the current branch.
        float                 max_z{ std::numeric_limits<float>::max() };
        uint32_t              layer_begin;
        uint32_t              layer_end;
    };

    std::vector<CollisionSphere> collision_spheres;
    collision_spheres.reserve(elements_with_link_down.size());
    for (const std::pair<SupportElement*, int> &element_with_link : elements_with_link_down) {
        const SupportElement &element   = *element_with_link.first;
        const int             link_down = element_with_link.second;
        collision_spheres.push_back({
            element,
            link_down,
            // locked
            element.parents.empty() || (link_down == -1 && element.state.layer_idx > 0),
            unscaled<float>(getRadius(config, element.state)),
            // 3D position
            to_3d(unscaled<float>(element.state.result_on_layer), float(layer_z(slicing_params, config, element.state.layer_idx)))
        });
        // Update min_z coordinate to min_z of the tree below.
        CollisionSphere &collision_sphere = collision_spheres.back();
        if (link_down != -1) {
            const size_t offset_below = linear_data_layers[element.state.layer_idx - 1];
            collision_sphere.min_z = collision_spheres[offset_below + link_down].min_z;
        } else
            collision_sphere.min_z = collision_sphere.position.z();
    }
    // Update max_z by propagating max_z from the tips of the branches.
    for (int collision_sphere_id = int(collision_spheres.size()) - 1; collision_sphere_id >= 0; -- collision_sphere_id) {
        CollisionSphere &collision_sphere = collision_spheres[collision_sphere_id];
        if (collision_sphere.element.parents.empty())
            // Tip
            collision_sphere.max_z = collision_sphere.position.z();
        else {
            // Below tip
            const size_t offset_above = linear_data_layers[collision_sphere.element.state.layer_idx + 1];
            for (auto iparent : collision_sphere.element.parents) {
                float parent_z = collision_spheres[offset_above + iparent].max_z;
//                    collision_sphere.max_z = collision_sphere.max_z == std::numeric_limits<float>::max() ? parent_z : std::max(collision_sphere.max_z, parent_z);
                collision_sphere.max_z = std::min(collision_sphere.max_z, parent_z);
            }
        }
    }
    // Update min_z / max_z to limit the search Z span of a given sphere for collision detection.
    for (CollisionSphere &collision_sphere : collision_spheres) {
        //FIXME limit the collision span by the tree slope.
        collision_sphere.min_z = std::max(collision_sphere.min_z, collision_sphere.position.z() - collision_sphere.radius);
        collision_sphere.max_z = std::min(collision_sphere.max_z, collision_sphere.position.z() + collision_sphere.radius);
        collision_sphere.layer_begin = std::min(collision_sphere.element.state.layer_idx, layer_idx_ceil(slicing_params, config, collision_sphere.min_z));
        assert(collision_sphere.layer_begin < layer_collision_cache.size());
        collision_sphere.layer_end   = std::min(LayerIndex(layer_collision_cache.size()), std::max(collision_sphere.element.state.layer_idx, layer_idx_floor(slicing_params, config, collision_sphere.max_z)) + 1);
    }

    throw_on_cancel();

    static constexpr const double collision_extra_gap = 0.1;
    static constexpr const double max_nudge_collision_avoidance = 0.5;
    static constexpr const double max_nudge_smoothing = 0.2;
    static constexpr const size_t num_iter = 100; // 1000;
    for (size_t iter = 0; iter < num_iter; ++ iter) {
        // Back up prev position before Laplacian smoothing.
        for (CollisionSphere &collision_sphere : collision_spheres)
            collision_sphere.prev_position = collision_sphere.position;
        std::atomic<size_t> num_moved{ 0 };
        tbb::parallel_for(tbb::blocked_range<size_t>(0, collision_spheres.size()),
            [&collision_spheres, &layer_collision_cache, &slicing_params, &config, &move_bounds, &linear_data_layers, &num_moved, &throw_on_cancel](const tbb::blocked_range<size_t> range) {
            for (size_t collision_sphere_id = range.begin(); collision_sphere_id < range.end(); ++ collision_sphere_id)
                if (CollisionSphere &collision_sphere = collision_spheres[collision_sphere_id]; ! collision_sphere.locked) {
                    // Calculate collision of multiple 2D layers against a collision sphere.
                    collision_sphere.last_collision_depth = - std::numeric_limits<double>::max();
                    for (uint32_t layer_id = collision_sphere.layer_begin; layer_id != collision_sphere.layer_end; ++ layer_id) {
                        double dz = (layer_id - collision_sphere.element.state.layer_idx) * slicing_params.layer_height;
                        if (double r2 = sqr(collision_sphere.radius) - sqr(dz); r2 > 0) {
                            if (const LayerCollisionCache &layer_collision_cache_item = layer_collision_cache[layer_id]; ! layer_collision_cache_item.empty()) {
                                size_t hit_idx_out;
                                Vec2d  hit_point_out;
                                if (double dist = sqrt(AABBTreeLines::squared_distance_to_indexed_lines(
                                    layer_collision_cache_item.lines, layer_collision_cache_item.aabbtree_lines, Vec2d(to_2d(collision_sphere.position).cast<double>()),
                                    hit_idx_out, hit_point_out, r2)); dist >= 0.) {
                                    double collision_depth = sqrt(r2) - dist;
                                    if (collision_depth > collision_sphere.last_collision_depth) {
                                        collision_sphere.last_collision_depth = collision_depth;
                                        collision_sphere.last_collision = to_3d(hit_point_out.cast<float>(), float(layer_z(slicing_params, config, layer_id)));
                                    }
                                }
                            }
                        }
                    }
                    if (collision_sphere.last_collision_depth > 0) {
                        // Collision detected to be removed.
                        // Nudge the circle center away from the collision.
                        if (collision_sphere.last_collision_depth > EPSILON)
                            // a little bit of hysteresis to detect end of 
                            ++ num_moved;
                        // Shift by maximum 2mm.
                        double nudge_dist = std::min(std::max(0., collision_sphere.last_collision_depth + collision_extra_gap), max_nudge_collision_avoidance);
                        Vec2d nudge_vector = (to_2d(collision_sphere.position) - to_2d(collision_sphere.last_collision)).cast<double>().normalized() * nudge_dist;
                        collision_sphere.position.head<2>() += (nudge_vector * nudge_dist).cast<float>();
                    }
                    // Laplacian smoothing
                    Vec2d avg{ 0, 0 };
                    const SupportElements &above = move_bounds[collision_sphere.element.state.layer_idx + 1];
                    const size_t           offset_above = linear_data_layers[collision_sphere.element.state.layer_idx + 1];
                    double weight = 0.;
                    for (auto iparent : collision_sphere.element.parents) {
                        double w = collision_sphere.radius;
                        avg += w * to_2d(collision_spheres[offset_above + iparent].prev_position.cast<double>());
                        weight += w;
                    }
                    if (collision_sphere.element_below_id != -1) {
                        const size_t offset_below = linear_data_layers[collision_sphere.element.state.layer_idx - 1];
                        const double w = weight; //  config.getRadius(move_bounds[element.state.layer_idx - 1][below].state);
                        avg += w * to_2d(collision_spheres[offset_below + collision_sphere.element_below_id].prev_position.cast<double>());
                        weight += w;
                    }
                    avg /= weight;
                    static constexpr const double smoothing_factor = 0.5;
                    Vec2d old_pos = to_2d(collision_sphere.position).cast<double>();
                    Vec2d new_pos = (1. - smoothing_factor) * old_pos + smoothing_factor * avg;
                    Vec2d shift   = new_pos - old_pos;
                    double nudge_dist_max = shift.norm();
                    // Shift by maximum 1mm, less than the collision avoidance factor.
                    double nudge_dist = std::min(std::max(0., nudge_dist_max), max_nudge_smoothing);
                    collision_sphere.position.head<2>() += (shift.normalized() * nudge_dist).cast<float>();

                    throw_on_cancel();
                }
        });
#if 0
        std::vector<double> stat;
        for (CollisionSphere& collision_sphere : collision_spheres)
            if (!collision_sphere.locked)
                stat.emplace_back(collision_sphere.last_collision_depth);
        std::sort(stat.begin(), stat.end());
        printf("iteration: %d, moved: %d, collision depth: min %lf, max %lf, median %lf\n", int(iter), int(num_moved), stat.front(), stat.back(), stat[stat.size() / 2]);
#endif
        if (num_moved == 0)
            break;
    }

    for (size_t i = 0; i < collision_spheres.size(); ++ i)
        elements_with_link_down[i].first->state.result_on_layer = scaled<coord_t>(to_2d(collision_spheres[i].position));
}
#else // TREE_SUPPORT_ORGANIC_NUDGE_NEW
// Old version using OpenVDB, works but it is extremely slow for complex meshes.
static void organic_smooth_branches_avoid_collisions(
    const PrintObject                                   &print_object,
    const TreeModelVolumes                              &volumes,
    const TreeSupportSettings                           &config,
    std::vector<SupportElements>                        &move_bounds,
    const std::vector<std::pair<SupportElement*, int>>  &elements_with_link_down,
    const std::vector<size_t>                           &linear_data_layers,
    std::function<void()>                                throw_on_cancel)
{
    TriangleMesh mesh = print_object.model_object()->raw_mesh();
    mesh.transform(print_object.trafo_centered());
    double scale = 10.;
    openvdb::FloatGrid::Ptr grid = mesh_to_grid(mesh.its, openvdb::math::Transform{}, scale, 0., 0.);
    std::unique_ptr<openvdb::tools::ClosestSurfacePoint<openvdb::FloatGrid>> closest_surface_point = openvdb::tools::ClosestSurfacePoint<openvdb::FloatGrid>::create(*grid);
    std::vector<openvdb::Vec3R> pts, prev, projections;
    std::vector<float> distances;
    for (const std::pair<SupportElement*, int>& element : elements_with_link_down) {
        Vec3d pt = to_3d(unscaled<double>(element.first->state.result_on_layer), layer_z(print_object.slicing_parameters(), config, element.first->state.layer_idx)) * scale;
        pts.push_back({ pt.x(), pt.y(), pt.z() });
    }

    const double collision_extra_gap = 1. * scale;
    const double max_nudge_collision_avoidance = 2. * scale;
    const double max_nudge_smoothing = 1. * scale;

    static constexpr const size_t num_iter = 100; // 1000;
    for (size_t iter = 0; iter < num_iter; ++ iter) {
        prev = pts;
        projections = pts;
        distances.assign(pts.size(), std::numeric_limits<float>::max());
        closest_surface_point->searchAndReplace(projections, distances);
        size_t num_moved = 0;
        for (size_t i = 0; i < projections.size(); ++ i) {
            const SupportElement &element = *elements_with_link_down[i].first;
            const int            below    = elements_with_link_down[i].second;
            const bool           locked   = (below == -1 && element.state.layer_idx > 0) || element.state.locked();
            if (! locked && pts[i] != projections[i]) {
                // Nudge the circle center away from the collision.
                Vec3d v{ projections[i].x() - pts[i].x(), projections[i].y() - pts[i].y(), projections[i].z() - pts[i].z() };
                double depth = v.norm();
                assert(std::abs(distances[i] - depth) < EPSILON);
                double radius = unscaled<double>(config.getRadius(element.state)) * scale;
                if (depth < radius) {
                    // Collision detected to be removed.
                    ++ num_moved;
                    double dxy = sqrt(sqr(radius) - sqr(v.z()));
                    double nudge_dist_max = dxy - std::hypot(v.x(), v.y())
                        //FIXME 1mm gap
                        + collision_extra_gap;
                    // Shift by maximum 2mm.
                    double nudge_dist = std::min(std::max(0., nudge_dist_max), max_nudge_collision_avoidance);
                    Vec2d nudge_v = to_2d(v).normalized() * (- nudge_dist);
                    pts[i].x() += nudge_v.x();
                    pts[i].y() += nudge_v.y();
                }
            }
            // Laplacian smoothing
            if (! locked && ! element.parents.empty()) {
                Vec2d avg{ 0, 0 };
                const SupportElements &above = move_bounds[element.state.layer_idx + 1];
                const size_t           offset_above = linear_data_layers[element.state.layer_idx + 1];
                double weight = 0.;
                for (auto iparent : element.parents) {
                    double w = config.getRadius(above[iparent].state);
                    avg.x() += w * prev[offset_above + iparent].x();
                    avg.y() += w * prev[offset_above + iparent].y();
                    weight += w;
                }
                size_t cnt = element.parents.size();
                if (below != -1) {
                    const size_t offset_below = linear_data_layers[element.state.layer_idx - 1];
                    const double w = weight; //  config.getRadius(move_bounds[element.state.layer_idx - 1][below].state);
                    avg.x() += w * prev[offset_below + below].x();
                    avg.y() += w * prev[offset_below + below].y();
                    ++ cnt;
                    weight += w;
                }
                //avg /= double(cnt);
                avg /= weight;
                static constexpr const double smoothing_factor = 0.5;
                Vec2d old_pos{ pts[i].x(), pts[i].y() };
                Vec2d new_pos = (1. - smoothing_factor) * old_pos + smoothing_factor * avg;
                Vec2d shift = new_pos - old_pos;
                double nudge_dist_max = shift.norm();
                // Shift by maximum 1mm, less than the collision avoidance factor.
                double nudge_dist = std::min(std::max(0., nudge_dist_max), max_nudge_smoothing);
                Vec2d nudge_v = shift.normalized() * nudge_dist;
                pts[i].x() += nudge_v.x();
                pts[i].y() += nudge_v.y();
            }
        }
//            printf("iteration: %d, moved: %d\n", int(iter), int(num_moved));
        if (num_moved == 0)
            break;
    }

    for (size_t i = 0; i < projections.size(); ++ i) {
        elements_with_link_down[i].first->state.result_on_layer.x() = scaled<coord_t>(pts[i].x()) / scale;
        elements_with_link_down[i].first->state.result_on_layer.y() = scaled<coord_t>(pts[i].y()) / scale;
    }
}
#endif // TREE_SUPPORT_ORGANIC_NUDGE_NEW

// Organic specific: Smooth branches and produce one cummulative mesh to be sliced.
void organic_draw_branches(
    PrintObject                     &print_object,
    TreeModelVolumes                &volumes, 
    const TreeSupportSettings       &config,
    std::vector<SupportElements>    &move_bounds,

    // I/O:
    SupportGeneratorLayersPtr       &bottom_contacts,
    SupportGeneratorLayersPtr       &top_contacts,
    InterfacePlacer                 &interface_placer,

    // Output:
    SupportGeneratorLayersPtr       &intermediate_layers,
    SupportGeneratorLayerStorage    &layer_storage,

    std::function<void()>            throw_on_cancel)
{
    // All SupportElements are put into a layer independent storage to improve parallelization.
    std::vector<std::pair<SupportElement*, int>> elements_with_link_down;
    std::vector<size_t>                          linear_data_layers;
    {
        std::vector<std::pair<SupportElement*, int>> map_downwards_old;
        std::vector<std::pair<SupportElement*, int>> map_downwards_new;
        linear_data_layers.emplace_back(0);
        for (LayerIndex layer_idx = 0; layer_idx < LayerIndex(move_bounds.size()); ++ layer_idx) {
            SupportElements *layer_above = layer_idx + 1 < move_bounds.size() ? &move_bounds[layer_idx + 1] : nullptr;
            map_downwards_new.clear();
            std::sort(map_downwards_old.begin(), map_downwards_old.end(), [](auto& l, auto& r) { return l.first < r.first;  });
            SupportElements &layer = move_bounds[layer_idx];
            for (size_t elem_idx = 0; elem_idx < layer.size(); ++ elem_idx) {
                SupportElement &elem = layer[elem_idx];
                int child = -1;
                if (layer_idx > 0) {
                    auto it = std::lower_bound(map_downwards_old.begin(), map_downwards_old.end(), &elem, [](auto& l, const SupportElement* r) { return l.first < r; });
                    if (it != map_downwards_old.end() && it->first == &elem) {
                        child = it->second;
                        // Only one link points to a node above from below.
                        assert(!(++it != map_downwards_old.end() && it->first == &elem));
                    }
#ifndef NDEBUG
                    {
                        const SupportElement *pchild = child == -1 ? nullptr : &move_bounds[layer_idx - 1][child];
                        assert(pchild ? pchild->state.result_on_layer_is_set() : elem.state.target_height > layer_idx);
                    }
#endif // NDEBUG
                }
                for (int32_t parent_idx : elem.parents) {
                    SupportElement &parent = (*layer_above)[parent_idx];
                    if (parent.state.result_on_layer_is_set())
                        map_downwards_new.emplace_back(&parent, elem_idx);
                }

                elements_with_link_down.push_back({ &elem, int(child) });
            }
            std::swap(map_downwards_old, map_downwards_new);
            linear_data_layers.emplace_back(elements_with_link_down.size());
        }
    }

    throw_on_cancel();

    organic_smooth_branches_avoid_collisions(print_object, volumes, config, move_bounds, elements_with_link_down, linear_data_layers, throw_on_cancel);

    // Reduce memory footprint. After this point only finalize_interface_and_support_areas() will use volumes and from that only collisions with zero radius will be used.
    volumes.clear_all_but_object_collision();

    // Unmark all nodes.
    for (SupportElements &elements : move_bounds)
        for (SupportElement &element : elements)
            element.state.marked = false;

    // Traverse all nodes, generate tubes.
    // Traversal stack with nodes and their current parent

    struct Branch {
        std::vector<const SupportElement*> path;
        bool                               has_root{ false };
        bool                               has_tip { false };
    };

    struct Slice {
        Polygons polygons;
        Polygons bottom_contacts;
        size_t   num_branches{ 0 };
    };

    struct Tree {
        std::vector<Branch>  branches;

        std::vector<Slice>   slices;
        LayerIndex           first_layer_id{ -1 };
    };

    std::vector<Tree>        trees;

    struct TreeVisitor {
        static void visit_recursive(std::vector<SupportElements> &move_bounds, SupportElement &start_element, Tree &out) {
            assert(! start_element.state.marked && ! start_element.parents.empty());
            // Collect elements up to a bifurcation above.
            start_element.state.marked = true;
            // For each branch bifurcating from this point:
            SupportElements &layer       = move_bounds[start_element.state.layer_idx];
            SupportElements &layer_above = move_bounds[start_element.state.layer_idx + 1];
            bool root = out.branches.empty();
            for (size_t parent_idx = 0; parent_idx < start_element.parents.size(); ++ parent_idx) {
                Branch branch;
                branch.path.emplace_back(&start_element);
                // Traverse each branch until it branches again.
                SupportElement &first_parent = layer_above[start_element.parents[parent_idx]];
                assert(! first_parent.state.marked);
                assert(branch.path.back()->state.layer_idx + 1 == first_parent.state.layer_idx);
                branch.path.emplace_back(&first_parent);
                if (first_parent.parents.size() < 2)
                    first_parent.state.marked = true;
                SupportElement *next_branch = nullptr;
                if (first_parent.parents.size() == 1) {
                    for (SupportElement *parent = &first_parent;;) {
                        assert(parent->state.marked);
                        SupportElement &next_parent = move_bounds[parent->state.layer_idx + 1][parent->parents.front()];
                        assert(! next_parent.state.marked);
                        assert(branch.path.back()->state.layer_idx + 1 == next_parent.state.layer_idx);
                        branch.path.emplace_back(&next_parent);
                        if (next_parent.parents.size() > 1) {
                            // Branching point was reached.
                            next_branch = &next_parent;
                            break;
                        }
                        next_parent.state.marked = true;
                        if (next_parent.parents.size() == 0)
                            // Tip is reached.
                            break;
                        parent = &next_parent;
                    }
                } else if (first_parent.parents.size() > 1)
                    // Branching point was reached.
                    next_branch = &first_parent;
                assert(branch.path.size() >= 2);
                assert(next_branch == nullptr || ! next_branch->state.marked);
                branch.has_root = root;
                branch.has_tip  = ! next_branch;
                out.branches.emplace_back(std::move(branch));
                if (next_branch)
                    visit_recursive(move_bounds, *next_branch, out);
            }
        }
    };

    for (LayerIndex layer_idx = 0; layer_idx + 1 < LayerIndex(move_bounds.size()); ++ layer_idx) {
//        int ielement;
        for (SupportElement& start_element : move_bounds[layer_idx]) {
            if (!start_element.state.marked && !start_element.parents.empty()) {
#if 0
                int found = 0;
                if (layer_idx > 0) {
                    for (auto& el : move_bounds[layer_idx - 1]) {
                        for (auto iparent : el.parents)
                            if (iparent == ielement)
                                ++found;
                    }
                    if (found != 0)
                        printf("Found: %d\n", found);
                }
#endif
                trees.push_back({});
                TreeVisitor::visit_recursive(move_bounds, start_element, trees.back());
                assert(!trees.back().branches.empty());
                //FIXME debugging
#if 1
                if (start_element.state.lost) {
                }
                else if (start_element.state.verylost) {
                } else
                    trees.pop_back();
#endif
            }
//            ++ ielement;
        }
    }

    const SlicingParameters &slicing_params = print_object.slicing_parameters();
    MeshSlicingParams mesh_slicing_params;
    mesh_slicing_params.mode = MeshSlicingParams::SlicingMode::Positive;

    tbb::parallel_for(tbb::blocked_range<size_t>(0, trees.size(), 1),
        [&trees, &volumes, &config, &slicing_params, &move_bounds, &interface_placer, &mesh_slicing_params, &throw_on_cancel](const tbb::blocked_range<size_t> &range) {
            indexed_triangle_set    partial_mesh;
            std::vector<float>      slice_z;
            std::vector<Polygons>   bottom_contacts;
            for (size_t tree_id = range.begin(); tree_id < range.end(); ++ tree_id) {
                Tree &tree = trees[tree_id];
                for (const Branch &branch : tree.branches) {
                    // Triangulate the tube.
                    partial_mesh.clear();
                    std::pair<float, float> zspan = extrude_branch(branch.path, config, slicing_params, move_bounds, partial_mesh);
                    LayerIndex layer_begin = branch.has_root ?
                        branch.path.front()->state.layer_idx : 
                        std::min(branch.path.front()->state.layer_idx, layer_idx_ceil(slicing_params, config, zspan.first));
                    LayerIndex layer_end   = (branch.has_tip ?
                        branch.path.back()->state.layer_idx :
                        std::max(branch.path.back()->state.layer_idx, layer_idx_floor(slicing_params, config, zspan.second))) + 1;
                    slice_z.clear();
                    for (LayerIndex layer_idx = layer_begin; layer_idx < layer_end; ++ layer_idx) {
                        const double print_z  = layer_z(slicing_params, config, layer_idx);
                        const double bottom_z = layer_idx > 0 ? layer_z(slicing_params, config, layer_idx - 1) : 0.;
                        slice_z.emplace_back(float(0.5 * (bottom_z + print_z)));
                    }
                    std::vector<Polygons> slices = slice_mesh(partial_mesh, slice_z, mesh_slicing_params, throw_on_cancel);
                    bottom_contacts.clear();
                    //FIXME parallelize?
                    for (LayerIndex i = 0; i < LayerIndex(slices.size()); ++ i)
                        slices[i] = diff_clipped(slices[i], volumes.getCollision(0, layer_begin + i, true)); //FIXME parent_uses_min || draw_area.element->state.use_min_xy_dist);

                    size_t num_empty = 0;
                    if (slices.front().empty()) {
                        // Some of the initial layers are empty.
                        num_empty = std::find_if(slices.begin(), slices.end(), [](auto &s) { return !s.empty(); }) - slices.begin();
                    } else {
                        if (branch.has_root) {
                            if (branch.path.front()->state.to_model_gracious) {
                                if (config.settings.support_floor_layers > 0)
                                    //FIXME one may just take the whole tree slice as bottom interface.
                                    bottom_contacts.emplace_back(intersection_clipped(slices.front(), volumes.getPlaceableAreas(0, layer_begin, [] {})));
                            } else if (layer_begin > 0) {
                                // Drop down areas that do rest non - gracefully on the model to ensure the branch actually rests on something.
                                struct BottomExtraSlice {
                                    Polygons polygons;
                                    double   area;
                                };
                                std::vector<BottomExtraSlice>   bottom_extra_slices;
                                Polygons                        rest_support;
                                coord_t                         bottom_radius = getRadius(config, branch.path.front()->state);
                                // Don't propagate further than 1.5 * bottom radius.
                                //LayerIndex                      layers_propagate_max = 2 * bottom_radius / config.layer_height;
                                LayerIndex                      layers_propagate_max = 5 * bottom_radius / config.layer_height;
                                LayerIndex                      layer_bottommost = branch.path.front()->state.verylost ? 
                                    // If the tree bottom is hanging in the air, bring it down to some surface.
                                    0 : 
                                    //FIXME the "verylost" branches should stop when crossing another support.
                                    std::max(0, layer_begin - layers_propagate_max);
                                double                          support_area_min_radius = M_PI * sqr(double(config.branch_radius));
                                double                          support_area_stop = std::max(0.2 * M_PI * sqr(double(bottom_radius)), 0.5 * support_area_min_radius);
                                 // Only propagate until the rest area is smaller than this threshold.
                                double                          support_area_min = 0.1 * support_area_min_radius;
                                for (LayerIndex layer_idx = layer_begin - 1; layer_idx >= layer_bottommost; -- layer_idx) {
                                    rest_support = diff_clipped(rest_support.empty() ? slices.front() : rest_support, volumes.getCollision(0, layer_idx, false));
                                    double rest_support_area = area(rest_support);
                                    if (rest_support_area < support_area_stop)
                                        // Don't propagate a fraction of the tree contact surface.
                                        break;
                                    bottom_extra_slices.push_back({ rest_support, rest_support_area });
                                }
                                // Now remove those bottom slices that are not supported at all.
#if 0
                                while (! bottom_extra_slices.empty()) {
                                    Polygons this_bottom_contacts = intersection_clipped(
                                        bottom_extra_slices.back().polygons, volumes.getPlaceableAreas(0, layer_begin - LayerIndex(bottom_extra_slices.size()), [] {}));
                                    if (area(this_bottom_contacts) < support_area_min)
                                        bottom_extra_slices.pop_back();
                                    else {
                                        // At least a fraction of the tree bottom is considered to be supported.
                                        if (config.settings.support_floor_layers > 0)
                                            // Turn this fraction of the tree bottom into a contact layer.
                                            bottom_contacts.emplace_back(std::move(this_bottom_contacts));
                                        break;
                                    }
                                }
#endif
                                if (config.settings.support_floor_layers > 0)
                                    for (int i = int(bottom_extra_slices.size()) - 2; i >= 0; -- i)
                                        bottom_contacts.emplace_back(
                                            intersection_clipped(bottom_extra_slices[i].polygons, volumes.getPlaceableAreas(0, layer_begin - i - 1, [] {})));
                                layer_begin -= LayerIndex(bottom_extra_slices.size());
                                slices.insert(slices.begin(), bottom_extra_slices.size(), {});
                                auto it_dst = slices.begin();
                                for (auto it_src = bottom_extra_slices.rbegin(); it_src != bottom_extra_slices.rend(); ++ it_src)
                                    *it_dst ++ = std::move(it_src->polygons);
                            }
                        }
                        
#if 0
                        //FIXME branch.has_tip seems to not be reliable.
                        if (branch.has_tip && interface_placer.support_parameters.has_top_contacts)
                            // Add top slices to top contacts / interfaces / base interfaces.
                            for (int i = int(branch.path.size()) - 1; i >= 0; -- i) {
                                const SupportElement &el = *branch.path[i];
                                if (el.state.missing_roof_layers == 0)
                                    break;
                                //FIXME Move or not?
                                interface_placer.add_roof(std::move(slices[int(slices.size()) - i - 1]), el.state.layer_idx,
                                    interface_placer.support_parameters.num_top_interface_layers + 1 - el.state.missing_roof_layers);
                            }
#endif
                    }

                    layer_begin += LayerIndex(num_empty);
                    while (! slices.empty() && slices.back().empty()) {
                        slices.pop_back();
                        -- layer_end;
                    }
                    if (layer_begin < layer_end) {
                        LayerIndex new_begin = tree.first_layer_id == -1 ? layer_begin : std::min(tree.first_layer_id, layer_begin);
                        LayerIndex new_end   = tree.first_layer_id == -1 ? layer_end : std::max(tree.first_layer_id + LayerIndex(tree.slices.size()), layer_end);
                        size_t     new_size  = size_t(new_end - new_begin);
                        if (tree.first_layer_id == -1) {
                        } else if (tree.slices.capacity() < new_size) {
                            std::vector<Slice> new_slices;
                            new_slices.reserve(new_size);
                            if (LayerIndex dif = tree.first_layer_id - new_begin; dif > 0)
                                new_slices.insert(new_slices.end(), dif, {});
                            append(new_slices, std::move(tree.slices));
                            tree.slices.swap(new_slices);
                        } else if (LayerIndex dif = tree.first_layer_id - new_begin; dif > 0)
                            tree.slices.insert(tree.slices.begin(), tree.first_layer_id - new_begin, {});
                        tree.slices.insert(tree.slices.end(), new_size - tree.slices.size(), {});
                        layer_begin -= LayerIndex(num_empty);
                        for (LayerIndex i = layer_begin; i != layer_end; ++ i) {
                            int j = i - layer_begin;
                            if (Polygons &src = slices[j]; ! src.empty()) {
                                Slice &dst = tree.slices[i - new_begin];
                                if (++ dst.num_branches > 1) {
                                    append(dst.polygons, std::move(src));
                                    if (j < bottom_contacts.size())
                                        append(dst.bottom_contacts, std::move(bottom_contacts[j]));
                                } else {
                                    dst.polygons = std::move(std::move(src));
                                    if (j < bottom_contacts.size())
                                        dst.bottom_contacts = std::move(bottom_contacts[j]);
                                }
                            }
                        }
                        tree.first_layer_id = new_begin;
                    }
                }
            }
        }, tbb::simple_partitioner());

    tbb::parallel_for(tbb::blocked_range<size_t>(0, trees.size(), 1),
        [&trees, &throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        for (size_t tree_id = range.begin(); tree_id < range.end(); ++ tree_id) {
            Tree &tree = trees[tree_id];
            for (Slice &slice : tree.slices)
                if (slice.num_branches > 1) {
                    slice.polygons        = union_(slice.polygons);
                    slice.bottom_contacts = union_(slice.bottom_contacts);
                    slice.num_branches = 1;
                }
            throw_on_cancel();
        }
    }, tbb::simple_partitioner());

    size_t num_layers = 0;
    for (Tree &tree : trees)
        if (tree.first_layer_id >= 0)
            num_layers = std::max(num_layers, size_t(tree.first_layer_id + tree.slices.size()));

    std::vector<Slice> slices(num_layers, Slice{});
    for (Tree &tree : trees)
        if (tree.first_layer_id >= 0) {
            for (LayerIndex i = tree.first_layer_id; i != tree.first_layer_id + LayerIndex(tree.slices.size()); ++ i)
                if (Slice &src = tree.slices[i - tree.first_layer_id]; ! src.polygons.empty()) {
                    Slice &dst = slices[i];
                    if (++ dst.num_branches > 1) {
                        append(dst.polygons,        std::move(src.polygons));
                        append(dst.bottom_contacts, std::move(src.bottom_contacts));
                    } else {
                        dst.polygons        = std::move(src.polygons);
                        dst.bottom_contacts = std::move(src.bottom_contacts);
                    }
                }
        }

    tbb::parallel_for(tbb::blocked_range<size_t>(0, std::min(move_bounds.size(), slices.size()), 1),
        [&print_object, &config, &slices, &bottom_contacts, &top_contacts, &intermediate_layers, &layer_storage, &throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            Slice &slice = slices[layer_idx];
            assert(intermediate_layers[layer_idx] == nullptr);
            Polygons base_layer_polygons     = slice.num_branches > 1 ? union_(slice.polygons) : std::move(slice.polygons);
            Polygons bottom_contact_polygons = slice.num_branches > 1 ? union_(slice.bottom_contacts) : std::move(slice.bottom_contacts);

            if (! base_layer_polygons.empty()) {
                // Most of the time in this function is this union call. Can take 300+ ms when a lot of areas are to be unioned.
                base_layer_polygons = smooth_outward(union_(base_layer_polygons), config.support_line_width); //FIXME was .smooth(50);
                //smooth_outward(closing(std::move(bottom), closing_distance + minimum_island_radius, closing_distance, SUPPORT_SURFACES_OFFSET_PARAMETERS), smoothing_distance) :
                // simplify a bit, to ensure the output does not contain outrageous amounts of vertices. Should not be necessary, just a precaution.
                base_layer_polygons = polygons_simplify(base_layer_polygons, std::min(scaled<double>(0.03), double(config.resolution)), polygons_strictly_simple);
            }

            // Subtract top contact layer polygons from support base.
            SupportGeneratorLayer *top_contact_layer = top_contacts.empty() ? nullptr : top_contacts[layer_idx];
            if (top_contact_layer && ! top_contact_layer->polygons.empty() && ! base_layer_polygons.empty()) {
                base_layer_polygons = diff(base_layer_polygons, top_contact_layer->polygons);
                if (! bottom_contact_polygons.empty())
                    //FIXME it may be better to clip bottom contacts with top contacts first after they are propagated to produce interface layers.
                    bottom_contact_polygons = diff(bottom_contact_polygons, top_contact_layer->polygons);
            }
            if (! bottom_contact_polygons.empty()) {
                base_layer_polygons = diff(base_layer_polygons, bottom_contact_polygons);
                SupportGeneratorLayer *bottom_contact_layer = bottom_contacts[layer_idx] = &layer_allocate(
                    layer_storage, SupporLayerType::BottomContact, print_object.slicing_parameters(), config, layer_idx);
                bottom_contact_layer->polygons = std::move(bottom_contact_polygons);
            }
            if (! base_layer_polygons.empty()) {
                SupportGeneratorLayer *base_layer = intermediate_layers[layer_idx] = &layer_allocate(
                    layer_storage, SupporLayerType::Base, print_object.slicing_parameters(), config, layer_idx);
                base_layer->polygons = union_(base_layer_polygons);
            }

            throw_on_cancel();
        }
    }, tbb::simple_partitioner());
}

} // namespace FFFTreeSupport

} // namespace Slic3r