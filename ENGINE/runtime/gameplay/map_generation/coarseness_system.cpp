#include "gameplay/map_generation/coarseness_system.hpp"

#include "gameplay/map_generation/room.hpp"
#include "utils/area.hpp"
#include "utils/grid.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <limits>
#include <random>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#ifdef VIBBLE_HAS_CLIPPER2
#include <clipper2/clipper.h>
#endif

namespace vibble::mapgen::coarseness {
namespace {

constexpr int kMinCoarseness = 0;
constexpr int kMaxCoarseness = 4000;
constexpr int kMinCoarsenessRadius = 8;
constexpr double kMinValidArea = 0.5;

struct ValidationResult {
    bool valid = false;
    std::string reason;
};

struct PointKey {
    int x = 0;
    int y = 0;
    bool operator==(const PointKey& other) const noexcept { return x == other.x && y == other.y; }
};

struct PointKeyHash {
    std::size_t operator()(const PointKey& p) const noexcept {
        return (static_cast<std::size_t>(static_cast<unsigned int>(p.x)) << 32U) ^
               static_cast<std::size_t>(static_cast<unsigned int>(p.y));
    }
};

std::string point_to_string(SDL_Point p) {
    return "(" + std::to_string(p.x) + "," + std::to_string(p.y) + ")";
}

std::string bounds_to_string(const std::tuple<int, int, int, int>& bounds) {
    auto [minx, miny, maxx, maxy] = bounds;
    return "[" + std::to_string(minx) + "," + std::to_string(miny) + " -> " +
           std::to_string(maxx) + "," + std::to_string(maxy) + "]";
}

vibble::weighted_range::WeightedIntRange coarseness_range_from_legacy(int value) {
    const int clamped = std::clamp(value, kMinCoarseness, kMaxCoarseness);
    if (clamped <= 0) {
        return vibble::weighted_range::make_flat(0);
    }
    const int min_radius = std::max(kMinCoarsenessRadius, 12 + (clamped / 18));
    const int max_radius = std::max(min_radius, 36 + (clamped / 4));
    return vibble::weighted_range::make_legacy_uniform(min_radius, max_radius);
}

std::optional<vibble::weighted_range::WeightedIntRange> read_coarseness_range(const Room* room) {
    if (!room) return std::nullopt;
    const auto& data = room->assets_data();
    if (!data.is_object() || !data.contains("coarseness")) {
        return std::nullopt;
    }

    const auto& value = data["coarseness"];
    if (value.is_null() || (value.is_object() && value.empty())) {
        return std::nullopt;
    }

    if (value.is_number_integer()) {
        return coarseness_range_from_legacy(value.get<int>());
    }
    if (value.is_number_float()) {
        return coarseness_range_from_legacy(static_cast<int>(std::lround(value.get<double>())));
    }

    const auto parsed = vibble::weighted_range::from_json(value, vibble::weighted_range::make_flat(0));
    if (!vibble::weighted_range::is_valid(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

bool coarseness_range_enabled(const vibble::weighted_range::WeightedIntRange& range) {
    if (!vibble::weighted_range::is_valid(range)) {
        return false;
    }
    const std::int64_t max_value = range.center + std::llabs(range.span);
    return max_value > 0;
}

int resolve_radius_from_coarseness_range(const vibble::weighted_range::WeightedIntRange& range, std::mt19937& rng) {
    const std::int64_t resolved = vibble::weighted_range::resolve(range, rng);
    if (resolved <= 0 || resolved > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        return 0;
    }
    return static_cast<int>(resolved);
}

std::vector<SDL_Point> sanitize_points(const std::vector<SDL_Point>& input) {
    std::vector<SDL_Point> out;
    out.reserve(input.size());
    for (const SDL_Point& point : input) {
        if (!out.empty() && out.back().x == point.x && out.back().y == point.y) {
            continue;
        }
        out.push_back(point);
    }
    if (out.size() > 1 && out.front().x == out.back().x && out.front().y == out.back().y) {
        out.pop_back();
    }
    return out;
}

double signed_area(const std::vector<SDL_Point>& points) {
    if (points.size() < 3) return 0.0;
    long double twice_area = 0.0;
    for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
        twice_area += static_cast<long double>(points[j].x) * static_cast<long double>(points[i].y) -
                      static_cast<long double>(points[i].x) * static_cast<long double>(points[j].y);
    }
    return static_cast<double>(twice_area * 0.5L);
}

int orientation(SDL_Point a, SDL_Point b, SDL_Point c) {
    const long long v = static_cast<long long>(b.y - a.y) * static_cast<long long>(c.x - b.x) -
                        static_cast<long long>(b.x - a.x) * static_cast<long long>(c.y - b.y);
    if (v == 0) return 0;
    return v > 0 ? 1 : 2;
}

bool on_segment(SDL_Point a, SDL_Point b, SDL_Point c) {
    return b.x <= std::max(a.x, c.x) && b.x >= std::min(a.x, c.x) &&
           b.y <= std::max(a.y, c.y) && b.y >= std::min(a.y, c.y);
}

bool segments_intersect(SDL_Point p1, SDL_Point q1, SDL_Point p2, SDL_Point q2) {
    const int o1 = orientation(p1, q1, p2);
    const int o2 = orientation(p1, q1, q2);
    const int o3 = orientation(p2, q2, p1);
    const int o4 = orientation(p2, q2, q1);
    if (o1 != o2 && o3 != o4) return true;
    if (o1 == 0 && on_segment(p1, p2, q1)) return true;
    if (o2 == 0 && on_segment(p1, q2, q1)) return true;
    if (o3 == 0 && on_segment(p2, p1, q2)) return true;
    if (o4 == 0 && on_segment(p2, q1, q2)) return true;
    return false;
}

bool polygon_has_self_intersection(const std::vector<SDL_Point>& points) {
    const std::size_t n = points.size();
    if (n < 4) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const SDL_Point a0 = points[i];
        const SDL_Point a1 = points[(i + 1) % n];
        for (std::size_t j = i + 1; j < n; ++j) {
            if (j == i || j == i + 1 || (i == 0 && j == n - 1)) {
                continue;
            }
            const SDL_Point b0 = points[j];
            const SDL_Point b1 = points[(j + 1) % n];
            if (segments_intersect(a0, a1, b0, b1)) {
                return true;
            }
        }
    }
    return false;
}

ValidationResult validate_polygon_points(const std::vector<SDL_Point>& raw_points) {
    const std::vector<SDL_Point> points = sanitize_points(raw_points);
    if (points.size() < 3) {
        return {false, "fewer_than_three_points"};
    }
    const double area = std::abs(signed_area(points));
    if (!(area > kMinValidArea)) {
        return {false, "non_positive_area"};
    }

    std::unordered_set<PointKey, PointKeyHash> seen;
    seen.reserve(points.size());
    for (const SDL_Point& point : points) {
        const PointKey key{point.x, point.y};
        if (!seen.insert(key).second) {
            return {false, std::string("duplicate_point_") + point_to_string(point)};
        }
    }

    if (polygon_has_self_intersection(points)) {
        return {false, "self_intersection"};
    }

    Area probe("coarseness_validation", points);
    auto [minx, miny, maxx, maxy] = probe.get_bounds();
    if (minx >= maxx || miny >= maxy) {
        return {false, "unusable_bounds"};
    }
    return {true, {}};
}


struct CirclePlacement {
    SDL_Point center{0, 0};
    int radius = 0;
    double perimeter_distance = 0.0;
};

double distance_between(SDL_Point a, SDL_Point b) {
    const double dx = static_cast<double>(b.x - a.x);
    const double dy = static_cast<double>(b.y - a.y);
    return std::sqrt(dx * dx + dy * dy);
}

double perimeter_length(const std::vector<SDL_Point>& points) {
    if (points.size() < 2) return 0.0;
    double total = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        total += distance_between(points[i], points[(i + 1) % points.size()]);
    }
    return total;
}

SDL_Point point_at_perimeter_distance(const std::vector<SDL_Point>& points, double target_distance, int resolution) {
    if (points.empty()) return SDL_Point{0, 0};
    const double total = perimeter_length(points);
    if (!(total > 0.0)) return points.front();
    double remaining = std::fmod(std::max(0.0, target_distance), total);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const SDL_Point a = points[i];
        const SDL_Point b = points[(i + 1) % points.size()];
        const double len = distance_between(a, b);
        if (len <= 0.0) continue;
        if (remaining <= len) {
            const double t = remaining / len;
            SDL_Point p{
                static_cast<int>(std::lround(static_cast<double>(a.x) + (static_cast<double>(b.x - a.x) * t))),
                static_cast<int>(std::lround(static_cast<double>(a.y) + (static_cast<double>(b.y - a.y) * t)))
            };
            return vibble::grid::snap_world_to_vertex(p, resolution);
        }
        remaining -= len;
    }
    return vibble::grid::snap_world_to_vertex(points.back(), resolution);
}

bool circles_intersect(const CirclePlacement& a, const CirclePlacement& b) {
    return distance_between(a.center, b.center) <= static_cast<double>(a.radius + b.radius);
}

std::vector<CirclePlacement> build_continuous_circle_chain(const std::vector<SDL_Point>& boundary,
                                                           const vibble::weighted_range::WeightedIntRange& range,
                                                           int resolution,
                                                           std::mt19937& rng,
                                                           CoarsenessExpansionResult& result) {
    std::vector<CirclePlacement> chain;
    const double total_perimeter = perimeter_length(boundary);
    result.perimeter_length_processed = total_perimeter;
    if (!(total_perimeter > 0.0)) {
        result.uncovered_perimeter_segments = 1;
        return chain;
    }

    std::uniform_real_distribution<double> start_offset_distribution(0.0, total_perimeter);
    std::uniform_real_distribution<double> step_factor_distribution(0.65, 0.95);
    double cursor = start_offset_distribution(rng);
    double traveled = 0.0;
    int radius = resolve_radius_from_coarseness_range(range, rng);
    if (radius <= 0) {
        ++result.circles_skipped;
        result.uncovered_perimeter_segments = 1;
        return chain;
    }
    chain.push_back(CirclePlacement{point_at_perimeter_distance(boundary, cursor, resolution), radius, cursor});

    while (traveled < total_perimeter) {
        int next_radius = resolve_radius_from_coarseness_range(range, rng);
        if (next_radius <= 0) {
            ++result.circles_skipped;
            next_radius = chain.back().radius;
            if (next_radius <= 0) break;
        }

        const double max_intersecting_step = static_cast<double>(chain.back().radius + next_radius);
        const double desired_step = std::max(1.0, std::floor(max_intersecting_step * step_factor_distribution(rng)));
        const double remaining = total_perimeter - traveled;

        if (remaining <= static_cast<double>(chain.back().radius + chain.front().radius)) {
            if (circles_intersect(chain.back(), chain.front())) {
                ++result.circle_intersections_succeeded;
            } else {
                ++result.uncovered_perimeter_segments;
            }
            break;
        }

        const double step = std::min(desired_step, remaining);
        traveled += step;
        cursor = std::fmod(cursor + step, total_perimeter);

        CirclePlacement next{point_at_perimeter_distance(boundary, cursor, resolution), next_radius, cursor};
        if (circles_intersect(chain.back(), next)) {
            ++result.circle_intersections_succeeded;
        } else {
            ++result.uncovered_perimeter_segments;
            // Keep the boundary covered even if snapping pushed the centers apart: retry halfway.
            const double retry_distance = std::fmod(chain.back().perimeter_distance + (step * 0.5), total_perimeter);
            CirclePlacement bridge{point_at_perimeter_distance(boundary, retry_distance, resolution),
                                   std::max(chain.back().radius, next_radius),
                                   retry_distance};
            if (circles_intersect(chain.back(), bridge) && circles_intersect(bridge, next)) {
                chain.push_back(bridge);
                result.circle_intersections_succeeded += 2;
                --result.uncovered_perimeter_segments;
            }
        }
        chain.push_back(next);
    }

    if (chain.size() > 1 && !circles_intersect(chain.back(), chain.front())) {
        ++result.uncovered_perimeter_segments;
    }
    return chain;
}

std::vector<SDL_Point> make_circle_polygon(SDL_Point center,
                                           int radius,
                                           int resolution,
                                           int segments = 32,
                                           double phase_radians = 0.0) {
    (void)resolution;
    constexpr double kTwoPi = 6.28318530717958647692;
    std::vector<SDL_Point> out;
    segments = std::max(12, segments);
    out.reserve(static_cast<std::size_t>(segments));
    for (int i = 0; i < segments; ++i) {
        const double t = phase_radians + (kTwoPi * static_cast<double>(i)) / static_cast<double>(segments);
        SDL_Point p{
            center.x + static_cast<int>(std::lround(std::cos(t) * static_cast<double>(radius))),
            center.y + static_cast<int>(std::lround(std::sin(t) * static_cast<double>(radius)))
        };
        out.push_back(p);
    }
    return sanitize_points(out);
}

int item_resolution(const CoarsenessGeometryItem& item) {
    if (item.geometry && item.geometry->resolution() > 0) {
        return item.geometry->resolution();
    }
    return vibble::grid::clamp_resolution(item.grid_settings.grid_resolution);
}

#ifdef VIBBLE_HAS_CLIPPER2
Clipper2Lib::PathD to_path(const std::vector<SDL_Point>& points) {
    Clipper2Lib::PathD out;
    out.reserve(points.size());
    for (const auto& p : points) {
        out.push_back(Clipper2Lib::PointD{static_cast<double>(p.x), static_cast<double>(p.y)});
    }
    return out;
}

Clipper2Lib::PathsD to_paths(const std::vector<SDL_Point>& points) {
    Clipper2Lib::PathsD out;
    out.push_back(to_path(points));
    return out;
}

std::vector<SDL_Point> to_points(const Clipper2Lib::PathD& path, int resolution) {
    std::vector<SDL_Point> out;
    out.reserve(path.size());
    for (const auto& p : path) {
        out.push_back(vibble::grid::snap_world_to_vertex(SDL_Point{
            static_cast<int>(std::lround(p.x)),
            static_cast<int>(std::lround(p.y))
        }, resolution));
    }
    return sanitize_points(out);
}

double path_area_abs(const Clipper2Lib::PathD& path) {
    return std::abs(Clipper2Lib::Area(path));
}

double paths_area_abs(const Clipper2Lib::PathsD& paths) {
    double total = 0.0;
    for (const auto& path : paths) {
        total += path_area_abs(path);
    }
    return total;
}

std::vector<SDL_Point> largest_valid_path_points(const Clipper2Lib::PathsD& paths,
                                                 int resolution,
                                                 const std::string& label,
                                                 const std::string& identifier) {
    std::vector<SDL_Point> best;
    double best_area = 0.0;
    for (const auto& path : paths) {
        std::vector<SDL_Point> candidate = to_points(path, resolution);
        const ValidationResult validation = validate_polygon_points(candidate);
        if (!validation.valid) {
            vibble::log::debug("[Coarseness] " + identifier + " skipped invalid " + label +
                               " path: " + validation.reason);
            continue;
        }
        const double area = std::abs(signed_area(candidate));
        if (area > best_area) {
            best_area = area;
            best = std::move(candidate);
        }
    }
    return best;
}

bool validate_paths(const Clipper2Lib::PathsD& paths,
                    int resolution,
                    const std::string& operation,
                    const std::string& identifier,
                    bool allow_empty = false) {
    if (paths.empty()) {
        if (allow_empty) return true;
        vibble::log::debug("[Coarseness] " + identifier + " invalid " + operation + ": empty_path_set");
        return false;
    }
    for (const auto& path : paths) {
        const ValidationResult validation = validate_polygon_points(to_points(path, resolution));
        if (!validation.valid) {
            vibble::log::debug("[Coarseness] " + identifier + " invalid " + operation + ": " + validation.reason);
            return false;
        }
    }
    return true;
}

Area make_area_from_points(const CoarsenessGeometryItem& item,
                           const std::vector<SDL_Point>& points,
                           const std::string& fallback_name,
                           int resolution) {
    const std::string name = item.geometry ? item.geometry->get_name() : fallback_name;
    Area out(name, points, resolution);
    if (item.geometry) {
        out.set_type(item.geometry->get_type());
    }
    return out;
}
#endif

} // namespace

