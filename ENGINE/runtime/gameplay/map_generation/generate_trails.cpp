#include "generate_trails.hpp"
#include "generate_rooms.hpp"
#include "trail_geometry.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>
#include "utils/display_color.hpp"
#include "utils/map_grid_settings.hpp"
using json = nlohmann::json;

namespace {

constexpr int kNearestNeighborCount = 4;
constexpr double kLoopConnectionChance = 0.35;
constexpr double kLoopCapRatio = 0.25;
constexpr int kTrailBaseAttempts = 96;
constexpr int kIsolatedMaxPasses = 48;
constexpr int kIsolatedNoProgressLimit = 5;
constexpr int kIsolatedConnectionAttempts = 80;
constexpr int kIsolatedIntersectionIncreaseInterval = 5;

struct DisjointSet {
    explicit DisjointSet(size_t count) : parent(count), rank(count, 0) {
        std::iota(parent.begin(), parent.end(), 0);
    }

    size_t find(size_t x) {
        if (parent[x] != x) {
            parent[x] = find(parent[x]);
        }
        return parent[x];
    }

    bool unite(size_t a, size_t b) {
        size_t root_a = find(a);
        size_t root_b = find(b);
        if (root_a == root_b) {
            return false;
        }
        if (rank[root_a] < rank[root_b]) {
            std::swap(root_a, root_b);
        }
        parent[root_b] = root_a;
        if (rank[root_a] == rank[root_b]) {
            ++rank[root_a];
        }
        return true;
    }

    std::vector<size_t> parent;
    std::vector<int> rank;
};

struct PointerPairHash {
    size_t operator()(const std::pair<Room*, Room*>& value) const noexcept {
        auto a = reinterpret_cast<std::uintptr_t>(value.first);
        auto b = reinterpret_cast<std::uintptr_t>(value.second);
        return std::hash<std::uintptr_t>{}(a) ^ (std::hash<std::uintptr_t>{}(b) << 1);
    }
};

struct PointerPairEqual {
    bool operator()(const std::pair<Room*, Room*>& lhs, const std::pair<Room*, Room*>& rhs) const noexcept {
        return lhs.first == rhs.first && lhs.second == rhs.second;
    }
};

std::pair<Room*, Room*> canonical_pair(Room* a, Room* b) {
    if (!a || !b) {
        return {nullptr, nullptr};
    }
    if (a > b) {
        std::swap(a, b);
    }
    return {a, b};
}

std::pair<double, double> room_center(Room* room) {
    if (!room) {
        return {0.0, 0.0};
    }
    if (room->room_area) {
        SDL_Point c = room->room_area->get_center();
        return {static_cast<double>(c.x), static_cast<double>(c.y)};
    }
    return {static_cast<double>(room->map_origin.first), static_cast<double>(room->map_origin.second)};
}

std::vector<SDL_Point> build_centerline(const SDL_Point& start,
                                        const SDL_Point& end,
                                        int curvyness,
                                        std::mt19937& rng)
{
    std::vector<SDL_Point> line;
    line.reserve(static_cast<size_t>(curvyness) + 2);
    line.push_back(start);
    if (curvyness > 0) {
        double dx = static_cast<double>(end.x - start.x);
        double dy = static_cast<double>(end.y - start.y);
        double len = std::hypot(dx, dy);
        if (len <= 0.0) len = 1.0;
        double max_offset = len * 0.25 * (static_cast<double>(curvyness) / 8.0);
        std::uniform_real_distribution<double> offset_dist(-max_offset, max_offset);
        for (int i = 1; i <= curvyness; ++i) {
            double t  = static_cast<double>(i) / (curvyness + 1);
            double px = start.x + t * dx;
            double py = start.y + t * dy;
            double nx = -dy / len;
            double ny =  dx / len;
            double off = offset_dist(rng);
            line.push_back(SDL_Point{
                static_cast<int>(std::lround(px + nx * off)),
                static_cast<int>(std::lround(py + ny * off))
            });
        }
    }
    line.push_back(end);
    return line;
}

namespace detail {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

constexpr double kCenterlineSpacingMin = 4.0;
constexpr double kCenterlineSpacingMax = 16.0;

inline Vec2 to_vec(const SDL_Point& raw_point) {
    return Vec2{ static_cast<double>(raw_point.x), static_cast<double>(raw_point.y) };
}

inline SDL_Point to_point(const Vec2& point) {
    return SDL_Point{
        static_cast<int>(std::lround(point.x)),
        static_cast<int>(std::lround(point.y))
    };
}

inline Vec2 lerp(const Vec2& a, const Vec2& b, double t) {
    return Vec2{
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t
    };
}

inline double length(const Vec2& value) {
    return std::hypot(value.x, value.y);
}

inline Vec2 normalize(const Vec2& value) {
    double len = length(value);
    if (len <= 1e-6) {
        return Vec2{ 1.0, 0.0 };
    }
    return Vec2{ value.x / len, value.y / len };
}

inline Vec2 perp(const Vec2& value) {
    return Vec2{ -value.y, value.x };
}

double endpoint_fade(size_t index, size_t count) {
    if (count < 2) {
        return 0.0;
    }
    double relative = static_cast<double>(index) / (count - 1);
    double fade = std::min(relative, 1.0 - relative);
    return std::clamp(fade * 4.0, 0.0, 1.0);
}

Vec2 compute_tangent(const std::vector<Vec2>& line, size_t index) {
    if (line.empty()) {
        return Vec2{ 1.0, 0.0 };
    }
    Vec2 previous = line[index];
    Vec2 next = line[index];
    if (index > 0) {
        previous = line[index - 1];
    }
    if (index + 1 < line.size()) {
        next = line[index + 1];
    }
    return normalize(Vec2{ next.x - previous.x, next.y - previous.y });
}

void smooth_profile(std::vector<double>& values, int passes) {
    if (values.size() < 3 || passes <= 0) {
        return;
    }
    std::vector<double> buffer(values.size());
    for (int pass_idx = 0; pass_idx < passes; ++pass_idx) {
        buffer = values;
        for (size_t i = 1; i + 1 < values.size(); ++i) {
            buffer[i] = (values[i - 1] + values[i] * 2.0 + values[i + 1]) * 0.25;
        }
        values.swap(buffer);
    }
}

std::vector<Vec2> resample_polyline(const std::vector<Vec2>& raw, double spacing) {
    if (raw.size() < 2) {
        return raw;
    }
    double total_length = 0.0;
    std::vector<double> segment_lengths;
    segment_lengths.reserve(raw.size() - 1);
    for (size_t i = 0; i + 1 < raw.size(); ++i) {
        double seg_len = length(Vec2{ raw[i + 1].x - raw[i].x, raw[i + 1].y - raw[i].y });
        segment_lengths.push_back(seg_len);
        total_length += seg_len;
    }
    if (total_length <= 0.0 || spacing <= 0.0) {
        if (raw.size() <= trail_generation::kTrailGeometryMaxSamples) {
            return raw;
        }
        return std::vector<Vec2>(raw.begin(), raw.begin() + trail_generation::kTrailGeometryMaxSamples);
    }
    double clamped_spacing = std::clamp(spacing, kCenterlineSpacingMin, kCenterlineSpacingMax);
    size_t target_samples = static_cast<size_t>(std::ceil(total_length / clamped_spacing)) + 1;
    target_samples = std::clamp(target_samples, static_cast<size_t>(2), trail_generation::kTrailGeometryMaxSamples);
    std::vector<Vec2> result;
    result.reserve(target_samples);
    double step = (target_samples > 1) ? (total_length / (target_samples - 1)) : 0.0;
    size_t segment_index = 0;
    double consumed = 0.0;
    result.push_back(raw.front());
    for (size_t sample = 1; sample + 1 < target_samples; ++sample) {
        double target = step * sample;
        while (segment_index < segment_lengths.size() && consumed + segment_lengths[segment_index] < target) {
            consumed += segment_lengths[segment_index];
            ++segment_index;
        }
        if (segment_index >= segment_lengths.size()) {
            break;
        }
        double seg_length = segment_lengths[segment_index];
        double local_target = target - consumed;
        double t = (seg_length <= 0.0) ? 0.0 : (local_target / seg_length);
        result.push_back(lerp(raw[segment_index], raw[segment_index + 1], t));
    }
    result.push_back(raw.back());
    if (result.size() > trail_generation::kTrailGeometryMaxSamples) {
        result.resize(trail_generation::kTrailGeometryMaxSamples);
    }
    return result;
}

std::vector<Vec2> chaikin_smooth(const std::vector<Vec2>& chain, int passes) {
    if (chain.size() < 2 || passes <= 0) {
        return chain;
    }
    std::vector<Vec2> current = chain;
    for (int pass_idx = 0; pass_idx < passes && current.size() < trail_generation::kTrailGeometryMaxSamples; ++pass_idx) {
        std::vector<Vec2> next;
        next.reserve(std::min(trail_generation::kTrailGeometryMaxSamples, current.size() * 2));
        next.push_back(current.front());
        for (size_t i = 0; i + 1 < current.size(); ++i) {
            next.push_back(lerp(current[i], current[i + 1], 0.25));
            if (next.size() >= trail_generation::kTrailGeometryMaxSamples) {
                break;
            }
            next.push_back(lerp(current[i], current[i + 1], 0.75));
            if (next.size() >= trail_generation::kTrailGeometryMaxSamples) {
                break;
            }
        }
        next.push_back(current.back());
        current.swap(next);
    }
    return current;
}

}  // namespace detail

}  // namespace

