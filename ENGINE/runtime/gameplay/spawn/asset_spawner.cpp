#include "asset_spawner.hpp"
#include "asset_spawn_planner.hpp"
#include "spacing_util.hpp"
#include "spawn_context.hpp"
#include "methods/spawn_method.hpp"
#include "check.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "utils/grid.hpp"
#include "utils/grid_occupancy.hpp"

namespace {

int floor_box_grid_resolution_or_default(int resolution) {
    return std::clamp(vibble::grid::clamp_resolution(resolution), 2, 8);
}

std::optional<Area> make_floor_box_spawn_area(const Asset& owner, const Asset::RuntimeFloorBox& box) {
    const float half_width = std::max(0.0f, box.width * 0.5f);
    const float half_depth = std::max(0.0f, box.depth * 0.5f);
    if (half_width <= 0.0f || half_depth <= 0.0f) {
        return std::nullopt;
    }

    const float center_x = static_cast<float>(owner.world_x()) + box.position_x;
    const float center_z = static_cast<float>(owner.world_z()) + box.position_z;
    const int min_x = static_cast<int>(std::floor(center_x - half_width));
    const int max_x = static_cast<int>(std::ceil(center_x + half_width));
    const int min_z = static_cast<int>(std::floor(center_z - half_depth));
    const int max_z = static_cast<int>(std::ceil(center_z + half_depth));
    if (max_x <= min_x || max_z <= min_z) {
        return std::nullopt;
    }

    std::vector<SDL_Point> polygon{
        SDL_Point{min_x, min_z},
        SDL_Point{max_x, min_z},
        SDL_Point{max_x, max_z},
        SDL_Point{min_x, max_z},
    };
    Area area("floor_box_spawn", polygon);
    area.set_type("floor_box_spawn");
    return area;
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

        run_floor_box_candidate_spawning();

        current_room_ = nullptr;
        room.add_room_assets(std::move(all_));
}

void AssetSpawner::spawn_edge_detail_candidates(Room& room, const Area& expansion_area, const nlohmann::json& edge_detail_candidates) {
        if (!asset_library_ || !room.room_area) {
                return;
        }
        if (!edge_detail_candidates.is_object()) {
                return;
        }
        if (!edge_detail_candidates.contains("candidates") || !edge_detail_candidates["candidates"].is_array()) {
                return;
        }

        nlohmann::json group = nlohmann::json::object();
        group["display_name"] = "Edge Detail";
        group["spawn_id"] = std::string("coarse_edge_") + room.room_name;
        group["position"] = "Random";
        group["min_number"] = 0;
        group["max_number"] = std::max(1, static_cast<int>(std::lround(expansion_area.get_area() / 2500.0)));
        group["enforce_spacing"] = true;
        group["resolution"] = edge_detail_candidates.value(
            "resolution", vibble::grid::clamp_resolution(map_grid_settings_.grid_resolution));
        group["candidates"] = edge_detail_candidates["candidates"];

        nlohmann::json source = nlohmann::json::object();
        source["spawn_groups"] = nlohmann::json::array({group});
        std::vector<nlohmann::json> sources;
        sources.push_back(std::move(source));
        std::vector<AssetSpawnPlanner::SourceContext> contexts(1);
        contexts[0].json_ref = &sources[0];
        AssetSpawnPlanner planner(sources, expansion_area, *asset_library_, contexts);

        current_room_ = &room;
        map_grid_settings_ = room.map_grid_settings();
        run_spawning(&planner, expansion_area);
        current_room_ = nullptr;
        if (!all_.empty()) {
                room.add_room_assets(std::move(all_));
        }
}

std::vector<std::unique_ptr<Asset>> AssetSpawner::extract_all_assets() {
	return std::move(all_);
}

void AssetSpawner::run_spawning(AssetSpawnPlanner* planner, const Area& area) {
        asset_info_library_ = asset_library_->all();
        const vibble::spawn::RuntimeCandidates::AssetCatalogView spawn_catalog{&asset_info_library_, false};
        spawn_queue_ = planner->get_spawn_queue();
        if (boundary_mode_) {
                run_edge_spawning(area);
                return;
        }
        auto spacing_names = collect_spacing_asset_names(spawn_queue_, spawn_catalog);
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
                        for (const auto& c : queue_item.candidates.entries()) {
                                if (c.kind == vibble::spawn::CandidateKind::Tag) {
                                        has_asset = true;
                                        break;
                                }
                                if (c.is_null || c.key.empty()) {
                                        continue;
                                }
                                if (current_room_->find_area(c.key) != nullptr) {
                                        has_area = true;
                                }
                                if (spawn_catalog.find_info(c.key) != nullptr) {
                                        has_asset = true;
                                        break;
                                }
                        }
                        if (has_area && !has_asset) {
                                continue;
                        }
                }

