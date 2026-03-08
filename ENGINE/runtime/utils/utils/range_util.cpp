#include "range_util.hpp"

#include <cmath>
#include <limits>

#include "assets/Asset.hpp"
#include "utils/grid.hpp"

namespace {
bool is_within_radius(long long ax, long long ay, long long bx, long long by, int radius) {
    const long long dx = ax - bx;
    const long long dy = ay - by;
    const long long r = static_cast<long long>(radius);
    return dx * dx + dy * dy <= r * r;
}

long long distance_squared(long long ax, long long ay, long long bx, long long by) {
    const long long dx = ax - bx;
    const long long dy = ay - by;
    return dx * dx + dy * dy;
}

bool resolve_asset_plane_pos(const Asset* asset, long long& right, long long& depth) {
    if (!asset) {
        return false;
    }
    vibble::grid::Grid& grid = vibble::grid::global_grid();
    const int          resolution = vibble::grid::clamp_resolution(asset->grid_resolution);
    SDL_Point          plane      = SDL_Point{asset->world_x(), asset->world_z()};
    SDL_Point          snapped    = grid.snap_to_vertex(plane, resolution);
    right = static_cast<long long>(snapped.x);
    depth = static_cast<long long>(snapped.y);
    return true;
}
}

bool Range::plane_coords(const Asset* a, double& right, double& depth) {
    right = 0.0;
    depth = 0.0;
    if (!a) return false;
    vibble::grid::Grid& grid = vibble::grid::global_grid();
    const int          resolution = vibble::grid::clamp_resolution(a->grid_resolution);
    SDL_Point          plane   = SDL_Point{a->world_x(), a->world_z()};
    SDL_Point          snapped = grid.snap_to_vertex(plane, resolution);
    right = static_cast<double>(snapped.x);
    depth = static_cast<double>(snapped.y);
    return true;
}

bool Range::plane_coords(const SDL_Point& p, double& right, double& depth) {
    vibble::grid::Grid& grid = vibble::grid::global_grid();
    SDL_Point          snapped = grid.snap_to_vertex(p, 0);
    right = static_cast<double>(snapped.x);
    depth = static_cast<double>(snapped.y);
    return true;
}

bool Range::in_range_plane(double left_right, double left_depth, double right_right, double right_depth, int radius) {
    double dx = left_right - right_right;
    double dy = left_depth - right_depth;
    const double r2 = static_cast<double>(radius) * static_cast<double>(radius);
    const double d2 = dx * dx + dy * dy;
    return d2 <= r2;
}

double Range::distance_plane(double left_right, double left_depth, double right_right, double right_depth) {
    double dx = left_right - right_right;
    double dy = left_depth - right_depth;
    return std::sqrt(dx * dx + dy * dy);
}

bool Range::is_in_range(const Asset* a, const Asset* b, int radius) {
    long long left_right, left_depth, right_right, right_depth;
    if (!resolve_asset_plane_pos(a, left_right, left_depth) || !resolve_asset_plane_pos(b, right_right, right_depth)) {
        return false;
    }
    return is_within_radius(left_right, left_depth, right_right, right_depth, radius);
}

bool Range::is_in_range(const Asset* a, const SDL_Point& b, int radius) {
    long long left_right, left_depth;
    if (!resolve_asset_plane_pos(a, left_right, left_depth)) {
        return false;
    }
    vibble::grid::Grid& grid = vibble::grid::global_grid();
    SDL_Point          snapped_b = grid.snap_to_vertex(b, 0);
    return is_within_radius(left_right, left_depth, static_cast<long long>(snapped_b.x), static_cast<long long>(snapped_b.y), radius);
}

bool Range::is_in_range(const SDL_Point& a, const Asset* b, int radius) {
    long long right_right, right_depth;
    if (!resolve_asset_plane_pos(b, right_right, right_depth)) {
        return false;
    }
    vibble::grid::Grid& grid = vibble::grid::global_grid();
    SDL_Point          snapped_a = grid.snap_to_vertex(a, 0);
    return is_within_radius(static_cast<long long>(snapped_a.x), static_cast<long long>(snapped_a.y), right_right, right_depth, radius);
}

bool Range::is_in_range(const SDL_Point& a, const SDL_Point& b, int radius) {
    vibble::grid::Grid& grid = vibble::grid::global_grid();
    SDL_Point          snapped_a = grid.snap_to_vertex(a, 0);
    SDL_Point          snapped_b = grid.snap_to_vertex(b, 0);
    return is_within_radius(static_cast<long long>(snapped_a.x), static_cast<long long>(snapped_a.y), static_cast<long long>(snapped_b.x), static_cast<long long>(snapped_b.y), radius);
}

