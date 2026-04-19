#include "generate_trails.hpp"

#include "utils/display_color.hpp"
#include "utils/log.hpp"
#include "utils/map_grid_settings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
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
constexpr double kCenterlineCurvatureScaleWorldPx = 45.0;
constexpr double kCenterlineControlSpacingWorldPx = 300.0;
constexpr double kCenterlineValidationSpacingWorldPx = 48.0;
constexpr double kPointEpsilon = 1e-6;

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

struct Bounds {
    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
};

struct RoomObstacle {
    Room* room = nullptr;
    const Area* area = nullptr;
    Bounds bounds{};
};

struct TrailObstacle {
    std::vector<SDL_Point> polygon;
    Bounds bounds{};
};

struct CenterlineSample {
    double distance = 0.0;
    Vec2 point{};
    Vec2 tangent{};
    Vec2 normal{};
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

enum class TrailFailureReason {
    None = 0,
    InvalidArgs,
    DegenerateCenterline,
    EmptySectionDistances,
    EmptyValidationDistances,
    BlockedByRoom,
    BlockedByTrail,
    SectionPlacementFailed,
    PolygonTooSmall,
    LayoutAttemptsExhausted
};

struct TrailAttemptStats {
    int layout_attempts = 0;
    int straight_attempts = 0;
    int curved_attempts = 0;
    int centerline_room_rejections = 0;
    int centerline_trail_rejections = 0;
    int section_failures = 0;
    int polygon_failures = 0;
    TrailFailureReason last_failure = TrailFailureReason::None;
};

struct GenerationPerfSummary {
    int total_connections = 0;
    int successful_connections = 0;
    int failed_connections = 0;
    int total_asset_attempts = 0;
    int total_layout_attempts = 0;
    int total_straight_attempts = 0;
    int total_curved_attempts = 0;
    int total_room_rejections = 0;
    int total_trail_rejections = 0;
    int total_section_failures = 0;
    int total_polygon_failures = 0;
    int total_rooms_considered = 0;
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

const char* failure_reason_name(TrailFailureReason reason) {
    switch (reason) {
        case TrailFailureReason::None: return "none";
        case TrailFailureReason::InvalidArgs: return "invalid_args";
        case TrailFailureReason::DegenerateCenterline: return "degenerate_centerline";
        case TrailFailureReason::EmptySectionDistances: return "empty_section_distances";
        case TrailFailureReason::EmptyValidationDistances: return "empty_validation_distances";
        case TrailFailureReason::BlockedByRoom: return "blocked_by_room";
        case TrailFailureReason::BlockedByTrail: return "blocked_by_trail";
        case TrailFailureReason::SectionPlacementFailed: return "section_placement_failed";
        case TrailFailureReason::PolygonTooSmall: return "polygon_too_small";
        case TrailFailureReason::LayoutAttemptsExhausted: return "layout_attempts_exhausted";
        default: return "unknown";
    }
}

template <typename ClockDuration>
double duration_ms(ClockDuration value) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(value).count();
}

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

Vec2 normalize_or_default(const Vec2& value, const Vec2& fallback) {
    const double len = length(value);
    if (len <= kPointEpsilon) {
        return fallback;
    }
    return scale(value, 1.0 / len);
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

Bounds bounds_from_points(const std::vector<SDL_Point>& points) {
    Bounds bounds{};
    if (points.empty()) {
        return bounds;
    }

    bounds.min_x = bounds.max_x = static_cast<double>(points.front().x);
    bounds.min_y = bounds.max_y = static_cast<double>(points.front().y);
    for (const SDL_Point& point : points) {
        bounds.min_x = std::min(bounds.min_x, static_cast<double>(point.x));
        bounds.min_y = std::min(bounds.min_y, static_cast<double>(point.y));
        bounds.max_x = std::max(bounds.max_x, static_cast<double>(point.x));
        bounds.max_y = std::max(bounds.max_y, static_cast<double>(point.y));
    }
    return bounds;
}

Bounds bounds_from_area(const Area& area) {
    int min_x = 0;
    int min_y = 0;
    int max_x = 0;
    int max_y = 0;
    std::tie(min_x, min_y, max_x, max_y) = area.get_bounds();
    return Bounds{
        static_cast<double>(min_x),
        static_cast<double>(min_y),
        static_cast<double>(max_x),
        static_cast<double>(max_y),
    };
}

bool bounds_contains(const Bounds& bounds, const SDL_Point& point) {
    const double px = static_cast<double>(point.x);
    const double py = static_cast<double>(point.y);
    return px >= bounds.min_x && px <= bounds.max_x && py >= bounds.min_y && py <= bounds.max_y;
}

bool point_inside_any_room(const SDL_Point& point, const Area* room_a, const Area* room_b) {
    if (room_a && room_a->contains_point(point)) {
        return true;
    }
    if (room_b && room_b->contains_point(point)) {
        return true;
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
    constexpr double kMaxDistance = 32768.0;

    auto point_at_distance = [&](double distance) {
        return SDL_Point{
            static_cast<int>(std::lround(static_cast<double>(center.x) + dir_x * distance)),
            static_cast<int>(std::lround(static_cast<double>(center.y) + dir_y * distance)),
        };
    };

    double low = 0.0;
    double high = 1.0;
    SDL_Point edge = center;
    while (high <= kMaxDistance) {
        SDL_Point candidate = point_at_distance(high);
        if (!area->contains_point(candidate)) {
            break;
        }
        edge = candidate;
        low = high;
        high *= 2.0;
    }

    high = std::min(high, kMaxDistance);
    for (int iter = 0; iter < 24; ++iter) {
        const double mid = (low + high) * 0.5;
        SDL_Point candidate = point_at_distance(mid);
        if (area->contains_point(candidate)) {
            edge = candidate;
            low = mid;
        } else {
            high = mid;
        }
    }
    return edge;
}

std::vector<double> build_distances(double trail_length,
                                    double spacing,
                                    const std::vector<double>& required_distances) {
    std::vector<double> distances;
    if (trail_length <= 0.0) {
        distances.push_back(0.0);
        return distances;
    }

    distances.push_back(0.0);
    const double sanitized_spacing = std::max(1.0, spacing);
    for (double d = sanitized_spacing; d < trail_length; d += sanitized_spacing) {
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

std::vector<double> build_section_distances(double trail_length, const std::vector<double>& required_distances) {
    return build_distances(trail_length,
                           static_cast<double>(std::max(1, kTrailPerpendicularSectionSpacingWorldPx)),
                           required_distances);
}

std::vector<double> build_control_distances(double trail_length) {
    return build_distances(trail_length, kCenterlineControlSpacingWorldPx, {});
}

std::vector<double> build_validation_distances(double trail_length, const std::vector<double>& required_distances) {
    return build_distances(trail_length, kCenterlineValidationSpacingWorldPx, required_distances);
}

std::vector<double> build_control_offsets(const std::vector<double>& control_distances,
                                          double trail_length,
                                          int curvyness,
                                          bool straight_only,
                                          std::mt19937& rng) {
    std::vector<double> offsets(control_distances.size(), 0.0);
    if (straight_only || curvyness <= 0 || control_distances.size() <= 2 || trail_length <= 0.0) {
        return offsets;
    }

    const double raw_limit = static_cast<double>(curvyness) * kCenterlineCurvatureScaleWorldPx;
    const double max_offset = std::min(raw_limit, trail_length * 0.3);
    if (max_offset <= 0.0) {
        return offsets;
    }

    std::uniform_real_distribution<double> dist(-max_offset, max_offset);
    for (std::size_t i = 1; i + 1 < control_distances.size(); ++i) {
        const double t = std::clamp(control_distances[i] / trail_length, 0.0, 1.0);
        const double envelope = std::sin(t * 3.14159265358979323846);
        offsets[i] = dist(rng) * envelope;
    }
    return offsets;
}

double sample_offset_at_distance(const std::vector<double>& control_distances,
                                 const std::vector<double>& control_offsets,
                                 double distance) {
    if (control_distances.empty() || control_offsets.empty()) {
        return 0.0;
    }
    if (control_distances.size() == 1 || control_offsets.size() == 1) {
        return control_offsets.front();
    }
    if (distance <= control_distances.front()) {
        return control_offsets.front();
    }
    if (distance >= control_distances.back()) {
        return control_offsets.back();
    }

    auto upper = std::lower_bound(control_distances.begin(), control_distances.end(), distance);
    std::size_t hi = static_cast<std::size_t>(std::distance(control_distances.begin(), upper));
    if (hi == 0) {
        return control_offsets.front();
    }
    const std::size_t lo = hi - 1;
    const double d0 = control_distances[lo];
    const double d1 = control_distances[hi];
    const double span = std::max(kPointEpsilon, d1 - d0);
    const double t = std::clamp((distance - d0) / span, 0.0, 1.0);
    const double smooth_t = t * t * (3.0 - 2.0 * t);
    return control_offsets[lo] + (control_offsets[hi] - control_offsets[lo]) * smooth_t;
}

std::vector<CenterlineSample> build_centerline_samples(const Vec2& start,
                                                       const Vec2& dir,
                                                       const Vec2& base_normal,
                                                       const std::vector<double>& sample_distances,
                                                       const std::vector<double>& control_distances,
                                                       const std::vector<double>& control_offsets) {
    std::vector<CenterlineSample> samples;
    if (sample_distances.empty()) {
        return samples;
    }

    samples.reserve(sample_distances.size());
    for (double distance : sample_distances) {
        const double offset = sample_offset_at_distance(control_distances, control_offsets, distance);
        const Vec2 point = add(add(start, scale(dir, distance)), scale(base_normal, offset));
        CenterlineSample sample;
        sample.distance = distance;
        sample.point = point;
        sample.tangent = dir;
        sample.normal = base_normal;
        samples.push_back(sample);
    }

    for (std::size_t i = 0; i < samples.size(); ++i) {
        const Vec2 prev = (i == 0) ? samples[i].point : samples[i - 1].point;
        const Vec2 next = (i + 1 < samples.size()) ? samples[i + 1].point : samples[i].point;
        const Vec2 tangent = normalize_or_default(subtract(next, prev), dir);
        samples[i].tangent = tangent;
        samples[i].normal = Vec2{-tangent.y, tangent.x};
    }

    return samples;
}

bool point_inside_blocking_room(const SDL_Point& point,
                                Room* room_a,
                                Room* room_b,
                                const std::vector<RoomObstacle>& room_obstacles) {
    for (const RoomObstacle& obstacle : room_obstacles) {
        if (!obstacle.room || !obstacle.area) {
            continue;
        }
        if (obstacle.room == room_a || obstacle.room == room_b) {
            continue;
        }
        if (!bounds_contains(obstacle.bounds, point)) {
            continue;
        }
        if (obstacle.area->contains_point(point)) {
            return true;
        }
    }
    return false;
}

bool point_inside_existing_trail(const SDL_Point& point,
                                 const std::vector<TrailObstacle>& existing_trails) {
    for (const TrailObstacle& trail : existing_trails) {
        if (trail.polygon.size() < 3) {
            continue;
        }
        if (!bounds_contains(trail.bounds, point)) {
            continue;
        }
        if (point_in_polygon(to_vec2(point), trail.polygon)) {
            return true;
        }
    }
    return false;
}

bool centerline_is_valid(Room* room_a,
                         Room* room_b,
                         const std::vector<CenterlineSample>& validation_samples,
                         const std::vector<RoomObstacle>& room_obstacles,
                         const std::vector<TrailObstacle>& existing_trails,
                         TrailAttemptStats* stats) {
    for (const CenterlineSample& sample : validation_samples) {
        const SDL_Point point = to_point(sample.point);
        if (point_inside_blocking_room(point, room_a, room_b, room_obstacles)) {
            if (stats) {
                ++stats->centerline_room_rejections;
                stats->last_failure = TrailFailureReason::BlockedByRoom;
            }
            return false;
        }
        if (point_inside_any_room(point, room_a ? room_a->room_area.get() : nullptr, room_b ? room_b->room_area.get() : nullptr)) {
            continue;
        }
        if (point_inside_existing_trail(point, existing_trails)) {
            if (stats) {
                ++stats->centerline_trail_rejections;
                stats->last_failure = TrailFailureReason::BlockedByTrail;
            }
            return false;
        }
    }
    return true;
}

bool shift_is_unique(const std::vector<double>& used_shifts, double value) {
    for (double existing : used_shifts) {
        if (std::abs(existing - value) <= 0.01) {
            return false;
        }
    }
    return true;
}

bool place_sections(const std::vector<CenterlineSample>& section_samples,
                    int min_width,
                    int max_width,
                    int curvyness,
                    std::mt19937& rng,
                    TrailAttemptStats* stats,
                    std::vector<TrailSection>* out_sections) {
    if (!out_sections) {
        if (stats) {
            stats->last_failure = TrailFailureReason::InvalidArgs;
        }
        return false;
    }
    out_sections->clear();
    out_sections->reserve(section_samples.size());

    std::uniform_int_distribution<int> width_dist(min_width, max_width);
    std::vector<double> used_shifts;
    used_shifts.reserve(section_samples.size());

    for (const CenterlineSample& sample : section_samples) {
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

            const Vec2 section_center = add(sample.point, scale(sample.normal, shift));
            const Vec2 left = add(section_center, scale(sample.normal, half_width));
            const Vec2 right = subtract(section_center, scale(sample.normal, half_width));

            TrailSection section;
            section.distance = sample.distance;
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
            if (stats) {
                ++stats->section_failures;
                stats->last_failure = TrailFailureReason::SectionPlacementFailed;
            }
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

bool build_trail_layout(const SDL_Point& start_tip,
                        const SDL_Point& end_tip,
                        int min_width,
                        int max_width,
                        int curvyness,
                        const std::vector<double>& required_distances,
                        Room* room_a,
                        Room* room_b,
                        const std::vector<RoomObstacle>& room_obstacles,
                        const std::vector<TrailObstacle>& existing_trails,
                        std::mt19937& rng,
                        TrailAttemptStats* stats,
                        TrailBuildResult* out_result) {
    if (!out_result) {
        if (stats) {
            stats->last_failure = TrailFailureReason::InvalidArgs;
        }
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
        if (stats) {
            stats->last_failure = TrailFailureReason::DegenerateCenterline;
        }
        return false;
    }

    const Vec2 dir = scale(axis, 1.0 / trail_length);
    const Vec2 normal{-dir.y, dir.x};
    const std::vector<double> section_distances = build_section_distances(trail_length, required_distances);
    if (section_distances.empty()) {
        if (stats) {
            stats->last_failure = TrailFailureReason::EmptySectionDistances;
        }
        return false;
    }

    const std::vector<double> control_distances = build_control_distances(trail_length);
    const std::vector<double> validation_distances = build_validation_distances(trail_length, required_distances);
    if (validation_distances.empty()) {
        if (stats) {
            stats->last_failure = TrailFailureReason::EmptyValidationDistances;
        }
        return false;
    }

    for (int attempt = 0; attempt < kTrailPairAttempts; ++attempt) {
        const bool straight_only = (attempt == 0);
        if (stats) {
            ++stats->layout_attempts;
            if (straight_only) {
                ++stats->straight_attempts;
            } else {
                ++stats->curved_attempts;
            }
        }

        const std::vector<double> control_offsets = build_control_offsets(control_distances,
                                                                          trail_length,
                                                                          curvyness,
                                                                          straight_only,
                                                                          rng);
        const std::vector<CenterlineSample> validation_samples = build_centerline_samples(start,
                                                                                          dir,
                                                                                          normal,
                                                                                          validation_distances,
                                                                                          control_distances,
                                                                                          control_offsets);
        if (!centerline_is_valid(room_a, room_b, validation_samples, room_obstacles, existing_trails, stats)) {
            continue;
        }

        const std::vector<CenterlineSample> section_samples = build_centerline_samples(start,
                                                                                       dir,
                                                                                       normal,
                                                                                       section_distances,
                                                                                       control_distances,
                                                                                       control_offsets);
        std::vector<TrailSection> sections;
        if (!place_sections(section_samples, min_width, max_width, curvyness, rng, stats, &sections)) {
            continue;
        }

        std::vector<SDL_Point> polygon = build_polygon(start, end, sections);
        if (polygon.size() < 4) {
            if (stats) {
                ++stats->polygon_failures;
                stats->last_failure = TrailFailureReason::PolygonTooSmall;
            }
            continue;
        }

        out_result->sections = std::move(sections);
        out_result->polygon = std::move(polygon);
        if (stats) {
            stats->last_failure = TrailFailureReason::None;
        }
        return true;
    }

    if (stats) {
        stats->last_failure = TrailFailureReason::LayoutAttemptsExhausted;
    }
    return false;
}

bool attempt_trail_connection(Room* a,
                              Room* b,
                              const std::vector<RoomObstacle>& room_obstacles,
                              std::vector<TrailObstacle>& existing_trails,
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
                              Room::ManifestWriter manifest_writer,
                              TrailAttemptStats* stats) {
    using clock = std::chrono::steady_clock;

    if (!a || !b || !a->room_area || !b->room_area || !trail_config) {
        if (stats) {
            stats->last_failure = TrailFailureReason::InvalidArgs;
        }
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
        if (stats) {
            stats->last_failure = TrailFailureReason::DegenerateCenterline;
        }
        return false;
    }
    const Vec2 center_dir = scale(center_axis, 1.0 / center_len);

    auto project_ratio = [&](const SDL_Point& point) {
        const double projected = dot(subtract(to_vec2(point), center_start), center_dir);
        return std::clamp(projected / center_len, 0.0, 1.0);
    };

    std::vector<double> required_distances;
    required_distances.reserve(2);
    required_distances.push_back(project_ratio(a_edge) * center_len);
    required_distances.push_back(project_ratio(b_edge) * center_len);

    TrailBuildResult build_result;

    const auto layout_start = clock::now();
    if (!build_trail_layout(a_center,
                            b_center,
                            min_width,
                            max_width,
                            curvyness,
                            required_distances,
                            a,
                            b,
                            room_obstacles,
                            existing_trails,
                            rng,
                            stats,
                            &build_result)) {
        return false;
    }
    const auto layout_end = clock::now();

    Area candidate("trail_candidate", build_result.polygon, 3);

    const auto room_build_start = clock::now();
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
        manifest_writer,
        false);
    const auto room_build_end = clock::now();

    a->add_connecting_room(trail_room.get());
    b->add_connecting_room(trail_room.get());
    trail_room->add_connecting_room(a);
    trail_room->add_connecting_room(b);

    trail_room->camera_height_px = std::clamp(config.value("camera_height_px", 1000), 1, 2000);
    trail_room->camera_tilt_deg = std::clamp(config.value("camera_tilt_deg", 60.0f), 0.0f, 360.0f);
    trail_room->camera_zoom_percent = std::clamp(config.value("camera_zoom_percent", 0), 0, 100);
    trail_room->camera_center_dx = config.value("camera_center_dx", 0);
    trail_room->camera_center_dz = config.value("camera_center_dz", 0);

    TrailObstacle obstacle;
    obstacle.polygon = build_result.polygon;
    obstacle.bounds = bounds_from_points(obstacle.polygon);
    existing_trails.push_back(std::move(obstacle));
    trail_rooms.push_back(std::move(trail_room));

    vibble::log::info(
        std::string("[GenerateTrails] Built trail room") +
        " rooms=" + a->room_name + "<->" + b->room_name +
        " layout_ms=" + std::to_string(duration_ms(layout_end - layout_start)) +
        " room_ctor_ms=" + std::to_string(duration_ms(room_build_end - room_build_start)));

    if (testing) {
        vibble::log::debug(
            std::string("[GenerateTrails] Trail placed: ") +
            a->room_name + " <-> " + b->room_name +
            " layout_attempts=" + std::to_string(stats ? stats->layout_attempts : 0) +
            " straight_attempts=" + std::to_string(stats ? stats->straight_attempts : 0) +
            " curved_attempts=" + std::to_string(stats ? stats->curved_attempts : 0) +
            " room_rejects=" + std::to_string(stats ? stats->centerline_room_rejections : 0) +
            " trail_rejects=" + std::to_string(stats ? stats->centerline_trail_rejections : 0) +
            " section_failures=" + std::to_string(stats ? stats->section_failures : 0));
    }

    return true;
}


std::vector<RoomObstacle> build_room_obstacles(const std::vector<Room*>& rooms) {
    std::vector<RoomObstacle> obstacles;
    obstacles.reserve(rooms.size());
    for (Room* room : rooms) {
        if (!room || !room->room_area) {
            continue;
        }
        RoomObstacle obstacle;
        obstacle.room = room;
        obstacle.area = room->room_area.get();
        obstacle.bounds = bounds_from_area(*room->room_area);
        obstacles.push_back(obstacle);
    }
    return obstacles;
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
                                                                   const std::string& manifest_context,
                                                                   AssetLibrary* asset_lib,
                                                                   double map_radius,
                                                                   nlohmann::json* map_manifest,
                                                                   devmode::core::ManifestStore* manifest_store,
                                                                   Room::ManifestWriter manifest_writer) {
    using clock = std::chrono::steady_clock;
    const auto generation_start = clock::now();

    std::vector<std::unique_ptr<Room>> trail_rooms;
    std::vector<TrailObstacle> existing_trails;
    const std::vector<RoomObstacle> room_obstacles = build_room_obstacles(all_rooms_reference_);

    const auto planning_start = clock::now();
    const auto connection_plan = plan_maze_connections(all_rooms_reference_, room_pairs);
    const auto planning_end = clock::now();

    GenerationPerfSummary perf;
    perf.total_connections = static_cast<int>(connection_plan.size());
    perf.total_rooms_considered = static_cast<int>(all_rooms_reference_.size());

    vibble::log::info(
        std::string("[GenerateTrails] Begin generation") +
        " rooms=" + std::to_string(perf.total_rooms_considered) +
        " forced_pairs=" + std::to_string(static_cast<int>(room_pairs.size())) +
        " planned_connections=" + std::to_string(perf.total_connections) +
        " room_obstacles=" + std::to_string(static_cast<int>(room_obstacles.size())) +
        " planning_ms=" + std::to_string(duration_ms(planning_end - planning_start))
    );

    const int asset_attempts = std::max(1, static_cast<int>(available_assets_.size()) * 2);

    for (std::size_t connection_index = 0; connection_index < connection_plan.size(); ++connection_index) {
        Room* a = connection_plan[connection_index].first;
        Room* b = connection_plan[connection_index].second;
        if (!a || !b) {
            vibble::log::warn(
                std::string("[GenerateTrails] Skip connection index=") +
                std::to_string(connection_index) +
                " reason=null_room_pointer"
            );
            continue;
        }

        const auto connection_start = clock::now();
        vibble::log::debug(
            std::string("[GenerateTrails] Connection start") +
            " index=" + std::to_string(connection_index + 1) + "/" + std::to_string(connection_plan.size()) +
            " rooms=" + a->room_name + "<->" + b->room_name +
            " existing_trails=" + std::to_string(static_cast<int>(existing_trails.size())) +
            " asset_attempt_budget=" + std::to_string(asset_attempts)
        );

        bool success = false;
        TrailAttemptStats best_failed_stats;
        bool have_failed_stats = false;

        for (int attempt = 0; attempt < asset_attempts && !success; ++attempt) {
            ++perf.total_asset_attempts;

            const auto* asset_ref = pick_random_asset();
            if (!asset_ref) {
                vibble::log::warn(
                    std::string("[GenerateTrails] Connection asset selection failed") +
                    " rooms=" + a->room_name + "<->" + b->room_name +
                    " attempt=" + std::to_string(attempt + 1) +
                    " reason=no_available_asset"
                );
                continue;
            }

            TrailAttemptStats stats;
            const auto attempt_start = clock::now();
            success = attempt_trail_connection(a,
                                               b,
                                               room_obstacles,
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
                                               manifest_writer,
                                               &stats);
            const auto attempt_end = clock::now();

            perf.total_layout_attempts += stats.layout_attempts;
            perf.total_straight_attempts += stats.straight_attempts;
            perf.total_curved_attempts += stats.curved_attempts;
            perf.total_room_rejections += stats.centerline_room_rejections;
            perf.total_trail_rejections += stats.centerline_trail_rejections;
            perf.total_section_failures += stats.section_failures;
            perf.total_polygon_failures += stats.polygon_failures;

            if (!success) {
                if (!have_failed_stats || stats.layout_attempts >= best_failed_stats.layout_attempts) {
                    best_failed_stats = stats;
                    have_failed_stats = true;
                }

                vibble::log::debug(
                    std::string("[GenerateTrails] Connection attempt failed") +
                    " rooms=" + a->room_name + "<->" + b->room_name +
                    " template=" + asset_ref->name +
                    " asset_attempt=" + std::to_string(attempt + 1) + "/" + std::to_string(asset_attempts) +
                    " layout_attempts=" + std::to_string(stats.layout_attempts) +
                    " straight_attempts=" + std::to_string(stats.straight_attempts) +
                    " curved_attempts=" + std::to_string(stats.curved_attempts) +
                    " room_rejects=" + std::to_string(stats.centerline_room_rejections) +
                    " trail_rejects=" + std::to_string(stats.centerline_trail_rejections) +
                    " section_failures=" + std::to_string(stats.section_failures) +
                    " polygon_failures=" + std::to_string(stats.polygon_failures) +
                    " last_failure=" + std::string(failure_reason_name(stats.last_failure)) +
                    " attempt_ms=" + std::to_string(duration_ms(attempt_end - attempt_start))
                );
            } else {
                ++perf.successful_connections;
                vibble::log::info(
                    std::string("[GenerateTrails] Connection success") +
                    " rooms=" + a->room_name + "<->" + b->room_name +
                    " template=" + asset_ref->name +
                    " asset_attempt=" + std::to_string(attempt + 1) + "/" + std::to_string(asset_attempts) +
                    " layout_attempts=" + std::to_string(stats.layout_attempts) +
                    " straight_attempts=" + std::to_string(stats.straight_attempts) +
                    " curved_attempts=" + std::to_string(stats.curved_attempts) +
                    " room_rejects=" + std::to_string(stats.centerline_room_rejections) +
                    " trail_rejects=" + std::to_string(stats.centerline_trail_rejections) +
                    " section_failures=" + std::to_string(stats.section_failures) +
                    " polygon_failures=" + std::to_string(stats.polygon_failures) +
                    " attempt_ms=" + std::to_string(duration_ms(attempt_end - attempt_start))
                );
            }
        }

        if (!success) {
            ++perf.failed_connections;
            vibble::log::warn(
                std::string("[GenerateTrails] Connection failed") +
                " rooms=" + a->room_name + "<->" + b->room_name +
                " final_failure=" + std::string(failure_reason_name(best_failed_stats.last_failure)) +
                " layout_attempts=" + std::to_string(best_failed_stats.layout_attempts) +
                " straight_attempts=" + std::to_string(best_failed_stats.straight_attempts) +
                " curved_attempts=" + std::to_string(best_failed_stats.curved_attempts) +
                " room_rejects=" + std::to_string(best_failed_stats.centerline_room_rejections) +
                " trail_rejects=" + std::to_string(best_failed_stats.centerline_trail_rejections) +
                " section_failures=" + std::to_string(best_failed_stats.section_failures) +
                " polygon_failures=" + std::to_string(best_failed_stats.polygon_failures) +
                " connection_ms=" + std::to_string(duration_ms(clock::now() - connection_start))
            );
        } else {
            vibble::log::debug(
                std::string("[GenerateTrails] Connection complete") +
                " rooms=" + a->room_name + "<->" + b->room_name +
                " connection_ms=" + std::to_string(duration_ms(clock::now() - connection_start)) +
                " placed_trails=" + std::to_string(static_cast<int>(trail_rooms.size()))
            );
        }
    }

    const auto generation_end = clock::now();
    vibble::log::info(
        std::string("[GenerateTrails] Summary") +
        " total_connections=" + std::to_string(perf.total_connections) +
        " successes=" + std::to_string(perf.successful_connections) +
        " failures=" + std::to_string(perf.failed_connections) +
        " asset_attempts=" + std::to_string(perf.total_asset_attempts) +
        " layout_attempts=" + std::to_string(perf.total_layout_attempts) +
        " straight_attempts=" + std::to_string(perf.total_straight_attempts) +
        " curved_attempts=" + std::to_string(perf.total_curved_attempts) +
        " room_rejects=" + std::to_string(perf.total_room_rejections) +
        " trail_rejects=" + std::to_string(perf.total_trail_rejections) +
        " section_failures=" + std::to_string(perf.total_section_failures) +
        " polygon_failures=" + std::to_string(perf.total_polygon_failures) +
        " total_ms=" + std::to_string(duration_ms(generation_end - generation_start))
    );

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

    std::vector<TrailObstacle> trail_obstacles;
    trail_obstacles.reserve(existing_trails.size());
    for (const Area& area : existing_trails) {
        TrailObstacle obstacle;
        obstacle.polygon = area.get_points();
        obstacle.bounds = bounds_from_points(obstacle.polygon);
        trail_obstacles.push_back(std::move(obstacle));
    }

    std::mt19937 rng(seed);
    TrailBuildResult build_result;
    TrailAttemptStats stats;
    if (!build_trail_layout(start_tip,
                            end_tip,
                            min_width,
                            max_width,
                            curvyness,
                            {},
                            nullptr,
                            nullptr,
                            {},
                            trail_obstacles,
                            rng,
                            &stats,
                            &build_result)) {
        vibble::log::warn(
            std::string("[TrailGeometryTest] build_layout_for_tests failed") +
            " layout_attempts=" + std::to_string(stats.layout_attempts) +
            " straight_attempts=" + std::to_string(stats.straight_attempts) +
            " curved_attempts=" + std::to_string(stats.curved_attempts) +
            " room_rejects=" + std::to_string(stats.centerline_room_rejections) +
            " trail_rejects=" + std::to_string(stats.centerline_trail_rejections) +
            " section_failures=" + std::to_string(stats.section_failures) +
            " polygon_failures=" + std::to_string(stats.polygon_failures) +
            " last_failure=" + std::string(failure_reason_name(stats.last_failure))
        );
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