                if (current_room_) {
                        bool has_area = false;
                        bool has_asset = false;
                        for (const auto& c : queue_item.candidates.entries()) {
                                if (c.kind == vibble::spawn::CandidateKind::Tag) {
                                        has_asset = true;
                                        break;
                                }
                                if (c.is_null || c.key.empty()) {
                                        continue;
                                }
                                if (current_room_->find_area(c.key) != nullptr) {
                                        has_area = true;
                                }
                                if (spawn_catalog.find_info(c.key) != nullptr) {
                                        has_asset = true;
                                        break;
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
                        const vibble::spawn::RuntimeCandidates::AssetCatalogView batch_catalog{
                            &batch_ctx.info_library(), false};

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
                                std::unordered_set<int> attempted_entries;
                                const size_t max_candidate_attempts = queue_item.candidates.entries().size();
                                const bool enforce_spacing = queue_item.check_min_spacing;
                                for (size_t attempt = 0; attempt < max_candidate_attempts; ++attempt) {
                                        const auto candidate = queue_item.select_candidate_excluding(
                                            batch_ctx.rng(),
                                            batch_catalog,
                                            attempted_entries);
                                        if (!candidate) {
                                                break;
                                        }
                                        if (candidate->entry_index >= 0) {
                                                attempted_entries.insert(candidate->entry_index);
                                        }

                                        if (candidate->is_null || !candidate->info) {
                                                batch_occupancy.set_occupied(vertex, true);
                                                placed = true;
                                                break;
                                        }
                                        const auto spawn_gp = batch_ctx.to_grid_point(spawn_pos);
                                        if (batch_ctx.checker().check(candidate->info,
                                                                spawn_gp,
                                                                batch_ctx.exclusion_zones(),
                                                                batch_ctx.all_assets(),
                                                                true,
                                                                enforce_spacing,
                                                                false,
                                                                false,
                                                                5)) {
                                                continue;
                                        }
                                        auto* result = batch_ctx.spawnAsset(candidate->resolved_asset_name,
                                                                            candidate->info,
                                                                            area,
                                                                            spawn_pos,
                                                                            0,
                                                                            queue_item.spawn_id,
                                                                            queue_item.position);
                                        if (!result) {
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

void AssetSpawner::run_floor_box_candidate_spawning() {
        if (!asset_library_ || all_.empty()) {
                return;
        }

        vibble::grid::Grid& grid_service = vibble::grid::global_grid();
        const vibble::spawn::RuntimeCandidates::AssetCatalogView catalog{&asset_info_library_, false};

        std::vector<Asset*> owner_snapshot;
        owner_snapshot.reserve(all_.size());
        for (const auto& asset_uptr : all_) {
                if (asset_uptr) {
                        owner_snapshot.push_back(asset_uptr.get());
                }
        }

        for (Asset* owner : owner_snapshot) {
                if (!owner || !owner->info || !owner->isFloorBoxesEnabled()) {
                        continue;
                }

                const auto& floor_boxes = owner->getFloorBoxes();
                if (floor_boxes.empty()) {
                        continue;
                }

                for (const auto& floor_box : floor_boxes) {
                        if (!floor_box.enabled || !floor_box.candidate.has_value()) {
                                continue;
                        }
                        if (!floor_box.candidate->has_positive_non_null_candidate) {
                                continue;
                        }

                        std::optional<Area> box_area = make_floor_box_spawn_area(*owner, floor_box);
                        if (!box_area.has_value()) {
                                continue;
                        }

                        const int grid_resolution =
                            floor_box_grid_resolution_or_default(floor_box.candidate->grid_resolution);
                        vibble::grid::Occupancy occupancy(*box_area, grid_resolution, grid_service);
                        std::vector<vibble::grid::Occupancy::Vertex*> vertices = occupancy.vertices_in_area(*box_area);
                        if (vertices.empty()) {
                                continue;
                        }

                        const SDL_Point owner_anchor_world = grid_service.snap_to_vertex(owner->world_xz_point(), grid_resolution);
                        Check floor_checker(false);
                        floor_checker.begin_session(grid_service, grid_resolution);
                        SpawnContext ctx(rng_,
                                         floor_checker,
                                         exclusion_zones,
                                         asset_info_library_,
                                         all_,
                                         asset_library_,
                                         grid_service,
                                         &occupancy);
                        ctx.set_map_grid_settings(map_grid_settings_);
                        ctx.set_spawn_resolution(grid_resolution);
                        ctx.set_clip_area(nullptr);
                        ctx.set_trail_areas({});

                        for (auto* vertex : vertices) {
                                if (!vertex) {
                                        continue;
                                }
                                if (vertex->world.x == owner_anchor_world.x &&
                                    vertex->world.y == owner_anchor_world.y) {
                                        continue;
                                }

                                const auto candidate = floor_box.candidate->candidates.pick_random(
                                    rng_,
                                    catalog,
                                    vibble::spawn::ZeroWeightPolicy::NoSelection);
                                if (!candidate || candidate->is_null || !candidate->info ||
                                    candidate->resolved_asset_name.empty()) {
                                        continue;
                                }

                                const SDL_Point spawn_pos = vertex->world;
                                const auto spawn_gp = ctx.to_grid_point(spawn_pos);
                                if (ctx.checker().check(candidate->info,
                                                        spawn_gp,
                                                        ctx.exclusion_zones(),
                                                        ctx.all_assets(),
                                                        true,
                                                        false,
                                                        false,
                                                        false,
                                                        5)) {
                                        continue;
                                }

                                Asset* spawned = ctx.spawnAsset(candidate->resolved_asset_name,
                                                                candidate->info,
                                                                *box_area,
                                                                spawn_pos,
                                                                0,
                                                                std::string{},
                                                                "FloorBox");
                                if (!spawned) {
                                        continue;
                                }
                                if (current_room_) {
                                        spawned->set_owning_room_name(current_room_->room_name);
                                }
                                ctx.checker().register_asset(spawned, false, false);
                        }
                        floor_checker.reset_session();
                }
        }
}

void AssetSpawner::run_edge_spawning(const Area& area) {
        auto point_in_exclusion = [&](const SDL_Point& pt) {
                return std::any_of(exclusion_zones.begin(), exclusion_zones.end(),
                [&](const Area& zone) { return zone.contains_point(pt); });
};

        vibble::grid::Grid& grid_service = vibble::grid::global_grid();
        const vibble::spawn::RuntimeCandidates::AssetCatalogView edge_catalog{&asset_info_library_, false};
        auto spacing_names = collect_spacing_asset_names(spawn_queue_, edge_catalog);
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
                        const auto candidate = queue_item.select_candidate(ctx.rng(), edge_catalog);

                        if (!candidate || candidate->is_null || !candidate->info) {
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

                        auto* result = ctx.spawnAsset(candidate->resolved_asset_name,
                                                      candidate->info,
                                                      area,
                                                      spawn_pos,
                                                      0,
                                                      queue_item.spawn_id,
                                                      queue_item.position);
                        if (result) {
                                ctx.checker().register_asset(result, enforce_spacing, false);
                        }

                        occupancy.set_occupied(vertex, true);
                }
                checker_.reset_session();
        }
}