long long Range::distance_sq(const Asset* a, const Asset* b) {
    long long ax, ay, bx, by;
    if (!resolve_asset_plane_pos(a, ax, ay) || !resolve_asset_plane_pos(b, bx, by)) {
        return std::numeric_limits<long long>::max();
    }
    return distance_squared(ax, ay, bx, by);
}

long long Range::distance_sq(const Asset* a, const SDL_Point& b) {
    long long ax, ay;
    if (!resolve_asset_plane_pos(a, ax, ay)) {
        return std::numeric_limits<long long>::max();
    }
    vibble::grid::Grid& grid = vibble::grid::global_grid();
    SDL_Point          snapped_b = grid.snap_to_vertex(b, 0);
    return distance_squared(ax, ay, static_cast<long long>(snapped_b.x), static_cast<long long>(snapped_b.y));
}

long long Range::distance_sq(const SDL_Point& a, const Asset* b) {
    long long bx, by;
    if (!resolve_asset_plane_pos(b, bx, by)) {
        return std::numeric_limits<long long>::max();
    }
    vibble::grid::Grid& grid = vibble::grid::global_grid();
    SDL_Point          snapped_a = grid.snap_to_vertex(a, 0);
    return distance_squared(static_cast<long long>(snapped_a.x), static_cast<long long>(snapped_a.y), bx, by);
}

long long Range::distance_sq(const SDL_Point& a, const SDL_Point& b) {
    vibble::grid::Grid& grid = vibble::grid::global_grid();
    SDL_Point          snapped_a = grid.snap_to_vertex(a, 0);
    SDL_Point          snapped_b = grid.snap_to_vertex(b, 0);
    return distance_squared(static_cast<long long>(snapped_a.x), static_cast<long long>(snapped_a.y), static_cast<long long>(snapped_b.x), static_cast<long long>(snapped_b.y));
}

double Range::get_distance(const Asset* a, const Asset* b) {
    double ax, ad, bx, bd;
    if (!plane_coords(a, ax, ad) || !plane_coords(b, bx, bd)) return std::numeric_limits<double>::infinity();
    return distance_plane(ax, ad, bx, bd);
}

double Range::get_distance(const Asset* a, const SDL_Point& b) {
    double ax, ad, bx, bd;
    if (!plane_coords(a, ax, ad) || !plane_coords(b, bx, bd)) return std::numeric_limits<double>::infinity();
    return distance_plane(ax, ad, bx, bd);
}

double Range::get_distance(const SDL_Point& a, const Asset* b) {
    double ax, ad, bx, bd;
    if (!plane_coords(a, ax, ad) || !plane_coords(b, bx, bd)) return std::numeric_limits<double>::infinity();
    return distance_plane(ax, ad, bx, bd);
}

double Range::get_distance(const SDL_Point& a, const SDL_Point& b) {
    double ax, ad, bx, bd;
    plane_coords(a, ax, ad);
    plane_coords(b, bx, bd);
    return distance_plane(ax, ad, bx, bd);
}

void Range::get_in_range(const SDL_Point& center,
                         int radius,
                         const std::vector<Asset*>& candidates,
                         std::vector<Asset*>& out) {
    out.clear();
    vibble::grid::Grid& grid = vibble::grid::global_grid();
    SDL_Point          snapped_center = grid.snap_to_vertex(center, 0);
    const long long    center_right   = static_cast<long long>(snapped_center.x);
    const long long    center_depth   = static_cast<long long>(snapped_center.y);
    const long long r = static_cast<long long>(radius);
    const long long r2 = r * r;
    for (Asset* a : candidates) {
        if (!a) continue;
        const int       resolution = vibble::grid::clamp_resolution(a->grid_resolution);
        SDL_Point       plane       = SDL_Point{a->world_x(), a->world_z()};
        SDL_Point       snapped    = grid.snap_to_vertex(plane, resolution);
        const long long asset_right = static_cast<long long>(snapped.x);
        const long long asset_depth = static_cast<long long>(snapped.y);
        const long long dx = asset_right - center_right;
        const long long dy = asset_depth - center_depth;
        if (dx * dx + dy * dy <= r2) {
            out.push_back(a);
        }
    }
}
