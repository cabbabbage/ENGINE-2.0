#include "spawn_method.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <vector>

#include <SDL3/SDL.h>

#include "spawn_context.hpp"
#include "spawn_info.hpp"
#include "check.hpp"
#include "asset_spawn_planner.hpp"
#include "spawn_group_codec.hpp"
#include "assets/asset/asset_info.hpp"
#include "utils/area.hpp"
#include "utils/grid.hpp"
#include "utils/relative_room_position.hpp"

namespace {

enum class AttemptResult {
    Spawned,
    InvalidCandidate,
    PositionRejected,
    CheckRejected,
    SpawnFailed
};

struct AttemptOptions {
    bool respect_exclusion_zones = false;
    bool treat_as_edge_asset = false;
    bool treat_as_map_asset = false;
    bool track_spacing = true;
    std::function<void()> on_success{};
};

AttemptResult attempt_spawn(const SpawnInfo& item,
                            const Area& area,
                            SpawnContext& ctx,
                            SDL_Point pos,
                            const AttemptOptions& opts) {
    if (!ctx.position_allowed(area, pos)) {
        return AttemptResult::PositionRejected;
    }

    const SpawnCandidate* candidate = item.select_candidate(ctx.rng());
    if (!candidate || candidate->is_null || !candidate->info) {
        return AttemptResult::InvalidCandidate;
    }

    const bool enforce_spacing = item.check_min_spacing;
    const auto gp = ctx.to_grid_point(pos);
    if (ctx.checks_enabled() &&
        ctx.checker().check(candidate->info,
                            gp,
                            ctx.exclusion_zones(),
                            ctx.all_assets(),
                            opts.respect_exclusion_zones,
                            enforce_spacing,
                            opts.treat_as_edge_asset,
                            opts.treat_as_map_asset,
                            5)) {
        return AttemptResult::CheckRejected;
    }

    auto* result = ctx.spawnAsset(candidate->name, candidate->info, area, pos, 0, item.spawn_id, item.position);
    if (!result) {
        return AttemptResult::SpawnFailed;
    }

    if (ctx.checks_enabled()) {
        const bool track_spacing = opts.track_spacing && ctx.track_spacing_for(result->info, enforce_spacing);
        ctx.checker().register_asset(result, enforce_spacing, track_spacing);
    }

    if (opts.on_success) {
        opts.on_success();
    }

    return AttemptResult::Spawned;
}

struct Edge {
    SDL_FPoint start{0.0f, 0.0f};
    SDL_FPoint delta{0.0f, 0.0f};
    double length = 0.0;
};

std::vector<Edge> build_edges(const Area& area, double& total_length) {
    std::vector<Edge> edges;
    total_length = 0.0;
    const auto& pts = area.get_points();
    const size_t n = pts.size();
    if (n < 2) {
        return edges;
    }
    edges.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const SDL_Point& a = pts[i];
        const SDL_Point& b = pts[(i + 1) % n];
        const double dx = static_cast<double>(b.x - a.x);
        const double dy = static_cast<double>(b.y - a.y);
        const double len = std::hypot(dx, dy);
        if (len <= 1e-6) {
            continue;
        }
        Edge edge;
        edge.start = SDL_FPoint{static_cast<float>(a.x), static_cast<float>(a.y)};
        edge.delta = SDL_FPoint{static_cast<float>(dx), static_cast<float>(dy)};
        edge.length = len;
        total_length += len;
        edges.push_back(edge);
    }
    return edges;
}

bool point_along_edges(const std::vector<Edge>& edges,
                       double perimeter,
                       double distance,
                       SDL_FPoint& out_point) {
    if (edges.empty() || perimeter <= 0.0) {
        return false;
    }
    double wrapped = std::fmod(distance, perimeter);
    if (wrapped < 0.0) {
        wrapped += perimeter;
    }
    for (const auto& edge : edges) {
        if (edge.length <= 0.0) {
            continue;
        }
        if (wrapped <= edge.length || std::fabs(wrapped - edge.length) < 1e-6) {
            const double t = std::clamp(wrapped / edge.length, 0.0, 1.0);
            out_point.x = edge.start.x + static_cast<float>(edge.delta.x * t);
            out_point.y = edge.start.y + static_cast<float>(edge.delta.y * t);
            return true;
        }
        wrapped -= edge.length;
    }
    const Edge& last = edges.back();
    out_point.x = last.start.x + last.delta.x;
    out_point.y = last.start.y + last.delta.y;
    return true;
}

SDL_Point apply_inset(SDL_Point center, const SDL_FPoint& edge_point, int inset_percent) {
    const double scale = std::clamp(static_cast<double>(inset_percent) / 100.0, 0.0, 2.0);
    const double vx = static_cast<double>(edge_point.x) - static_cast<double>(center.x);
    const double vy = static_cast<double>(edge_point.y) - static_cast<double>(center.y);
    const double target_x = static_cast<double>(center.x) + vx * scale;
    const double target_y = static_cast<double>(center.y) + vy * scale;
    return SDL_Point{static_cast<int>(std::lround(target_x)), static_cast<int>(std::lround(target_y))};
}

