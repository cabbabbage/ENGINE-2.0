#include "generate_trails.hpp"

#include "utils/display_color.hpp"
#include "utils/map_grid_settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numeric>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using json = nlohmann::json;

namespace {

constexpr int kNearestNeighborCount = 4;
constexpr double kLoopConnectionChance = 0.35;
constexpr double kLoopCapRatio = 0.25;
constexpr int kTrailPairAttempts = 96;
constexpr int kSectionPlacementAttempts = 128;
constexpr double kCurvynessShiftScaleWorldPx = 20.0;
constexpr double kPointEpsilon = 1e-6;

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

struct TrailSection {
    double distance = 0.0;
    int width = 0;
    double shift = 0.0;
    Vec2 center{};
    Vec2 left{};
    Vec2 right{};
};

struct TrailBuildResult {
    SDL_Point start_tip{0, 0};
    SDL_Point end_tip{0, 0};
    std::vector<TrailSection> sections;
    std::vector<SDL_Point> polygon;
};

struct DisjointSet {
    explicit DisjointSet(std::size_t count) : parent(count), rank(count, 0) {
        std::iota(parent.begin(), parent.end(), 0);
    }

    std::size_t find(std::size_t x) {
        if (parent[x] != x) {
            parent[x] = find(parent[x]);
        }
        return parent[x];
    }