namespace trail_generation {

std::vector<SDL_Point> build_trail_polygon(const std::vector<SDL_Point>& base_centerline,
                                           int min_width,
                                           int max_width,
                                           int curvyness,
                                           std::mt19937& rng,
                                           TrailGeometryReport* report)
{
    if (base_centerline.size() < 2) {
        if (report) {
            report->centerline.clear();
            report->local_widths.clear();
            report->resampled_points = 0;
            report->boundary_points = 0;
            report->smoothing_passes = 0;
        }
        return {};
    }

    std::vector<detail::Vec2> raw;
    raw.reserve(base_centerline.size());
    for (const auto& pt : base_centerline) {
        raw.push_back(detail::to_vec(pt));
    }

    double desired_spacing = std::clamp(
        static_cast<double>(std::max(min_width, max_width)) * 0.2,
        detail::kCenterlineSpacingMin,
        detail::kCenterlineSpacingMax);
    auto dense = detail::resample_polyline(raw, desired_spacing);
    if (dense.size() < 2) {
        if (report) {
            report->centerline.clear();
            report->local_widths.clear();
            report->resampled_points = dense.size();
            report->boundary_points = 0;
            report->smoothing_passes = 0;
        }
        return {};
    }

    double sorted_min = static_cast<double>(std::min(min_width, max_width));
    double sorted_max = static_cast<double>(std::max(min_width, max_width));
    double width_range = sorted_max - sorted_min;
    std::vector<double> width_profile(dense.size(), sorted_min);

    if (width_range > 0.0) {
        double curvy_factor = std::clamp(static_cast<double>(curvyness) / 8.0, 0.0, 1.0);
        std::uniform_real_distribution<double> profile_noise(-1.0, 1.0);
        std::vector<double> normalized(dense.size(), 0.5);
        double noise_scale = 0.25 + 0.5 * curvy_factor;
        for (size_t i = 0; i < normalized.size(); ++i) {
            normalized[i] = 0.5 + profile_noise(rng) * noise_scale;
        }
        detail::smooth_profile(normalized, 2);
        for (size_t i = 0; i < normalized.size(); ++i) {
            normalized[i] = std::clamp(normalized[i], 0.0, 1.0);
            double fade = detail::endpoint_fade(i, normalized.size());
            double blend = 0.5 + (normalized[i] - 0.5) * fade;
            blend = std::clamp(blend, 0.0, 1.0);
            width_profile[i] = sorted_min + blend * width_range;
        }
    } else {
        std::fill(width_profile.begin(), width_profile.end(), sorted_min);
    }

    double jitter_factor = std::clamp(static_cast<double>(curvyness) / 8.0, 0.0, 1.0);
    double jitter_scale = (4.0 + width_range * 0.5) * (0.2 + 0.8 * jitter_factor);
    std::uniform_real_distribution<double> offset_noise(-1.0, 1.0);

    if (report) {
        report->centerline.clear();
        report->centerline.reserve(dense.size());
    }

    for (size_t i = 0; i < dense.size(); ++i) {
        double fade = detail::endpoint_fade(i, dense.size());
        detail::Vec2 tangent = detail::compute_tangent(dense, i);
        detail::Vec2 normal = detail::perp(tangent);
        double offset = offset_noise(rng) * jitter_scale * fade;
        dense[i].x += normal.x * offset;
        dense[i].y += normal.y * offset;
        if (report) {
            report->centerline.push_back({ dense[i].x, dense[i].y });
        }
    }

    if (report) {
        report->local_widths = width_profile;
    }

    std::vector<detail::Vec2> left;
    std::vector<detail::Vec2> right;
    left.reserve(dense.size());
    right.reserve(dense.size());
    for (size_t i = 0; i < dense.size(); ++i) {
        detail::Vec2 tangent = detail::compute_tangent(dense, i);
        detail::Vec2 normal = detail::perp(tangent);
        double half_width = std::max(1.0, width_profile[i] * 0.5);
        left.push_back(detail::Vec2{ dense[i].x + normal.x * half_width, dense[i].y + normal.y * half_width });
        right.push_back(detail::Vec2{ dense[i].x - normal.x * half_width, dense[i].y - normal.y * half_width });
    }

    auto smooth_left = detail::chaikin_smooth(left, kTrailGeometryBoundarySmoothPasses);
    auto smooth_right = detail::chaikin_smooth(right, kTrailGeometryBoundarySmoothPasses);

    std::vector<SDL_Point> polygon;
    polygon.reserve(smooth_left.size() + smooth_right.size());
    for (const auto& point : smooth_left) {
        polygon.push_back(detail::to_point(point));
    }
    for (auto it = smooth_right.rbegin(); it != smooth_right.rend(); ++it) {
        polygon.push_back(detail::to_point(*it));
    }

    if (polygon.size() < 4) {
        return {};
    }

    if (report) {
        report->resampled_points = dense.size();
        report->boundary_points = polygon.size();
        report->smoothing_passes = kTrailGeometryBoundarySmoothPasses;
    }

    return polygon;
}

}  // namespace trail_generation

