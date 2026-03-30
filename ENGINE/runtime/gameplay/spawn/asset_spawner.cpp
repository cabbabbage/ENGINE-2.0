#include "asset_spawner.hpp"
#include "asset_spawn_planner.hpp"
#include "spacing_util.hpp"
#include "spawn_context.hpp"
#include "methods/spawn_method.hpp"
#include "check.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "utils/grid.hpp"
#include "utils/grid_occupancy.hpp"

namespace {

constexpr std::uint64_t kGoldenRatio = 0x9e3779b97f4a7c15ULL;

std::uint64_t mix_value(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + kGoldenRatio + (seed << 6) + (seed >> 2);
    return seed;
}

std::uint64_t seed_for_cell(const std::string& map_seed, SDL_Point index) {
    std::uint64_t seed = std::hash<std::string>{}(map_seed);
    seed = mix_value(seed, static_cast<std::uint64_t>(static_cast<std::int64_t>(index.x)));
    seed = mix_value(seed, static_cast<std::uint64_t>(static_cast<std::int64_t>(index.y)));
    return seed;
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

SDL_Point asset_plane_point(const Asset* asset) {
    if (!asset) {
        return SDL_Point{0, 0};
    }
    return SDL_Point{asset->world_x(), asset->world_z()};
}

Room* resolve_owner(SDL_Point world_point, const std::vector<std::unique_ptr<Room>>& rooms) {
    Room* fallback = nullptr;
    for (const auto& room_ptr : rooms) {
        Room* room = room_ptr.get();
        if (!room || !room->room_area) {
            continue;
        }
        if (!room->room_area->contains_point(world_point)) {
            continue;
        }
        if (room->inherits_map_assets()) {
            return room;
        }
        if (!fallback) {
            fallback = room;
        }
    }
    return fallback;
}

}

AssetSpawner::AssetSpawner(AssetLibrary* asset_library,
                           std::vector<Area> exclusion_zones)
: asset_library_(asset_library),
exclusion_zones(std::move(exclusion_zones)),
rng_(std::random_device{}()),
checker_(false) {}

void AssetSpawner::spawn(Room& room) {
	if (!room.planner) {
		std::cerr << "[AssetSpawner] Room planner is null — skipping room: " << room.room_name << "\n";
		return;
	}
	const Area& spawn_area = *room.room_area;
        current_room_ = &room;
        map_grid_settings_ = room.map_grid_settings();
        run_spawning(room.planner.get(), spawn_area);

        try {
                nlohmann::json& root = room.assets_data();

                std::unordered_map<std::string, int> area_selection_counts;
                if (room.planner) {
                        const auto& queue = room.planner->get_spawn_queue();
                        for (const auto& item : queue) {

                                std::vector<std::string> names;
                                std::vector<double> weights;
                                const auto& entries = item.candidates.entries();
                                for (const auto& cand : entries) {
                                        if (cand.kind != vibble::spawn::CandidateKind::Asset ||
                                            cand.is_null ||
                                            cand.key.empty()) {
                                                continue;
                                        }
                                        if (room.find_area(cand.key) != nullptr) {
                                                names.push_back(cand.key);
                                                double w = cand.weight;
                                                if (w < 0.0) w = 0.0;
                                                weights.push_back(w);
                                        }
                                }
                                if (names.empty()) continue;

                                bool any_positive = false;
                                for (double w : weights) if (w > 0.0) { any_positive = true; break; }
                                if (!any_positive) {
                                        std::fill(weights.begin(), weights.end(), 1.0);
                                }
                                std::discrete_distribution<size_t> chooser(weights.begin(), weights.end());
                                for (int i = 0; i < std::max(0, item.quantity); ++i) {
                                        size_t idx = chooser(rng_);
                                        if (idx < names.size()) {
                                                area_selection_counts[names[idx]] += 1;
                                        }
                                }
                        }
                }

                if (root.is_object() && root.contains("areas") && root["areas"].is_array()) {

                        bool selective = !area_selection_counts.empty();
                        for (auto& area_entry : root["areas"]) {
                                if (!area_entry.is_object()) continue;
                                auto it = area_entry.find("spawn_groups");
                                if (it == area_entry.end() || !it->is_array() || it->empty()) continue;
                                std::string area_name = area_entry.value("name", std::string{});
                                if (area_name.empty()) continue;
                                Area* area_ptr = room.find_area(area_name);
                                if (!area_ptr) continue;

                                int times = 1;
                                if (selective) {
                                        auto ct = area_selection_counts.find(area_name);
                                        if (ct == area_selection_counts.end() || ct->second <= 0) {
                                                continue;
                                        }
                                        times = ct->second;
                                }

                                for (int pass = 0; pass < times; ++pass) {
                                        std::vector<nlohmann::json> sources;
                                        sources.push_back(nlohmann::json::object());
                                        sources.back()["spawn_groups"] = *it;
                                        std::vector<AssetSpawnPlanner::SourceContext> contexts;
                                        contexts.resize(1);
                                        contexts[0].json_ref = &sources.back();
                                        contexts[0].persist = [&area_entry](const nlohmann::json& src){
                                                if (src.is_object() && src.contains("spawn_groups") && src["spawn_groups"].is_array()) {
                                                        area_entry["spawn_groups"] = src["spawn_groups"];
                                                }
};

                                        AssetSpawnPlanner area_planner(sources, *area_ptr, *asset_library_, contexts);
                                        run_spawning(&area_planner, *area_ptr);
                                }
                        }
                }
        } catch (...) {

        }

        current_room_ = nullptr;
        room.add_room_assets(std::move(all_));
}

void AssetSpawner::spawn_map_wide(std::vector<std::unique_ptr<Room>>& rooms,
                                  nlohmann::json& map_assets_json,
                                  const std::string& map_seed) {
        if (!asset_library_ || rooms.empty()) {
                return;
        }
        if (!map_assets_json.is_object()) {
                map_assets_json = nlohmann::json::object();
        }
        auto groups_it = map_assets_json.find("spawn_groups");
        if (groups_it == map_assets_json.end() || !groups_it->is_array() || groups_it->empty()) {
                return;
        }

        int min_x = std::numeric_limits<int>::max();
        int min_z = std::numeric_limits<int>::max();
        int max_x = std::numeric_limits<int>::min();
        int max_z = std::numeric_limits<int>::min();
        bool have_area = false;
        for (const auto& room_ptr : rooms) {
                if (!room_ptr || !room_ptr->room_area) {
                        continue;
                }
                auto [rminx, rminz, rmaxx, rmaxz] = room_ptr->room_area->get_bounds();
                min_x = std::min(min_x, rminx);
                min_z = std::min(min_z, rminz);
                max_x = std::max(max_x, rmaxx);
                max_z = std::max(max_z, rmaxz);
                have_area = true;
        }
        if (!have_area || min_x >= max_x || min_z >= max_z) {
                return;
        }

        std::vector<SDL_Point> polygon{
            SDL_Point{min_x, min_z},
            SDL_Point{max_x, min_z},
            SDL_Point{max_x, max_z},
            SDL_Point{min_x, max_z},
        };
        Area sweep_area("map_wide_sweep", polygon);
        sweep_area.set_type("map_wide");

        std::vector<nlohmann::json> sources{map_assets_json};
        AssetSpawnPlanner::SourceContext source_context;
        source_context.json_ref = &map_assets_json;
        source_context.persist = [&map_assets_json](const nlohmann::json& updated) {
                map_assets_json = updated;
        };
        AssetSpawnPlanner planner(sources, sweep_area, *asset_library_,
                                  std::vector<AssetSpawnPlanner::SourceContext>{source_context});
        const auto& queue = planner.get_spawn_queue();
        if (queue.empty()) {
                return;
        }

        const SpawnInfo* spawn_info = nullptr;
        for (const auto& info : queue) {
                if (!info.has_candidates()) {
                        continue;
                }
                if (info.uses_batch_grid()) {
                        spawn_info = &info;
                        break;
                }
        }
        if (!spawn_info) {
                for (const auto& info : queue) {
                        if (info.has_candidates()) {
                                spawn_info = &info;
                                break;
                        }
                }
        }
        if (!spawn_info) {
                return;
        }

        std::unordered_set<std::string> spacing_names;
        if (spawn_info->check_min_spacing) {
                for (const auto& cand : spawn_info->candidates) {
                        if (!cand.info || cand.info->name.empty()) {
                                continue;
                        }
                        spacing_names.insert(cand.info->name);
                }
        }

        std::size_t total_existing = 0;
        for (const auto& room_ptr : rooms) {
                if (!room_ptr) {
                        continue;
                }
                total_existing += room_ptr->assets.size();
        }

        std::vector<std::unique_ptr<Asset>> global_assets;
        global_assets.reserve(total_existing);
        std::unordered_map<Asset*, Room*> owner_map;
        owner_map.reserve(total_existing);

        for (auto& room_ptr : rooms) {
                if (!room_ptr) {
                        continue;
                }
                auto& room_assets = room_ptr->assets;
                for (auto& asset_uptr : room_assets) {
                        if (Asset* raw = asset_uptr.get()) {
                                owner_map[raw] = room_ptr.get();
                        }
                        global_assets.push_back(std::move(asset_uptr));
                }
                room_assets.clear();
        }

        int resolution = spawn_info->grid_resolution > 0
                ? spawn_info->grid_resolution
                : std::max(0, map_grid_settings_.grid_resolution);
        resolution = vibble::grid::clamp_resolution(resolution);

        vibble::grid::Grid& grid_service = vibble::grid::global_grid();
        vibble::grid::Occupancy occupancy(sweep_area, resolution, grid_service);
        for (const auto& asset_uptr : global_assets) {
                if (!asset_uptr) {
                        continue;
                }
                occupancy.set_occupied_at(asset_plane_point(asset_uptr.get()), true);
        }

        std::vector<const Area*> trail_areas;
        trail_areas.reserve(rooms.size());
        for (const auto& room_ptr : rooms) {
                if (!room_ptr || !room_ptr->room_area) {
                        continue;
                }
                if (to_lower_copy(room_ptr->type) == "trail") {
                        trail_areas.push_back(room_ptr->room_area.get());
                }
        }

        std::unordered_map<vibble::grid::Occupancy::Vertex*, Room*> ownership;
        ownership.reserve(1024);
        for (const auto& room_ptr : rooms) {
                Room* room = room_ptr.get();
                if (!room || !room->room_area) {
                        continue;
                }
                auto verts = occupancy.vertices_in_area(*room->room_area);
                ownership.reserve(ownership.size() + verts.size());
                for (auto* vertex : verts) {
                        if (!vertex) {
                                continue;
                        }
                        auto [it, inserted] = ownership.emplace(vertex, room);
                        if (!inserted) {
                                Room* current_owner = it->second;
                                if ((!current_owner || !current_owner->inherits_map_assets()) &&
                                    room->inherits_map_assets()) {
                                        it->second = room;
                                }
                        }
                }
        }

        struct Cell {
                vibble::grid::Occupancy::Vertex* vertex = nullptr;
                Room* owner = nullptr;
        };
        std::vector<Cell> cells;
        cells.reserve(ownership.size());
        for (auto& entry : ownership) {
                cells.push_back(Cell{entry.first, entry.second});
        }

        if (!cells.empty()) {
                std::sort(cells.begin(), cells.end(), [](const Cell& lhs, const Cell& rhs) {
                        if (lhs.vertex->index.y != rhs.vertex->index.y) {
                                return lhs.vertex->index.y < rhs.vertex->index.y;
                        }
                        return lhs.vertex->index.x < rhs.vertex->index.x;
                });

                Check checker(false);
                checker.begin_session(grid_service, resolution);
                std::vector<Area> local_exclusion_zones;
                auto asset_info_library = asset_library_->all();
                std::mt19937 local_rng;
                SpawnContext context(local_rng, checker, local_exclusion_zones, asset_info_library,
                                     global_assets, asset_library_, grid_service, &occupancy);
                context.set_map_grid_settings(map_grid_settings_);
                context.set_spawn_resolution(resolution);
                context.set_trail_areas(trail_areas);
                context.set_spacing_filter(std::move(spacing_names));

                for (const auto& cell : cells) {
                        auto* vertex = cell.vertex;
                        Room* owner = cell.owner;
                        if (!vertex || !owner || !owner->room_area) {
                                continue;
                        }
                        if (vertex->occupied) {
                                continue;
                        }
                        if (!owner->inherits_map_assets()) {
                                occupancy.set_occupied(vertex, true);
                                continue;
                        }

                        const auto& candidates = spawn_info->candidates;
                        if (candidates.empty()) {
                                occupancy.set_occupied(vertex, true);
                                continue;
                        }

                        const std::uint64_t seed_value = seed_for_cell(map_seed, vertex->index);
                        const std::uint32_t low = static_cast<std::uint32_t>(seed_value & 0xFFFFFFFFULL);
                        const std::uint32_t high = static_cast<std::uint32_t>((seed_value >> 32) & 0xFFFFFFFFULL);
                        std::seed_seq seq{low, high};
                        local_rng.seed(seq);

                        const SpawnCandidate* candidate = spawn_info->select_candidate(local_rng);
                        if (!candidate || candidate->is_null || !candidate->info) {
                                occupancy.set_occupied(vertex, true);
                                continue;
                        }

                        SDL_Point spawn_pos = vertex->world;
                        context.set_clip_area(owner->room_area.get());
                        const bool enforce_spacing = spawn_info->check_min_spacing;
                        const auto spawn_gp = context.to_grid_point(spawn_pos);
                        if (context.checker().check(candidate->info,
                                                    spawn_gp,
                                                    context.exclusion_zones(),
                                                    context.all_assets(),
                                                    true,
                                                    enforce_spacing,
                                                    false,
                                                    true,
                                                    5)) {
                                occupancy.set_occupied(vertex, true);
                                continue;
                        }

                        Asset* spawned = context.spawnAsset(candidate->name,
                                                            candidate->info,
                                                            *owner->room_area,
                                                            spawn_pos,
                                                            0,
                                                            spawn_info->spawn_id,
                                                            "MapWide");
                        if (spawned) {
                                spawned->set_owning_room_name(owner->room_name);
                                owner_map[spawned] = owner;
                                context.checker().register_asset(spawned, enforce_spacing, false);
                        }
                        occupancy.set_occupied(vertex, true);
                }

                checker.reset_session();
        }

        for (auto& asset_uptr : global_assets) {
                if (!asset_uptr) {
                        continue;
                }
                Room* owner = nullptr;
                auto it = owner_map.find(asset_uptr.get());
                if (it != owner_map.end()) {
                        owner = it->second;
                }
                if (!owner) {
                        owner = resolve_owner(asset_plane_point(asset_uptr.get()), rooms);
                }
                if (!owner) {
                        continue;
                }
                if (asset_uptr->owning_room_name().empty()) {
                        asset_uptr->set_owning_room_name(owner->room_name);
                }
                owner->assets.push_back(std::move(asset_uptr));
        }
}

std::vector<std::unique_ptr<Asset>> AssetSpawner::spawn_boundary_from_json(const nlohmann::json& boundary_json,
                                                                          const Area& spawn_area,
                                                                          const std::string& source_name) {
        if (boundary_json.is_null() || !boundary_json.is_object()) {
                return {};
        }
        const auto selectors_it = boundary_json.find("candidate_selectors");
        if (selectors_it == boundary_json.end() || !selectors_it->is_array() || selectors_it->empty()) {
                return {};
        }

        nlohmann::json source = nlohmann::json::object();
        source["spawn_groups"] = *selectors_it;
        std::vector<nlohmann::json> json_sources{ source };

        group_resolution_map_.clear();
        try {
                for (const auto& entry : *selectors_it) {
                        if (!entry.is_object()) continue;
                        const std::string sid = entry.value("spawn_id", std::string{});
                        if (sid.empty()) continue;
                        int r = entry.value("grid_resolution", 5);
                        r = vibble::grid::clamp_resolution(r);
                        group_resolution_map_.insert_or_assign(sid, r);
                }
        } catch (...) {

        }
        AssetSpawnPlanner planner(json_sources, spawn_area, *asset_library_);
        boundary_mode_ = true;
        run_spawning(&planner, spawn_area);
        boundary_mode_ = false;
        return extract_all_assets();
}

std::vector<std::unique_ptr<Asset>> AssetSpawner::extract_all_assets() {
	return std::move(all_);
}

void AssetSpawner::run_spawning(AssetSpawnPlanner* planner, const Area& area) {
        asset_info_library_ = asset_library_->all();
        spawn_queue_ = planner->get_spawn_queue();
        if (boundary_mode_) {
                run_edge_spawning(area);
                return;
        }
        auto spacing_names = collect_spacing_asset_names(spawn_queue_);
    const int resolution = std::max(0, map_grid_settings_.grid_resolution);
    vibble::grid::Grid& grid_service = vibble::grid::global_grid();
    checker_.begin_session(grid_service, resolution);
    vibble::grid::Occupancy occupancy(area, resolution, grid_service);
    SpawnContext ctx(rng_, checker_, exclusion_zones, asset_info_library_, all_, asset_library_, grid_service, &occupancy);
    ctx.set_spacing_filter(std::move(spacing_names));
    ctx.set_map_grid_settings(map_grid_settings_);
    ctx.set_spawn_resolution(resolution);
        std::vector<const Area*> trail_areas;
        auto add_trail_area = [&trail_areas](const Area* candidate, const std::string& type) {
                if (!candidate) {
                        return;
                }
                std::string lowered = type;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                });
                if (lowered == "trail") {
                        trail_areas.push_back(candidate);
                }
};
        if (current_room_) {
                if (current_room_->room_area) {
                        add_trail_area(current_room_->room_area.get(), current_room_->room_area->get_type());
                }
                for (const auto& named : current_room_->areas) {
                        add_trail_area(named.area.get(), named.type);
                }
        }
        ctx.set_trail_areas(std::move(trail_areas));
        SpawnMethod method;
        for (auto& queue_item : spawn_queue_) {
                if (!queue_item.has_candidates()) continue;
                if (current_room_) {
                        bool has_area = false;
                        bool has_asset = false;
                        for (const auto& c : queue_item.candidates) {
                                if (c.info) { has_asset = true; break; }
                                if (!c.name.empty() && current_room_->find_area(c.name) != nullptr) {
                                        has_area = true;
                                }
                        }
                        if (has_area && !has_asset) {
                                continue;
                        }
                }

                if (current_room_) {
                        bool has_area = false;
                        bool has_asset = false;
                        for (const auto& c : queue_item.candidates) {
                                if (c.info) { has_asset = true; break; }
                                if (!c.name.empty() && current_room_->find_area(c.name) != nullptr) {
                                        has_area = true;
                                }
                        }
                        if (has_area && !has_asset) {
                                continue;
                        }
                }
                if (current_room_ && !queue_item.link_area_name.empty()) {
                        Area* link = current_room_->find_area(queue_item.link_area_name);
                        ctx.set_clip_area(link);
                } else {
                        ctx.set_clip_area(nullptr);
                }

                if (queue_item.uses_batch_grid()) {

                        int batch_resolution = queue_item.grid_resolution > 0 ? queue_item.grid_resolution : resolution;
                        Check batch_checker(false);
                        batch_checker.begin_session(grid_service, batch_resolution);
                        vibble::grid::Occupancy batch_occupancy(area, batch_resolution, grid_service);
                        SpawnContext batch_ctx(rng_, batch_checker, exclusion_zones, asset_info_library_, all_, asset_library_, grid_service, &batch_occupancy);

                        std::vector<double> base_weights;
                        base_weights.reserve(queue_item.candidates.size());
                        double total_weight = 0.0;
                        for (const auto& cand : queue_item.candidates) {
                                double weight = cand.weight;
                                if (weight < 0.0) weight = 0.0;
                                if (weight > 0.0) total_weight += weight;
                                base_weights.push_back(weight);
                        }
                        if (total_weight <= 0.0 && !base_weights.empty()) {
                                std::fill(base_weights.begin(), base_weights.end(), 1.0);
                        }

                        auto vertices = batch_occupancy.vertices_in_area(area);
                        if (vertices.empty()) {
                                batch_checker.reset_session();
                                continue;
                        }
                        std::shuffle(vertices.begin(), vertices.end(), batch_ctx.rng());

                        for (auto* vertex : vertices) {
                                if (!vertex) continue;
                                SDL_Point spawn_pos{ vertex->world.x, vertex->world.y };
                                bool placed = false;
                                std::vector<double> attempt_weights = base_weights;
                                const size_t max_candidate_attempts = queue_item.candidates.size();
                                const bool enforce_spacing = queue_item.check_min_spacing;
                                for (size_t attempt = 0; attempt < max_candidate_attempts; ++attempt) {
                                        double total_weight = std::accumulate(attempt_weights.begin(), attempt_weights.end(), 0.0);
                                        if (total_weight <= 0.0) break;
                                        std::discrete_distribution<size_t> dist(attempt_weights.begin(), attempt_weights.end());
                                        size_t idx = dist(batch_ctx.rng());
                                        if (idx >= queue_item.candidates.size()) break;
                                        if (attempt_weights[idx] <= 0.0) {
                                                attempt_weights[idx] = 0.0;
                                                continue;
                                        }
                                        const SpawnCandidate& candidate = queue_item.candidates[idx];

                                        if (candidate.is_null || !candidate.info) {
                                                batch_occupancy.set_occupied(vertex, true);
                                                placed = true;
                                                break;
                                        }
                                        const auto spawn_gp = batch_ctx.to_grid_point(spawn_pos);
                                        if (batch_ctx.checker().check(candidate.info,
                                                                spawn_gp,
                                                                batch_ctx.exclusion_zones(),
                                                                batch_ctx.all_assets(),
                                                                true,
                                                                enforce_spacing,
                                                                false,
                                                                false,
                                                                5)) {
                                                attempt_weights[idx] = 0.0;
                                                continue;
                                        }
                                        auto* result = batch_ctx.spawnAsset(candidate.name, candidate.info, area, spawn_pos, 0, queue_item.spawn_id, queue_item.position);
                                        if (!result) {
                                                attempt_weights[idx] = 0.0;
                                                continue;
                                        }
                                        const bool track_spacing = batch_ctx.track_spacing_for(result->info, enforce_spacing);
                                        batch_ctx.checker().register_asset(result, enforce_spacing, track_spacing);
                                        batch_occupancy.set_occupied(vertex, true);

                                        placed = true;
                                        break;
                                }
                                if (!placed) {
                                        batch_occupancy.set_occupied(vertex, true);
                                }
                        }
                        batch_checker.reset_session();
                        continue;
                }
                SpawnMethod method;
                method.spawn(queue_item, &area, ctx);

                if (!ctx.all_assets().empty()) {
                        Asset* last = ctx.all_assets().back().get();
                }
        }
        checker_.reset_session();
}