    bool unite(std::size_t a, std::size_t b) {
        std::size_t root_a = find(a);
        std::size_t root_b = find(b);
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

    std::vector<std::size_t> parent;
    std::vector<int> rank;
};

struct PointerPairHash {
    std::size_t operator()(const std::pair<Room*, Room*>& value) const noexcept {
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

Vec2 to_vec2(const SDL_Point& point) {
    return Vec2{static_cast<double>(point.x), static_cast<double>(point.y)};
}

SDL_Point to_point(const Vec2& value) {
    return SDL_Point{
        static_cast<int>(std::lround(value.x)),
        static_cast<int>(std::lround(value.y)),
    };
}

Vec2 add(const Vec2& a, const Vec2& b) {
    return Vec2{a.x + b.x, a.y + b.y};
}

Vec2 subtract(const Vec2& a, const Vec2& b) {
    return Vec2{a.x - b.x, a.y - b.y};
}

Vec2 scale(const Vec2& a, double value) {
    return Vec2{a.x * value, a.y * value};
}

double dot(const Vec2& a, const Vec2& b) {
    return a.x * b.x + a.y * b.y;
}

double cross(const Vec2& a, const Vec2& b) {
    return a.x * b.y - a.y * b.x;
}

double length(const Vec2& a) {
    return std::hypot(a.x, a.y);
}

bool nearly_equal(double a, double b, double eps = kPointEpsilon) {
    return std::abs(a - b) <= eps;
}

bool on_segment(const Vec2& a, const Vec2& b, const Vec2& c) {
    const double min_x = std::min(a.x, c.x) - kPointEpsilon;
    const double max_x = std::max(a.x, c.x) + kPointEpsilon;
    const double min_y = std::min(a.y, c.y) - kPointEpsilon;
    const double max_y = std::max(a.y, c.y) + kPointEpsilon;
    if (b.x < min_x || b.x > max_x || b.y < min_y || b.y > max_y) {
        return false;
    }
    return nearly_equal(cross(subtract(b, a), subtract(c, a)), 0.0);
}

bool segments_intersect(const Vec2& p1, const Vec2& p2, const Vec2& q1, const Vec2& q2) {
    const Vec2 r = subtract(p2, p1);
    const Vec2 s = subtract(q2, q1);
    const double rxs = cross(r, s);
    const double qpxr = cross(subtract(q1, p1), r);

    if (nearly_equal(rxs, 0.0) && nearly_equal(qpxr, 0.0)) {
        return on_segment(p1, q1, p2) || on_segment(p1, q2, p2) || on_segment(q1, p1, q2) || on_segment(q1, p2, q2);
    }
    if (nearly_equal(rxs, 0.0) && !nearly_equal(qpxr, 0.0)) {
        return false;
    }

    const double t = cross(subtract(q1, p1), s) / rxs;
    const double u = cross(subtract(q1, p1), r) / rxs;
    return t >= -kPointEpsilon && t <= 1.0 + kPointEpsilon && u >= -kPointEpsilon && u <= 1.0 + kPointEpsilon;
}

bool point_in_polygon(const Vec2& point, const std::vector<SDL_Point>& polygon) {
    if (polygon.size() < 3) {
        return false;
    }
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vec2 a = to_vec2(polygon[i]);
        const Vec2 b = to_vec2(polygon[j]);

        if (on_segment(a, point, b)) {
            return true;
        }

        const bool intersect =
            ((a.y > point.y) != (b.y > point.y)) &&
            (point.x < (b.x - a.x) * (point.y - a.y) / ((b.y - a.y) + kPointEpsilon) + a.x);
        if (intersect) {
            inside = !inside;
        }
    }
    return inside;
}

bool polygons_overlap(const std::vector<SDL_Point>& a, const std::vector<SDL_Point>& b) {
    if (a.size() < 3 || b.size() < 3) {
        return false;
    }

    for (std::size_t i = 0; i < a.size(); ++i) {
        const Vec2 a0 = to_vec2(a[i]);
        const Vec2 a1 = to_vec2(a[(i + 1) % a.size()]);
        for (std::size_t j = 0; j < b.size(); ++j) {
            const Vec2 b0 = to_vec2(b[j]);
            const Vec2 b1 = to_vec2(b[(j + 1) % b.size()]);
            if (segments_intersect(a0, a1, b0, b1)) {
                return true;
            }
        }
    }

    if (point_in_polygon(to_vec2(a.front()), b)) {
        return true;
    }
    if (point_in_polygon(to_vec2(b.front()), a)) {
        return true;
    }
    return false;
}

bool segment_intersects_polygon(const Vec2& a, const Vec2& b, const std::vector<SDL_Point>& polygon) {
    if (polygon.size() < 2) {
        return false;
    }
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Vec2 p0 = to_vec2(polygon[i]);
        const Vec2 p1 = to_vec2(polygon[(i + 1) % polygon.size()]);
        if (segments_intersect(a, b, p0, p1)) {
            return true;
        }
    }
    return false;
}

SDL_Point compute_edge_point(const SDL_Point& center, const SDL_Point& toward, const Area* area) {
    if (!area) {
        return center;
    }

    const double dx = static_cast<double>(toward.x - center.x);
    const double dy = static_cast<double>(toward.y - center.y);
    const double len = std::hypot(dx, dy);
    if (len <= 0.0) {
        return center;
    }

    const double dir_x = dx / len;
    const double dir_y = dy / len;
    constexpr int kMaxSteps = 20000;
    constexpr double kStepSize = 1.0;

    double current_distance = 0.0;
    SDL_Point edge = center;
    for (int i = 1; i <= kMaxSteps; ++i) {
        current_distance += kStepSize;
        const int px = static_cast<int>(std::lround(static_cast<double>(center.x) + dir_x * current_distance));
        const int py = static_cast<int>(std::lround(static_cast<double>(center.y) + dir_y * current_distance));
        SDL_Point candidate{px, py};
        if (area->contains_point(candidate)) {
            edge = candidate;
        } else {
            break;
        }
    }
    return edge;
}

bool point_inside_any_room(const Vec2& point, const Area* room_a, const Area* room_b) {
    const SDL_Point p = to_point(point);
    if (room_a && room_a->contains_point(p)) {
        return true;
    }
    if (room_b && room_b->contains_point(p)) {
        return true;
    }
    return false;
}

std::vector<double> build_section_distances(double trail_length, const std::vector<double>& required_distances) {
    std::vector<double> distances;
    if (trail_length <= 0.0) {
        distances.push_back(0.0);
        return distances;
    }

    distances.push_back(0.0);
    const double spacing = static_cast<double>(std::max(1, kTrailPerpendicularSectionSpacingWorldPx));
    for (double d = spacing; d < trail_length; d += spacing) {
        distances.push_back(d);
    }
    distances.push_back(trail_length);
    for (double required : required_distances) {
        if (required > 0.0 && required < trail_length) {
            distances.push_back(required);
        }
    }

    std::sort(distances.begin(), distances.end());
    distances.erase(std::unique(distances.begin(), distances.end(), [](double lhs, double rhs) {
                        return std::abs(lhs - rhs) <= kPointEpsilon;
                    }),
                    distances.end());
    return distances;
}

bool shift_is_unique(const std::vector<double>& used_shifts, double value) {
    for (double existing : used_shifts) {
        if (std::abs(existing - value) <= 0.01) {
            return false;
        }
    }
    return true;
}

bool place_sections(const Vec2& start,
                    const Vec2& dir,
                    const Vec2& normal,
                    const std::vector<double>& distances,
                    int min_width,
                    int max_width,
                    int curvyness,
                    const std::vector<std::vector<SDL_Point>>& existing_trails,
                    const Area* room_a,
                    const Area* room_b,
                    std::mt19937& rng,
                    std::vector<TrailSection>* out_sections) {
    if (!out_sections) {
        return false;
    }
    out_sections->clear();
    out_sections->reserve(distances.size());

    std::uniform_int_distribution<int> width_dist(min_width, max_width);
    std::vector<double> used_shifts;
    used_shifts.reserve(distances.size());

    for (double distance_along : distances) {
        bool placed = false;
        for (int attempt = 0; attempt < kSectionPlacementAttempts && !placed; ++attempt) {
            const int width = width_dist(rng);
            const double half_width = static_cast<double>(std::max(1, width)) * 0.5;
            const double curvy_limit = std::max(0.0, static_cast<double>(curvyness) * kCurvynessShiftScaleWorldPx);
            const double straddle_limit = std::max(0.0, half_width - 1.0);
            const double shift_limit = std::min(curvy_limit, straddle_limit);

            double shift = 0.0;
            if (shift_limit > 0.0) {
                std::uniform_real_distribution<double> shift_dist(-shift_limit, shift_limit);
                shift = shift_dist(rng);
                if (!shift_is_unique(used_shifts, shift)) {
                    continue;
                }
            }

            const Vec2 base_center = add(start, scale(dir, distance_along));
            const Vec2 section_center = add(base_center, scale(normal, shift));
            const Vec2 left = add(section_center, scale(normal, half_width));
            const Vec2 right = subtract(section_center, scale(normal, half_width));

            bool overlap = false;
            // Allow trail overlap while this section still lies inside either endpoint room.
            if (!(point_inside_any_room(left, room_a, room_b) && point_inside_any_room(right, room_a, room_b))) {
                for (const auto& trail_polygon : existing_trails) {
                    if (trail_polygon.size() < 3) {
                        continue;
                    }
                    if (point_in_polygon(left, trail_polygon) || point_in_polygon(right, trail_polygon)) {
                        overlap = true;
                        break;
                    }
                    if (segment_intersects_polygon(left, right, trail_polygon)) {
                        overlap = true;
                        break;
                    }
                }
            }
            if (overlap) {
                continue;
            }

            TrailSection section;
            section.distance = distance_along;
            section.width = width;
            section.shift = shift;
            section.center = section_center;
            section.left = left;
            section.right = right;
            out_sections->push_back(section);
            used_shifts.push_back(shift);
            placed = true;
        }

        if (!placed) {
            return false;
        }
    }
    return true;
}

std::vector<SDL_Point> build_polygon(const Vec2& start_tip,
                                     const Vec2& end_tip,
                                     const std::vector<TrailSection>& sections) {
    std::vector<SDL_Point> polygon;
    if (sections.empty()) {
        return polygon;
    }

    polygon.reserve(2 + sections.size() * 2);
    polygon.push_back(to_point(start_tip));
    for (const TrailSection& section : sections) {
        polygon.push_back(to_point(section.left));
    }
    polygon.push_back(to_point(end_tip));
    for (std::size_t i = sections.size(); i-- > 0;) {
        polygon.push_back(to_point(sections[i].right));
    }
    return polygon;
}

bool polygons_overlap_outside_rooms(const std::vector<SDL_Point>& candidate,
                                    const std::vector<SDL_Point>& existing,
                                    const Area* room_a,
                                    const Area* room_b) {
    if (candidate.size() < 3 || existing.size() < 3) {
        return false;
    }

    auto outside_endpoint_rooms = [&](const SDL_Point& p) {
        if (room_a && room_a->contains_point(p)) {
            return false;
        }
        if (room_b && room_b->contains_point(p)) {
            return false;
        }
        return true;
    };

    for (std::size_t i = 0; i < candidate.size(); ++i) {
        const SDL_Point a0 = candidate[i];
        const SDL_Point a1 = candidate[(i + 1) % candidate.size()];
        if (!outside_endpoint_rooms(a0) && !outside_endpoint_rooms(a1)) {
            continue;
        }
        const Vec2 av0 = to_vec2(a0);
        const Vec2 av1 = to_vec2(a1);
        for (std::size_t j = 0; j < existing.size(); ++j) {
            const Vec2 bv0 = to_vec2(existing[j]);
            const Vec2 bv1 = to_vec2(existing[(j + 1) % existing.size()]);
            if (segments_intersect(av0, av1, bv0, bv1)) {
                return true;
            }
        }
    }

    for (const SDL_Point& p : candidate) {
        if (!outside_endpoint_rooms(p)) {
            continue;
        }
        if (point_in_polygon(to_vec2(p), existing)) {
            return true;
        }
    }
    for (const SDL_Point& p : existing) {
        if (!outside_endpoint_rooms(p)) {
            continue;
        }
        if (point_in_polygon(to_vec2(p), candidate)) {
            return true;
        }
    }
    return false;
}

bool build_trail_layout(const SDL_Point& start_tip,
                        const SDL_Point& end_tip,
                        int min_width,
                        int max_width,
                        int curvyness,
                        const std::vector<std::vector<SDL_Point>>& existing_trails,
                        const std::vector<double>& required_distances,
                        const Area* room_a,
                        const Area* room_b,
                        std::mt19937& rng,
                        TrailBuildResult* out_result) {
    if (!out_result) {
        return false;
    }
    out_result->sections.clear();
    out_result->polygon.clear();
    out_result->start_tip = start_tip;
    out_result->end_tip = end_tip;

    if (max_width < min_width) {
        std::swap(min_width, max_width);
    }
    min_width = std::max(1, min_width);
    max_width = std::max(min_width, max_width);
    curvyness = std::max(0, curvyness);

    const Vec2 start = to_vec2(start_tip);
    const Vec2 end = to_vec2(end_tip);
    const Vec2 axis = subtract(end, start);
    const double trail_length = length(axis);
    if (trail_length <= 1.0) {
        return false;
    }

    const Vec2 dir = scale(axis, 1.0 / trail_length);
    const Vec2 normal{-dir.y, dir.x};
    const std::vector<double> distances = build_section_distances(trail_length, required_distances);
    if (distances.empty()) {
        return false;
    }

    for (int attempt = 0; attempt < kTrailPairAttempts; ++attempt) {
        std::vector<TrailSection> sections;
        if (!place_sections(start, dir, normal, distances, min_width, max_width, curvyness, existing_trails, room_a, room_b, rng, &sections)) {
            continue;
        }

        std::vector<SDL_Point> polygon = build_polygon(start, end, sections);
        if (polygon.size() < 4) {
            continue;
        }

        bool overlap = false;
        for (const auto& trail_polygon : existing_trails) {
            if (polygons_overlap_outside_rooms(polygon, trail_polygon, room_a, room_b)) {
                overlap = true;
                break;
            }
        }
        if (overlap) {
            continue;
        }

        out_result->sections = std::move(sections);
        out_result->polygon = std::move(polygon);
        return true;
    }

    return false;
}

bool attempt_trail_connection(Room* a,
                              Room* b,
                              std::vector<std::vector<SDL_Point>>& existing_trails,
                              const std::string& manifest_context,
                              AssetLibrary* asset_lib,
                              std::vector<std::unique_ptr<Room>>& trail_rooms,
                              nlohmann::json* trail_config,
                              const std::string& trail_name,
                              double map_radius,
                              bool testing,
                              std::mt19937& rng,
                              nlohmann::json* map_manifest,
                              devmode::core::ManifestStore* manifest_store,
                              Room::ManifestWriter manifest_writer) {
    if (!a || !b || !a->room_area || !b->room_area || !trail_config) {
        return false;
    }

    json& config = *trail_config;
    const int min_width = config.value("min_width", 40);
    const int max_width = config.value("max_width", min_width);
    const int curvyness = config.value("curvyness", 2);
    const std::string name = config.value("name", trail_name.empty() ? std::string("trail_segment") : trail_name);

    const SDL_Point a_center = a->room_area->get_center();
    const SDL_Point b_center = b->room_area->get_center();
    const SDL_Point a_edge = compute_edge_point(a_center, b_center, a->room_area.get());
    const SDL_Point b_edge = compute_edge_point(b_center, a_center, b->room_area.get());

    const Vec2 center_start = to_vec2(a_center);
    const Vec2 center_end = to_vec2(b_center);
    const Vec2 center_axis = subtract(center_end, center_start);
    const double center_len = length(center_axis);
    if (center_len <= 1.0) {
        return false;
    }
    const Vec2 center_dir = scale(center_axis, 1.0 / center_len);

    auto project_distance = [&](const SDL_Point& p) {
        return dot(subtract(to_vec2(p), center_start), center_dir);
    };

    std::vector<double> required_distances;
    required_distances.reserve(2);
    required_distances.push_back(project_distance(a_edge));
    required_distances.push_back(project_distance(b_edge));

    TrailBuildResult build_result;
    if (!build_trail_layout(a_center,
                            b_center,
                            min_width,
                            max_width,
                            curvyness,
                            existing_trails,
                            required_distances,
                            a->room_area.get(),
                            b->room_area.get(),
                            rng,
                            &build_result)) {
        return false;
    }

    Area candidate("trail_candidate", build_result.polygon, 3);
    auto trail_room = std::make_unique<Room>(
        a->map_origin,
        "trail",
        name,
        nullptr,
        manifest_context,
        asset_lib,
        &candidate,
        trail_config,
        MapGridSettings::defaults(),
        map_radius,
        "trails_data",
        map_manifest,
        manifest_store,
        manifest_context,
        manifest_writer);

    a->add_connecting_room(trail_room.get());
    b->add_connecting_room(trail_room.get());
    trail_room->add_connecting_room(a);
    trail_room->add_connecting_room(b);

    trail_room->camera_height_px = std::clamp(config.value("camera_height_px", 1000), 1, 2000);
    trail_room->camera_tilt_deg = std::clamp(config.value("camera_tilt_deg", 60.0f), 0.0f, 360.0f);
    trail_room->camera_zoom_percent = std::clamp(config.value("camera_zoom_percent", 0), 0, 100);
    trail_room->camera_center_dx = config.value("camera_center_dx", 0);
    trail_room->camera_center_dz = config.value("camera_center_dz", 0);

    existing_trails.push_back(build_result.polygon);
    trail_rooms.push_back(std::move(trail_room));

    if (testing) {
        std::cout << "[TrailGeometry] Trail placed: " << a->room_name << " <-> " << b->room_name << "\n";
    }
    return true;
}

} // namespace

GenerateTrails::GenerateTrails(nlohmann::json& trail_data, std::vector<SDL_Color> reserved_colors)
    : rng_(std::random_device{}()), trails_data_(&trail_data), trail_colors_(std::move(reserved_colors)) {
    if (!trail_data.is_object()) {
        trail_data = nlohmann::json::object();
    }
    for (auto it = trail_data.begin(); it != trail_data.end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        available_assets_.push_back({it.key(), &(*it)});
        utils::display_color::ensure(*available_assets_.back().data, trail_colors_);
    }
    if (available_assets_.empty()) {
        throw std::runtime_error("[GenerateTrails] No trail templates found in trails_data");
    }
}

void GenerateTrails::set_all_rooms_reference(const std::vector<Room*>& rooms) {
    all_rooms_reference_ = rooms;
}

std::vector<std::unique_ptr<Room>> GenerateTrails::generate_trails(const std::vector<std::pair<Room*, Room*>>& room_pairs,
                                                                   const std::vector<Area>& existing_areas,
                                                                   const std::string& manifest_context,
                                                                   AssetLibrary* asset_lib,
                                                                   double map_radius,
                                                                   nlohmann::json* map_manifest,
                                                                   devmode::core::ManifestStore* manifest_store,
                                                                   Room::ManifestWriter manifest_writer) {
    (void)existing_areas;
    std::vector<std::unique_ptr<Room>> trail_rooms;
    std::vector<std::vector<SDL_Point>> existing_trails;

    const auto connection_plan = plan_maze_connections(all_rooms_reference_, room_pairs);
    if (testing_) {
        std::cout << "[GenerateTrails] Planned " << connection_plan.size() << " connections\n";
    }

    for (const auto& [a, b] : connection_plan) {
        if (!a || !b) {
            continue;
        }
        bool success = false;
        for (int attempt = 0; attempt < kTrailPairAttempts && !success; ++attempt) {
            if (const auto* asset_ref = pick_random_asset()) {
                success = attempt_trail_connection(a,
                                                   b,
                                                   existing_trails,
                                                   manifest_context,
                                                   asset_lib,
                                                   trail_rooms,
                                                   asset_ref->data,
                                                   asset_ref->name,
                                                   map_radius,
                                                   testing_,
                                                   rng_,
                                                   map_manifest,
                                                   manifest_store,
                                                   manifest_writer);
            }
        }
        if (testing_ && !success) {
            std::cout << "[GenerateTrails] Failed to connect " << a->room_name << " and " << b->room_name << "\n";
        }
    }

    return trail_rooms;
}

const GenerateTrails::TrailTemplateRef* GenerateTrails::pick_random_asset() {
    if (available_assets_.empty()) {
        return nullptr;
    }
    std::uniform_int_distribution<std::size_t> dist(0, available_assets_.size() - 1);
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
        if (!room) {
            continue;
        }
        if (seen.insert(room).second) {
            unique_rooms.push_back(room);
        }
    }
    if (unique_rooms.size() < 2) {
        return planned;
    }