std::vector<SDL_Point> plan_edge_positions(const SpawnInfo& item,
                                           const Area& area,
                                           std::mt19937& rng,
                                           vibble::grid::Grid& grid,
                                           int resolution,
                                           SDL_Point center,
                                           const std::function<bool(SDL_Point)>& overlaps_trail) {
    std::vector<SDL_Point> results;
    if (item.quantity <= 0) {
        return results;
    }

    double perimeter = 0.0;
    std::vector<Edge> edges = build_edges(area, perimeter);
    if (edges.empty() || perimeter <= 0.0) {
        return results;
    }

    const double step = perimeter / static_cast<double>(item.quantity);
    double start_offset = 0.0;
    if (step > 0.0) {
        std::uniform_real_distribution<double> dist(0.0, step);
        start_offset = dist(rng);
    }

    for (int i = 0; i < item.quantity; ++i) {
        const double distance = start_offset + step * static_cast<double>(i);
        SDL_FPoint edge_point{0.0f, 0.0f};
        if (!point_along_edges(edges, perimeter, distance, edge_point)) {
            continue;
        }

        SDL_Point spawn_point = apply_inset(center, edge_point, item.edge_inset_percent);
        if (resolution > 0) {
            spawn_point = grid.snap_to_vertex(spawn_point, resolution);
        }

        if (overlaps_trail && overlaps_trail(spawn_point)) {
            continue;
        }

        results.push_back(spawn_point);
    }

    return results;
}

void spawn_center(const SpawnInfo& item, const Area* area, SpawnContext& ctx) {
    if (!area || !item.has_candidates() || item.quantity <= 0) {
        return;
    }

    SDL_Point center = ctx.get_area_center(*area);
    if (auto* occupancy = ctx.occupancy()) {
        if (auto* vertex = occupancy->nearest_vertex(center)) {
            center = vertex->world;
        }
    }

    for (int attempt = 0; attempt < item.quantity; ++attempt) {
        attempt_spawn(item, *area, ctx, center, AttemptOptions{});
    }
}

void spawn_exact(const SpawnInfo& item, const Area* area, SpawnContext& ctx) {
    if (!area || !item.has_candidates() || item.quantity <= 0) {
        return;
    }

    auto [minx, miny, maxx, maxy] = area->get_bounds();
    const int curr_w = std::max(1, maxx - minx);
    const int curr_h = std::max(1, maxy - miny);

    SDL_Point center = ctx.get_area_center(*area);
    RelativeRoomPosition relative(item.exact_offset, item.exact_origin_w, item.exact_origin_h);
    SDL_Point final_pos = relative.resolve(center, curr_w, curr_h);

    vibble::grid::Occupancy::Vertex* snapped = nullptr;
    if (auto* occupancy = ctx.occupancy()) {
        snapped = occupancy->nearest_vertex(final_pos);
        if (snapped) {
            final_pos = snapped->world;
        }
    }

    AttemptOptions opts{};
    if (snapped && ctx.occupancy()) {
        opts.on_success = [&ctx, snapped]() { ctx.occupancy()->set_occupied(snapped, true); };
    }

    for (int attempt = 0; attempt < item.quantity; ++attempt) {
        attempt_spawn(item, *area, ctx, final_pos, opts);
    }
}

void spawn_perimeter(const SpawnInfo& item, const Area* area, SpawnContext& ctx) {
    if (!area || item.quantity <= 0 || !item.has_candidates()) {
        return;
    }

    const int R = item.perimeter_radius;
    if (R <= 0) {
        return;
    }

    auto [minx, miny, maxx, maxy] = area->get_bounds();
    const int curr_w = std::max(1, maxx - minx);
    const int curr_h = std::max(1, maxy - miny);

    SDL_Point room_center = ctx.get_area_center(*area);
    RelativeRoomPosition relative(item.exact_offset, item.exact_origin_w, item.exact_origin_h);
    SDL_Point circle_center = relative.resolve(room_center, curr_w, curr_h);

    std::uniform_real_distribution<double> phase_dist(0.0, 2.0 * SDL_PI_D);
    const double start = phase_dist(ctx.rng());
    const double step  = (item.quantity > 0) ? (2.0 * SDL_PI_D / static_cast<double>(item.quantity)) : 0.0;

    for (int i = 0; i < item.quantity; ++i) {
        const double angle = start + step * static_cast<double>(i);
        const int x = circle_center.x + static_cast<int>(std::lround(R * std::cos(angle)));
        const int y = circle_center.y + static_cast<int>(std::lround(R * std::sin(angle)));

        SDL_Point pos{x, y};
        attempt_spawn(item, *area, ctx, pos, AttemptOptions{});
    }
}