SDL_Point compute_edge_point(const SDL_Point& center,
                             const SDL_Point& toward,
                             const Area* area)
{
    if (!area) return center;
    double dx = static_cast<double>(toward.x - center.x);
    double dy = static_cast<double>(toward.y - center.y);
    double len = std::hypot(dx, dy);
    if (len <= 0.0) return center;
    double dirX = dx / len;
    double dirY = dy / len;
    const int max_steps = 2000;
    const double step_size = 1.0;
    const double max_distance = 10000.0;
    double current_distance = 0.0;
    SDL_Point edge = center;
    for (int i = 1; i <= max_steps && current_distance < max_distance; ++i) {
        current_distance += step_size;
        double px = center.x + dirX * current_distance;
        double py = center.y + dirY * current_distance;
        int ipx = static_cast<int>(std::lround(px));
        int ipy = static_cast<int>(std::lround(py));
        if (area->contains_point(SDL_Point{ ipx, ipy })) {
            edge = SDL_Point{ ipx, ipy };
        } else {
            break;
        }
    }
    return edge;
}

bool attempt_trail_connection(Room* a,
                              Room* b,
                              std::vector<Area>& existing_areas,
                              const std::string& manifest_context,
                              AssetLibrary* asset_lib,
                              std::vector<std::unique_ptr<Room>>& trail_rooms,
                              int allowed_intersections,
                              nlohmann::json* trail_config,
                              const std::string& trail_name,
                              const nlohmann::json* map_assets_data,
                              double map_radius,
                              bool testing,
                              std::mt19937& rng,
                              nlohmann::json* map_manifest,
                              devmode::core::ManifestStore* manifest_store,
                              Room::ManifestWriter manifest_writer)
{
    if (testing) std::cout << "[TrailGeometry] Attempting trail " << a->room_name << " -> " << b->room_name << "\n";
    if (!trail_config) return false;
    json& config = *trail_config;
    const int min_width = config.value("min_width", 40);
    const int max_width = config.value("max_width", min_width);
    const int curvyness = config.value("curvyness", 2);
    const std::string name = config.value("name", trail_name.empty() ? std::string("trail_segment") : trail_name);
    const int width_for_depth = std::max(min_width, max_width);
    if (testing) std::cout << "[TrailGeometry] Template " << name << " width_range=[" << min_width << "," << max_width << "] curvyness=" << curvyness << "\n";
    const SDL_Point a_center = a->room_area->get_center();
    const SDL_Point b_center = b->room_area->get_center();
    const double overshoot = 100.0;
    const double min_interior_depth = std::max(40.0, static_cast<double>(width_for_depth) * 0.75);

    auto make_edge_triplet =
        [&](const SDL_Point& center, const SDL_Point& toward, const Area* area)
        -> std::tuple<SDL_Point, SDL_Point, SDL_Point>
    {
        SDL_Point edge = compute_edge_point(center, toward, area);
        double dx = static_cast<double>(edge.x - center.x);
        double dy = static_cast<double>(edge.y - center.y);
        double len = std::hypot(dx, dy);
        if (len <= 0.0) len = 1.0;
        double ux = dx / len;
        double uy = dy / len;
        SDL_Point outside{
            static_cast<int>(std::lround(edge.x + ux * overshoot)), static_cast<int>(std::lround(edge.y + uy * overshoot)) };
        SDL_Point interior{
            static_cast<int>(std::lround(edge.x - ux * min_interior_depth)), static_cast<int>(std::lround(edge.y - uy * min_interior_depth)) };
        auto is_inside = [&](const SDL_Point& p)->bool{
            return area->contains_point(p);
        };
        if (!is_inside(interior)) {
            const int max_fix_steps = 1024;
            const double step = 2.0;
            double px = static_cast<double>(interior.x);
            double py = static_cast<double>(interior.y);
            for (int i = 0; i < max_fix_steps; ++i) {
                SDL_Point test{ static_cast<int>(std::lround(px)),
                                static_cast<int>(std::lround(py)) };
                if (is_inside(test)) { interior = test; break; }
                px -= ux * step;
                py -= uy * step;
                if (std::hypot(px - center.x, py - center.y) > len + 2.0) {
                    break;
                }
            }
            if (!is_inside(interior)) {
                interior = center;
            }
        }
        return std::make_tuple(interior, edge, outside);
    };

    SDL_Point a_interior, a_edge, a_outside;
    std::tie(a_interior, a_edge, a_outside) = make_edge_triplet(a_center, b_center, a->room_area.get());

    SDL_Point b_interior, b_edge, b_outside;
    std::tie(b_interior, b_edge, b_outside) = make_edge_triplet(b_center, a_center, b->room_area.get());

    auto [aminx, aminy, amaxx, amaxy] = a->room_area->get_bounds();
    auto [bminx, bminy, bmaxx, bmaxy] = b->room_area->get_bounds();

    std::vector<SDL_Point> base_line;
    base_line.reserve(static_cast<size_t>(curvyness) + 6);
    base_line.push_back(a_interior);
    base_line.push_back(a_edge);
    auto middle = build_centerline(a_outside, b_outside, curvyness, rng);
    base_line.insert(base_line.end(), middle.begin(), middle.end());
    base_line.push_back(b_edge);
    base_line.push_back(b_interior);

    auto polygon = trail_generation::build_trail_polygon(base_line, min_width, max_width, curvyness, rng);
    if (polygon.empty()) {
        if (testing) {
            std::cout << "[TrailGeometry] Geometry builder produced no polygon\n";
        }
        return false;
    }

    int cminx = polygon[0].x, cmaxx = polygon[0].x;
    int cminy = polygon[0].y, cmaxy = polygon[0].y;
    for (const auto& p : polygon) {
        cminx = std::min(cminx, p.x);
        cmaxx = std::max(cmaxx, p.x);
        cminy = std::min(cminy, p.y);
        cmaxy = std::max(cmaxy, p.y);
    }

    Area candidate("trail_candidate", polygon, 3);

    int intersection_count = 0;
    for (auto& area : existing_areas) {
        auto [minx, miny, maxx, maxy] = area.get_bounds();
        bool isA = (minx == aminx && miny == aminy && maxx == amaxx && maxy == amaxy);
        bool isB = (minx == bminx && miny == bminy && maxx == bmaxx && maxy == bmaxy);
        if (isA || isB) continue;
        if (cmaxx < minx || maxx < cminx || cmaxy < miny || maxy < cminy) {
            continue;
        }
        if (candidate.intersects(area)) {
            if (++intersection_count > allowed_intersections) {
                break;
            }
        }
    }
    if (intersection_count > allowed_intersections) {
        if (testing) {
            std::cout << "[TrailGeometry] Abort due to excessive intersections\n";
        }
        return false;
    }

    auto trail_room = std::make_unique<Room>( a->map_origin, "trail", name, nullptr, manifest_context, asset_lib, &candidate, trail_config, map_assets_data, MapGridSettings::defaults(), map_radius, "trails_data", map_manifest, manifest_store, manifest_context, manifest_writer );
    a->add_connecting_room(trail_room.get());
    b->add_connecting_room(trail_room.get());
    trail_room->add_connecting_room(a);
    trail_room->add_connecting_room(b);

    existing_areas.push_back(candidate);
    trail_rooms.push_back(std::move(trail_room));

    if (testing) {
        std::cout << "[TrailGeometry] Trail placed successfully\n";
    }
    return true;
}

}
GenerateTrails::GenerateTrails(nlohmann::json& trail_data, std::vector<SDL_Color> reserved_colors)
: rng_(std::random_device{}()),
trails_data_(&trail_data),
trail_colors_(std::move(reserved_colors))
{
        if (!trail_data.is_object()) {
                trail_data = nlohmann::json::object();
        }
        if (trail_data.is_object()) {
                for (auto it = trail_data.begin(); it != trail_data.end(); ++it) {
                        if (!it->is_object()) {
                                continue;
                        }
                        available_assets_.push_back({ it.key(), &(*it) });
                        utils::display_color::ensure(*available_assets_.back().data, trail_colors_);
                }
        }
        if (testing) {
                std::cout << "[GenerateTrails] Loaded " << available_assets_.size() << " trail templates\n";
        }
        if (available_assets_.empty()) {
                throw std::runtime_error("[GenerateTrails] No trail templates found in trails_data");
        }
}

