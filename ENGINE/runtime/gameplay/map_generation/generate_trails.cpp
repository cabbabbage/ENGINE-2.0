#include "generate_trails.hpp"

#include "utils/display_color.hpp"
#include "utils/log.hpp"
#include "utils/map_grid_settings.hpp"

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
constexpr double kCurvynessShiftScaleWorldPx = 20.0;
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
constexpr int kTrailRouteSearchMaxExpansions = 500000;
constexpr int kTrailRouteSearchMaxCellSpan = 512;
constexpr int kTrailRouteInitialCellSizeWorldPx = 96;
constexpr int kTrailRouteSampleSpacingWorldPx = 32;
constexpr int kTrailBoundaryZoneDistanceWorldPx = 240;
constexpr int kTrailContactGateIterations = 24;
constexpr double kTrailContactGateStepWorldPx = 24.0;
constexpr int kTrailRoutePairBudget = 256;
constexpr double kRoomMarginDistanceWorldPx = 750.0;
constexpr double kCurvyRouteAmplitudeScaleWorldPx = 24.0;
constexpr double kCurvyRouteSampleSpacingBaseWorldPx = 140.0;
constexpr double kCurvyRouteSampleSpacingMinWorldPx = 56.0;
constexpr int kCurvyVariantCount = 5;

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