void spawn_percent(const SpawnInfo& item, const Area* area, SpawnContext& ctx) {
    if (!area || item.quantity <= 0 || !item.has_candidates()) {
        return;
    }

    auto [minx, miny, maxx, maxy] = area->get_bounds();
    const int w = std::max(1, maxx - minx);
    const int h = std::max(1, maxy - miny);

    SDL_Point center = ctx.get_area_center(*area);

    int attempts = 0;
    int slots_used = 0;
    const int target_attempts = item.quantity;
    const int max_attempts = std::max(1, target_attempts * 20);

    constexpr int kDefaultMin = -100;
    constexpr int kDefaultMax = 100;

    std::uniform_int_distribution<int> dist_x(kDefaultMin, kDefaultMax);
    std::uniform_int_distribution<int> dist_y(kDefaultMin, kDefaultMax);

    while (slots_used < target_attempts && attempts < max_attempts) {
        ++attempts;

        const int px = dist_x(ctx.rng());
        const int py = dist_y(ctx.rng());

        const double offset_x = (px / 100.0) * (w / 2.0);
        const double offset_y = (py / 100.0) * (h / 2.0);

        SDL_Point final_pos{
            center.x + static_cast<int>(std::lround(offset_x)), center.y + static_cast<int>(std::lround(offset_y)) };

        vibble::grid::Occupancy::Vertex* snapped = ctx.occupancy() ? ctx.occupancy()->nearest_vertex(final_pos) : nullptr;
        if (snapped) {
            final_pos = snapped->world;
        }

        AttemptOptions opts{};
        opts.respect_exclusion_zones = true;
        if (snapped && ctx.occupancy()) {
            opts.on_success = [&ctx, snapped]() { ctx.occupancy()->set_occupied(snapped, true); };
        }

        const AttemptResult result = attempt_spawn(item, *area, ctx, final_pos, opts);
        if (result == AttemptResult::Spawned ||
            result == AttemptResult::InvalidCandidate ||
            result == AttemptResult::SpawnFailed) {
            ++slots_used;
        }
    }
}

void spawn_random(const SpawnInfo& item, const Area* area, SpawnContext& ctx) {
    if (!area || !item.has_candidates() || item.quantity <= 0) {
        return;
    }

    int attempt_slots_used = 0;
    int attempts = 0;
    const int desired_attempts = item.quantity;
    const int max_attempts = std::max(1, desired_attempts * 20);

    auto* occupancy = ctx.occupancy();
    const Area* sample_area = ctx.clip_area();
    if (!sample_area) {
        sample_area = area;
    }
    const Area* spawn_area = sample_area ? sample_area : area;

    if (!spawn_area || spawn_area->get_points().empty()) {
        return;
    }

    while (attempt_slots_used < desired_attempts && attempts < max_attempts) {
        ++attempts;

        vibble::grid::Occupancy::Vertex* vertex = occupancy ? occupancy->random_vertex_in_area(*spawn_area, ctx.rng()) : nullptr;
        if (!vertex) {
            break;
        }

        SDL_Point pos = vertex->world;
        if (spawn_area && !spawn_area->get_points().empty()) {
            pos = ctx.get_point_within_area(*spawn_area);
        }

        AttemptOptions opts{};
        opts.respect_exclusion_zones = true;
        if (occupancy) {
            opts.on_success = [occupancy, vertex]() { occupancy->set_occupied(vertex, true); };
        }

        const AttemptResult result = attempt_spawn(item, *spawn_area, ctx, pos, opts);
        if (result == AttemptResult::Spawned ||
            result == AttemptResult::InvalidCandidate ||
            result == AttemptResult::SpawnFailed) {
            ++attempt_slots_used;
        }
    }
}

void spawn_edge(const SpawnInfo& item, const Area* area, SpawnContext& ctx) {
    const Area* target_area = ctx.clip_area() ? ctx.clip_area() : area;
    if (!target_area || !item.has_candidates() || item.quantity <= 0) {
        return;
    }

    const int resolution = ctx.spawn_resolution();
    auto positions = plan_edge_positions(item,
                                         *target_area,
                                         ctx.rng(),
                                         ctx.grid(),
                                         resolution,
                                         ctx.get_area_center(*target_area),
                                         [&](SDL_Point pt) { return ctx.point_overlaps_trail(pt, target_area); });

    AttemptOptions opts{};
    opts.treat_as_edge_asset = true;
    opts.track_spacing = false;

    for (SDL_Point spawn_point : positions) {
        attempt_spawn(item, *target_area, ctx, spawn_point, opts);
    }
}

} // namespace

void SpawnMethod::spawn(const SpawnInfo& item, const Area* area, SpawnContext& ctx) const {
    const std::string method = vibble::spawn_group_codec::normalize_method(item.position);
    if (method == "Exact") {
        spawn_exact(item, area, ctx);
    } else if (method == "Center") {
        spawn_center(item, area, ctx);
    } else if (method == "Perimeter") {
        spawn_perimeter(item, area, ctx);
    } else if (method == "Edge") {
        spawn_edge(item, area, ctx);
    } else if (method == "Percent") {
        spawn_percent(item, area, ctx);
    } else {
        spawn_random(item, area, ctx);
    }
}
