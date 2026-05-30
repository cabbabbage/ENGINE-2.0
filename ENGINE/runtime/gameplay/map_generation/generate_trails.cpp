#include "generate_trails.hpp"

#include "utils/display_color.hpp"
#include "utils/log.hpp"
#include "utils/map_grid_settings.hpp"
#include "utils/utils/weighted_range.hpp"
#include "rendering/render/camera_controller.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
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
constexpr double kCenterlineCurvatureScaleWorldPx = 45.0;
constexpr double kCenterlineControlSpacingWorldPx = 300.0;
constexpr double kCenterlineValidationSpacingWorldPx = 48.0;
constexpr double kPointEpsilon = 1e-6;
constexpr double kDegreesFullRotation = 360.0;
constexpr double kTrailSectorDefaultDirectionDeg = 0.0;
constexpr int kTrailSectorDefaultWidthPercent = 100;
constexpr int kTrailSectorMinWidthPercent = 25;
constexpr int kTrailSectorMaxWidthPercent = 100;
constexpr int kTrailContactAngleSamples = 36;

json make_default_trail_template() {
    return json::object({
        {"name", "MainTrail"},
        {"display_color", json::array({85, 242, 143, 255})},
        {"geometry", "Square"},
        {"width", vibble::weighted_range::to_json(vibble::weighted_range::make_legacy_uniform(400, 800))},
        {"tags", json::array()},
        {"anti_tags", json::array()},
        {"spawn_groups", json::array()}
    });
}

bool is_valid_trail_template(const json& candidate) {
    return candidate.is_object();
}

vibble::weighted_range::WeightedIntRange read_weighted_range_field(const json& src,
                                                                   const char* key,
                                                                   const vibble::weighted_range::WeightedIntRange& fallback) {
    if (!src.is_object() || !src.contains(key)) {
        return fallback;
    }
    return vibble::weighted_range::from_json(src.at(key), fallback);
}

vibble::weighted_range::WeightedIntRange read_weighted_range_legacy_pair(const json& src,
                                                                         const char* min_key,
                                                                         const char* max_key,
                                                                         const vibble::weighted_range::WeightedIntRange& fallback) {
    if (!src.is_object()) {
        return fallback;
    }
    bool has_min = false;
    bool has_max = false;
    std::int64_t min_value = fallback.center;
    std::int64_t max_value = fallback.center;
    if (src.contains(min_key)) {
        if (src.at(min_key).is_number_integer()) {
            min_value = src.at(min_key).get<std::int64_t>();
            has_min = true;
        } else if (src.at(min_key).is_number_float()) {
            min_value = static_cast<std::int64_t>(std::llround(src.at(min_key).get<double>()));
            has_min = true;
        }
    }
    if (src.contains(max_key)) {
        if (src.at(max_key).is_number_integer()) {
            max_value = src.at(max_key).get<std::int64_t>();
            has_max = true;
        } else if (src.at(max_key).is_number_float()) {
            max_value = static_cast<std::int64_t>(std::llround(src.at(max_key).get<double>()));
            has_max = true;
        }
    }
    if (!has_min && !has_max) {
        return fallback;
    }
    if (!has_min) {
        min_value = max_value;
    }
    if (!has_max) {
        max_value = min_value;
    }
    return vibble::weighted_range::make_legacy_uniform(min_value, max_value);
}

int resolve_weighted_dimension(const vibble::weighted_range::WeightedIntRange& range, std::mt19937& rng) {
    const std::int64_t resolved = vibble::weighted_range::resolve(range, rng);
    if (resolved < static_cast<std::int64_t>(std::numeric_limits<int>::min())) {
        return std::numeric_limits<int>::min();
    }
    if (resolved > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(resolved);
}
constexpr int kTrailRouteSearchMaxExpansions = 120000;
constexpr int kTrailRouteSearchMaxCellSpan = 320;
constexpr int kTrailRouteInitialCellSizeWorldPx = 96;
constexpr int kTrailRouteSampleSpacingWorldPx = 32;
constexpr int kTrailBoundaryZoneDistanceWorldPx = 240;
constexpr int kTrailContactGateIterations = 24;
constexpr double kTrailContactGateStepWorldPx = 24.0;
constexpr int kTrailRoutePairBudget = 64;
constexpr double kRoomMarginDistanceWorldPx = 512.0;
constexpr double kTrailEndpointContainmentSafetyWorldPx = 12.0;
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
    Room* room = nullptr;
    std::vector<SDL_Point> polygon;
    std::vector<SDL_Point> centerline;
    Bounds bounds{};
    bool has_split = false;
};

struct TrailConnectionSector {
    double direction_deg = kTrailSectorDefaultDirectionDeg;
    int width_percent = kTrailSectorDefaultWidthPercent;
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
    std::vector<SDL_Point> centerline;
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
    LayoutAttemptsExhausted,
    NoValidSectorContacts,
    SectorBoundaryZoneViolation,
    RouteSearchBudgetExceeded,
    AnchorExitStartFailed,
    AnchorExitEndFailed,
    RouteFailed,
    PolygonFailed,
    EndpointContainmentFailedStart,
    EndpointContainmentFailedEnd,
};

struct EndpointFrame {
    SDL_Point contact{0, 0};
    SDL_Point margin{0, 0};
    Vec2 outward{1.0, 0.0};
    double inside_depth = kTrailEndpointContainmentSafetyWorldPx;
};

struct EndpointAllowance {
    double start_distance = 0.0;
    double end_distance = 0.0;
};

struct TrailAttemptStats {
    int layout_attempts = 0;
    int straight_attempts = 0;
    int curved_attempts = 0;
    int centerline_room_rejections = 0;
    int centerline_trail_rejections = 0;
    int section_failures = 0;
    int polygon_failures = 0;
    int sector_contact_failures = 0;
    int sector_boundary_failures = 0;
    int route_budget_failures = 0;
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
    int total_sector_contact_failures = 0;
    int total_sector_boundary_failures = 0;
    int total_route_budget_failures = 0;
    int total_rooms_considered = 0;
    int split_attempts = 0;
    int split_successes = 0;
    int split_exhausted = 0;
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
        case TrailFailureReason::NoValidSectorContacts: return "no_valid_sector_contacts";
        case TrailFailureReason::SectorBoundaryZoneViolation: return "sector_boundary_zone_violation";
        case TrailFailureReason::RouteSearchBudgetExceeded: return "route_search_budget_exceeded";
        case TrailFailureReason::AnchorExitStartFailed: return "anchor_exit_start";
        case TrailFailureReason::AnchorExitEndFailed: return "anchor_exit_end";
        case TrailFailureReason::RouteFailed: return "route_failed";
        case TrailFailureReason::PolygonFailed: return "polygon_failed";
        case TrailFailureReason::EndpointContainmentFailedStart: return "endpoint_containment_failed_start";
        case TrailFailureReason::EndpointContainmentFailedEnd: return "endpoint_containment_failed_end";
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

double normalize_angle_degrees(double value) {
    if (!std::isfinite(value)) {
        return kTrailSectorDefaultDirectionDeg;
    }
    double normalized = std::fmod(value, kDegreesFullRotation);
    if (normalized < 0.0) {
        normalized += kDegreesFullRotation;
    }
    if (normalized >= kDegreesFullRotation) {
        normalized -= kDegreesFullRotation;
    }
    return normalized;
}

double shortest_angular_distance_degrees(double a, double b) {
    const double na = normalize_angle_degrees(a);
    const double nb = normalize_angle_degrees(b);
    double delta = std::abs(na - nb);
    if (delta > 180.0) {
        delta = kDegreesFullRotation - delta;
    }
    return delta;
}

Vec2 direction_for_angle_degrees(double angle_deg) {
    const double normalized = normalize_angle_degrees(angle_deg);
    const double radians = normalized * (3.14159265358979323846 / 180.0);
    return Vec2{std::sin(radians), -std::cos(radians)};
}

double angle_degrees_from_center(const SDL_Point& center, const SDL_Point& point) {
    const double dx = static_cast<double>(point.x - center.x);
    const double dy = static_cast<double>(point.y - center.y);
    const double angle_rad = std::atan2(dx, -dy);
    return normalize_angle_degrees(angle_rad * (180.0 / 3.14159265358979323846));
}

TrailConnectionSector sector_from_room(const Room* room) {
    TrailConnectionSector sector;
    if (!room) {
        return sector;
    }
    const nlohmann::json& data = room->assets_data();
    if (!data.is_object()) {
        return sector;
    }
    auto sector_it = data.find("trail_connection_sector");
    if (sector_it == data.end() || !sector_it->is_object()) {
        return sector;
    }
    if (sector_it->contains("direction_deg")) {
        const auto& value = (*sector_it)["direction_deg"];
        if (value.is_number_integer()) {
            sector.direction_deg = static_cast<double>(value.get<int>());
        } else if (value.is_number_float()) {
            sector.direction_deg = value.get<double>();
        }
    }
    if (sector_it->contains("width_percent")) {
        const auto& value = (*sector_it)["width_percent"];
        if (value.is_number_integer()) {
            sector.width_percent = value.get<int>();
        } else if (value.is_number_float()) {
            sector.width_percent = static_cast<int>(std::lround(value.get<double>()));
        }
    }
    sector.direction_deg = normalize_angle_degrees(sector.direction_deg);
    sector.width_percent = std::clamp(sector.width_percent, kTrailSectorMinWidthPercent, kTrailSectorMaxWidthPercent);
    return sector;
}

bool angle_in_sector(double angle_deg, const TrailConnectionSector& sector) {
    const double span = (kDegreesFullRotation * static_cast<double>(sector.width_percent)) / 100.0;
    if (span >= kDegreesFullRotation - 1e-6) {
        return true;
    }
    const double center = normalize_angle_degrees(sector.direction_deg);
    return shortest_angular_distance_degrees(angle_deg, center) <= (span * 0.5 + 1e-6);
}

bool point_is_in_room_sector(const SDL_Point& point, const Room* room, const TrailConnectionSector& sector) {
    if (!room || !room->room_area) {
        return false;
    }
    const SDL_Point center = room->room_area->get_center();
    const double angle_deg = angle_degrees_from_center(center, point);
    return angle_in_sector(angle_deg, sector);
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

double signed_polygon_area(const std::vector<SDL_Point>& polygon) {
    if (polygon.size() < 3) {
        return 0.0;
    }
    double twice_area = 0.0;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        twice_area += static_cast<double>(polygon[j].x) * static_cast<double>(polygon[i].y) -
                      static_cast<double>(polygon[i].x) * static_cast<double>(polygon[j].y);
    }
    return twice_area * 0.5;
}

bool bounds_overlap(const Bounds& a, const Bounds& b) {
    return !(a.max_x < b.min_x || b.max_x < a.min_x || a.max_y < b.min_y || b.max_y < a.min_y);
}

bool polygon_has_duplicate_or_collapsed_edges(const std::vector<SDL_Point>& polygon) {
    if (polygon.size() < 3) {
        return true;
    }
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const SDL_Point& a = polygon[i];
        const SDL_Point& b = polygon[(i + 1) % polygon.size()];
        if (a.x == b.x && a.y == b.y) {
            return true;
        }
        for (std::size_t j = i + 1; j < polygon.size(); ++j) {
            if (polygon[j].x == a.x && polygon[j].y == a.y) {
                return true;
            }
        }
    }
    return false;
}

bool polygon_has_self_intersection(const std::vector<SDL_Point>& polygon) {
    const std::size_t n = polygon.size();
    if (n < 4) {
        return false;
    }
    auto adjacent_edges = [n](std::size_t a, std::size_t b) {
        return a == b || (a + 1) % n == b || (b + 1) % n == a;
    };
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2 a0 = to_vec2(polygon[i]);
        const Vec2 a1 = to_vec2(polygon[(i + 1) % n]);
        for (std::size_t j = i + 1; j < n; ++j) {
            if (adjacent_edges(i, j)) {
                continue;
            }
            const Vec2 b0 = to_vec2(polygon[j]);
            const Vec2 b1 = to_vec2(polygon[(j + 1) % n]);
            if (segments_intersect(a0, a1, b0, b1)) {
                return true;
            }
        }
    }
    return false;
}

bool polygon_intersects_polygon(const std::vector<SDL_Point>& a, const std::vector<SDL_Point>& b) {
    if (a.size() < 3 || b.size() < 3) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const Vec2 a0 = to_vec2(a[i]);
        const Vec2 a1 = to_vec2(a[(i + 1) % a.size()]);
        for (std::size_t j = 0; j < b.size(); ++j) {
            if (segments_intersect(a0, a1, to_vec2(b[j]), to_vec2(b[(j + 1) % b.size()]))) {
                return true;
            }
        }
    }
    if (point_in_polygon(to_vec2(a.front()), b)) {
        return true;
    }
    return point_in_polygon(to_vec2(b.front()), a);
}