void GenerateTrails::set_all_rooms_reference(const std::vector<Room*>& rooms) {
	all_rooms_reference = rooms;
}

std::vector<std::unique_ptr<Room>> GenerateTrails::generate_trails(
                                                                       const std::vector<std::pair<Room*, Room*>>& room_pairs,
                                                                       const std::vector<Area>& existing_areas,
                                                                       const std::string& manifest_context,
                                                                       AssetLibrary* asset_lib,
                                                                       const nlohmann::json* map_assets_data,
                                                                       double map_radius,
                                                                       nlohmann::json* map_manifest,
                                                                       devmode::core::ManifestStore* manifest_store,
                                                                       Room::ManifestWriter manifest_writer)
{
        std::cout << "[GenerateTrails] Starting trail generation with " << room_pairs.size() << " forced connections\n";
        trail_areas_.clear();
        std::vector<std::unique_ptr<Room>> trail_rooms;
        std::vector<Area> all_areas = existing_areas;
        auto connection_plan = plan_maze_connections(all_rooms_reference, room_pairs);
        std::cout << "[GenerateTrails] Planned " << connection_plan.size() << " trail connections (" << room_pairs.size() << " forced)\n";
        if (testing) {
                std::cout << "[GenerateTrails] Planned " << connection_plan.size() << " trail connections (" << room_pairs.size() << " forced).\n";
        }
        for (const auto& [a, b] : connection_plan) {
                if (!a || !b) continue;
        std::cout << "[GenerateTrails] Attempting connection: " << a->room_name << " <-> " << b->room_name << " (budget " << kTrailBaseAttempts << ")\n";
        if (testing) {
                std::cout << "[GenerateTrails] Connecting: " << a->room_name
                << " <--> " << b->room_name << "\n";
        }
        bool success = false;
        int attempts_made = 0;
        for (int attempts = 0; attempts < kTrailBaseAttempts && !success; ++attempts) {
                if (const auto* asset_ref = pick_random_asset()) {
                                ++attempts_made;
                                success = attempt_trail_connection(
                                        a,
                                        b,
                                        all_areas,
                                        manifest_context,
                                        asset_lib,
                                        trail_rooms,
                                        1,
                                        asset_ref->data,
                                        asset_ref->name,
                                        map_assets_data,
                                        map_radius,
                                        testing,
                                        rng_,
                                        map_manifest,
                                        manifest_store,
                                        manifest_writer);
                                if (success && !trail_rooms.empty()) {
                                        auto& trail_room = trail_rooms.back();
                                        const nlohmann::json& config = asset_ref->data ? *asset_ref->data : nlohmann::json::object();
                                        trail_room->camera_height_px = std::clamp(config.value("camera_height_px", 1000), 1, 2000);
                                        trail_room->camera_tilt_deg = std::clamp(config.value("camera_tilt_deg", 60.0f), 0.0f, 360.0f);
                                        trail_room->camera_zoom_percent = std::clamp(config.value("camera_zoom_percent", 0), 0, 100);
                                        trail_room->camera_center_dx = config.value("camera_center_dx", 0);
                                        trail_room->camera_center_dz = config.value("camera_center_dz", 0);
                                }
                        }
                }
        if (success) {
                std::cout << "[GenerateTrails] Connection succeeded after " << attempts_made << " attempt(s)\n";
        } else {
                std::cout << "[GenerateTrails] Connection failed after " << kTrailBaseAttempts << " attempts\n";
                if (testing) {
                        std::cout << "[TrailGen] Failed to place trail between "
                                << a->room_name << " and " << b->room_name << "\n";
                }
        }
        }
        std::cout << "[GenerateTrails] Starting isolated room connections\n";
        find_and_connect_isolated(manifest_context, asset_lib, all_areas, trail_rooms, map_assets_data, map_radius, map_manifest, manifest_store, manifest_writer);
        std::cout << "[GenerateTrails] Isolated connections completed\n";
        std::cout << "[GenerateTrails] Total trail rooms created: " << trail_rooms.size() << "\n";
        if (testing) {
                std::cout << "[TrailGen] Total trail rooms created: " << trail_rooms.size() << "\n";
        }
        return trail_rooms;
}