    std::unordered_map<Room*, std::size_t> index;
    index.reserve(unique_rooms.size());
    for (std::size_t i = 0; i < unique_rooms.size(); ++i) {
        index[unique_rooms[i]] = i;
    }

    DisjointSet dsu(unique_rooms.size());
    std::unordered_set<std::pair<Room*, Room*>, PointerPairHash, PointerPairEqual> blocked_pairs;
    blocked_pairs.reserve(unique_rooms.size() * kNearestNeighborCount + forced_connections.size());

    for (const auto& edge : forced_connections) {
        Room* a = edge.first;
        Room* b = edge.second;
        if (!a || !b) {
            continue;
        }
        auto ia = index.find(a);
        auto ib = index.find(b);
        if (ia == index.end() || ib == index.end()) {
            continue;
        }
        dsu.unite(ia->second, ib->second);
        auto key = canonical_pair(a, b);
        if (!key.first || !key.second) {
            continue;
        }
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

    for (std::size_t i = 0; i < unique_rooms.size(); ++i) {
        Room* a = unique_rooms[i];
        if (!a) {
            continue;
        }
        std::vector<std::pair<double, Room*>> nearest;
        nearest.reserve(unique_rooms.size());
        const auto [ax, ay] = room_center(a);
        for (Room* b : unique_rooms) {
            if (!b || b == a) {
                continue;
            }
            const auto [bx, by] = room_center(b);
            const double dx = ax - bx;
            const double dy = ay - by;
            nearest.emplace_back(dx * dx + dy * dy, b);
        }
        std::sort(nearest.begin(), nearest.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });
        if (nearest.size() > static_cast<std::size_t>(kNearestNeighborCount)) {
            nearest.resize(static_cast<std::size_t>(kNearestNeighborCount));
        }
        for (const auto& candidate : nearest) {
            Room* b = candidate.second;
            auto key = canonical_pair(a, b);
            if (!key.first || !key.second) {
                continue;
            }
            if (!blocked_pairs.insert(key).second) {
                continue;
            }
            candidates.push_back(CandidateEdge{a, b, std::sqrt(candidate.first), 0.0});
        }
    }