bool trail_polygon_is_clean(const std::vector<SDL_Point>& polygon,
                            const std::vector<TrailObstacle>& existing_trails) {
    if (polygon.size() < 4) {
        return false;
    }
    if (polygon_has_duplicate_or_collapsed_edges(polygon)) {
        return false;
    }
    if (std::abs(signed_polygon_area(polygon)) <= 1.0) {
        return false;
    }
    if (polygon_has_self_intersection(polygon)) {
        return false;
    }
    Bounds bounds{};
    bounds.min_x = bounds.max_x = static_cast<double>(polygon.front().x);
    bounds.min_y = bounds.max_y = static_cast<double>(polygon.front().y);
    for (const SDL_Point& point : polygon) {
        bounds.min_x = std::min(bounds.min_x, static_cast<double>(point.x));
        bounds.max_x = std::max(bounds.max_x, static_cast<double>(point.x));
        bounds.min_y = std::min(bounds.min_y, static_cast<double>(point.y));
        bounds.max_y = std::max(bounds.max_y, static_cast<double>(point.y));
    }
    for (const TrailObstacle& trail : existing_trails) {
        if (trail.polygon.size() < 3 || !bounds_overlap(bounds, trail.bounds)) {
            continue;
        }
        if (polygon_intersects_polygon(polygon, trail.polygon)) {
            return false;
        }
    }
    return true;
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

SDL_Point make_far_target(const SDL_Point& center, double angle_deg) {
    const Vec2 dir = direction_for_angle_degrees(angle_deg);
    return SDL_Point{
        static_cast<int>(std::lround(static_cast<double>(center.x) + dir.x * 32768.0)),
        static_cast<int>(std::lround(static_cast<double>(center.y) + dir.y * 32768.0)),
    };
}

std::vector<SDL_Point> collect_sector_contact_candidates_for_area(const Area* area,
                                                                  const TrailConnectionSector& sector,
                                                                  const SDL_Point& target_center) {
    std::vector<SDL_Point> points;
    if (!area) {
        return points;
    }
    const SDL_Point center = area->get_center();
    const double span_deg = (kDegreesFullRotation * static_cast<double>(sector.width_percent)) / 100.0;
    const double start_deg = normalize_angle_degrees(sector.direction_deg - span_deg * 0.5);
    const double target_angle = angle_degrees_from_center(center, target_center);

    auto add_candidate = [&](double angle_deg) {
        if (!angle_in_sector(angle_deg, sector)) {
            return;
        }
        const SDL_Point toward = make_far_target(center, angle_deg);
        const SDL_Point edge = compute_edge_point(center, toward, area);
        if (std::find_if(points.begin(), points.end(), [&](const SDL_Point& existing) {
                return existing.x == edge.x && existing.y == edge.y;
            }) == points.end()) {
            points.push_back(edge);
        }
    };

    add_candidate(target_angle);
    const int samples = std::max(6, kTrailContactAngleSamples);
    for (int i = 0; i < samples; ++i) {
        const double t = (samples == 1) ? 0.0 : static_cast<double>(i) / static_cast<double>(samples - 1);
        const double angle = normalize_angle_degrees(start_deg + span_deg * t);
        add_candidate(angle);
    }

    std::stable_sort(points.begin(), points.end(), [&](const SDL_Point& lhs, const SDL_Point& rhs) {
        const double da = shortest_angular_distance_degrees(angle_degrees_from_center(center, lhs), target_angle);
        const double db = shortest_angular_distance_degrees(angle_degrees_from_center(center, rhs), target_angle);
        return da < db;
    });
    return points;
}

std::vector<SDL_Point> collect_sector_contact_candidates(Room* room, const SDL_Point& target_center) {
    if (!room || !room->room_area) {
        return {};
    }
    const TrailConnectionSector sector = sector_from_room(room);
    return collect_sector_contact_candidates_for_area(room->room_area.get(), sector, target_center);
}

std::vector<SDL_Point> dedupe_consecutive_points(const std::vector<SDL_Point>& points) {
    std::vector<SDL_Point> deduped;
    deduped.reserve(points.size());
    for (const SDL_Point& point : points) {
        if (!deduped.empty() && deduped.back().x == point.x && deduped.back().y == point.y) {
            continue;
        }
        deduped.push_back(point);
    }
    return deduped;
}

std::vector<double> build_polyline_cumulative_lengths(const std::vector<SDL_Point>& points) {
    std::vector<double> distances;
    if (points.empty()) {
        return distances;
    }
    distances.reserve(points.size());
    distances.push_back(0.0);
    double total = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const double dx = static_cast<double>(points[i].x - points[i - 1].x);
        const double dy = static_cast<double>(points[i].y - points[i - 1].y);
        total += std::hypot(dx, dy);
        distances.push_back(total);
    }
    return distances;
}

double polyline_total_length(const std::vector<double>& cumulative_lengths) {
    if (cumulative_lengths.empty()) {
        return 0.0;
    }
    return cumulative_lengths.back();
}

bool sample_polyline_at_distance(const std::vector<SDL_Point>& points,
                                 const std::vector<double>& cumulative_lengths,
                                 double distance,
                                 Vec2* out_point,
                                 Vec2* out_tangent) {
    if (!out_point || !out_tangent || points.size() < 2 || cumulative_lengths.size() != points.size()) {
        return false;
    }

    const double total = polyline_total_length(cumulative_lengths);
    if (total <= kPointEpsilon) {
        return false;
    }
    const double d = std::clamp(distance, 0.0, total);

    for (std::size_t i = 1; i < points.size(); ++i) {
        const Vec2 p0 = to_vec2(points[i - 1]);
        const Vec2 p1 = to_vec2(points[i]);
        const Vec2 segment = subtract(p1, p0);
        const double segment_len = length(segment);
        if (segment_len <= kPointEpsilon) {
            continue;
        }
        const double seg_start = cumulative_lengths[i - 1];
        const double seg_end = cumulative_lengths[i];
        if (d <= seg_end + 1e-6 || i + 1 == points.size()) {
            const double t = std::clamp((d - seg_start) / std::max(kPointEpsilon, seg_end - seg_start), 0.0, 1.0);
            *out_point = add(p0, scale(segment, t));
            *out_tangent = scale(segment, 1.0 / segment_len);
            return true;
        }
    }

    return false;
}