const GenerateTrails::TrailTemplateRef* GenerateTrails::pick_random_asset() {
        if (available_assets_.empty()) return nullptr;
        std::uniform_int_distribution<size_t> dist(0, available_assets_.size() - 1);
        return &available_assets_[dist(rng_)];
}

std::vector<std::pair<Room*, Room*>> GenerateTrails::plan_maze_connections(
        const std::vector<Room*>& rooms,
        const std::vector<std::pair<Room*, Room*>>& forced_connections) {

        std::vector<std::pair<Room*, Room*>> planned;
        if (rooms.empty()) {
                return planned;
        }

        std::vector<Room*> unique_rooms;
        unique_rooms.reserve(rooms.size());
        std::unordered_set<Room*> seen;
        seen.reserve(rooms.size());
        for (Room* room : rooms) {
                if (!room) continue;
                if (seen.insert(room).second) {
                        unique_rooms.push_back(room);
                }
        }

        if (unique_rooms.size() < 2) {
                return planned;
        }

        std::unordered_map<Room*, size_t> index;
        index.reserve(unique_rooms.size());
        for (size_t i = 0; i < unique_rooms.size(); ++i) {
                index[unique_rooms[i]] = i;
        }

        DisjointSet dsu(unique_rooms.size());
        std::unordered_set<std::pair<Room*, Room*>, PointerPairHash, PointerPairEqual> blocked_pairs;
        blocked_pairs.reserve(unique_rooms.size() * kNearestNeighborCount + forced_connections.size());

        for (const auto& edge : forced_connections) {
                Room* a = edge.first;
                Room* b = edge.second;
                if (!a || !b) continue;
                auto ia = index.find(a);
                auto ib = index.find(b);
                if (ia == index.end() || ib == index.end()) continue;
                dsu.unite(ia->second, ib->second);
                auto key = canonical_pair(a, b);
                if (!key.first || !key.second) continue;
                if (blocked_pairs.insert(key).second) {
                        planned.emplace_back(a, b);
                }
        }

        struct CandidateEdge {
                Room* a = nullptr;
                Room* b = nullptr;
                double distance = 0.0;
                double jitter = 0.0;
};

        std::vector<CandidateEdge> candidates;
        candidates.reserve(unique_rooms.size() * kNearestNeighborCount);

        RoomSpatialIndex spatial_index(unique_rooms);
        for (size_t i = 0; i < unique_rooms.size(); ++i) {
                Room* a = unique_rooms[i];
                if (!a) continue;
                auto [ax, ay] = room_center(a);
                auto neighbors = spatial_index.find_k_nearest({static_cast<int>(ax), static_cast<int>(ay)}, kNearestNeighborCount + 1);
                for (const auto& [dist_sq, b] : neighbors) {
                        if (b == a) continue;
                        auto key = canonical_pair(a, b);
                        if (!key.first || !key.second) continue;
                        if (!blocked_pairs.insert(key).second) continue;
                        candidates.push_back(CandidateEdge{ a, b, std::sqrt(dist_sq), 0.0 });
                }
        }

        std::uniform_real_distribution<double> jitter_dist(0.0, 1.0);
        for (auto& candidate : candidates) {
                candidate.jitter = jitter_dist(rng_);
        }
        std::sort(candidates.begin(), candidates.end(), [](const CandidateEdge& lhs, const CandidateEdge& rhs) {
                double lw = lhs.distance + lhs.jitter * 25.0;
                double rw = rhs.distance + rhs.jitter * 25.0;
                if (lw == rw) {
                        if (lhs.a == rhs.a) return lhs.b < rhs.b;
                        return lhs.a < rhs.a;
                }
                return lw < rw;
        });

        std::uniform_real_distribution<double> loop_dist(0.0, 1.0);
        size_t loop_cap = static_cast<size_t>(std::ceil(unique_rooms.size() * kLoopCapRatio));
        if (loop_cap == 0 && unique_rooms.size() > 2) {
                loop_cap = 1;
        }
        size_t loops_added = 0;

        for (const auto& candidate : candidates) {
                Room* a = candidate.a;
                Room* b = candidate.b;
                if (!a || !b) continue;
                auto ia = index.find(a);
                auto ib = index.find(b);
                if (ia == index.end() || ib == index.end()) continue;
                if (dsu.unite(ia->second, ib->second)) {
                        planned.emplace_back(a, b);
                } else if (loops_added < loop_cap && loop_dist(rng_) < kLoopConnectionChance) {
                        planned.emplace_back(a, b);
                        ++loops_added;
                }
        }

        auto rebuild_components = [&]() {
                std::unordered_map<size_t, std::vector<size_t>> components;
                components.reserve(unique_rooms.size());
                for (size_t i = 0; i < unique_rooms.size(); ++i) {
                        components[dsu.find(i)].push_back(i);
                }
                return components;
};

        std::vector<std::pair<double, double>> cached_centers;
        cached_centers.reserve(unique_rooms.size());
        for (Room* room : unique_rooms) {
                if (room) {
                        cached_centers.emplace_back(room_center(room));
                } else {
                        cached_centers.emplace_back(0.0, 0.0);
                }
        }

        auto components = rebuild_components();
        while (components.size() > 1) {
                std::vector<std::vector<size_t>> groups;
                groups.reserve(components.size());
                for (auto& entry : components) {
                        groups.push_back(std::move(entry.second));
                }
                size_t base_index = 0;
                for (size_t i = 1; i < groups.size(); ++i) {
                        if (!groups[i].empty() && (groups[base_index].empty() || groups[i].size() < groups[base_index].size())) {
                                base_index = i;
                        }
                }
                const auto& base_group = groups[base_index];
                if (base_group.empty()) {
                        break;
                }
                double best_dist = std::numeric_limits<double>::max();
                size_t best_a = base_group.front();
                size_t best_b = base_group.front();
                for (size_t idx_a : base_group) {
                        const auto [ax, ay] = cached_centers[idx_a];
                        for (size_t g = 0; g < groups.size(); ++g) {
                                if (g == base_index) continue;
                                for (size_t idx_b : groups[g]) {
                                        const auto [bx, by] = cached_centers[idx_b];
                                        double dist = std::hypot(ax - bx, ay - by);
                                        if (dist < best_dist) {
                                                best_dist = dist;
                                                best_a = idx_a;
                                                best_b = idx_b;
                                        }
                                }
                        }
                }
                if (best_dist == std::numeric_limits<double>::max()) {
                        break;
                }
                Room* a = unique_rooms[best_a];
                Room* b = unique_rooms[best_b];
                if (!a || !b) {
                        break;
                }
                if (blocked_pairs.insert(canonical_pair(a, b)).second) {
                        planned.emplace_back(a, b);
                }
                dsu.unite(best_a, best_b);
                components = rebuild_components();
        }

        return planned;
}

