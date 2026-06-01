#include "gameplay/map_generation/coarseness_system.hpp"

#include "gameplay/map_generation/room.hpp"
#include "utils/area.hpp"
#include "utils/coarseness_range.hpp"
#include "utils/grid.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

constexpr double kMinValidArea = 0.5;
constexpr std::uint64_t kDefaultDeterministicSeed = 0x9e3779b97f4a7c15ULL;

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

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

std::uint64_t mix_u64(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    return seed;
}

std::uint64_t hash_points(const std::vector<SDL_Point>& points) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const SDL_Point& p : points) {
        hash = mix_u64(hash, static_cast<std::uint64_t>(static_cast<std::int64_t>(p.x)));
        hash = mix_u64(hash, static_cast<std::uint64_t>(static_cast<std::int64_t>(p.y)));
    }
    return hash;
}

std::string point_to_string(SDL_Point p) {
    return "(" + std::to_string(p.x) + "," + std::to_string(p.y) + ")";
}

std::string bounds_to_string(const std::tuple<int, int, int, int>& bounds) {
    auto [minx, miny, maxx, maxy] = bounds;
    return "[" + std::to_string(minx) + "," + std::to_string(miny) + " -> " +
           std::to_string(maxx) + "," + std::to_string(maxy) + "]";
}

std::optional<vibble::weighted_range::WeightedIntRange> read_coarseness_range(const Room* room) {
    if (!room) return std::nullopt;
    return vibble::coarseness::read_optional_range(room->assets_data());
}

bool coarseness_range_enabled(const vibble::weighted_range::WeightedIntRange& range) {
    return vibble::coarseness::enabled(range);
}

int resolve_radius_from_coarseness_range(const vibble::weighted_range::WeightedIntRange& range, std::mt19937& rng) {
    const std::int64_t resolved = vibble::weighted_range::resolve(range, rng);
    if (resolved <= 0 || resolved > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        return 0;
    }
    return static_cast<int>(resolved);
}

std::pair<int, int> resolve_coarseness_bounds(const vibble::weighted_range::WeightedIntRange& range) {
    return vibble::coarseness::bounds(range);
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

double vec_length(const Vec2& v) {
    return std::sqrt(v.x * v.x + v.y * v.y);
}

Vec2 vec_sub(const Vec2& a, const Vec2& b) {
    return Vec2{a.x - b.x, a.y - b.y};
}

Vec2 vec_add(const Vec2& a, const Vec2& b) {
    return Vec2{a.x + b.x, a.y + b.y};
}

Vec2 vec_mul(const Vec2& v, double scalar) {
    return Vec2{v.x * scalar, v.y * scalar};
}

Vec2 vec_normalize(const Vec2& v) {
    const double len = vec_length(v);
    if (len <= 1e-9) {
        return Vec2{0.0, 0.0};
    }
    return Vec2{v.x / len, v.y / len};
}

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

Vec2 point_at_perimeter_distance(const std::vector<SDL_Point>& points, double target_distance) {
    if (points.empty()) return Vec2{};
    const double total = perimeter_length(points);
    if (!(total > 0.0)) {
        return Vec2{static_cast<double>(points.front().x), static_cast<double>(points.front().y)};
    }
    double remaining = std::fmod(std::max(0.0, target_distance), total);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const SDL_Point a = points[i];
        const SDL_Point b = points[(i + 1) % points.size()];
        const double len = distance_between(a, b);
        if (len <= 0.0) continue;
        if (remaining <= len) {
            const double t = remaining / len;
            return Vec2{
                static_cast<double>(a.x) + (static_cast<double>(b.x - a.x) * t),
                static_cast<double>(a.y) + (static_cast<double>(b.y - a.y) * t),
            };
        }
        remaining -= len;
    }
    return Vec2{static_cast<double>(points.back().x), static_cast<double>(points.back().y)};
}