    std::uniform_real_distribution<double> jitter_dist(0.0, 1.0);
    for (auto& candidate : candidates) {
        candidate.jitter = jitter_dist(rng_);
    }
    std::sort(candidates.begin(), candidates.end(), [](const CandidateEdge& lhs, const CandidateEdge& rhs) {
        const double lw = lhs.distance + lhs.jitter * 25.0;
        const double rw = rhs.distance + rhs.jitter * 25.0;
        if (lw == rw) {
            if (lhs.a == rhs.a) {
                return lhs.b < rhs.b;
            }
            return lhs.a < rhs.a;
        }
        return lw < rw;
    });

    std::uniform_real_distribution<double> loop_dist(0.0, 1.0);
    std::size_t loop_cap = static_cast<std::size_t>(std::ceil(unique_rooms.size() * kLoopCapRatio));
    if (loop_cap == 0 && unique_rooms.size() > 2) {
        loop_cap = 1;
    }
    std::size_t loops_added = 0;

    for (const auto& candidate : candidates) {
        Room* a = candidate.a;
        Room* b = candidate.b;
        if (!a || !b) {
            continue;
        }
        auto ia = index.find(a);
        auto ib = index.find(b);
        if (ia == index.end() || ib == index.end()) {
            continue;
        }
        if (dsu.unite(ia->second, ib->second)) {
            planned.emplace_back(a, b);
        } else if (loops_added < loop_cap && loop_dist(rng_) < kLoopConnectionChance) {
            planned.emplace_back(a, b);
            ++loops_added;
        }
    }