void GenerateTrails::find_and_connect_isolated(
                                                   const std::string& manifest_context,
                                                   AssetLibrary* asset_lib,
                                                   std::vector<Area>& existing_areas,
                                                   std::vector<std::unique_ptr<Room>>& trail_rooms,
                                                   const nlohmann::json* map_assets_data,
                                                   double map_radius,
                                                   nlohmann::json* map_manifest,
                                                   devmode::core::ManifestStore* manifest_store,
                                                   Room::ManifestWriter manifest_writer)
{
    int allowed_intersections = 0;
    int no_progress_count = 0;
    std::cout << "[GenerateTrails] Starting isolated connection (max " << kIsolatedMaxPasses << " passes)\n";
    int pass = 0;
    int passes_done = 0;
    for (; pass < kIsolatedMaxPasses && no_progress_count < kIsolatedNoProgressLimit; ++pass) {
        if (testing) {
            std::cout << "[GenerateTrails] Isolated pass " << pass + 1 << " with " << allowed_intersections << " allowed intersections\n";
        }
		std::unordered_set<Room*> visited;
		std::unordered_set<Room*> connected_to_spawn;
		std::vector<std::vector<Room*>> isolated_groups;
		auto mark_connected = [&](Room* room, auto&& self) -> void {
			if (!room || connected_to_spawn.count(room)) return;
			connected_to_spawn.insert(room);
			for (Room* neighbor : room->connected_rooms) {
					self(neighbor, self);
			}
};
		auto collect_group = [&](Room* room, std::vector<Room*>& group, auto&& self) -> void {
			if (!room || visited.count(room) || connected_to_spawn.count(room)) return;
			visited.insert(room);
			group.push_back(room);
			for (Room* neighbor : room->connected_rooms) {
					self(neighbor, group, self);
			}
};
		for (Room* room : all_rooms_reference) {
			if (room && room->layer == 0) {
					mark_connected(room, mark_connected);
					break;
			}
		}
		for (Room* room : all_rooms_reference) {
			if (!visited.count(room) && !connected_to_spawn.count(room)) {
					std::vector<Room*> group;
					collect_group(room, group, collect_group);
					if (!group.empty()) {
								isolated_groups.push_back(std::move(group));
					}
			}
		}
        if (isolated_groups.empty()) {
            if (testing) {
                    std::cout << "[ConnectIsolated] All rooms connected after " << pass << " passes.\n";
            }
            ++passes_done;
            break;
        }
		if (testing) {
			std::cout << "[ConnectIsolated] Pass " << pass + 1 << " - " << isolated_groups.size() << " disconnected groups found | allowed intersections: " << allowed_intersections << "\n";
		}
		bool any_connection_made = false;
		for (const auto& group : isolated_groups) {
			if (group.empty()) continue;
			std::vector<Room*> sorted_group = group;
			std::sort(sorted_group.begin(), sorted_group.end(), [](Room* a, Room* b) {
					return a->connected_rooms.size() < b->connected_rooms.size();
             });
			for (Room* roomA : sorted_group) {
					std::vector<Room*> candidates;
                                        for (Room* candidate : all_rooms_reference) {
								if (candidate == roomA || connected_to_spawn.count(candidate)) continue;
								bool illegal = std::any_of(illegal_connections.begin(), illegal_connections.end(),
								[&](const std::pair<Room*, Room*>& p) {
													return (p.first == roomA && p.second == candidate) ||
													(p.first == candidate && p.second == roomA);
                                   });
								if (illegal) continue;
								std::unordered_set<Room*> check_visited;
								std::function<bool(Room*)> dfs = [&](Room* current) -> bool {
													if (!current || check_visited.count(current)) return false;
													if (current->layer == 0) return true;
													check_visited.insert(current);
													for (Room* neighbor : current->connected_rooms) {
																					if (dfs(neighbor)) return true;
													}
													return false;
};
								if (dfs(candidate)) {
													candidates.push_back(candidate);
								}
					}
					if (candidates.empty()) continue;
					std::sort(candidates.begin(), candidates.end(), [](Room* a, Room* b) {
								return a->connected_rooms.size() < b->connected_rooms.size();
               });
					if (candidates.size() > 5) candidates.resize(5);
                                        for (Room* roomB : candidates) {
                                        for (int attempt = 0; attempt < kIsolatedConnectionAttempts; ++attempt) {
                                                                                                        if (const auto* asset_ref = pick_random_asset()) {
                                                                                                                if (attempt_trail_connection(
                                                                                roomA,
                                                                                roomB,
                                                                                existing_areas,
                                                                                manifest_context,
                                                                                asset_lib,
                                                                                trail_rooms,
                                                                                allowed_intersections,
                                                                                asset_ref->data,
                                                                                asset_ref->name,
                                                                                map_assets_data,
                                                                                map_radius,
                                                                                testing,
                                                                                rng_,
                                                                                map_manifest,
                                                                                manifest_store,
                                                                                manifest_writer)) {
                                                                                                                               any_connection_made = true;
                                                                                                                               goto next_group;
                                                                                                        }
                                                                                                        }
                                                                }
                                        }
                        }
			next_group:;
		}
		if (!any_connection_made && testing) {
			std::cout << "[ConnectIsolated] No connections made on pass " << pass + 1 << "\n";
		}
		if ((pass + 1) % kIsolatedIntersectionIncreaseInterval == 0) {
			++allowed_intersections;
			if (testing) {
					std::cout << "[ConnectIsolated] Increasing allowed intersections to " << allowed_intersections << "\n";
			}
		}
        if (any_connection_made) {
            no_progress_count = 0;
        } else {
            ++no_progress_count;
        }
        ++passes_done;
	}
	std::cout << "[GenerateTrails] Isolated connections completed after " << passes_done << " pass(es); allowed intersections " << allowed_intersections << "\n";
}