std::vector<CoarsenessExpansionResult> apply_coarseness_pass(const std::vector<CoarsenessGeometryItem>& items) {
    std::vector<CoarsenessExpansionResult> results;
    results.reserve(items.size());

    std::vector<std::unique_ptr<Area>> original_snapshots;
    original_snapshots.reserve(items.size());
    for (const CoarsenessGeometryItem& item : items) {
        if (item.geometry) {
            original_snapshots.push_back(std::make_unique<Area>(*item.geometry));
        } else {
            original_snapshots.push_back(nullptr);
        }
    }

#ifndef VIBBLE_HAS_CLIPPER2
    vibble::log::warn("[Coarseness] Skipping coarseness expansion: Clipper2 polygon boolean backend is unavailable.");
    for (const CoarsenessGeometryItem& item : items) {
        CoarsenessExpansionResult result;
        result.identifier = item.identifier;
        results.push_back(std::move(result));
    }
    return results;
#else
    std::mt19937 rng(std::random_device{}());

    for (std::size_t index = 0; index < items.size(); ++index) {
        const CoarsenessGeometryItem& item = items[index];
        CoarsenessExpansionResult result;
        result.identifier = item.identifier;

        if (!item.geometry || !original_snapshots[index]) {
            vibble::log::debug("[Coarseness] skipped '" + item.identifier + "': missing geometry");
            results.push_back(std::move(result));
            continue;
        }
        if (!item.coarseness_range || !coarseness_range_enabled(*item.coarseness_range)) {
            vibble::log::debug("[Coarseness] skipped '" + item.identifier + "': no valid coarseness range");
            results.push_back(std::move(result));
            continue;
        }

        const int resolution = item_resolution(item);
        const Area& original = *original_snapshots[index];
        const std::vector<SDL_Point> original_boundary = sanitize_points(original.get_points());
        const ValidationResult original_validation = validate_polygon_points(original_boundary);
        if (!original_validation.valid) {
            vibble::log::debug("[Coarseness] skipped '" + item.identifier + "': invalid original geometry " +
                               original_validation.reason);
            results.push_back(std::move(result));
            continue;
        }

        Clipper2Lib::PathsD active_paths = to_paths(original_boundary);
        const Clipper2Lib::PathsD original_paths = to_paths(original_boundary);
        const double original_area = original.get_area();
        vibble::log::debug("[Coarseness] processing '" + item.identifier + "' range=" +
                           vibble::weighted_range::to_json(*item.coarseness_range).dump() +
                           " original_area=" + std::to_string(original_area) +
                           " original_bounds=" + bounds_to_string(original.get_bounds()) +
                           " resolution=" + std::to_string(resolution));

        const std::vector<CirclePlacement> circle_chain = build_continuous_circle_chain(
            original_boundary, *item.coarseness_range, resolution, rng, result);
        vibble::log::debug("[Coarseness] '" + item.identifier + "' perimeter_chain perimeter_length=" +
                           std::to_string(result.perimeter_length_processed) +
                           " generated=" + std::to_string(circle_chain.size()) +
                           " skipped=" + std::to_string(result.circles_skipped) +
                           " circle_intersections=" + std::to_string(result.circle_intersections_succeeded) +
                           " uncovered_segments=" + std::to_string(result.uncovered_perimeter_segments));
        std::uniform_real_distribution<double> circle_phase_distribution(0.0, 6.28318530717958647692);

        for (const CirclePlacement& circle : circle_chain) {
            const SDL_Point center = circle.center;
            const int radius = circle.radius;
            if (radius <= 0) {
                ++result.circles_skipped;
                continue;
            }

            ++result.circles_attempted;
            const std::vector<SDL_Point> circle_points =
                make_circle_polygon(center, radius, resolution, 32, circle_phase_distribution(rng));
            const ValidationResult circle_validation = validate_polygon_points(circle_points);
            if (!circle_validation.valid) {
                ++result.circles_skipped;
                vibble::log::debug("[Coarseness] '" + item.identifier + "' circle attempted at " +
                                   point_to_string(center) + " radius=" + std::to_string(radius) +
                                   " rejected: " + circle_validation.reason);
                continue;
            }

            Clipper2Lib::PathsD circle_paths = to_paths(circle_points);
            Clipper2Lib::PathsD grown_paths;
            try {
                grown_paths = Clipper2Lib::Union(active_paths, circle_paths, Clipper2Lib::FillRule::NonZero, 2);
            } catch (const std::exception& ex) {
                ++result.circles_skipped;
                vibble::log::warn("[Coarseness] Boolean union failed for '" + item.identifier + "': " + ex.what());
                continue;
            }

            if (!validate_paths(grown_paths, resolution, "circle_union", item.identifier)) {
                ++result.circles_skipped;
                vibble::log::debug("[Coarseness] '" + item.identifier + "' circle attempted at " +
                                   point_to_string(center) + " radius=" + std::to_string(radius) +
                                   " rejected after union validation");
                continue;
            }

            Clipper2Lib::PathsD expansion_paths_after_circle;
            try {
                expansion_paths_after_circle = Clipper2Lib::Difference(grown_paths, original_paths, Clipper2Lib::FillRule::NonZero, 2);
            } catch (const std::exception& ex) {
                ++result.circles_skipped;
                vibble::log::warn("[Coarseness] Boolean difference failed for '" + item.identifier + "' after circle at " +
                                  point_to_string(center) + ": " + ex.what());
                continue;
            }
            if (!validate_paths(expansion_paths_after_circle, resolution, "circle_expansion_difference", item.identifier, true)) {
                ++result.circles_skipped;
                vibble::log::debug("[Coarseness] '" + item.identifier + "' circle attempted at " +
                                   point_to_string(center) + " radius=" + std::to_string(radius) +
                                   " rejected after expansion difference validation");
                continue;
            }

            const double before_area = paths_area_abs(active_paths);
            const double after_area = paths_area_abs(grown_paths);
            const double expansion_area_after_circle = paths_area_abs(expansion_paths_after_circle);
            active_paths = std::move(grown_paths);
            ++result.circles_applied;
            vibble::log::debug("[Coarseness] '" + item.identifier + "' circle applied at " + point_to_string(center) +
                               " radius=" + std::to_string(radius) +
                               " perimeter_distance=" + std::to_string(circle.perimeter_distance) +
                               " area_before=" + std::to_string(before_area) +
                               " area_after=" + std::to_string(after_area) +
                               " expansion_area=" + std::to_string(expansion_area_after_circle));
        }

        Clipper2Lib::PathsD expansion_paths;
        try {
            expansion_paths = Clipper2Lib::Difference(active_paths, original_paths, Clipper2Lib::FillRule::NonZero, 2);
        } catch (const std::exception& ex) {
            vibble::log::warn("[Coarseness] Boolean difference failed for '" + item.identifier + "': " + ex.what());
            results.push_back(std::move(result));
            continue;
        }

        if (!validate_paths(expansion_paths, resolution, "final_expansion_difference", item.identifier, true)) {
            vibble::log::debug("[Coarseness] '" + item.identifier + "' rejected final expansion area after difference validation");
            results.push_back(std::move(result));
            continue;
        }

        const std::vector<SDL_Point> expanded_points = largest_valid_path_points(active_paths, resolution, "expanded", item.identifier);
        if (expanded_points.empty()) {
            vibble::log::debug("[Coarseness] '" + item.identifier + "' rejected final expanded geometry: no valid path");
            results.push_back(std::move(result));
            continue;
        }

        Area expanded = make_area_from_points(item, expanded_points, item.identifier, resolution);
        const double before_area = original.get_area();
        const double after_area = expanded.get_area();
        *item.geometry = expanded;

        const std::vector<SDL_Point> expansion_points = largest_valid_path_points(expansion_paths, resolution, "expansion", item.identifier);
        if (!expansion_points.empty()) {
            result.expansion_area = std::make_unique<Area>(item.identifier + "_coarse_added", expansion_points, resolution);
            result.expansion_area->set_type(item.geometry->get_type());
        }

        const double expansion_area = result.expansion_area ? result.expansion_area->get_area() : 0.0;
        result.expanded_area_size = expansion_area;
        vibble::log::debug("[Coarseness] '" + item.identifier + "' geometry area before=" + std::to_string(before_area) +
                           " after=" + std::to_string(after_area) +
                           " expansion_area=" + std::to_string(expansion_area));
        vibble::log::debug("[Coarseness] '" + item.identifier + "' validation perimeter_length_processed=" +
                           std::to_string(result.perimeter_length_processed) +
                           " total_circles_generated=" + std::to_string(circle_chain.size()) +
                           " circles_attempted=" + std::to_string(result.circles_attempted) +
                           " circles_applied=" + std::to_string(result.circles_applied) +
                           " circles_skipped=" + std::to_string(result.circles_skipped) +
                           " circle_to_circle_intersection_success=" +
                           std::to_string(result.circle_intersections_succeeded) +
                           " uncovered_perimeter_segments=" +
                           std::to_string(result.uncovered_perimeter_segments) +
                           " expanded_area_size=" + std::to_string(result.expanded_area_size) +
                           " final_expanded_bounds=" + bounds_to_string(item.geometry->get_bounds()));

        results.push_back(std::move(result));
    }

    return results;
#endif
}

void apply_coarseness_expansion(std::vector<Room*>& rooms) {
    std::vector<CoarsenessGeometryItem> items;
    std::vector<Room*> item_rooms;
    items.reserve(rooms.size());
    item_rooms.reserve(rooms.size());
    for (Room* room : rooms) {
        if (!room) continue;
        items.push_back(CoarsenessGeometryItem{
            room->room_name,
            room->room_area.get(),
            read_coarseness_range(room),
            room->map_grid_settings(),
        });
        item_rooms.push_back(room);
    }

    std::vector<CoarsenessExpansionResult> results = apply_coarseness_pass(items);
    const std::size_t count = std::min(results.size(), item_rooms.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (!item_rooms[i]) continue;
        item_rooms[i]->coarseness_added_area = std::move(results[i].expansion_area);
    }
}

} // namespace vibble::mapgen::coarseness