std::vector<CenterlineSample> build_centerline_samples_from_polyline(
    const std::vector<SDL_Point>& polyline_points,
    const std::vector<double>& sample_distances) {
    std::vector<CenterlineSample> samples;
    if (polyline_points.size() < 2 || sample_distances.empty()) {
        return samples;
    }

    const std::vector<double> cumulative_lengths = build_polyline_cumulative_lengths(polyline_points);
    if (polyline_total_length(cumulative_lengths) <= 1.0) {
        return samples;
    }

    samples.reserve(sample_distances.size());
    for (double distance : sample_distances) {
        Vec2 point{};
        Vec2 tangent{};
        if (!sample_polyline_at_distance(polyline_points, cumulative_lengths, distance, &point, &tangent)) {
            continue;
        }

        CenterlineSample sample;
        sample.distance = distance;
        sample.point = point;
        sample.tangent = tangent;
        sample.normal = Vec2{-tangent.y, tangent.x};
        samples.push_back(sample);
    }

    return samples;
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

std::vector<double> build_validation_distances(double trail_length, const std::vector<double>& required_distances) {
    return build_distances(trail_length, kCenterlineValidationSpacingWorldPx, required_distances);
}

std::vector<CenterlineSample> build_centerline_samples(const Vec2& start,
                                                       const Vec2& dir,
                                                       const Vec2& base_normal,
                                                       const std::vector<double>& sample_distances) {
    std::vector<CenterlineSample> samples;
    if (sample_distances.empty()) {
        return samples;
    }

    samples.reserve(sample_distances.size());
    for (double distance : sample_distances) {
        const Vec2 point = add(start, scale(dir, distance));
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

bool polygon_overlaps_unrelated_room(const std::vector<SDL_Point>& polygon,
                                     Room* room_a,
                                     Room* room_b,
                                     const std::vector<RoomObstacle>& room_obstacles) {
    if (polygon.size() < 3) {
        return true;
    }
    const Bounds polygon_bounds = bounds_from_points(polygon);
    for (const RoomObstacle& obstacle : room_obstacles) {
        if (!obstacle.room || !obstacle.area) {
            continue;
        }
        if (obstacle.room == room_a || obstacle.room == room_b) {
            continue;
        }
        if (!bounds_overlap(polygon_bounds, obstacle.bounds)) {
            continue;
        }
        if (polygon_intersects_polygon(polygon, obstacle.area->get_points())) {
            return true;
        }
        for (const SDL_Point& point : polygon) {
            if (obstacle.area->contains_point(point)) {
                return true;
            }
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

struct RouteSearchResult {
    bool success = false;
    bool budget_exhausted = false;
    std::vector<SDL_Point> points;
};

std::vector<SDL_Point> sanitize_diagonal_centerline(const std::vector<SDL_Point>& points);
bool polyline_has_only_exact_diagonal_segments(const std::vector<SDL_Point>& polyline);
std::vector<SDL_Point> simplify_route_points(const std::vector<SDL_Point>& points,
                                             Room* room_a,
                                             Room* room_b,
                                             const std::vector<RoomObstacle>& room_obstacles,
                                             const std::vector<TrailObstacle>& existing_trails,
                                             int clearance_px);
bool polyline_is_valid_for_route(const std::vector<SDL_Point>& points,
                                 Room* room_a,
                                 Room* room_b,
                                 const std::vector<RoomObstacle>& room_obstacles,
                                 const std::vector<TrailObstacle>& existing_trails,
                                 int clearance_px);

bool build_fast_diagonal_route(const SDL_Point& start_gate,
                                 const SDL_Point& end_gate,
                                 Room* room_a,
                                 Room* room_b,
                                 const std::vector<RoomObstacle>& room_obstacles,
                                 const std::vector<TrailObstacle>& existing_trails,
                                 int clearance_px,
                                 std::vector<SDL_Point>* out_points) {
    if (!out_points) {
        return false;
    }
    out_points->clear();
    std::vector<std::vector<SDL_Point>> candidates;
    candidates.reserve(10);
    candidates.push_back({start_gate, SDL_Point{end_gate.x, start_gate.y}, end_gate});
    candidates.push_back({start_gate, SDL_Point{start_gate.x, end_gate.y}, end_gate});

    const int detour = std::max(96, clearance_px * 4);
    candidates.push_back({start_gate, SDL_Point{start_gate.x, start_gate.y + detour}, SDL_Point{end_gate.x, start_gate.y + detour}, end_gate});
    candidates.push_back({start_gate, SDL_Point{start_gate.x, start_gate.y - detour}, SDL_Point{end_gate.x, start_gate.y - detour}, end_gate});
    candidates.push_back({start_gate, SDL_Point{start_gate.x + detour, start_gate.y}, SDL_Point{start_gate.x + detour, end_gate.y}, end_gate});
    candidates.push_back({start_gate, SDL_Point{start_gate.x - detour, start_gate.y}, SDL_Point{start_gate.x - detour, end_gate.y}, end_gate});

    for (std::vector<SDL_Point>& candidate : candidates) {
        candidate = sanitize_diagonal_centerline(candidate);
        candidate = simplify_route_points(candidate, room_a, room_b, room_obstacles, existing_trails, clearance_px);
        candidate = sanitize_diagonal_centerline(candidate);
        if (candidate.size() < 2 || !polyline_has_only_exact_diagonal_segments(candidate)) {
            continue;
        }
        if (!polyline_is_valid_for_route(candidate, room_a, room_b, room_obstacles, existing_trails, clearance_px)) {
            continue;
        }
        *out_points = std::move(candidate);
        return true;
    }
    return false;
}

bool point_blocked_for_route(const SDL_Point& point,
                             Room* room_a,
                             Room* room_b,
                             const std::vector<RoomObstacle>& room_obstacles,
                             const std::vector<TrailObstacle>& existing_trails) {
    if (room_a && room_a->room_area && room_a->room_area->contains_point(point)) {
        return true;
    }
    if (room_b && room_b->room_area && room_b->room_area->contains_point(point)) {
        return true;
    }
    if (point_inside_blocking_room(point, room_a, room_b, room_obstacles)) {
        return true;
    }
    return point_inside_existing_trail(point, existing_trails);
}

bool point_blocked_for_route_with_clearance(const SDL_Point& point,
                                            Room* room_a,
                                            Room* room_b,
                                            const std::vector<RoomObstacle>& room_obstacles,
                                            const std::vector<TrailObstacle>& existing_trails,
                                            int clearance_px) {
    if (point_blocked_for_route(point, room_a, room_b, room_obstacles, existing_trails)) {
        return true;
    }
    if (clearance_px <= 1) {
        return false;
    }
    const int half = std::max(1, clearance_px / 2);
    const std::array<SDL_Point, 8> offsets{
        SDL_Point{half, 0},
        SDL_Point{-half, 0},
        SDL_Point{0, half},
        SDL_Point{0, -half},
        SDL_Point{half, half},
        SDL_Point{-half, half},
        SDL_Point{half, -half},
        SDL_Point{-half, -half},
    };
    for (const SDL_Point& offset : offsets) {
        SDL_Point sample{point.x + offset.x, point.y + offset.y};
        if (point_blocked_for_route(sample, room_a, room_b, room_obstacles, existing_trails)) {
            return true;
        }
    }
    return false;
}

std::vector<SDL_Point> dedupe_path_points(const std::vector<SDL_Point>& points) {
    std::vector<SDL_Point> result;
    result.reserve(points.size());
    for (const SDL_Point& point : points) {
        if (!result.empty() && result.back().x == point.x && result.back().y == point.y) {
            continue;
        }
        result.push_back(point);
    }
    return result;
}

bool segment_clear_for_route(const SDL_Point& a,
                             const SDL_Point& b,
                             Room* room_a,
                             Room* room_b,
                             const std::vector<RoomObstacle>& room_obstacles,
                             const std::vector<TrailObstacle>& existing_trails,
                             int clearance_px) {
    const double dx = static_cast<double>(b.x - a.x);
    const double dy = static_cast<double>(b.y - a.y);
    const double dist = std::hypot(dx, dy);
    if (dist <= kPointEpsilon) {
        return !point_blocked_for_route_with_clearance(a, room_a, room_b, room_obstacles, existing_trails, clearance_px);
    }
    const int samples = std::max(2, static_cast<int>(std::ceil(dist / static_cast<double>(kTrailRouteSampleSpacingWorldPx))));
    for (int i = 0; i <= samples; ++i) {
        if (i == 0 || i == samples) {
            continue;
        }
        const double t = static_cast<double>(i) / static_cast<double>(samples);
        SDL_Point sample{
            static_cast<int>(std::lround(static_cast<double>(a.x) + dx * t)),
            static_cast<int>(std::lround(static_cast<double>(a.y) + dy * t)),
        };
        if (point_blocked_for_route_with_clearance(sample, room_a, room_b, room_obstacles, existing_trails, clearance_px)) {
            return false;
        }
    }
    return true;
}

std::vector<SDL_Point> simplify_route_points(const std::vector<SDL_Point>& points,
                                             Room* room_a,
                                             Room* room_b,
                                             const std::vector<RoomObstacle>& room_obstacles,
                                             const std::vector<TrailObstacle>& existing_trails,
                                             int clearance_px) {
    std::vector<SDL_Point> deduped = dedupe_path_points(points);
    if (deduped.size() <= 2) {
        return deduped;
    }
    std::vector<SDL_Point> simplified;
    simplified.reserve(deduped.size());
    std::size_t i = 0;
    simplified.push_back(deduped.front());
    while (i + 1 < deduped.size()) {
        std::size_t best = i + 1;
        for (std::size_t j = deduped.size() - 1; j > i + 1; --j) {
            if (segment_clear_for_route(deduped[i], deduped[j], room_a, room_b, room_obstacles, existing_trails, clearance_px)) {
                best = j;
                break;
            }
        }
        simplified.push_back(deduped[best]);
        i = best;
    }
    return dedupe_path_points(simplified);
}

bool same_checkerboard_parity(const SDL_Point& a, const SDL_Point& b) {
    return ((a.x + a.y - b.x - b.y) & 1) == 0;
}

SDL_Point snap_to_checkerboard_parity(SDL_Point point, const SDL_Point& parity_source) {
    if (same_checkerboard_parity(point, parity_source)) {
        return point;
    }
    point.x += 1;
    return point;
}

bool is_exact_diagonal_segment(const SDL_Point& a, const SDL_Point& b) {
    const int dx = b.x - a.x;
    const int dy = b.y - a.y;
    return dx != 0 && std::abs(dx) == std::abs(dy);
}

Vec2 diagonal_direction(const SDL_Point& a, const SDL_Point& b) {
    const int dx = b.x - a.x;
    const int dy = b.y - a.y;
    const double sx = dx >= 0 ? 1.0 : -1.0;
    const double sy = dy >= 0 ? 1.0 : -1.0;
    constexpr double kInvSqrt2 = 0.70710678118654752440;
    return Vec2{sx * kInvSqrt2, sy * kInvSqrt2};
}

Vec2 nearest_diagonal_direction(const Vec2& vector, const Vec2& fallback = Vec2{1.0, 1.0}) {
    Vec2 source = length(vector) > kPointEpsilon ? vector : fallback;
    constexpr double kInvSqrt2 = 0.70710678118654752440;
    return Vec2{source.x >= 0.0 ? kInvSqrt2 : -kInvSqrt2,
                source.y >= 0.0 ? kInvSqrt2 : -kInvSqrt2};
}

SDL_Point diagonal_uv_to_point(int u, int v) {
    if (((u + v) & 1) != 0) {
        ++v;
    }
    return SDL_Point{(u + v) / 2, (u - v) / 2};
}

std::array<SDL_Point, 2> diagonal_corner_candidates(const SDL_Point& start, const SDL_Point& end) {
    const int su = start.x + start.y;
    const int sv = start.x - start.y;
    int eu = end.x + end.y;
    int ev = end.x - end.y;
    if (((su + ev) & 1) != 0) {
        ++ev;
    }
    if (((eu + sv) & 1) != 0) {
        ++eu;
    }
    return {diagonal_uv_to_point(su, ev), diagonal_uv_to_point(eu, sv)};
}

std::vector<SDL_Point> to_diagonal_polyline(const std::vector<SDL_Point>& points) {
    std::vector<SDL_Point> out;
    if (points.empty()) {
        return out;
    }
    out.reserve(points.size() * 2);
    out.push_back(points.front());
    for (std::size_t i = 1; i < points.size(); ++i) {
        const SDL_Point prev = out.back();
        SDL_Point next = snap_to_checkerboard_parity(points[i], out.front());
        if (prev.x == next.x && prev.y == next.y) {
            continue;
        }
        if (is_exact_diagonal_segment(prev, next)) {
            out.push_back(next);
            continue;
        }

        const auto corners = diagonal_corner_candidates(prev, next);
        const SDL_Point corner_a = corners[0];
        const SDL_Point corner_b = corners[1];
        const double len_a = std::hypot(static_cast<double>(corner_a.x - prev.x), static_cast<double>(corner_a.y - prev.y)) +
                             std::hypot(static_cast<double>(next.x - corner_a.x), static_cast<double>(next.y - corner_a.y));
        const double len_b = std::hypot(static_cast<double>(corner_b.x - prev.x), static_cast<double>(corner_b.y - prev.y)) +
                             std::hypot(static_cast<double>(next.x - corner_b.x), static_cast<double>(next.y - corner_b.y));
        const SDL_Point corner = len_a <= len_b ? corner_a : corner_b;
        if (!(corner.x == prev.x && corner.y == prev.y) && is_exact_diagonal_segment(prev, corner)) {
            out.push_back(corner);
        }
        if (!(next.x == out.back().x && next.y == out.back().y)) {
            if (!is_exact_diagonal_segment(out.back(), next)) {
                const auto repair_corners = diagonal_corner_candidates(out.back(), next);
                for (const SDL_Point& repair : repair_corners) {
                    if (!(repair.x == out.back().x && repair.y == out.back().y) &&
                        is_exact_diagonal_segment(out.back(), repair) &&
                        is_exact_diagonal_segment(repair, next)) {
                        out.push_back(repair);
                        break;
                    }
                }
            }
            out.push_back(next);
        }
    }
    return dedupe_path_points(out);
}


bool polyline_has_only_exact_diagonal_segments(const std::vector<SDL_Point>& polyline) {
    if (polyline.size() < 2) {
        return false;
    }
    for (std::size_t i = 1; i < polyline.size(); ++i) {
        if (!is_exact_diagonal_segment(polyline[i - 1], polyline[i])) {
            return false;
        }
    }
    return true;
}

std::vector<SDL_Point> sanitize_diagonal_centerline(const std::vector<SDL_Point>& points) {
    std::vector<SDL_Point> polyline = dedupe_consecutive_points(to_diagonal_polyline(points));
    if (polyline.size() < 3) {
        return polyline;
    }

    bool changed = true;
    while (changed && polyline.size() >= 3) {
        changed = false;
        std::vector<SDL_Point> reduced;
        reduced.reserve(polyline.size());
        reduced.push_back(polyline.front());
        for (std::size_t i = 1; i + 1 < polyline.size(); ++i) {
            const SDL_Point prev = reduced.back();
            const SDL_Point curr = polyline[i];
            const SDL_Point next = polyline[i + 1];
            if (!is_exact_diagonal_segment(prev, curr) || !is_exact_diagonal_segment(curr, next)) {
                reduced.push_back(curr);
                continue;
            }
            const Vec2 in_dir = diagonal_direction(prev, curr);
            const Vec2 out_dir = diagonal_direction(curr, next);
            if (nearly_equal(in_dir.x, out_dir.x) && nearly_equal(in_dir.y, out_dir.y)) {
                changed = true;
                continue;
            }
            if (nearly_equal(in_dir.x, -out_dir.x) && nearly_equal(in_dir.y, -out_dir.y)) {
                changed = true;
                continue;
            }
            reduced.push_back(curr);
        }
        reduced.push_back(polyline.back());
        polyline = dedupe_consecutive_points(reduced);
    }
    return polyline;
}

struct OffsetDiagonalLine {
    Vec2 point{};
    Vec2 dir{};
};

OffsetDiagonalLine offset_line_for_segment(const SDL_Point& a, const SDL_Point& b, double side_distance) {
    const Vec2 dir = diagonal_direction(a, b);
    const Vec2 normal{-dir.y, dir.x};
    return OffsetDiagonalLine{add(to_vec2(a), scale(normal, side_distance)), dir};
}

Vec2 intersect_offset_lines(const OffsetDiagonalLine& a, const OffsetDiagonalLine& b, const SDL_Point& fallback) {
    const double denom = cross(a.dir, b.dir);
    if (std::abs(denom) <= kPointEpsilon) {
        return to_vec2(fallback);
    }
    const Vec2 delta = subtract(b.point, a.point);
    const double t = cross(delta, b.dir) / denom;
    return add(a.point, scale(a.dir, t));
}

SDL_Point offset_endpoint_for_segment(const SDL_Point& point,
                                      const SDL_Point& other,
                                      double side_distance) {
    const Vec2 dir = diagonal_direction(point, other);
    const Vec2 normal{-dir.y, dir.x};
    return to_point(add(to_vec2(point), scale(normal, side_distance)));
}

bool build_offset_chain(const std::vector<SDL_Point>& centerline,
                        double side_distance,
                        std::vector<SDL_Point>* out_chain) {
    if (!out_chain || centerline.size() < 2) {
        return false;
    }
    out_chain->clear();
    out_chain->reserve(centerline.size());
    out_chain->push_back(offset_endpoint_for_segment(centerline.front(), centerline[1], side_distance));
    for (std::size_t i = 1; i + 1 < centerline.size(); ++i) {
        const OffsetDiagonalLine prev = offset_line_for_segment(centerline[i - 1], centerline[i], side_distance);
        const OffsetDiagonalLine next = offset_line_for_segment(centerline[i], centerline[i + 1], side_distance);
        out_chain->push_back(to_point(intersect_offset_lines(prev, next, centerline[i])));
    }
    out_chain->push_back(offset_endpoint_for_segment(centerline.back(), centerline[centerline.size() - 2], side_distance));
    *out_chain = dedupe_consecutive_points(*out_chain);
    return out_chain->size() >= 2;
}

void normalize_polygon_order(std::vector<SDL_Point>* polygon) {
    if (!polygon || polygon->size() < 3) {
        return;
    }
    std::vector<SDL_Point>& points = *polygon;
    if (std::abs(signed_polygon_area(points)) > 1e-6 && signed_polygon_area(points) > 0.0) {
        std::reverse(points.begin(), points.end());
    }
    std::size_t start_index = 0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (points[i].y < points[start_index].y ||
            (points[i].y == points[start_index].y && points[i].x < points[start_index].x)) {
            start_index = i;
        }
    }
    std::rotate(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(start_index), points.end());
}

bool build_diagonal_corridor_polygon(const std::vector<SDL_Point>& centerline,
                                       int width,
                                       std::vector<SDL_Point>* out_polygon) {
    if (!out_polygon || centerline.size() < 2 || !polyline_has_only_exact_diagonal_segments(centerline)) {
        return false;
    }
    const int clamped_width = std::max(1, width);
    const double left_distance = static_cast<double>(clamped_width) * 0.5;
    const double right_distance = -left_distance;
    std::vector<SDL_Point> left_chain;
    std::vector<SDL_Point> right_chain;
    if (!build_offset_chain(centerline, left_distance, &left_chain) ||
        !build_offset_chain(centerline, right_distance, &right_chain)) {
        return false;
    }
    std::vector<SDL_Point> polygon;
    polygon.reserve(left_chain.size() + right_chain.size());
    polygon.insert(polygon.end(), left_chain.begin(), left_chain.end());
    for (std::size_t i = right_chain.size(); i-- > 0;) {
        polygon.push_back(right_chain[i]);
    }
    polygon = dedupe_consecutive_points(polygon);
    if (polygon.size() >= 2 &&
        polygon.front().x == polygon.back().x &&
        polygon.front().y == polygon.back().y) {
        polygon.pop_back();
    }
    normalize_polygon_order(&polygon);
    if (polygon.size() < 4) {
        return false;
    }
    *out_polygon = std::move(polygon);
    return true;
}

RouteSearchResult build_routed_polyline(const SDL_Point& start_gate,
                                        const SDL_Point& end_gate,
                                        Room* room_a,
                                        Room* room_b,
                                        const std::vector<RoomObstacle>& room_obstacles,
                                        const std::vector<TrailObstacle>& existing_trails,
                                        int clearance_px) {
    RouteSearchResult result;

    Bounds bounds{};
    bounds.min_x = std::min(start_gate.x, end_gate.x);
    bounds.max_x = std::max(start_gate.x, end_gate.x);
    bounds.min_y = std::min(start_gate.y, end_gate.y);
    bounds.max_y = std::max(start_gate.y, end_gate.y);
    for (const RoomObstacle& obstacle : room_obstacles) {
        bounds.min_x = std::min(bounds.min_x, obstacle.bounds.min_x);
        bounds.max_x = std::max(bounds.max_x, obstacle.bounds.max_x);
        bounds.min_y = std::min(bounds.min_y, obstacle.bounds.min_y);
        bounds.max_y = std::max(bounds.max_y, obstacle.bounds.max_y);
    }
    for (const TrailObstacle& trail : existing_trails) {
        bounds.min_x = std::min(bounds.min_x, trail.bounds.min_x);
        bounds.max_x = std::max(bounds.max_x, trail.bounds.max_x);
        bounds.min_y = std::min(bounds.min_y, trail.bounds.min_y);
        bounds.max_y = std::max(bounds.max_y, trail.bounds.max_y);
    }

    const double margin = std::max(200.0, static_cast<double>(clearance_px) * 3.0);
    bounds.min_x -= margin;
    bounds.min_y -= margin;
    bounds.max_x += margin;
    bounds.max_y += margin;

    int cell_size = std::max(kTrailRouteInitialCellSizeWorldPx, std::max(16, clearance_px / 2));
    int grid_w = 0;
    int grid_h = 0;
    auto recompute_grid = [&]() {
        grid_w = std::max(3, static_cast<int>(std::ceil((bounds.max_x - bounds.min_x) / static_cast<double>(cell_size))) + 1);
        grid_h = std::max(3, static_cast<int>(std::ceil((bounds.max_y - bounds.min_y) / static_cast<double>(cell_size))) + 1);
    };
    recompute_grid();
    while ((grid_w > kTrailRouteSearchMaxCellSpan || grid_h > kTrailRouteSearchMaxCellSpan) && cell_size < 2048) {
        cell_size = static_cast<int>(std::ceil(cell_size * 1.5));
        recompute_grid();
    }

    auto to_cell = [&](const SDL_Point& point) {
        const int x = static_cast<int>(std::floor((static_cast<double>(point.x) - bounds.min_x) / static_cast<double>(cell_size)));
        const int y = static_cast<int>(std::floor((static_cast<double>(point.y) - bounds.min_y) / static_cast<double>(cell_size)));
        return SDL_Point{std::clamp(x, 0, grid_w - 1), std::clamp(y, 0, grid_h - 1)};
    };
    auto to_world = [&](int cx, int cy) {
        return SDL_Point{
            static_cast<int>(std::lround(bounds.min_x + (static_cast<double>(cx) + 0.5) * static_cast<double>(cell_size))),
            static_cast<int>(std::lround(bounds.min_y + (static_cast<double>(cy) + 0.5) * static_cast<double>(cell_size))),
        };
    };
    auto cell_index = [&](int cx, int cy) {
        return cy * grid_w + cx;
    };

    const SDL_Point raw_start = to_cell(start_gate);
    const SDL_Point raw_goal = to_cell(end_gate);

    std::vector<int8_t> blocked_cache(static_cast<std::size_t>(grid_w * grid_h), -1);
    auto is_blocked_cell = [&](int cx, int cy) {
        const int index = cell_index(cx, cy);
        int8_t cached = blocked_cache[static_cast<std::size_t>(index)];
        if (cached != -1) {
            return cached == 1;
        }
        const SDL_Point sample = to_world(cx, cy);
        const bool blocked = point_blocked_for_route_with_clearance(
            sample, room_a, room_b, room_obstacles, existing_trails, clearance_px);
        blocked_cache[static_cast<std::size_t>(index)] = blocked ? 1 : 0;
        return blocked;
    };

    auto find_open_cell = [&](const SDL_Point& seed) {
        if (!is_blocked_cell(seed.x, seed.y)) {
            return seed;
        }
        for (int radius = 1; radius <= 6; ++radius) {
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (std::abs(dx) != radius && std::abs(dy) != radius) {
                        continue;
                    }
                    const int nx = seed.x + dx;
                    const int ny = seed.y + dy;
                    if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h) {
                        continue;
                    }
                    if (!is_blocked_cell(nx, ny)) {
                        return SDL_Point{nx, ny};
                    }
                }
            }
        }
        return seed;
    };

    const SDL_Point start = find_open_cell(raw_start);
    SDL_Point goal = find_open_cell(raw_goal);
    if (((start.x + start.y - goal.x - goal.y) & 1) != 0) {
        const std::array<SDL_Point, 4> parity_repairs{
            SDL_Point{1, 0}, SDL_Point{-1, 0}, SDL_Point{0, 1}, SDL_Point{0, -1},
        };
        for (const SDL_Point& repair : parity_repairs) {
            const int nx = goal.x + repair.x;
            const int ny = goal.y + repair.y;
            if (nx >= 0 && ny >= 0 && nx < grid_w && ny < grid_h && !is_blocked_cell(nx, ny)) {
                goal = SDL_Point{nx, ny};
                break;
            }
        }
    }
    if (is_blocked_cell(start.x, start.y) || is_blocked_cell(goal.x, goal.y) ||
        ((start.x + start.y - goal.x - goal.y) & 1) != 0) {
        return result;
    }

    struct OpenNode {
        int index = -1;
        double score = 0.0;
        bool operator<(const OpenNode& other) const {
            return score > other.score;
        }
    };

    const int node_count = grid_w * grid_h;
    std::vector<double> g_score(static_cast<std::size_t>(node_count), std::numeric_limits<double>::infinity());
    std::vector<int> parent(static_cast<std::size_t>(node_count), -1);
    std::vector<uint8_t> closed(static_cast<std::size_t>(node_count), 0);
    std::priority_queue<OpenNode> open;

    const int start_index = cell_index(start.x, start.y);
    const int goal_index = cell_index(goal.x, goal.y);
    g_score[static_cast<std::size_t>(start_index)] = 0.0;
    open.push(OpenNode{start_index, 0.0});

    auto heuristic = [&](int cx, int cy) {
        const double dx = static_cast<double>(goal.x - cx);
        const double dy = static_cast<double>(goal.y - cy);
        return std::hypot(dx, dy);
    };

    static const std::array<SDL_Point, 4> kNeighbors{
        SDL_Point{1, 1}, SDL_Point{1, -1}, SDL_Point{-1, 1}, SDL_Point{-1, -1},
    };

    int expansions = 0;
    while (!open.empty()) {
        const OpenNode current = open.top();
        open.pop();
        if (current.index < 0 || current.index >= node_count) {
            continue;
        }
        if (closed[static_cast<std::size_t>(current.index)] != 0) {
            continue;
        }
        closed[static_cast<std::size_t>(current.index)] = 1;
        if (++expansions > kTrailRouteSearchMaxExpansions) {
            result.budget_exhausted = true;
            return result;
        }
        if (current.index == goal_index) {
            break;
        }

        const int cx = current.index % grid_w;
        const int cy = current.index / grid_w;
        for (const SDL_Point& offset : kNeighbors) {
            const int nx = cx + offset.x;
            const int ny = cy + offset.y;
            if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h) {
                continue;
            }
            if (is_blocked_cell(nx, ny)) {
                continue;
            }
            const int neighbor_index = cell_index(nx, ny);
            const double tentative = g_score[static_cast<std::size_t>(current.index)] + 1.4142135623730951;
            if (tentative + 1e-9 >= g_score[static_cast<std::size_t>(neighbor_index)]) {
                continue;
            }
            g_score[static_cast<std::size_t>(neighbor_index)] = tentative;
            parent[static_cast<std::size_t>(neighbor_index)] = current.index;
            const double f_score = tentative + heuristic(nx, ny);
            open.push(OpenNode{neighbor_index, f_score});
        }
    }

    if (parent[static_cast<std::size_t>(goal_index)] == -1 && goal_index != start_index) {
        return result;
    }

    std::vector<SDL_Point> route_points;
    int walk = goal_index;
    route_points.push_back(end_gate);
    route_points.push_back(to_world(goal.x, goal.y));
    while (walk != -1 && walk != start_index) {
        const int cx = walk % grid_w;
        const int cy = walk / grid_w;
        route_points.push_back(to_world(cx, cy));
        walk = parent[static_cast<std::size_t>(walk)];
    }
    route_points.push_back(to_world(start.x, start.y));
    route_points.push_back(start_gate);
    std::reverse(route_points.begin(), route_points.end());
    route_points = to_diagonal_polyline(route_points);
    route_points = simplify_route_points(route_points, room_a, room_b, room_obstacles, existing_trails, clearance_px);
    route_points = to_diagonal_polyline(route_points);
    result.success = route_points.size() >= 2;
    result.points = std::move(route_points);
    return result;
}

bool compute_contact_gate_from_center(const SDL_Point& contact,
                                      const SDL_Point& center,
                                      Room* room_a,
                                      Room* room_b,
                                      const std::vector<RoomObstacle>& room_obstacles,
                                      const std::vector<TrailObstacle>& existing_trails,
                                      int clearance_px,
                                      SDL_Point* out_gate) {
    if (!out_gate) {
        return false;
    }

    const Vec2 radial = subtract(to_vec2(contact), to_vec2(center));
    if (length(radial) <= kPointEpsilon) {
        return false;
    }
    const Vec2 dir = nearest_diagonal_direction(radial);

    for (int i = 0; i <= kTrailContactGateIterations; ++i) {
        const double distance = static_cast<double>(i) * kTrailContactGateStepWorldPx;
        SDL_Point candidate{
            static_cast<int>(std::lround(static_cast<double>(contact.x) + dir.x * distance)),
            static_cast<int>(std::lround(static_cast<double>(contact.y) + dir.y * distance)),
        };
        if (!point_blocked_for_route_with_clearance(
                candidate, room_a, room_b, room_obstacles, existing_trails, std::max(1, clearance_px))) {
            *out_gate = candidate;
            return true;
        }
    }

    return false;
}

bool compute_contact_gate(const SDL_Point& contact,
                          Room* anchor_room,
                          Room* room_a,
                          Room* room_b,
                          const std::vector<RoomObstacle>& room_obstacles,
                          const std::vector<TrailObstacle>& existing_trails,
                          int clearance_px,
                          SDL_Point* out_gate) {
    if (!anchor_room || !anchor_room->room_area || !out_gate) {
        return false;
    }

    const SDL_Point center = anchor_room->room_area->get_center();
    return compute_contact_gate_from_center(
        contact, center, room_a, room_b, room_obstacles, existing_trails, clearance_px, out_gate);
}

std::vector<SDL_Point> stitch_route_with_contacts(const SDL_Point& start_contact,
                                                  const std::vector<SDL_Point>& routed_points,
                                                  const SDL_Point& end_contact) {
    std::vector<SDL_Point> stitched;
    stitched.reserve(routed_points.size() + 2);
    stitched.push_back(start_contact);
    stitched.insert(stitched.end(), routed_points.begin(), routed_points.end());
    stitched.push_back(end_contact);
    return dedupe_consecutive_points(stitched);
}

bool point_blocked_for_route_with_clearance(const SDL_Point& point,
                                            Room* room_a,
                                            Room* room_b,
                                            const std::vector<RoomObstacle>& room_obstacles,
                                            const std::vector<TrailObstacle>& existing_trails,
                                            int clearance_px);

SDL_Point room_margin_point(const SDL_Point& room_center, const SDL_Point& room_edge_contact) {
    const Vec2 outward = nearest_diagonal_direction(subtract(to_vec2(room_edge_contact), to_vec2(room_center)));
    const Vec2 margin = add(to_vec2(room_edge_contact), scale(outward, kRoomMarginDistanceWorldPx));
    return snap_to_checkerboard_parity(to_point(margin), room_edge_contact);
}

EndpointFrame make_endpoint_frame(const SDL_Point& room_center,
                                  const SDL_Point& room_edge_contact,
                                  double inside_depth) {
    EndpointFrame frame;
    frame.contact = room_edge_contact;
    frame.margin = room_margin_point(room_center, room_edge_contact);
    frame.outward = nearest_diagonal_direction(subtract(to_vec2(room_edge_contact), to_vec2(room_center)));
    frame.inside_depth = std::max(0.0, inside_depth);
    return frame;
}

SDL_Point endpoint_inside_tip(const EndpointFrame& frame) {
    return to_point(subtract(to_vec2(frame.contact), scale(frame.outward, frame.inside_depth)));
}

std::vector<SDL_Point> build_centerline_with_endpoint_frames(const EndpointFrame& start_frame,
                                                             const std::vector<SDL_Point>& routed_points,
                                                             const EndpointFrame& end_frame) {
    std::vector<SDL_Point> points;
    points.reserve(routed_points.size() + 6);
    points.push_back(endpoint_inside_tip(start_frame));
    points.push_back(start_frame.contact);
    points.push_back(start_frame.margin);
    points.insert(points.end(), routed_points.begin(), routed_points.end());
    points.push_back(end_frame.margin);
    points.push_back(end_frame.contact);
    points.push_back(endpoint_inside_tip(end_frame));
    return dedupe_consecutive_points(points);
}

double endpoint_required_depth_from_section(const TrailSection& section) {
    const double half_width = std::max(0.0, length(subtract(section.left, section.right)) * 0.5);
    return std::ceil(half_width + kTrailEndpointContainmentSafetyWorldPx);
}

double max_endpoint_depth_to_room_center(const SDL_Point& room_center, const SDL_Point& room_edge_contact) {
    const Vec2 delta = subtract(to_vec2(room_edge_contact), to_vec2(room_center));
    return std::max(0.0, length(delta) - 2.0);
}

bool endpoint_section_contained(const TrailSection& section, const Area* room_area) {
    if (!room_area) {
        return false;
    }
    const SDL_Point center = to_point(section.center);
    const SDL_Point left = to_point(section.left);
    const SDL_Point right = to_point(section.right);
    return room_area->contains_point(center) && room_area->contains_point(left) && room_area->contains_point(right);
}

bool polyline_is_valid_for_route(const std::vector<SDL_Point>& points,
                                 Room* room_a,
                                 Room* room_b,
                                 const std::vector<RoomObstacle>& room_obstacles,
                                 const std::vector<TrailObstacle>& existing_trails,
                                 int clearance_px) {
    if (points.size() < 2) {
        return false;
    }
    const double spacing = std::max(12.0, static_cast<double>(kTrailRouteSampleSpacingWorldPx) * 0.5);
    for (std::size_t i = 1; i < points.size(); ++i) {
        const Vec2 a = to_vec2(points[i - 1]);
        const Vec2 b = to_vec2(points[i]);
        const Vec2 delta = subtract(b, a);
        const double len = length(delta);
        if (len <= kPointEpsilon) {
            continue;
        }
        const int samples = std::max(1, static_cast<int>(std::ceil(len / spacing)));
        for (int s = 0; s <= samples; ++s) {
            const double t = static_cast<double>(s) / static_cast<double>(samples);
            const SDL_Point sample = to_point(add(a, scale(delta, t)));
            if (point_blocked_for_route_with_clearance(
                    sample, room_a, room_b, room_obstacles, existing_trails, clearance_px)) {
                return false;
            }
        }
    }
    return true;
}

bool polyline_is_valid_for_anchor_exit(const std::vector<SDL_Point>& points,
                                       Room* anchor_room,
                                       Room* room_a,
                                       Room* room_b,
                                       const std::vector<RoomObstacle>& room_obstacles,
                                       const std::vector<TrailObstacle>& existing_trails,
                                       int clearance_px) {
    if (!anchor_room || !anchor_room->room_area || points.size() < 2) {
        return false;
    }

    const double spacing = std::max(12.0, static_cast<double>(kTrailRouteSampleSpacingWorldPx) * 0.5);
    for (std::size_t i = 1; i < points.size(); ++i) {
        const Vec2 a = to_vec2(points[i - 1]);
        const Vec2 b = to_vec2(points[i]);
        const Vec2 delta = subtract(b, a);
        const double len = length(delta);
        if (len <= kPointEpsilon) {
            continue;
        }
        const int samples = std::max(1, static_cast<int>(std::ceil(len / spacing)));
        for (int s = 0; s <= samples; ++s) {
            const bool segment_endpoint = (i == 1 && s == 0) || (i + 1 == points.size() && s == samples);
            if (segment_endpoint) {
                continue;
            }
            const double t = static_cast<double>(s) / static_cast<double>(samples);
            const SDL_Point sample = to_point(add(a, scale(delta, t)));

            const bool in_anchor_room = anchor_room->room_area->contains_point(sample);
            if (in_anchor_room) {
                continue;
            }
            if (point_inside_blocking_room(sample, room_a, room_b, room_obstacles)) {
                return false;
            }
            if (room_a && room_a != anchor_room && room_a->room_area && room_a->room_area->contains_point(sample)) {
                return false;
            }
            if (room_b && room_b != anchor_room && room_b->room_area && room_b->room_area->contains_point(sample)) {
                return false;
            }
            if (point_inside_existing_trail(sample, existing_trails)) {
                return false;
            }

            if (clearance_px > 1) {
                const int half = std::max(1, clearance_px / 2);
                const std::array<SDL_Point, 8> offsets{
                    SDL_Point{half, 0},
                    SDL_Point{-half, 0},
                    SDL_Point{0, half},
                    SDL_Point{0, -half},
                    SDL_Point{half, half},
                    SDL_Point{-half, half},
                    SDL_Point{half, -half},
                    SDL_Point{-half, -half},
                };
                for (const SDL_Point& offset : offsets) {
                    const SDL_Point expanded{sample.x + offset.x, sample.y + offset.y};
                    const bool expanded_in_anchor = anchor_room->room_area->contains_point(expanded);
                    if (expanded_in_anchor) {
                        continue;
                    }
                    if (point_inside_blocking_room(expanded, room_a, room_b, room_obstacles) ||
                        point_inside_existing_trail(expanded, existing_trails) ||
                        (room_a && room_a != anchor_room && room_a->room_area && room_a->room_area->contains_point(expanded)) ||
                        (room_b && room_b != anchor_room && room_b->room_area && room_b->room_area->contains_point(expanded))) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

std::vector<std::vector<SDL_Point>> build_centerline_variants(const std::vector<SDL_Point>& base_points,
                                                              int,
                                                              const std::string& room_a_name,
                                                              const std::string& room_b_name) {
    (void)room_a_name;
    (void)room_b_name;
    std::vector<std::vector<SDL_Point>> variants;
    variants.push_back(dedupe_consecutive_points(base_points));
    return variants;
}

bool boundary_zone_within_sector_for_area(const std::vector<SDL_Point>& centerline_points,
                                          const Area* area,
                                          const TrailConnectionSector& sector,
                                          bool from_start) {
    if (!area || centerline_points.size() < 2) {
        return false;
    }

    const std::vector<double> cumulative = build_polyline_cumulative_lengths(centerline_points);
    const double total_length = polyline_total_length(cumulative);
    if (total_length <= kPointEpsilon) {
        return false;
    }

    const double zone_length = std::min(total_length, static_cast<double>(kTrailBoundaryZoneDistanceWorldPx));
    const double step = std::max(12.0, static_cast<double>(kTrailRouteSampleSpacingWorldPx) * 0.5);
    const int sample_count = std::max(1, static_cast<int>(std::ceil(zone_length / step)));
    for (int i = 0; i <= sample_count; ++i) {
        const double local = std::min(zone_length, static_cast<double>(i) * step);
        const double distance = from_start ? local : (total_length - local);
        Vec2 point{};
        Vec2 tangent{};
        if (!sample_polyline_at_distance(centerline_points, cumulative, distance, &point, &tangent)) {
            return false;
        }
        const SDL_Point point_px = to_point(point);
        const SDL_Point center = area->get_center();
        const double angle_deg = angle_degrees_from_center(center, point_px);
        if (!angle_in_sector(angle_deg, sector)) {
            return false;
        }
    }

    return true;
}

bool boundary_zone_within_sector(const std::vector<SDL_Point>& centerline_points, Room* room, bool from_start) {
    if (!room || !room->room_area) {
        return false;
    }
    const TrailConnectionSector sector = sector_from_room(room);
    return boundary_zone_within_sector_for_area(centerline_points, room->room_area.get(), sector, from_start);
}

bool boundary_zones_respect_room_sectors(const std::vector<SDL_Point>& centerline_points, Room* room_a, Room* room_b) {
    if (!room_a || !room_b || !room_a->room_area || !room_b->room_area || centerline_points.size() < 2) {
        return false;
    }
    const TrailConnectionSector sector_a = sector_from_room(room_a);
    const TrailConnectionSector sector_b = sector_from_room(room_b);
    return point_is_in_room_sector(centerline_points.front(), room_a, sector_a) &&
           point_is_in_room_sector(centerline_points.back(), room_b, sector_b);
}

bool centerline_is_valid(Room* room_a,
                         Room* room_b,
                         const std::vector<CenterlineSample>& validation_samples,
                         const std::vector<RoomObstacle>& room_obstacles,
                         const std::vector<TrailObstacle>& existing_trails,
                         TrailAttemptStats* stats,
                         bool allow_connected_room_interior,
                         const EndpointAllowance& allowance = EndpointAllowance{}) {
    const double total_distance = validation_samples.empty() ? 0.0 : validation_samples.back().distance;
    for (std::size_t i = 0; i < validation_samples.size(); ++i) {
        const CenterlineSample& sample = validation_samples[i];
        const SDL_Point point = to_point(sample.point);
        const bool within_start_allowance = sample.distance <= std::max(0.0, allowance.start_distance) + 1e-6;
        const bool within_end_allowance =
            (total_distance - sample.distance) <= std::max(0.0, allowance.end_distance) + 1e-6;
        if (point_inside_blocking_room(point, room_a, room_b, room_obstacles)) {
            if (stats) {
                ++stats->centerline_room_rejections;
                stats->last_failure = TrailFailureReason::BlockedByRoom;
            }
            return false;
        }
        const bool in_room_a = room_a && room_a->room_area && room_a->room_area->contains_point(point);
        const bool in_room_b = room_b && room_b->room_area && room_b->room_area->contains_point(point);
        if (in_room_a || in_room_b) {
            const bool endpoint_sample = (i == 0 || i + 1 == validation_samples.size());
            const bool in_allowed_endpoint_zone =
                (in_room_a && within_start_allowance) || (in_room_b && within_end_allowance);
            if (!allow_connected_room_interior && !endpoint_sample && !in_allowed_endpoint_zone) {
                if (stats) {
                    ++stats->centerline_room_rejections;
                    stats->last_failure = TrailFailureReason::BlockedByRoom;
                }
                return false;
            }
            continue;
        }
        if (point_inside_existing_trail(point, existing_trails)) {
            if (within_start_allowance || within_end_allowance) {
                continue;
            }
            if (stats) {
                ++stats->centerline_trail_rejections;
                stats->last_failure = TrailFailureReason::BlockedByTrail;
            }
            return false;
        }
    }
    return true;
}

bool place_sections(const std::vector<CenterlineSample>& section_samples,
                    int min_width,
                    int max_width,
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
    for (const CenterlineSample& sample : section_samples) {
        bool placed = false;
        for (int attempt = 0; attempt < kSectionPlacementAttempts && !placed; ++attempt) {
            const int width = width_dist(rng);
            const double half_width = static_cast<double>(std::max(1, width)) * 0.5;
            double shift = 0.0;
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
                        int,
                        const std::vector<double>& required_distances,
                        Room* room_a,
                        Room* room_b,
                        const std::vector<RoomObstacle>& room_obstacles,
                        const std::vector<TrailObstacle>& existing_trails,
                        std::mt19937& rng,
                        TrailAttemptStats* stats,
                        TrailBuildResult* out_result,
                        bool allow_connected_room_interior) {
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

        const std::vector<CenterlineSample> validation_samples = build_centerline_samples(start,
                                                                                          dir,
                                                                                          normal,
                                                                                          validation_distances);
        if (!centerline_is_valid(
                room_a, room_b, validation_samples, room_obstacles, existing_trails, stats, allow_connected_room_interior)) {
            continue;
        }

        const std::vector<CenterlineSample> section_samples = build_centerline_samples(start,
                                                                                       dir,
                                                                                       normal,
                                                                                       section_distances);
        std::vector<TrailSection> sections;
        if (!place_sections(section_samples, min_width, max_width, rng, stats, &sections)) {
            continue;
        }

        std::vector<SDL_Point> polygon = build_polygon(start, end, sections);
        if (!trail_polygon_is_clean(polygon, existing_trails)) {
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

bool sections_respect_connected_room_boundaries(const std::vector<TrailSection>& sections,
                                                Room* room_a,
                                                Room* room_b,
                                                const EndpointAllowance& allowance = EndpointAllowance{}) {
    if (sections.size() <= 2) {
        return true;
    }
    const Area* area_a = room_a ? room_a->room_area.get() : nullptr;
    const Area* area_b = room_b ? room_b->room_area.get() : nullptr;
    auto inside_connected = [&](const Vec2& value) {
        const SDL_Point point = to_point(value);
        if (area_a && area_a->contains_point(point)) {
            return true;
        }
        if (area_b && area_b->contains_point(point)) {
            return true;
        }
        return false;
    };

    const double total_distance = sections.empty() ? 0.0 : sections.back().distance;
    for (std::size_t i = 1; i + 1 < sections.size(); ++i) {
        const double distance = sections[i].distance;
        const bool in_start_allowance = distance <= std::max(0.0, allowance.start_distance) + 1e-6;
        const bool in_end_allowance = (total_distance - distance) <= std::max(0.0, allowance.end_distance) + 1e-6;
        if ((in_start_allowance || in_end_allowance)) {
            continue;
        }
        if (inside_connected(sections[i].center) || inside_connected(sections[i].left) || inside_connected(sections[i].right)) {
            return false;
        }
    }
    return true;
}

bool build_trail_layout_from_polyline(const std::vector<SDL_Point>& centerline_points,
                                      int min_width,
                                      int max_width,
                                      int,
                                      const std::vector<double>& required_distances,
                                      Room* room_a,
                                      Room* room_b,
                                      const std::vector<RoomObstacle>& room_obstacles,
                                      const std::vector<TrailObstacle>& existing_trails,
                                      std::mt19937& rng,
                                      TrailAttemptStats* stats,
                                      TrailBuildResult* out_result,
                                      const EndpointAllowance& endpoint_allowance = EndpointAllowance{}) {
    if (!out_result) {
        if (stats) {
            stats->last_failure = TrailFailureReason::InvalidArgs;
        }
        return false;
    }

    out_result->sections.clear();
    out_result->polygon.clear();
    out_result->centerline.clear();

    std::vector<SDL_Point> polyline = sanitize_diagonal_centerline(centerline_points);
    if (polyline.size() < 2 || !polyline_has_only_exact_diagonal_segments(polyline)) {
        if (stats) {
            stats->last_failure = TrailFailureReason::DegenerateCenterline;
        }
        return false;
    }

    if (max_width < min_width) {
        std::swap(min_width, max_width);
    }
    min_width = std::max(1, min_width);
    max_width = std::max(min_width, max_width);

    const std::vector<double> cumulative = build_polyline_cumulative_lengths(polyline);
    const double trail_length = polyline_total_length(cumulative);
    if (trail_length <= 1.0) {
        if (stats) {
            stats->last_failure = TrailFailureReason::DegenerateCenterline;
        }
        return false;
    }

    std::vector<double> required = required_distances;
    for (double& value : required) {
        value = std::clamp(value, 0.0, trail_length);
    }
    required.push_back(0.0);
    required.push_back(trail_length);

    const std::vector<double> section_distances = build_section_distances(trail_length, required);
    if (section_distances.empty()) {
        if (stats) {
            stats->last_failure = TrailFailureReason::EmptySectionDistances;
        }
        return false;
    }
    const std::vector<double> validation_distances = build_validation_distances(trail_length, required);
    if (validation_distances.empty()) {
        if (stats) {
            stats->last_failure = TrailFailureReason::EmptyValidationDistances;
        }
        return false;
    }

    if (stats) {
        ++stats->layout_attempts;
        ++stats->straight_attempts;
    }

    const std::vector<CenterlineSample> validation_samples =
        build_centerline_samples_from_polyline(polyline, validation_distances);
    if (validation_samples.empty()) {
        if (stats) {
            stats->last_failure = TrailFailureReason::DegenerateCenterline;
        }
        return false;
    }
    if (!centerline_is_valid(room_a,
                             room_b,
                             validation_samples,
                             room_obstacles,
                             existing_trails,
                             stats,
                             false,
                             endpoint_allowance)) {
        return false;
    }

    const std::vector<CenterlineSample> section_samples =
        build_centerline_samples_from_polyline(polyline, section_distances);
    if (section_samples.empty()) {
        if (stats) {
            stats->last_failure = TrailFailureReason::EmptySectionDistances;
        }
        return false;
    }

    std::vector<TrailSection> sections;
    if (!place_sections(section_samples, min_width, max_width, rng, stats, &sections)) {
        return false;
    }
    if (!sections_respect_connected_room_boundaries(sections, room_a, room_b, endpoint_allowance)) {
        if (stats) {
            ++stats->centerline_room_rejections;
            stats->last_failure = TrailFailureReason::BlockedByRoom;
        }
        return false;
    }

    std::vector<SDL_Point> polygon;
    if (!build_diagonal_corridor_polygon(polyline, min_width, &polygon)) {
        if (stats) {
            ++stats->polygon_failures;
            stats->last_failure = TrailFailureReason::PolygonFailed;
        }
        return false;
    }
    if (!trail_polygon_is_clean(polygon, existing_trails)) {
        if (stats) {
            ++stats->polygon_failures;
            stats->last_failure = TrailFailureReason::PolygonTooSmall;
        }
        return false;
    }

    out_result->start_tip = polyline.front();
    out_result->end_tip = polyline.back();
    out_result->sections = std::move(sections);
    out_result->polygon = std::move(polygon);
    out_result->centerline = std::move(polyline);
    if (stats) {
        stats->last_failure = TrailFailureReason::None;
    }
    return true;
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
                              TrailAttemptStats* stats,
                              Room* split_host_allow = nullptr) {
    using clock = std::chrono::steady_clock;

    if (!a || !b || !a->room_area || !b->room_area || !trail_config) {
        if (stats) {
            stats->last_failure = TrailFailureReason::InvalidArgs;
        }
        return false;
    }

    std::vector<TrailObstacle> filtered_trails;
    const std::vector<TrailObstacle>* routing_trails = &existing_trails;
    if (split_host_allow != nullptr) {
        filtered_trails.reserve(existing_trails.size());
        for (const TrailObstacle& obstacle : existing_trails) {
            if (obstacle.room == split_host_allow) {
                continue;
            }
            filtered_trails.push_back(obstacle);
        }
        routing_trails = &filtered_trails;
    }

    json& config = *trail_config;
    const auto default_width = vibble::weighted_range::make_legacy_uniform(40, 40);
    vibble::weighted_range::WeightedIntRange width_range = config.contains("width")
        ? read_weighted_range_field(config, "width", default_width)
        : (config.contains("min_width") || config.contains("max_width")
               ? read_weighted_range_legacy_pair(config, "min_width", "max_width", default_width)
               : default_width);
    if (!config.contains("width") && !config.contains("min_width") && !config.contains("max_width") &&
        (config.contains("min_radius") || config.contains("max_radius") || config.contains("radius"))) {
        width_range = read_weighted_range_legacy_pair(config, "min_radius", "max_radius", default_width);
    }
    const int resolved_width = std::max(1, resolve_weighted_dimension(width_range, rng));
    const int min_width = resolved_width;
    const int max_width = resolved_width;
    const std::string name = config.value("name", trail_name.empty() ? std::string("trail_segment") : trail_name);
    const int routing_clearance_px = std::max(8, max_width / 2 + 12);
    const int gate_clearance_px = std::max(4, max_width / 3);
    const double default_endpoint_depth =
        std::max(kTrailEndpointContainmentSafetyWorldPx, std::ceil(static_cast<double>(max_width) * 0.5 + kTrailEndpointContainmentSafetyWorldPx));

    const SDL_Point a_center = a->room_area->get_center();
    const SDL_Point b_center = b->room_area->get_center();
    std::vector<SDL_Point> a_contacts = collect_sector_contact_candidates(a, b_center);
    std::vector<SDL_Point> b_contacts = collect_sector_contact_candidates(b, a_center);
    if (a_contacts.empty() || b_contacts.empty()) {
        if (stats) {
            ++stats->sector_contact_failures;
            stats->last_failure = TrailFailureReason::NoValidSectorContacts;
        }
        return false;
    }

    struct ContactPairCandidate {
        std::size_t a_index = 0;
        std::size_t b_index = 0;
        double score = 0.0;
    };

    std::vector<ContactPairCandidate> pair_candidates;
    pair_candidates.reserve(a_contacts.size() * b_contacts.size());
    for (std::size_t i = 0; i < a_contacts.size(); ++i) {
        for (std::size_t j = 0; j < b_contacts.size(); ++j) {
            const double dx = static_cast<double>(a_contacts[i].x - b_contacts[j].x);
            const double dy = static_cast<double>(a_contacts[i].y - b_contacts[j].y);
            const double distance = std::hypot(dx, dy);
            const double rank_bias = static_cast<double>(i + j) * 25.0;
            pair_candidates.push_back(ContactPairCandidate{i, j, distance + rank_bias});
        }
    }
    std::sort(pair_candidates.begin(), pair_candidates.end(), [](const ContactPairCandidate& lhs, const ContactPairCandidate& rhs) {
        return lhs.score < rhs.score;
    });

    if (pair_candidates.size() > static_cast<std::size_t>(kTrailRoutePairBudget)) {
        vibble::log::warn(
            std::string("[GenerateTrails] Route pair budget clamp") +
            " rooms=" + a->room_name + "<->" + b->room_name +
            " candidates=" + std::to_string(pair_candidates.size()) +
            " budget=" + std::to_string(kTrailRoutePairBudget));
        pair_candidates.resize(static_cast<std::size_t>(kTrailRoutePairBudget));
    }

    TrailBuildResult build_result;
    bool route_budget_exhausted = false;
    bool saw_boundary_zone_violation = false;
    bool saw_route_candidate = false;

    auto place_built_trail = [&](const TrailBuildResult& placed_result,
                                 int route_pair_attempts,
                                 clock::time_point layout_start,
                                 clock::time_point layout_end) {
        if (polygon_overlaps_unrelated_room(placed_result.polygon, a, b, room_obstacles)) {
            if (stats) {
                ++stats->polygon_failures;
                stats->last_failure = TrailFailureReason::PolygonFailed;
            }
            return false;
        }

        Area candidate("trail_candidate", placed_result.polygon, 3);

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
        trail_room->camera_tilt_deg =
            camera_math::sanitize_pitch_degrees(config.value("camera_tilt_deg", 60.0f));
        trail_room->camera_zoom_percent = std::clamp(config.value("camera_zoom_percent", 0), 0, 100);
        trail_room->camera_center_dx = config.value("camera_center_dx", 0);
        trail_room->camera_center_dz = config.value("camera_center_dz", 0);
        trail_room->assets_data()["generation_metadata"]["has_split"] = false;

        TrailObstacle obstacle;
        obstacle.room = trail_room.get();
        obstacle.polygon = placed_result.polygon;
        obstacle.centerline = placed_result.centerline;
        obstacle.bounds = bounds_from_points(obstacle.polygon);
        obstacle.has_split = false;
        existing_trails.push_back(std::move(obstacle));
        trail_rooms.push_back(std::move(trail_room));

        vibble::log::info(
            std::string("[GenerateTrails] Built trail room") +
            " rooms=" + a->room_name + "<->" + b->room_name +
            " route_pair_attempts=" + std::to_string(route_pair_attempts) +
            " layout_ms=" + std::to_string(duration_ms(layout_end - layout_start)) +
            " room_ctor_ms=" + std::to_string(duration_ms(room_build_end - room_build_start)));

        if (testing) {
            vibble::log::debug(
                std::string("[GenerateTrails] Trail placed: ") +
                a->room_name + " <-> " + b->room_name +
                " route_pair_attempts=" + std::to_string(route_pair_attempts) +
                " layout_attempts=" + std::to_string(stats ? stats->layout_attempts : 0) +
                " straight_attempts=" + std::to_string(stats ? stats->straight_attempts : 0) +
                " curved_attempts=" + std::to_string(stats ? stats->curved_attempts : 0) +
                " room_rejects=" + std::to_string(stats ? stats->centerline_room_rejections : 0) +
                " trail_rejects=" + std::to_string(stats ? stats->centerline_trail_rejections : 0) +
                " section_failures=" + std::to_string(stats ? stats->section_failures : 0));
        }

        return true;
    };

    int route_pair_attempts = 0;
    for (const ContactPairCandidate& pair : pair_candidates) {
        ++route_pair_attempts;
        const SDL_Point start_contact = a_contacts[pair.a_index];
        const SDL_Point end_contact = b_contacts[pair.b_index];
        EndpointFrame start_frame = make_endpoint_frame(a_center, start_contact, default_endpoint_depth);
        EndpointFrame end_frame = make_endpoint_frame(b_center, end_contact, default_endpoint_depth);
        const double max_start_depth = max_endpoint_depth_to_room_center(a_center, start_contact);
        const double max_end_depth = max_endpoint_depth_to_room_center(b_center, end_contact);
        start_frame.inside_depth = std::min(start_frame.inside_depth, max_start_depth);
        end_frame.inside_depth = std::min(end_frame.inside_depth, max_end_depth);
        std::vector<SDL_Point> start_leg{start_frame.contact, start_frame.margin};
        std::vector<SDL_Point> end_leg{end_frame.margin, end_frame.contact};
        if (!polyline_is_valid_for_anchor_exit(start_leg, a, a, b, room_obstacles, *routing_trails, gate_clearance_px)) {
            if (stats) {
                stats->last_failure = TrailFailureReason::AnchorExitStartFailed;
            }
            continue;
        }
        if (!polyline_is_valid_for_anchor_exit(end_leg, b, a, b, room_obstacles, *routing_trails, gate_clearance_px)) {
            if (stats) {
                stats->last_failure = TrailFailureReason::AnchorExitEndFailed;
            }
            continue;
        }
        saw_route_candidate = true;

        RouteSearchResult route;
        std::vector<SDL_Point> fast_route_points;
        if (build_fast_diagonal_route(start_frame.margin,
                                        end_frame.margin,
                                        a,
                                        b,
                                        room_obstacles,
                                        *routing_trails,
                                        routing_clearance_px,
                                        &fast_route_points)) {
            route.success = true;
            route.points = std::move(fast_route_points);
        } else {
            route = build_routed_polyline(
                start_frame.margin, end_frame.margin, a, b, room_obstacles, *routing_trails, routing_clearance_px);
            if (route.budget_exhausted) {
                route_budget_exhausted = true;
                continue;
            }
            if (!route.success || route.points.size() < 2) {
                if (stats) {
                    stats->last_failure = TrailFailureReason::RouteFailed;
                }
                continue;
            }
            if (!polyline_is_valid_for_route(route.points, a, b, room_obstacles, *routing_trails, routing_clearance_px)) {
                if (stats) {
                    stats->last_failure = TrailFailureReason::RouteFailed;
                }
                continue;
            }
        }

        std::vector<SDL_Point> base_centerline = build_centerline_with_endpoint_frames(start_frame, route.points, end_frame);
        if (base_centerline.size() < 2) {
            continue;
        }

        const std::vector<std::vector<SDL_Point>> centerline_variants =
            build_centerline_variants(base_centerline, 0, a->room_name, b->room_name);
        bool built_from_variant = false;
        clock::time_point layout_start{};
        clock::time_point layout_end{};
        for (const std::vector<SDL_Point>& centerline_points : centerline_variants) {
            if (centerline_points.size() < 2) {
                continue;
            }
            if (!boundary_zones_respect_room_sectors(centerline_points, a, b)) {
                saw_boundary_zone_violation = true;
                continue;
            }

            EndpointFrame iter_start = start_frame;
            EndpointFrame iter_end = end_frame;
            constexpr int kEndpointDepthAdjustmentAttempts = 5;
            for (int endpoint_try = 0; endpoint_try < kEndpointDepthAdjustmentAttempts; ++endpoint_try) {
                const std::vector<SDL_Point> contained_centerline =
                    build_centerline_with_endpoint_frames(iter_start, route.points, iter_end);
                if (contained_centerline.size() < 2) {
                    break;
                }
                if (!boundary_zones_respect_room_sectors(contained_centerline, a, b)) {
                    saw_boundary_zone_violation = true;
                    break;
                }

                std::vector<double> required_distances;
                const std::vector<double> cumulative = build_polyline_cumulative_lengths(contained_centerline);
                if (cumulative.size() >= 4) {
                    required_distances.push_back(cumulative[1]);
                    required_distances.push_back(cumulative[cumulative.size() - 2]);
                }

                EndpointAllowance allowance;
                allowance.start_distance = iter_start.inside_depth + kRoomMarginDistanceWorldPx + 1.0;
                allowance.end_distance = iter_end.inside_depth + kRoomMarginDistanceWorldPx + 1.0;

                layout_start = clock::now();
                if (!build_trail_layout_from_polyline(contained_centerline,
                                                      min_width,
                                                      max_width,
                                                      0,
                                                      required_distances,
                                                      a,
                                                      b,
                                                      room_obstacles,
                                                      *routing_trails,
                                                      rng,
                                                      stats,
                                                      &build_result,
                                                      allowance)) {
                    break;
                }
                if (build_result.sections.empty()) {
                    break;
                }

                const double required_start_depth = endpoint_required_depth_from_section(build_result.sections.front());
                const double required_end_depth = endpoint_required_depth_from_section(build_result.sections.back());
                const double next_start_depth = std::min(max_start_depth, std::max(iter_start.inside_depth, required_start_depth));
                const double next_end_depth = std::min(max_end_depth, std::max(iter_end.inside_depth, required_end_depth));
                const bool depth_increased =
                    (next_start_depth > iter_start.inside_depth + 0.5) || (next_end_depth > iter_end.inside_depth + 0.5);
                iter_start.inside_depth = next_start_depth;
                iter_end.inside_depth = next_end_depth;

                const bool start_contained = endpoint_section_contained(build_result.sections.front(), a->room_area.get());
                const bool end_contained = endpoint_section_contained(build_result.sections.back(), b->room_area.get());
                if (start_contained && end_contained && !depth_increased) {
                    layout_end = clock::now();
                    built_from_variant = true;
                    break;
                }
                if (!start_contained && stats) {
                    stats->last_failure = TrailFailureReason::EndpointContainmentFailedStart;
                } else if (!end_contained && stats) {
                    stats->last_failure = TrailFailureReason::EndpointContainmentFailedEnd;
                }
            }
            if (built_from_variant) {
                break;
            }
        }
        if (!built_from_variant) {
            continue;
        }
        if (place_built_trail(build_result, route_pair_attempts, layout_start, layout_end)) {
            return true;
        }
    }

    // Last-resort seed/fallback: build a deterministic diagonal corridor between legal sector contacts.
    // This bypasses existing trail obstacles so the first trail can always seed the graph.
    const std::vector<TrailObstacle> no_trail_obstacles;
    const int forced_pair_limit = std::min<int>(static_cast<int>(pair_candidates.size()), 24);
    const std::array<double, 5> forced_offsets{0.0, 1024.0, -1024.0, 2048.0, -2048.0};
    for (int pair_index = 0; pair_index < forced_pair_limit; ++pair_index) {
        const ContactPairCandidate& pair = pair_candidates[static_cast<std::size_t>(pair_index)];
        ++route_pair_attempts;
        const SDL_Point start_contact = a_contacts[pair.a_index];
        const SDL_Point end_contact = b_contacts[pair.b_index];
        EndpointFrame start_frame = make_endpoint_frame(a_center, start_contact, default_endpoint_depth);
        EndpointFrame end_frame = make_endpoint_frame(b_center, end_contact, default_endpoint_depth);
        start_frame.inside_depth = std::min(start_frame.inside_depth, max_endpoint_depth_to_room_center(a_center, start_contact));
        end_frame.inside_depth = std::min(end_frame.inside_depth, max_endpoint_depth_to_room_center(b_center, end_contact));

        for (double offset : forced_offsets) {
            std::vector<std::vector<SDL_Point>> forced_routes;
            const SDL_Point sm = start_frame.margin;
            const SDL_Point em = end_frame.margin;
            forced_routes.push_back(dedupe_consecutive_points(std::vector<SDL_Point>{sm, SDL_Point{em.x, sm.y}, em}));
            forced_routes.push_back(dedupe_consecutive_points(std::vector<SDL_Point>{sm, SDL_Point{sm.x, em.y}, em}));
            if (std::abs(offset) > 0.5) {
                forced_routes.push_back(dedupe_consecutive_points(std::vector<SDL_Point>{
                    sm, SDL_Point{sm.x, sm.y + static_cast<int>(std::lround(offset))},
                    SDL_Point{em.x, sm.y + static_cast<int>(std::lround(offset))}, em}));
                forced_routes.push_back(dedupe_consecutive_points(std::vector<SDL_Point>{
                    sm, SDL_Point{sm.x + static_cast<int>(std::lround(offset)), sm.y},
                    SDL_Point{sm.x + static_cast<int>(std::lround(offset)), em.y}, em}));
            }

            for (const std::vector<SDL_Point>& route_points : forced_routes) {
                std::vector<SDL_Point> contained_centerline =
                    build_centerline_with_endpoint_frames(start_frame, route_points, end_frame);
                contained_centerline = sanitize_diagonal_centerline(contained_centerline);
                if (contained_centerline.size() < 2 ||
                    !boundary_zones_respect_room_sectors(contained_centerline, a, b)) {
                    continue;
                }

                std::vector<double> required_distances;
                const std::vector<double> cumulative = build_polyline_cumulative_lengths(contained_centerline);
                if (cumulative.size() >= 4) {
                    required_distances.push_back(cumulative[1]);
                    required_distances.push_back(cumulative[cumulative.size() - 2]);
                }

                EndpointAllowance allowance;
                allowance.start_distance = start_frame.inside_depth + kRoomMarginDistanceWorldPx + 1.0;
                allowance.end_distance = end_frame.inside_depth + kRoomMarginDistanceWorldPx + 1.0;

                (void)allowance;
                (void)no_trail_obstacles;

                TrailBuildResult forced_result;
                const auto layout_start = clock::now();
                const double trail_length = polyline_total_length(cumulative);
                std::vector<double> required = required_distances;
                required.push_back(0.0);
                required.push_back(trail_length);
                const std::vector<double> section_distances = build_section_distances(trail_length, required);
                const std::vector<CenterlineSample> section_samples =
                    build_centerline_samples_from_polyline(contained_centerline, section_distances);
                std::vector<TrailSection> forced_sections;
                if (section_samples.empty() ||
                    !place_sections(section_samples, min_width, max_width, rng, stats, &forced_sections)) {
                    continue;
                }
                forced_result.start_tip = contained_centerline.front();
                forced_result.end_tip = contained_centerline.back();
                forced_result.sections = std::move(forced_sections);
                forced_result.centerline = contained_centerline;
                if (!build_diagonal_corridor_polygon(forced_result.centerline, min_width, &forced_result.polygon)) {
                    continue;
                }
                const auto layout_end = clock::now();
                if (forced_result.sections.empty()) {
                    continue;
                }
                if (place_built_trail(forced_result, route_pair_attempts, layout_start, layout_end)) {
                    vibble::log::warn(
                        std::string("[GenerateTrails] Forced diagonal connector used") +
                        " rooms=" + a->room_name + "<->" + b->room_name);
                    return true;
                }
            }
        }
    }

    if (stats) {
        if (!saw_route_candidate) {
            ++stats->sector_contact_failures;
            stats->last_failure = TrailFailureReason::NoValidSectorContacts;
        } else if (route_budget_exhausted) {
            ++stats->route_budget_failures;
            stats->last_failure = TrailFailureReason::RouteSearchBudgetExceeded;
        } else if (saw_boundary_zone_violation) {
            ++stats->sector_boundary_failures;
            stats->last_failure = TrailFailureReason::SectorBoundaryZoneViolation;
        } else {
            ++stats->sector_contact_failures;
            stats->last_failure = TrailFailureReason::NoValidSectorContacts;
        }
    }
    return false;
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
    json* source_trails = &trail_data;
    if (!trail_data.is_object()) {
        source_trails = nullptr;
    }
    if (source_trails) {
        for (auto it = source_trails->begin(); it != source_trails->end(); ++it) {
            if (!is_valid_trail_template(*it)) {
                continue;
            }
            available_assets_.push_back({it.key(), &(*it)});
            utils::display_color::ensure(*available_assets_.back().data, trail_colors_);
        }
    }

    if (available_assets_.empty()) {
        fallback_trails_data_ = json::object({{"MainTrail", make_default_trail_template()}});
        trails_data_ = &fallback_trails_data_;
        for (auto it = trails_data_->begin(); it != trails_data_->end(); ++it) {
            if (!is_valid_trail_template(*it)) {
                continue;
            }
            available_assets_.push_back({it.key(), &(*it)});
            utils::display_color::ensure(*available_assets_.back().data, trail_colors_);
        }
        vibble::log::warn("[GenerateTrails] No valid trail templates found; using built-in default trail data for this run.");
    }

    if (available_assets_.empty()) {
        throw std::runtime_error("[GenerateTrails] No trail templates found in trails_data");
    }

}

void GenerateTrails::set_all_rooms_reference(const std::vector<Room*>& rooms) {
    all_rooms_reference_ = rooms;
}

TrailGenerationResult GenerateTrails::generate_trails(const std::vector<std::pair<Room*, Room*>>& room_pairs,
                                                      const std::string& manifest_context,
                                                      AssetLibrary* asset_lib,
                                                      double map_radius,
                                                      nlohmann::json* map_manifest,
                                                      devmode::core::ManifestStore* manifest_store,
                                                      Room::ManifestWriter manifest_writer) {
    using clock = std::chrono::steady_clock;
    const auto generation_start = clock::now();

    TrailGenerationResult result;
    std::vector<std::unique_ptr<Room>>& trail_rooms = result.trail_rooms;
    std::vector<TrailObstacle> existing_trails;
    const std::vector<RoomObstacle> room_obstacles = build_room_obstacles(all_rooms_reference_);

    GenerationPerfSummary perf;
    perf.total_connections = static_cast<int>(room_pairs.size());
    perf.total_rooms_considered = static_cast<int>(all_rooms_reference_.size());
    std::unordered_map<Room*, TrailUnresolvedRoom> unresolved_by_room;

    const int asset_attempts = std::max(1, static_cast<int>(available_assets_.size()) * 2);

    auto mark_unresolved = [&](Room* room, const std::string& phase, const std::string& reason) {
        if (!room) return;
        unresolved_by_room[room] = TrailUnresolvedRoom{room, phase, reason};
    };

    auto clear_unresolved = [&](Room* room) {
        if (!room) return;
        unresolved_by_room.erase(room);
    };

    auto try_connection = [&](Room* a, Room* b, bool required_connection, const std::string& phase, Room* split_host_allow = nullptr) {
        if (!a || !b) {
            if (required_connection) {
                result.required_failures.push_back(TrailConnectionFailure{a, b, "null_room_pointer"});
            } else {
                result.optional_skips.push_back(TrailConnectionFailure{a, b, "null_room_pointer"});
            }
            mark_unresolved(b ? b : a, phase, "null_room_pointer");
            return false;
        }

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
                                               &stats,
                                               split_host_allow);

            perf.total_layout_attempts += stats.layout_attempts;
            perf.total_straight_attempts += stats.straight_attempts;
            perf.total_curved_attempts += stats.curved_attempts;
            perf.total_room_rejections += stats.centerline_room_rejections;
            perf.total_trail_rejections += stats.centerline_trail_rejections;
            perf.total_section_failures += stats.section_failures;
            perf.total_polygon_failures += stats.polygon_failures;
            perf.total_sector_contact_failures += stats.sector_contact_failures;
            perf.total_sector_boundary_failures += stats.sector_boundary_failures;
            perf.total_route_budget_failures += stats.route_budget_failures;

            if (!success) {
                if (!have_failed_stats || stats.layout_attempts >= best_failed_stats.layout_attempts) {
                    best_failed_stats = stats;
                    have_failed_stats = true;
                }
            } else {
                ++perf.successful_connections;
                clear_unresolved(a);
                clear_unresolved(b);
            }
        }

        if (!success) {
            ++perf.failed_connections;
            TrailConnectionFailure failure{a, b, std::string(failure_reason_name(best_failed_stats.last_failure))};
            if (required_connection) {
                result.required_failures.push_back(std::move(failure));
            } else {
                result.optional_skips.push_back(std::move(failure));
            }
            mark_unresolved(b, phase, std::string(failure_reason_name(best_failed_stats.last_failure)));
        }
        return success;
    };

    auto compute_reachable = [&]() {
        std::unordered_set<Room*> reachable;
        Room* spawn = nullptr;
        for (Room* room : all_rooms_reference_) {
            if (!room) continue;
            if (spawn == nullptr || room->layer < spawn->layer) {
                spawn = room;
            }
        }
        if (!spawn) return reachable;
        std::vector<Room*> queue;
        queue.push_back(spawn);
        reachable.insert(spawn);
        for (std::size_t i = 0; i < queue.size(); ++i) {
            Room* current = queue[i];
            if (!current) continue;
            for (Room* next : current->connected_rooms) {
                if (!next) continue;
                if (reachable.insert(next).second) {
                    queue.push_back(next);
                }
            }
        }
        std::unordered_set<Room*> non_trail;
        for (Room* room : all_rooms_reference_) {
            if (room && reachable.find(room) != reachable.end()) {
                non_trail.insert(room);
            }
        }
        return non_trail;
    };

    auto candidate_rooms_for = [&](Room* room, const std::unordered_set<Room*>& reachable) {
        std::vector<Room*> candidates;
        if (!room) return candidates;
        for (Room* candidate : all_rooms_reference_) {
            if (!candidate || candidate == room) continue;
            if (reachable.find(candidate) == reachable.end()) continue;
            candidates.push_back(candidate);
        }
        std::sort(candidates.begin(), candidates.end(), [&](Room* lhs, Room* rhs) {
            auto priority = [&](Room* value) {
                if (!value) return 9;
                if (value->layer == room->layer - 1) return 0;
                if (value->layer == room->layer) return 1;
                if (value->layer == room->layer + 1) return 2;
                return 3 + std::abs(value->layer - room->layer);
            };
            const int lp = priority(lhs);
            const int rp = priority(rhs);
            if (lp != rp) return lp < rp;
            const auto [lx, ly] = room_center(lhs);
            const auto [rx, ry] = room_center(rhs);
            const auto [cx, cy] = room_center(room);
            const double ld = std::hypot(lx - cx, ly - cy);
            const double rd = std::hypot(rx - cx, ry - cy);
            if (ld != rd) return ld < rd;
            return lhs < rhs;
        });
        return candidates;
    };

    // Phase 1: ordered primary room pairs (required-child-first ordering supplied by GenerateRooms).
    for (const auto& pair : room_pairs) {
        try_connection(pair.first, pair.second, true, "primary");
    }

    // Phase 2: layer-by-layer room connection pass for rooms still unreachable from spawn.
    std::vector<Room*> layered_rooms = all_rooms_reference_;
    std::sort(layered_rooms.begin(), layered_rooms.end(), [](Room* a, Room* b) {
        if (!a || !b) return a < b;
        if (a->layer != b->layer) return a->layer < b->layer;
        return a->room_name < b->room_name;
    });
    for (Room* room : layered_rooms) {
        if (!room || room->layer <= 0) continue;
        const std::unordered_set<Room*> reachable = compute_reachable();
        if (reachable.find(room) != reachable.end()) continue;
        bool connected = false;
        for (Room* candidate : candidate_rooms_for(room, reachable)) {
            if (try_connection(candidate, room, false, "layer")) {
                connected = true;
                break;
            }
        }
        if (!connected) {
            mark_unresolved(room, "layer", "no_valid_route");
        }
    }

    // Phase 3: deferred reconnection for any unresolved rooms.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<Room*> deferred;
        deferred.reserve(unresolved_by_room.size());
        for (const auto& entry : unresolved_by_room) {
            if (entry.first) deferred.push_back(entry.first);
        }
        bool made_progress = false;
        for (Room* room : deferred) {
            const std::unordered_set<Room*> reachable = compute_reachable();
            if (reachable.find(room) != reachable.end()) {
                clear_unresolved(room);
                continue;
            }
            for (Room* candidate : candidate_rooms_for(room, reachable)) {
                if (try_connection(candidate, room, false, "deferred")) {
                    made_progress = true;
                    break;
                }
            }
        }
        if (!made_progress) {
            break;
        }
    }

    // Phase 4: split fallback. Connect unresolved rooms to nearest unsplit trail room.
    std::vector<Room*> split_targets;
    split_targets.reserve(unresolved_by_room.size());
    for (const auto& entry : unresolved_by_room) {
        if (entry.first) split_targets.push_back(entry.first);
    }
    for (Room* room : split_targets) {
        if (!room) continue;
        const std::unordered_set<Room*> reachable = compute_reachable();
        if (reachable.find(room) != reachable.end()) {
            clear_unresolved(room);
            continue;
        }
        ++perf.split_attempts;

        int best_index = -1;
        double best_distance = std::numeric_limits<double>::max();
        const auto [rx, ry] = room_center(room);
        for (std::size_t i = 0; i < existing_trails.size(); ++i) {
            const TrailObstacle& obstacle = existing_trails[i];
            if (obstacle.has_split || !obstacle.room || obstacle.centerline.empty()) {
                continue;
            }
            const SDL_Point sample = obstacle.centerline[obstacle.centerline.size() / 2];
            const double dx = static_cast<double>(sample.x) - rx;
            const double dy = static_cast<double>(sample.y) - ry;
            const double d = std::hypot(dx, dy);
            if (d < best_distance) {
                best_distance = d;
                best_index = static_cast<int>(i);
            }
        }
        if (best_index < 0) {
            ++perf.split_exhausted;
            mark_unresolved(room, "split", "no_available_unsplit_trail");
            continue;
        }

        TrailObstacle& host = existing_trails[static_cast<std::size_t>(best_index)];
        if (try_connection(host.room, room, false, "split", host.room)) {
            host.has_split = true;
            if (host.room) {
                host.room->assets_data()["generation_metadata"]["has_split"] = true;
            }
            ++perf.split_successes;
            clear_unresolved(room);
        } else {
            mark_unresolved(room, "split", "split_route_failed");
        }
    }

    for (const auto& entry : unresolved_by_room) {
        result.unresolved_rooms.push_back(entry.second);
    }
    for (const TrailUnresolvedRoom& unresolved : result.unresolved_rooms) {
        vibble::log::warn(
            std::string("[GenerateTrails] Unresolved room") +
            " room=" + (unresolved.room ? unresolved.room->room_name : std::string("<null>")) +
            " phase=" + unresolved.phase +
            " reason=" + unresolved.reason);
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
        " sector_contact_failures=" + std::to_string(perf.total_sector_contact_failures) +
        " sector_boundary_failures=" + std::to_string(perf.total_sector_boundary_failures) +
        " route_budget_failures=" + std::to_string(perf.total_route_budget_failures) +
        " split_attempts=" + std::to_string(perf.split_attempts) +
        " split_successes=" + std::to_string(perf.split_successes) +
        " split_exhausted=" + std::to_string(perf.split_exhausted) +
        " unresolved_rooms=" + std::to_string(result.unresolved_rooms.size()) +
        " total_ms=" + std::to_string(duration_ms(generation_end - generation_start))
    );

    result.all_required_connected = result.required_failures.empty() && result.unresolved_rooms.empty();
    result.counters.total_connections = perf.total_connections;
    result.counters.successful_connections = perf.successful_connections;
    result.counters.failed_connections = perf.failed_connections;
    result.counters.total_asset_attempts = perf.total_asset_attempts;
    result.counters.total_layout_attempts = perf.total_layout_attempts;
    result.counters.total_straight_attempts = perf.total_straight_attempts;
    result.counters.total_curved_attempts = perf.total_curved_attempts;
    result.counters.total_room_rejections = perf.total_room_rejections;
    result.counters.total_trail_rejections = perf.total_trail_rejections;
    result.counters.total_section_failures = perf.total_section_failures;
    result.counters.total_polygon_failures = perf.total_polygon_failures;
    result.counters.total_sector_contact_failures = perf.total_sector_contact_failures;
    result.counters.total_sector_boundary_failures = perf.total_sector_boundary_failures;
    result.counters.total_route_budget_failures = perf.total_route_budget_failures;
    result.counters.total_rooms_considered = perf.total_rooms_considered;
    result.counters.split_attempts = perf.split_attempts;
    result.counters.split_successes = perf.split_successes;
    result.counters.split_exhausted = perf.split_exhausted;
    return result;
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