void GenerateTrails::remove_connection(Room* a,
                                       Room* b,
                                       std::vector<std::unique_ptr<Room>>& trail_rooms,
                                       std::vector<Area>& existing_areas)
{
	if (!a || !b) return;
	std::cout << "[Debug][remove_connection] Removing connection between '"
	<< a->room_name << "' and '" << b->room_name << "'\n";
	a->remove_connecting_room(b);
	b->remove_connecting_room(a);
	std::cout << "[Debug][remove_connection] After removal, "
	<< a->room_name << " has " << a->connected_rooms.size() << " connections; " << b->room_name << " has " << b->connected_rooms.size() << " connections.\n";
	size_t before = trail_rooms.size();
	trail_rooms.erase(
	std::remove_if(trail_rooms.begin(), trail_rooms.end(),
	[&](const std::unique_ptr<Room>& trail) {
		if (!trail) return false;
                       bool connects_a = false, connects_b = false;
		for (Room* r : trail->connected_rooms) {
			if (r == a) connects_a = true;
			if (r == b) connects_b = true;
                       }
		if (connects_a && connects_b) {
			existing_areas.erase(
			std::remove_if(existing_areas.begin(), existing_areas.end(),
			[&](const Area& area) {
					return area.get_name() == trail->room_area->get_name();
                       }),
			existing_areas.end()
 );
                       return true;
                       }
                       return false;
                       }),
	trail_rooms.end()
 );
	std::cout << "[Debug][remove_connection] Removed "
	<< (before - trail_rooms.size()) << " trail room(s) connecting them.\n";
}

void GenerateTrails::remove_random_connection(std::vector<std::unique_ptr<Room>>& trail_rooms) {
	if (trail_rooms.empty()) {
		std::cout << "[Debug][remove_random_connection] No trail rooms to remove.\n";
		return;
	}
	std::uniform_int_distribution<size_t> dist(0, trail_rooms.size() - 1);
	size_t index = dist(rng_);
	Room* trail = trail_rooms[index].get();
	std::cout << "[Debug][remove_random_connection] Chosen trail index: "
	<< index << " (room: " << (trail ? trail->room_name : "<null>") << ")\n";
	if (!trail || trail->connected_rooms.size() < 2) {
		std::cout << "[Debug][remove_random_connection] Trail has fewer than 2 connections, skipping.\n";
		return;
	}
	Room* a = trail->connected_rooms[0];
	Room* b = trail->connected_rooms[1];
	std::cout << "[Debug][remove_random_connection] Disconnecting '"
	<< a->room_name << "' and '" << b->room_name << "'\n";
	if (a && b) {
		a->remove_connecting_room(b);
		b->remove_connecting_room(a);
		std::cout << "[Debug][remove_random_connection] After disconnect, "
		<< a->room_name << " has " << a->connected_rooms.size() << " connections; " << b->room_name << " has " << b->connected_rooms.size() << " connections.\n";
	}
	trail_rooms.erase(trail_rooms.begin() + index);
	std::cout << "[Debug][remove_random_connection] Erased trail room at index "
	<< index << ", remaining trail_rooms: " << trail_rooms.size() << "\n";
}