void AssetSpawner::run_edge_spawning(const Area& area) {
        auto point_in_exclusion = [&](const SDL_Point& pt) {
                return std::any_of(exclusion_zones.begin(), exclusion_zones.end(),
                [&](const Area& zone) { return zone.contains_point(pt); });
};

        vibble::grid::Grid& grid_service = vibble::grid::global_grid();
        auto spacing_names = collect_spacing_asset_names(spawn_queue_);
        for (auto& queue_item : spawn_queue_) {
                if (!queue_item.has_candidates()) continue;

                int edge_resolution = 5;
                auto it_res = group_resolution_map_.find(queue_item.spawn_id);
                if (it_res != group_resolution_map_.end()) {
                        edge_resolution = it_res->second;
                }
                edge_resolution = vibble::grid::clamp_resolution(edge_resolution);

                checker_.begin_session(grid_service, edge_resolution);

                vibble::grid::Occupancy occupancy(area, edge_resolution, grid_service);
                SpawnContext ctx(rng_, checker_, exclusion_zones, asset_info_library_, all_, asset_library_, grid_service, &occupancy);
                ctx.set_spacing_filter(&spacing_names);
                ctx.set_map_grid_settings(map_grid_settings_);
                ctx.set_spawn_resolution(edge_resolution);
                ctx.set_trail_areas({});

                if (current_room_ && !queue_item.link_area_name.empty()) {
                        Area* link = current_room_->find_area(queue_item.link_area_name);
                        ctx.set_clip_area(link);
                } else {
                        ctx.set_clip_area(nullptr);
                }

                std::vector<double> base_weights;
                base_weights.reserve(queue_item.candidates.size());
                double total_weight = 0.0;
                for (const auto& cand : queue_item.candidates) {
                        double weight = cand.weight;
                        if (weight < 0.0) weight = 0.0;
                        if (weight > 0.0) total_weight += weight;
                        base_weights.push_back(weight);
                }
                if (total_weight <= 0.0 && !base_weights.empty()) {
                        std::fill(base_weights.begin(), base_weights.end(), 1.0);
                }

                auto vertices = occupancy.vertices_in_area(area);
                std::vector<vibble::grid::Occupancy::Vertex*> eligible;
                eligible.reserve(vertices.size());
                for (auto* vertex : vertices) {
                        if (!vertex) continue;
                        if (point_in_exclusion(vertex->world)) continue;
                        eligible.push_back(vertex);
                }

                if (eligible.empty()) {
                        continue;
                }

                std::shuffle(eligible.begin(), eligible.end(), rng_);

                for (auto* vertex : eligible) {
                        if (!vertex) continue;
                        SDL_Point spawn_pos = vertex->world;

                        const bool enforce_spacing = queue_item.check_min_spacing;
                        const SpawnCandidate* candidate = queue_item.select_candidate(ctx.rng());

                        if (!candidate || candidate->is_null) {
                                occupancy.set_occupied(vertex, true);
                                continue;
                        }

                        const auto spawn_gp = ctx.to_grid_point(spawn_pos);
                        if (ctx.checker().check(candidate->info,
                                                spawn_gp,
                                                ctx.exclusion_zones(),
                                                ctx.all_assets(),
                                                true,
                                                enforce_spacing,
                                                true,
                                                false,
                                                5)) {
                                occupancy.set_occupied(vertex, true);
                                continue;
                        }

                        auto* result = ctx.spawnAsset(candidate->name, candidate->info, area, spawn_pos, 0, queue_item.spawn_id, queue_item.position);
                        if (result) {
                                ctx.checker().register_asset(result, enforce_spacing, false);
                        }

                        occupancy.set_occupied(vertex, true);
                }
                checker_.reset_session();
        }
}