double smoothstep(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double periodic_interp(const std::vector<double>& controls, double scaled_u) {
    if (controls.empty()) {
        return 0.0;
    }
    const double count = static_cast<double>(controls.size());
    double wrapped = std::fmod(scaled_u, count);
    if (wrapped < 0.0) wrapped += count;
    const std::size_t i0 = static_cast<std::size_t>(std::floor(wrapped)) % controls.size();
    const std::size_t i1 = (i0 + 1) % controls.size();
    const double frac = wrapped - std::floor(wrapped);
    const double t = smoothstep(frac);
    return controls[i0] + (controls[i1] - controls[i0]) * t;
}

void smooth_closed_scalars(std::vector<double>& values, int passes, int radius) {
    if (values.empty() || passes <= 0 || radius <= 0) {
        return;
    }
    const std::size_t n = values.size();
    std::vector<double> tmp(values.size(), 0.0);
    for (int pass = 0; pass < passes; ++pass) {
        for (std::size_t i = 0; i < n; ++i) {
            double sum = values[i];
            double weight = 1.0;
            for (int r = 1; r <= radius; ++r) {
                const std::size_t prev = (i + n - static_cast<std::size_t>(r)) % n;
                const std::size_t next = (i + static_cast<std::size_t>(r)) % n;
                const double w = 1.0 / static_cast<double>(r + 1);
                sum += (values[prev] + values[next]) * w;
                weight += 2.0 * w;
            }
            tmp[i] = sum / weight;
        }
        values.swap(tmp);
    }
}

void smooth_closed_points(std::vector<Vec2>& points, int passes) {
    if (points.size() < 3 || passes <= 0) {
        return;
    }
    const std::size_t n = points.size();
    std::vector<Vec2> tmp(points.size());
    for (int pass = 0; pass < passes; ++pass) {
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2& prev = points[(i + n - 1) % n];
            const Vec2& curr = points[i];
            const Vec2& next = points[(i + 1) % n];
            tmp[i] = Vec2{
                (prev.x + curr.x * 2.0 + next.x) * 0.25,
                (prev.y + curr.y * 2.0 + next.y) * 0.25,
            };
        }
        points.swap(tmp);
    }
}

std::vector<Vec2> sample_perimeter_evenly(const std::vector<SDL_Point>& contour, int sample_count) {
    std::vector<Vec2> samples;
    const double total = perimeter_length(contour);
    if (!(total > 0.0) || sample_count < 3) {
        return samples;
    }
    samples.reserve(static_cast<std::size_t>(sample_count));
    const double step = total / static_cast<double>(sample_count);
    for (int i = 0; i < sample_count; ++i) {
        samples.push_back(point_at_perimeter_distance(contour, static_cast<double>(i) * step));
    }
    return samples;
}

std::vector<Vec2> outward_normals_from_samples(const std::vector<Vec2>& samples, bool is_ccw) {
    std::vector<Vec2> normals(samples.size(), Vec2{0.0, 0.0});
    if (samples.size() < 3) {
        return normals;
    }
    const std::size_t n = samples.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2& prev = samples[(i + n - 1) % n];
        const Vec2& next = samples[(i + 1) % n];
        Vec2 tangent = vec_normalize(vec_sub(next, prev));
        if (vec_length(tangent) <= 1e-6) {
            tangent = Vec2{1.0, 0.0};
        }
        Vec2 normal = is_ccw ? Vec2{tangent.y, -tangent.x}
                             : Vec2{-tangent.y, tangent.x};
        normal = vec_normalize(normal);
        if (vec_length(normal) <= 1e-6) {
            normal = Vec2{0.0, -1.0};
        }
        normals[i] = normal;
    }
    return normals;
}

std::vector<double> build_noisy_offsets(int sample_count,
                                        const vibble::weighted_range::WeightedIntRange& range,
                                        std::mt19937& rng,
                                        double min_offset,
                                        double max_offset) {
    std::vector<double> offsets;
    if (sample_count < 3 || !(max_offset > 0.0)) {
        return offsets;
    }

    const int primary_count = std::clamp(sample_count / 12, 8, 128);
    const int secondary_count = std::clamp(sample_count / 5, 16, 256);
    std::vector<double> primary(static_cast<std::size_t>(primary_count), min_offset);
    std::vector<double> secondary(static_cast<std::size_t>(secondary_count), min_offset);

    auto resolve = [&](double fallback) {
        const int value = resolve_radius_from_coarseness_range(range, rng);
        if (value <= 0) return fallback;
        return static_cast<double>(value);
    };

    for (double& v : primary) {
        v = std::clamp(resolve(min_offset), min_offset, max_offset);
    }
    for (double& v : secondary) {
        v = std::clamp(resolve(min_offset), min_offset, max_offset);
    }

    offsets.resize(static_cast<std::size_t>(sample_count), min_offset);
    for (int i = 0; i < sample_count; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(sample_count);
        const double base = periodic_interp(primary, t * static_cast<double>(primary_count));
        const double micro = periodic_interp(secondary, t * static_cast<double>(secondary_count) + 0.37);
        const double offset = (base * 0.76) + (micro * 0.24);
        offsets[static_cast<std::size_t>(i)] = std::clamp(offset, min_offset, max_offset);
    }
    smooth_closed_scalars(offsets, 2, 2);
    for (double& v : offsets) {
        v = std::clamp(v, min_offset, max_offset);
    }
    return offsets;
}