struct RouteSearchResult {
    bool success = false;
    bool budget_exhausted = false;
    std::vector<SDL_Point> points;
};

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
    const SDL_Point goal = find_open_cell(raw_goal);
    if (is_blocked_cell(start.x, start.y) || is_blocked_cell(goal.x, goal.y)) {
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

    static const std::array<SDL_Point, 8> kNeighbors{
        SDL_Point{1, 0}, SDL_Point{-1, 0}, SDL_Point{0, 1}, SDL_Point{0, -1},
        SDL_Point{1, 1}, SDL_Point{-1, 1}, SDL_Point{1, -1}, SDL_Point{-1, -1},
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
            const double move_cost = (offset.x == 0 || offset.y == 0) ? 1.0 : 1.41421356237;
            const double tentative = g_score[static_cast<std::size_t>(current.index)] + move_cost;
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
    route_points = simplify_route_points(route_points, room_a, room_b, room_obstacles, existing_trails, clearance_px);
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

    Vec2 dir = subtract(to_vec2(contact), to_vec2(center));
    const double dir_len = length(dir);
    if (dir_len <= kPointEpsilon) {
        return false;
    }
    dir = scale(dir, 1.0 / dir_len);

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
    Vec2 outward = subtract(to_vec2(room_edge_contact), to_vec2(room_center));
    outward = normalize_or_default(outward, Vec2{1.0, 0.0});
    const Vec2 margin = add(to_vec2(room_edge_contact), scale(outward, kRoomMarginDistanceWorldPx));
    return to_point(margin);
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

std::uint32_t stable_route_seed(const std::string& a, const std::string& b, int variant_index) {
    const std::string key = (a < b) ? (a + "|" + b) : (b + "|" + a);
    std::uint32_t hash = 2166136261u;
    for (unsigned char ch : key) {
        hash ^= static_cast<std::uint32_t>(ch);
        hash *= 16777619u;
    }
    hash ^= static_cast<std::uint32_t>(variant_index * 2654435761u);
    return hash;
}

std::vector<SDL_Point> build_curvier_polyline(const std::vector<SDL_Point>& base_points,
                                              int curvyness,
                                              double amplitude_scale,
                                              double phase_shift) {
    std::vector<SDL_Point> polyline = dedupe_consecutive_points(base_points);
    if (polyline.size() < 2 || curvyness <= 0) {
        return polyline;
    }

    const std::vector<double> cumulative = build_polyline_cumulative_lengths(polyline);
    const double total = polyline_total_length(cumulative);
    if (total <= 1.0) {
        return polyline;
    }

    const double sample_spacing = std::max(
        kCurvyRouteSampleSpacingMinWorldPx,
        kCurvyRouteSampleSpacingBaseWorldPx - static_cast<double>(curvyness) * 8.0);
    const int sample_count = std::max(2, static_cast<int>(std::ceil(total / sample_spacing)));
    const double amplitude_limit = std::min(
        total * 0.2,
        static_cast<double>(curvyness) * kCurvyRouteAmplitudeScaleWorldPx * std::max(0.0, amplitude_scale));
    if (amplitude_limit <= 0.0) {
        return polyline;
    }

    std::vector<SDL_Point> curved;
    curved.reserve(static_cast<std::size_t>(sample_count) + 1);
    curved.push_back(polyline.front());
    for (int i = 1; i < sample_count; ++i) {
        const double d = total * (static_cast<double>(i) / static_cast<double>(sample_count));
        Vec2 point{};
        Vec2 tangent{};
        if (!sample_polyline_at_distance(polyline, cumulative, d, &point, &tangent)) {
            continue;
        }
        const Vec2 normal{-tangent.y, tangent.x};
        const double phase = (d / std::max(1.0, total)) * (2.0 * 3.14159265358979323846) + phase_shift;
        const double envelope = std::sin((d / std::max(1.0, total)) * 3.14159265358979323846);
        const double lateral = std::sin(phase) * amplitude_limit * envelope;
        curved.push_back(to_point(add(point, scale(normal, lateral))));
    }
    curved.push_back(polyline.back());
    return dedupe_consecutive_points(curved);
}

std::vector<std::vector<SDL_Point>> build_centerline_variants(const std::vector<SDL_Point>& base_points,
                                                              int curvyness,
                                                              const std::string& room_a_name,
                                                              const std::string& room_b_name) {
    std::vector<std::vector<SDL_Point>> variants;
    variants.push_back(dedupe_consecutive_points(base_points));  // Always keep safe fallback first.
    if (curvyness <= 0) {
        return variants;
    }

    for (int i = 0; i < kCurvyVariantCount; ++i) {
        const std::uint32_t seed = stable_route_seed(room_a_name, room_b_name, i);
        const double unit = static_cast<double>(seed % 10000u) / 9999.0;
        const double phase_shift = unit * 2.0 * 3.14159265358979323846;
        const double amplitude_scale = 0.45 + static_cast<double>(i + 1) / static_cast<double>(kCurvyVariantCount + 1);
        std::vector<SDL_Point> variant = build_curvier_polyline(base_points, curvyness, amplitude_scale, phase_shift);
        variant = dedupe_consecutive_points(variant);
        if (variant.size() < 2) {
            continue;
        }
        bool duplicate = false;
        for (const auto& existing : variants) {
            if (existing.size() != variant.size()) {
                continue;
            }
            const bool same_points = std::equal(
                existing.begin(),
                existing.end(),
                variant.begin(),
                [](const SDL_Point& lhs, const SDL_Point& rhs) {
                    return lhs.x == rhs.x && lhs.y == rhs.y;
                });
            if (same_points) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            variants.push_back(std::move(variant));
        }
    }
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
    return boundary_zone_within_sector(centerline_points, room_a, true) &&
           boundary_zone_within_sector(centerline_points, room_b, false);
}

bool centerline_is_valid(Room* room_a,
                         Room* room_b,
                         const std::vector<CenterlineSample>& validation_samples,
                         const std::vector<RoomObstacle>& room_obstacles,
                         const std::vector<TrailObstacle>& existing_trails,
                         TrailAttemptStats* stats,
                         bool allow_connected_room_interior) {
    for (std::size_t i = 0; i < validation_samples.size(); ++i) {
        const CenterlineSample& sample = validation_samples[i];
        const SDL_Point point = to_point(sample.point);
        if (point_inside_blocking_room(point, room_a, room_b, room_obstacles)) {
            if (stats) {
                ++stats->centerline_room_rejections;
                stats->last_failure = TrailFailureReason::BlockedByRoom;
            }
            return false;
        }
        if (point_inside_any_room(point, room_a ? room_a->room_area.get() : nullptr, room_b ? room_b->room_area.get() : nullptr)) {
            const bool endpoint_sample = (i == 0 || i + 1 == validation_samples.size());
            if (!allow_connected_room_interior && !endpoint_sample) {
                if (stats) {
                    ++stats->centerline_room_rejections;
                    stats->last_failure = TrailFailureReason::BlockedByRoom;
                }
                return false;
            }
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
        if (!centerline_is_valid(
                room_a, room_b, validation_samples, room_obstacles, existing_trails, stats, allow_connected_room_interior)) {
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

bool sections_respect_connected_room_boundaries(const std::vector<TrailSection>& sections,
                                                Room* room_a,
                                                Room* room_b) {
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

    for (std::size_t i = 1; i + 1 < sections.size(); ++i) {
        if (inside_connected(sections[i].center) || inside_connected(sections[i].left) || inside_connected(sections[i].right)) {
            return false;
        }
    }
    return true;
}

bool build_trail_layout_from_polyline(const std::vector<SDL_Point>& centerline_points,
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

    std::vector<SDL_Point> polyline = dedupe_consecutive_points(centerline_points);
    if (polyline.size() < 2) {
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
    curvyness = std::max(0, curvyness);

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
                             false)) {
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
    if (!place_sections(section_samples, min_width, max_width, curvyness, rng, stats, &sections)) {
        return false;
    }
    if (!sections_respect_connected_room_boundaries(sections, room_a, room_b)) {
        if (stats) {
            ++stats->centerline_room_rejections;
            stats->last_failure = TrailFailureReason::BlockedByRoom;
        }
        return false;
    }

    std::vector<SDL_Point> polygon = build_polygon(to_vec2(polyline.front()), to_vec2(polyline.back()), sections);
    if (polygon.size() < 4) {
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
    const int routing_clearance_px = std::max(8, max_width / 2 + 12);
    const int gate_clearance_px = std::max(4, max_width / 3);

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

    int route_pair_attempts = 0;
    for (const ContactPairCandidate& pair : pair_candidates) {
        ++route_pair_attempts;
        const SDL_Point start_contact = a_contacts[pair.a_index];
        const SDL_Point end_contact = b_contacts[pair.b_index];
        const SDL_Point start_margin = room_margin_point(a_center, start_contact);
        const SDL_Point end_margin = room_margin_point(b_center, end_contact);
        std::vector<SDL_Point> start_leg{start_contact, start_margin};
        std::vector<SDL_Point> end_leg{end_margin, end_contact};
        if (!polyline_is_valid_for_anchor_exit(start_leg, a, a, b, room_obstacles, existing_trails, gate_clearance_px)) {
            continue;
        }
        if (!polyline_is_valid_for_anchor_exit(end_leg, b, a, b, room_obstacles, existing_trails, gate_clearance_px)) {
            continue;
        }
        saw_route_candidate = true;

        RouteSearchResult route = build_routed_polyline(
            start_margin, end_margin, a, b, room_obstacles, existing_trails, routing_clearance_px);
        if (route.budget_exhausted) {
            route_budget_exhausted = true;
            continue;
        }
        if (!route.success || route.points.size() < 2) {
            continue;
        }
        if (!polyline_is_valid_for_route(route.points, a, b, room_obstacles, existing_trails, routing_clearance_px)) {
            continue;
        }

        std::vector<SDL_Point> base_centerline = stitch_route_with_contacts(start_contact, route.points, end_contact);
        if (base_centerline.size() < 2) {
            continue;
        }

        const std::vector<std::vector<SDL_Point>> centerline_variants =
            build_centerline_variants(base_centerline, curvyness, a->room_name, b->room_name);
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

            std::vector<double> required_distances;
            const std::vector<double> cumulative = build_polyline_cumulative_lengths(centerline_points);
            if (cumulative.size() >= 4) {
                required_distances.push_back(cumulative[1]);
                required_distances.push_back(cumulative[cumulative.size() - 2]);
            }

            layout_start = clock::now();
            if (!build_trail_layout_from_polyline(centerline_points,
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
                continue;
            }
            layout_end = clock::now();
            built_from_variant = true;
            break;
        }
        if (!built_from_variant) {
            continue;
        }

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
            perf.total_sector_contact_failures += stats.sector_contact_failures;
            perf.total_sector_boundary_failures += stats.sector_boundary_failures;
            perf.total_route_budget_failures += stats.route_budget_failures;

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
                    " sector_contact_failures=" + std::to_string(stats.sector_contact_failures) +
                    " sector_boundary_failures=" + std::to_string(stats.sector_boundary_failures) +
                    " route_budget_failures=" + std::to_string(stats.route_budget_failures) +
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
                    " sector_contact_failures=" + std::to_string(stats.sector_contact_failures) +
                    " sector_boundary_failures=" + std::to_string(stats.sector_boundary_failures) +
                    " route_budget_failures=" + std::to_string(stats.route_budget_failures) +
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
                " sector_contact_failures=" + std::to_string(best_failed_stats.sector_contact_failures) +
                " sector_boundary_failures=" + std::to_string(best_failed_stats.sector_boundary_failures) +
                " route_budget_failures=" + std::to_string(best_failed_stats.route_budget_failures) +
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
        " sector_contact_failures=" + std::to_string(perf.total_sector_contact_failures) +
        " sector_boundary_failures=" + std::to_string(perf.total_sector_boundary_failures) +
        " route_budget_failures=" + std::to_string(perf.total_route_budget_failures) +
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
                            &build_result,
                            true)) {
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

TrailConnectionSector make_test_sector(const TrailSectorDebug& sector) {
    TrailConnectionSector converted;
    converted.direction_deg = normalize_angle_degrees(sector.direction_deg);
    converted.width_percent = std::clamp(sector.width_percent, kTrailSectorMinWidthPercent, kTrailSectorMaxWidthPercent);
    return converted;
}

bool point_in_sector_for_tests(const SDL_Point& center,
                               const SDL_Point& point,
                               const TrailSectorDebug& sector) {
    const TrailConnectionSector converted = make_test_sector(sector);
    const double angle = angle_degrees_from_center(center, point);
    return angle_in_sector(angle, converted);
}

bool collect_sector_contacts_for_circle_tests(const SDL_Point& center,
                                              int radius,
                                              const TrailSectorDebug& sector,
                                              const SDL_Point& target_center,
                                              std::vector<SDL_Point>* out_contacts) {
    if (!out_contacts) {
        return false;
    }
    out_contacts->clear();
    if (radius <= 0) {
        return false;
    }

    const int map_span = std::max(radius * 8,
                                  std::max(std::abs(center.x), std::abs(center.y)) * 4 + radius * 4);
    Area room_area("sector_circle_test",
                   center,
                   radius * 2,
                   radius * 2,
                   "Circle",
                   2,
                   map_span,
                   map_span,
                   3);
    const TrailConnectionSector converted = make_test_sector(sector);
    *out_contacts = collect_sector_contact_candidates_for_area(&room_area, converted, target_center);
    return !out_contacts->empty();
}

bool build_routed_centerline_for_tests(const std::vector<SDL_Point>& room_a_polygon,
                                       const TrailSectorDebug& sector_a,
                                       const std::vector<SDL_Point>& room_b_polygon,
                                       const TrailSectorDebug& sector_b,
                                       const std::vector<std::vector<SDL_Point>>& blocking_room_polygons,
                                       int corridor_clearance_px,
                                       RoutedCenterlineDebug* out_debug) {
    if (!out_debug) {
        return false;
    }
    *out_debug = RoutedCenterlineDebug{};
    if (room_a_polygon.size() < 3 || room_b_polygon.size() < 3) {
        return false;
    }

    Area room_a_area("route_room_a", room_a_polygon, 3);
    Area room_b_area("route_room_b", room_b_polygon, 3);
    room_a_area.set_type("room");
    room_b_area.set_type("room");
    const TrailConnectionSector converted_a = make_test_sector(sector_a);
    const TrailConnectionSector converted_b = make_test_sector(sector_b);

    std::vector<Area> blocker_areas;
    blocker_areas.reserve(blocking_room_polygons.size());
    for (std::size_t i = 0; i < blocking_room_polygons.size(); ++i) {
        if (blocking_room_polygons[i].size() < 3) {
            continue;
        }
        blocker_areas.emplace_back("route_blocker_" + std::to_string(i), blocking_room_polygons[i], 3);
        blocker_areas.back().set_type("room");
    }

    std::vector<RoomObstacle> room_obstacles;
    room_obstacles.reserve(2 + blocker_areas.size());
    room_obstacles.push_back(RoomObstacle{nullptr, &room_a_area, bounds_from_area(room_a_area)});
    room_obstacles.push_back(RoomObstacle{nullptr, &room_b_area, bounds_from_area(room_b_area)});
    for (Area& blocker : blocker_areas) {
        room_obstacles.push_back(RoomObstacle{nullptr, &blocker, bounds_from_area(blocker)});
    }

    const SDL_Point center_a = room_a_area.get_center();
    const SDL_Point center_b = room_b_area.get_center();
    out_debug->contact_candidates_a = collect_sector_contact_candidates_for_area(&room_a_area, converted_a, center_b);
    out_debug->contact_candidates_b = collect_sector_contact_candidates_for_area(&room_b_area, converted_b, center_a);
    if (out_debug->contact_candidates_a.empty() || out_debug->contact_candidates_b.empty()) {
        return false;
    }

    struct PairCandidate {
        std::size_t ia = 0;
        std::size_t ib = 0;
        double score = 0.0;
    };
    std::vector<PairCandidate> pair_candidates;
    pair_candidates.reserve(out_debug->contact_candidates_a.size() * out_debug->contact_candidates_b.size());
    for (std::size_t ia = 0; ia < out_debug->contact_candidates_a.size(); ++ia) {
        for (std::size_t ib = 0; ib < out_debug->contact_candidates_b.size(); ++ib) {
            const double dx = static_cast<double>(out_debug->contact_candidates_a[ia].x - out_debug->contact_candidates_b[ib].x);
            const double dy = static_cast<double>(out_debug->contact_candidates_a[ia].y - out_debug->contact_candidates_b[ib].y);
            const double distance = std::hypot(dx, dy);
            const double rank_bias = static_cast<double>(ia + ib) * 200.0;
            pair_candidates.push_back(PairCandidate{ia, ib, distance + rank_bias});
        }
    }
    std::sort(pair_candidates.begin(), pair_candidates.end(), [](const PairCandidate& lhs, const PairCandidate& rhs) {
        return lhs.score < rhs.score;
    });
    if (pair_candidates.size() > static_cast<std::size_t>(kTrailRoutePairBudget)) {
        pair_candidates.resize(static_cast<std::size_t>(kTrailRoutePairBudget));
    }

    bool found_route = false;
    const int clearance = std::max(4, corridor_clearance_px);
    const int gate_clearance = std::max(2, corridor_clearance_px / 2);
    for (const PairCandidate& pair : pair_candidates) {
        const SDL_Point start_contact = out_debug->contact_candidates_a[pair.ia];
        const SDL_Point end_contact = out_debug->contact_candidates_b[pair.ib];

        SDL_Point start_gate{};
        SDL_Point end_gate{};
        if (!compute_contact_gate_from_center(
                start_contact, center_a, nullptr, nullptr, room_obstacles, {}, gate_clearance, &start_gate)) {
            continue;
        }
        if (!compute_contact_gate_from_center(
                end_contact, center_b, nullptr, nullptr, room_obstacles, {}, gate_clearance, &end_gate)) {
            continue;
        }

        RouteSearchResult route =
            build_routed_polyline(start_gate, end_gate, nullptr, nullptr, room_obstacles, {}, clearance);
        out_debug->route_budget_exhausted = out_debug->route_budget_exhausted || route.budget_exhausted;
        if (!route.success || route.points.size() < 2) {
            continue;
        }

        found_route = true;
        out_debug->routed_points = route.points;
        out_debug->centerline_points = stitch_route_with_contacts(start_contact, route.points, end_contact);
        out_debug->boundary_zone_ok =
            boundary_zone_within_sector_for_area(out_debug->centerline_points, &room_a_area, converted_a, true) &&
            boundary_zone_within_sector_for_area(out_debug->centerline_points, &room_b_area, converted_b, false);
        if (out_debug->boundary_zone_ok) {
            break;
        }
    }

    return found_route;
}

bool build_route_polyline_for_tests(const SDL_Point& start_gate,
                                    const SDL_Point& end_gate,
                                    const std::vector<std::vector<SDL_Point>>& blocking_room_polygons,
                                    int clearance_px,
                                    std::vector<SDL_Point>* out_points) {
    if (!out_points) {
        return false;
    }
    out_points->clear();

    std::vector<TrailObstacle> trail_obstacles;
    trail_obstacles.reserve(blocking_room_polygons.size());
    for (std::size_t i = 0; i < blocking_room_polygons.size(); ++i) {
        if (blocking_room_polygons[i].size() < 3) {
            continue;
        }
        TrailObstacle obstacle;
        obstacle.polygon = blocking_room_polygons[i];
        obstacle.bounds = bounds_from_points(obstacle.polygon);
        trail_obstacles.push_back(std::move(obstacle));
    }

    RouteSearchResult route =
        build_routed_polyline(start_gate, end_gate, nullptr, nullptr, {}, trail_obstacles, std::max(4, clearance_px));
    if (!route.success) {
        return false;
    }
    *out_points = route.points;
    return true;
}

} // namespace trail_generation::debug
#endif