    auto rebuild_components = [&]() {
        std::unordered_map<std::size_t, std::vector<std::size_t>> components;
        components.reserve(unique_rooms.size());
        for (std::size_t i = 0; i < unique_rooms.size(); ++i) {
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
        std::vector<std::vector<std::size_t>> groups;
        groups.reserve(components.size());
        for (auto& entry : components) {
            groups.push_back(std::move(entry.second));
        }

        std::size_t base_index = 0;
        for (std::size_t i = 1; i < groups.size(); ++i) {
            if (!groups[i].empty() && (groups[base_index].empty() || groups[i].size() < groups[base_index].size())) {
                base_index = i;
            }
        }

        const auto& base_group = groups[base_index];
        if (base_group.empty()) {
            break;
        }

        double best_dist = std::numeric_limits<double>::max();
        std::size_t best_a = base_group.front();
        std::size_t best_b = base_group.front();
        for (std::size_t idx_a : base_group) {
            const auto [ax, ay] = cached_centers[idx_a];
            for (std::size_t g = 0; g < groups.size(); ++g) {
                if (g == base_index) {
                    continue;
                }
                for (std::size_t idx_b : groups[g]) {
                    const auto [bx, by] = cached_centers[idx_b];
                    const double dist = std::hypot(ax - bx, ay - by);
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_a = idx_a;
                        best_b = idx_b;
                    }
                }
            }
        }

        if (!std::isfinite(best_dist)) {
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

#ifdef ENGINE_WORLD_TESTS
namespace trail_generation::debug {

bool build_layout_for_tests(const SDL_Point& start_tip,
                            const SDL_Point& end_tip,
                            int min_width,
                            int max_width,
                            int curvyness,
                            const std::vector<Area>& existing_trails,
                            std::uint32_t seed,
                            TrailLayoutDebug* out_layout) {
    if (!out_layout) {
        return false;
    }
    std::vector<std::vector<SDL_Point>> existing_polygons;
    existing_polygons.reserve(existing_trails.size());
    for (const Area& area : existing_trails) {
        existing_polygons.push_back(area.get_points());
    }

    std::mt19937 rng(seed);
    TrailBuildResult build_result;
    if (!build_trail_layout(start_tip, end_tip, min_width, max_width, curvyness, existing_polygons, {}, nullptr, nullptr, rng, &build_result)) {
        return false;
    }

    out_layout->start_tip = build_result.start_tip;
    out_layout->end_tip = build_result.end_tip;
    out_layout->polygon = build_result.polygon;
    out_layout->sections.clear();
    out_layout->sections.reserve(build_result.sections.size());
    for (const TrailSection& section : build_result.sections) {
        SectionDebug debug_section;
        debug_section.distance_along_centerline = section.distance;
        debug_section.width = section.width;
        debug_section.shift = section.shift;
        debug_section.left = to_point(section.left);
        debug_section.right = to_point(section.right);
        out_layout->sections.push_back(debug_section);
    }
    return true;
}

} // namespace trail_generation::debug
#endif