std::vector<SDL_Point> expand_contour_from_samples(const std::vector<Vec2>& samples,
                                                   const std::vector<Vec2>& normals,
                                                   const std::vector<double>& offsets,
                                                   int resolution,
                                                   int smooth_passes) {
    if (samples.size() < 3 || normals.size() != samples.size() || offsets.size() != samples.size()) {
        return {};
    }

    std::vector<Vec2> expanded;
    expanded.reserve(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        expanded.push_back(vec_add(samples[i], vec_mul(normals[i], offsets[i])));
    }
    smooth_closed_points(expanded, smooth_passes);

    std::vector<SDL_Point> out;
    out.reserve(expanded.size());
    for (const Vec2& v : expanded) {
        SDL_Point snapped = vibble::grid::snap_world_to_vertex(SDL_Point{
            static_cast<int>(std::lround(v.x)),
            static_cast<int>(std::lround(v.y)),
        }, resolution);
        out.push_back(snapped);
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

Area make_area_from_points(const CoarsenessGeometryItem& item,
                           const std::vector<SDL_Point>& points,
                           const std::string& fallback_name,
                           int resolution);

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

std::vector<Area> valid_path_areas(const Clipper2Lib::PathsD& paths,
                                   int resolution,
                                   const std::string& label,
                                   const std::string& identifier,
                                   const CoarsenessGeometryItem& item) {
    std::vector<Area> out;
    out.reserve(paths.size());
    int path_index = 0;
    for (const auto& path : paths) {
        std::vector<SDL_Point> candidate = to_points(path, resolution);
        const ValidationResult validation = validate_polygon_points(candidate);
        if (!validation.valid) {
            vibble::log::debug("[Coarseness] " + identifier + " skipped invalid " + label +
                               " path: " + validation.reason);
            ++path_index;
            continue;
        }
        Area area = make_area_from_points(item,
                                          candidate,
                                          item.identifier + "_" + label + "_" + std::to_string(path_index),
                                          resolution);
        area.set_name(item.identifier + "_" + label + "_" + std::to_string(path_index));
        out.push_back(std::move(area));
        ++path_index;
    }
    return out;
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

        const auto [range_min, range_max] = resolve_coarseness_bounds(*item.coarseness_range);
        if (range_max <= 0) {
            vibble::log::debug("[Coarseness] skipped '" + item.identifier + "': non-positive coarseness bounds");
            results.push_back(std::move(result));
            continue;
        }

        const double perimeter = perimeter_length(original_boundary);
        result.perimeter_length_processed = perimeter;
        if (!(perimeter > 0.0)) {
            vibble::log::debug("[Coarseness] skipped '" + item.identifier + "': empty perimeter");
            results.push_back(std::move(result));
            continue;
        }

        const int grid_step = std::max(1, vibble::grid::delta(resolution));
        const double expansion_radius = std::max(static_cast<double>(range_max), static_cast<double>(grid_step));
        const Clipper2Lib::PathsD original_paths = to_paths(original_boundary);
        const double original_area = original.get_area();
        vibble::log::debug("[Coarseness] processing '" + item.identifier + "' range=" +
                           vibble::weighted_range::to_json(*item.coarseness_range).dump() +
                           " original_area=" + std::to_string(original_area) +
                           " original_bounds=" + bounds_to_string(original.get_bounds()) +
                           " resolution=" + std::to_string(resolution) +
                           " expansion_radius=" + std::to_string(expansion_radius));

        Clipper2Lib::PathsD active_paths;
        try {
            active_paths = Clipper2Lib::InflatePaths(original_paths,
                                                     expansion_radius,
                                                     Clipper2Lib::JoinType::Round,
                                                     Clipper2Lib::EndType::Polygon,
                                                     2.0,
                                                     std::max(0.5, static_cast<double>(grid_step) * 0.5));
        } catch (const std::exception& ex) {
            vibble::log::warn("[Coarseness] Offset expansion failed for '" + item.identifier + "': " + ex.what());
            results.push_back(std::move(result));
            continue;
        }

        if (!validate_paths(active_paths, resolution, "expanded_union", item.identifier)) {
            vibble::log::debug("[Coarseness] '" + item.identifier + "' rejected expanded union paths");
            results.push_back(std::move(result));
            continue;
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

        result.expanded_areas = valid_path_areas(active_paths, resolution, "expanded", item.identifier, item);
        result.soft_boundary_areas = valid_path_areas(expansion_paths, resolution, "soft_boundary", item.identifier, item);
        if (result.expanded_areas.empty()) {
            vibble::log::debug("[Coarseness] '" + item.identifier + "' rejected final expanded geometry: no valid path");
            results.push_back(std::move(result));
            continue;
        }

        auto largest_area_it = std::max_element(result.expanded_areas.begin(),
                                                result.expanded_areas.end(),
                                                [](const Area& a, const Area& b) {
                                                    return a.get_area() < b.get_area();
                                                });
        Area expanded = *largest_area_it;
        expanded.set_name(item.identifier);
        const double before_area = original.get_area();
        const double after_area = expanded.get_area();
        result.base_area = std::make_unique<Area>(original);
        result.expanded_area = std::make_unique<Area>(expanded);
        *item.geometry = expanded;

        if (!result.soft_boundary_areas.empty()) {
            auto largest_soft_it = std::max_element(result.soft_boundary_areas.begin(),
                                                    result.soft_boundary_areas.end(),
                                                    [](const Area& a, const Area& b) {
                                                        return a.get_area() < b.get_area();
                                                    });
            result.expansion_area = std::make_unique<Area>(*largest_soft_it);
            result.expansion_area->set_name(item.identifier + "_coarse_added");
        }

        result.perimeter_samples = std::clamp(static_cast<int>(std::lround(perimeter / std::max(1.0, expansion_radius))),
                                              1,
                                              4096);
        result.expanded_paths = static_cast<int>(result.expanded_areas.size());
        result.soft_boundary_paths = static_cast<int>(result.soft_boundary_areas.size());
        result.uncovered_perimeter_segments = 0;
        result.expanded_area_size = paths_area_abs(active_paths);
        result.soft_boundary_area_size = paths_area_abs(expansion_paths);
        vibble::log::debug("[Coarseness] '" + item.identifier + "' geometry area before=" + std::to_string(before_area) +
                           " after=" + std::to_string(after_area) +
                           " expanded_paths=" + std::to_string(result.expanded_paths) +
                           " soft_boundary_paths=" + std::to_string(result.soft_boundary_paths) +
                           " soft_boundary_area=" + std::to_string(result.soft_boundary_area_size));
        vibble::log::debug("[Coarseness] '" + item.identifier + "' validation perimeter_length_processed=" +
                           std::to_string(result.perimeter_length_processed) +
                           " perimeter_samples=" + std::to_string(result.perimeter_samples) +
                           " expansion_radius=" + std::to_string(expansion_radius) +
                           " expanded_area_size=" + std::to_string(result.expanded_area_size) +
                           " final_expanded_bounds=" + bounds_to_string(item.geometry->get_bounds()));

        results.push_back(std::move(result));
    }

    return results;
#endif
}

void apply_coarseness_expansion(std::vector<Room*>& rooms, std::uint64_t deterministic_seed) {
    std::vector<CoarsenessGeometryItem> items;
    std::vector<Room*> item_rooms;
    items.reserve(rooms.size());
    item_rooms.reserve(rooms.size());

    std::uint64_t base_seed = deterministic_seed == 0 ? kDefaultDeterministicSeed : deterministic_seed;
    for (std::size_t i = 0; i < rooms.size(); ++i) {
        Room* room = rooms[i];
        if (!room) continue;
        const std::uint64_t item_seed =
            mix_u64(mix_u64(base_seed, static_cast<std::uint64_t>(i)),
                    static_cast<std::uint64_t>(std::hash<std::string>{}(room->room_name)));
        items.push_back(CoarsenessGeometryItem{
            room->room_name,
            room->room_area.get(),
            read_coarseness_range(room),
            room->map_grid_settings(),
            item_seed,
        });
        item_rooms.push_back(room);
    }

    std::vector<CoarsenessExpansionResult> results = apply_coarseness_pass(items);
    const std::size_t count = std::min(results.size(), item_rooms.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (!item_rooms[i]) continue;
        Room* room = item_rooms[i];
        if (results[i].base_area) {
            room->base_room_area = std::move(results[i].base_area);
        } else if (!room->base_room_area && room->room_area) {
            room->base_room_area = std::make_unique<Area>(*room->room_area);
        }
        room->coarseness_expanded_area = std::move(results[i].expanded_area);
        room->coarseness_added_area = std::move(results[i].expansion_area);
        room->coarseness_expanded_areas = std::move(results[i].expanded_areas);
        room->coarseness_soft_boundary_areas = std::move(results[i].soft_boundary_areas);
    }
}

} // namespace vibble::mapgen::coarseness