void GenerateTrails::remove_and_connect(std::vector<std::unique_ptr<Room>>& trail_rooms,
                                        std::vector<std::pair<Room*, Room*>>& illegal_connections,
                                        const std::string& manifest_context,
                                        AssetLibrary* asset_lib,
                                        std::vector<Area>& existing_areas,
                                        const nlohmann::json* map_assets_data,
                                        double map_radius,
                                        nlohmann::json* map_manifest,
                                        devmode::core::ManifestStore* manifest_store,
                                        Room::ManifestWriter manifest_writer)
{
	Room* target = nullptr;
	for (Room* room : all_rooms_reference) {
		if (room && room->layer > 2 && room->connected_rooms.size() > 3) {
			if (!target || room->connected_rooms.size() > target->connected_rooms.size()) {
					target = room;
			}
		}
	}
	if (!target) {
		std::cout << "[Debug][remove_and_connect] No target room with layer > 2 and >3 connections found.\n";
		return;
	}
	std::cout << "[Debug][remove_and_connect] Selected target room '" << target->room_name
	<< "' with " << target->connected_rooms.size() << " connections.\n";
	Room* most_connected = nullptr;
	for (Room* neighbor : target->connected_rooms) {
		if (neighbor->connected_rooms.size() <= 3) continue;
		if (!most_connected || neighbor->connected_rooms.size() > most_connected->connected_rooms.size()) {
			most_connected = neighbor;
		}
	}
	if (!most_connected) {
		std::cout << "[Debug][remove_and_connect] No neighbor with >3 connections found for target.\n";
		return;
	}
	std::cout << "[Debug][remove_and_connect] Selected neighbor '" << most_connected->room_name
	<< "' with " << most_connected->connected_rooms.size() << " connections.\n";
	remove_connection(target, most_connected, trail_rooms, existing_areas);
	illegal_connections.emplace_back(target, most_connected);
	std::cout << "[Debug][remove_and_connect] Marked connection illegal: ('"
	<< target->room_name << "', '" << most_connected->room_name << "')\n";
        find_and_connect_isolated(manifest_context, asset_lib, existing_areas, trail_rooms, map_assets_data, map_radius, map_manifest, manifest_store, manifest_writer);
	std::cout << "[Debug][remove_and_connect] Completed reconnect attempt for isolated groups.\n";
}

void GenerateTrails::circular_connection(std::vector<std::unique_ptr<Room>>& trail_rooms,
                                         const std::string& manifest_context,
                                         AssetLibrary* asset_lib,
                                         std::vector<Area>& existing_areas,
                                         const nlohmann::json* map_assets_data,
                                         double map_radius,
                                         nlohmann::json* map_manifest,
                                         devmode::core::ManifestStore* manifest_store,
                                         Room::ManifestWriter manifest_writer)
{
	if (all_rooms_reference.empty()) {
		std::cout << "[Debug][circular_connection] No rooms available.\n";
		return;
	}
	Room* outermost = nullptr;
	int max_layer = -1;
	for (Room* room : all_rooms_reference) {
		if (room && room->layer > max_layer) {
			max_layer = room->layer;
			outermost = room;
		}
	}
	if (!outermost) {
		std::cout << "[Debug][circular_connection] No outermost room found.\n";
		return;
	}
	std::cout << "[Debug][circular_connection] Outermost room: '" << outermost->room_name
	<< "', layer " << outermost->layer << "\n";
	std::unordered_set<Room*> lineage_set;
	for (Room* r = outermost; r; r = r->parent) {
		lineage_set.insert(r);
		if (r->layer == 0) break;
	}
        Room* current = outermost;
        int fail_counter = 0;
        bool first_iteration = true;
        while ((first_iteration || !lineage_set.count(current)) && fail_counter < 10) {
                first_iteration = false;
		std::vector<Room*> candidates;
		auto add_candidate = [&](Room* r) {
			if (!r || r->layer <= 1) return;
			if (std::find(current->connected_rooms.begin(), current->connected_rooms.end(), r) != current->connected_rooms.end())
			return;
			candidates.push_back(r);
};
		add_candidate(current->right_sibling);
		if (current->right_sibling) {
			add_candidate(current->right_sibling->parent);
			for (Room* child : current->right_sibling->children)
			add_candidate(child);
		}
		add_candidate(current->left_sibling);
		if (current->left_sibling) {
			add_candidate(current->left_sibling->parent);
			for (Room* child : current->left_sibling->children)
			add_candidate(child);
		}
		std::shuffle(candidates.begin(), candidates.end(), rng_);
		std::cout << "[Debug][circular_connection] Candidate count for '" << current->room_name
		<< "': " << candidates.size() << "\n";
		if (candidates.empty()) {
			std::cout << "[Debug][circular_connection] No candidates, breaking loop.\n";
			break;
		}
		Room* next = candidates.front();
		std::cout << "[Debug][circular_connection] Attempting to connect '"
		<< current->room_name << "' -> '" << next->room_name << "'\n";
		bool connected = false;
                for (int attempt = 0; attempt < 1000; ++attempt) {
                        if (const auto* asset_ref = pick_random_asset()) {
                                if (attempt_trail_connection(current,
                                                             next,
                                                             existing_areas,
                                                             manifest_context,
                                                             asset_lib,
                                                             trail_rooms,
                                                             1,
                                                             asset_ref->data,
                                                             asset_ref->name,
                                                             map_assets_data,
                                                             map_radius,
                                                             testing,
                                                             rng_,
                                                             map_manifest,
                                                             manifest_store,
                                                             manifest_writer)) {
                                        std::cout << "[Debug][circular_connection] Connected on attempt "
                                        << attempt + 1 << " using asset: " << asset_ref->name << "\n";
                                        current = next;
                                        connected = true;
                                        break;
                                }
                        }
                }
		if (!connected) {
			std::cout << "[Debug][circular_connection] Failed to connect '"
			<< current->room_name << "' -> '" << next->room_name << "' after 1000 attempts.\n";
			++fail_counter;
		} else {
			fail_counter = 0;
		}
	}
	std::cout << "[Debug][circular_connection] Circular connection complete.\n";
}
