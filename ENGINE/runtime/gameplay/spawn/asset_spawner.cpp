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
#include <tuple>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "utils/grid.hpp"
#include "utils/grid_occupancy.hpp"
#include "utils/log.hpp"

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


Area make_bounds_area(const std::string& name, int min_x, int min_y, int max_x, int max_y) {
    if (max_x <= min_x || max_y <= min_y) {
        return Area(name, 0);
    }
    return Area(name, std::vector<SDL_Point>{
        SDL_Point{min_x, min_y},
        SDL_Point{max_x, min_y},
        SDL_Point{max_x, max_y},
        SDL_Point{min_x, max_y},
    });
}

bool area_has_points(const Area& area) {
    return !area.get_points().empty() && area.get_area() > 0.0;
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

bool areas_overlap_precisely(const Area& a, const Area& b) {
    if (!area_has_points(a) || !area_has_points(b)) {
        return false;
    }
    auto [a_min_x, a_min_y, a_max_x, a_max_y] = a.get_bounds();
    auto [b_min_x, b_min_y, b_max_x, b_max_y] = b.get_bounds();
    if (a_max_x < b_min_x || b_max_x < a_min_x || a_max_y < b_min_y || b_max_y < a_min_y) {
        return false;
    }

    const auto& a_points = a.get_points();
    const auto& b_points = b.get_points();
    if (a_points.size() < 3 || b_points.size() < 3) {
        return a.intersects(b);
    }

    for (std::size_t i = 0; i < a_points.size(); ++i) {
        const SDL_Point a0 = a_points[i];
        const SDL_Point a1 = a_points[(i + 1) % a_points.size()];
        for (std::size_t j = 0; j < b_points.size(); ++j) {
            const SDL_Point b0 = b_points[j];
            const SDL_Point b1 = b_points[(j + 1) % b_points.size()];
            if (segments_intersect(a0, a1, b0, b1)) {
                return true;
            }
        }
    }

    if (a.contains_point(b_points.front())) {
        return true;
    }
    if (b.contains_point(a_points.front())) {
        return true;
    }
    return false;
}

bool intersects_any_area_precisely(const Area& area, const std::vector<Area>& areas) {
    return std::any_of(areas.begin(), areas.end(), [&](const Area& other) {
        return areas_overlap_precisely(area, other);
    });
}

std::string bounds_string(const Area& area) {
    auto [min_x, min_y, max_x, max_y] = area.get_bounds();
    return "[" + std::to_string(min_x) + "," + std::to_string(min_y) + " -> " +
           std::to_string(max_x) + "," + std::to_string(max_y) + "]";
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

std::optional<Area> AssetSpawner::asset_footprint_area(const Asset& asset, const std::string& name) {
        for (const auto& shape : asset.current_impassable_shapes()) {
                if (!shape.valid || shape.floor_points.size() < 3) {
                        continue;
                }
                std::vector<SDL_Point> points;
                points.reserve(shape.floor_points.size());
                for (const SDL_FPoint& pt : shape.floor_points) {
                        points.push_back(SDL_Point{static_cast<int>(std::lround(pt.x)),
                                                   static_cast<int>(std::lround(pt.y))});
                }
                Area area(name, points);
                area.set_type("asset_footprint");
                if (area_has_points(area)) {
                        return area;
                }
        }

        static constexpr const char* kMetadataFootprintNames[] = {
            "footprint",
            "footprint_area",
            "impassable",
            "impassable_area",
            "collision",
            "collision_area",
            "base",
            "base_area",
        };
        for (const char* footprint_name : kMetadataFootprintNames) {
                Area area = asset.get_area(footprint_name);
                if (area_has_points(area)) {
                        area.set_name(name);
                        area.set_type("asset_footprint");
                        return area;
                }
        }

        int width = static_cast<int>(std::lround(asset.runtime_width_px()));
        int depth = static_cast<int>(std::lround(asset.runtime_height_px() * 0.5f));
        if (width <= 0) {
                width = asset.width();
        }
        if (depth <= 0) {
                depth = std::max(1, asset.height() / 2);
        }
        if (width <= 0 && asset.info) {
                width = asset.info->original_canvas_width;
        }
        if (depth <= 0 && asset.info) {
                depth = std::max(1, asset.info->original_canvas_height / 2);
        }
        if (width <= 0 || depth <= 0) {
                width = depth = 16;
        }

        const SDL_Point p = asset.world_xz_point();
        Area area = make_bounds_area(name,
                                     p.x - width / 2,
                                     p.y - depth,
                                     p.x + (width + 1) / 2,
                                     p.y);
        area.set_type("asset_footprint");
        return area_has_points(area) ? std::optional<Area>{area} : std::nullopt;
}

void AssetSpawner::spawn_edge_detail_candidates(Room& room,
                                                const Area& expanded_area,
                                                const Area& original_area,
                                                const std::vector<Area>& original_spawn_exclusion_areas,
                                                const nlohmann::json& edge_detail_candidates,
                                                std::vector<Area>& claimed_edge_detail_regions) {
        if (!asset_library_ || !room.room_area) {
                vibble::log::debug("[EdgeDetail] skip room/trail '" + room.room_name + "': missing asset library or room area");
                return;
        }
        if (!area_has_points(expanded_area)) {
                vibble::log::debug("[EdgeDetail] skip room/trail '" + room.room_name + "': empty expansion area");
                return;
        }
        if (!edge_detail_candidates.is_object()) {
                vibble::log::debug("[EdgeDetail] skip room/trail '" + room.room_name + "': map edge_detail_candidates is not an object");
                return;
        }
        if (!edge_detail_candidates.contains("candidates") || !edge_detail_candidates["candidates"].is_array()) {
                vibble::log::debug("[EdgeDetail] skip room/trail '" + room.room_name + "': no map edge_detail_candidates.candidates array");
                return;
        }

        nlohmann::json group = nlohmann::json::object();
        group["display_name"] = "Edge Detail";
        group["spawn_id"] = std::string("coarse_edge_") + room.room_name;
        group["position"] = "Random";
        group["min_number"] = 1;
        // The planner quantity only activates the candidate pool. Edge-detail density is driven by
        // the resolved occupancy vertices at the configured resolution, so do not cap placement by area.
        group["max_number"] = 1;
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
        AssetSpawnPlanner planner(sources, expanded_area, *asset_library_, contexts);

        asset_info_library_ = asset_library_->all();
        spawn_queue_ = planner.get_spawn_queue();
        const vibble::spawn::RuntimeCandidates::AssetCatalogView edge_catalog{&asset_info_library_, false};
        const auto spacing_names = collect_spacing_asset_names(spawn_queue_, edge_catalog);
        vibble::grid::Grid& grid_service = vibble::grid::global_grid();
        current_room_ = &room;
        map_grid_settings_ = room.map_grid_settings();

        const std::size_t initial_edge_count = all_.size();
        std::size_t total_spawn_attempts = 0;
        std::size_t successful_spawns = 0;
        std::size_t skipped_overlap_regions = 0;
        for (auto& queue_item : spawn_queue_) {
                if (!queue_item.has_candidates() || queue_item.quantity <= 0) {
                        continue;
                }

                int edge_resolution = queue_item.grid_resolution > 0
                    ? queue_item.grid_resolution
                    : edge_detail_candidates.value("resolution", vibble::grid::clamp_resolution(map_grid_settings_.grid_resolution));
                edge_resolution = vibble::grid::clamp_resolution(edge_resolution);

                Check edge_checker(false);
                edge_checker.begin_session(grid_service, edge_resolution);
                vibble::grid::Occupancy occupancy(expanded_area, edge_resolution, grid_service);
                SpawnContext ctx(rng_, edge_checker, exclusion_zones, asset_info_library_, all_, asset_library_, grid_service, &occupancy);
                ctx.set_spacing_filter(&spacing_names);
                ctx.set_map_grid_settings(map_grid_settings_);
                ctx.set_spawn_resolution(edge_resolution);
                ctx.set_trail_areas({});
                ctx.set_clip_area(nullptr);

                auto vertices = occupancy.vertices_in_area(expanded_area);
                std::vector<decltype(vertices)::value_type> expansion_vertices;
                expansion_vertices.reserve(vertices.size());
                for (auto* vertex : vertices) {
                        if (!vertex) {
                                continue;
                        }
                        if (original_area.contains_point(vertex->world)) {
                                continue;
                        }
                        expansion_vertices.push_back(vertex);
                }
                if (expansion_vertices.empty()) {
                        vibble::log::debug("[EdgeDetail] room/trail '" + room.room_name + "' has no candidate vertices");
                        edge_checker.reset_session();
                        continue;
                }
                std::shuffle(expansion_vertices.begin(), expansion_vertices.end(), rng_);

                for (auto* vertex : expansion_vertices) {
                        if (!vertex) {
                                continue;
                        }
                        const SDL_Point spawn_pos = vertex->world;
                        const bool inside_other_original =
                            std::any_of(original_spawn_exclusion_areas.begin(),
                                        original_spawn_exclusion_areas.end(),
                                        [&](const Area& other_original) {
                                            return area_has_points(other_original) &&
                                                   other_original.contains_point(spawn_pos);
                                        });
                        if (inside_other_original) {
                                ++skipped_overlap_regions;
                                occupancy.set_occupied(vertex, true);
                                continue;
                        }
                        constexpr int kMaxCandidateRetriesPerVertex = 4;
                        std::unordered_set<std::string> attempted_candidates;

                        for (int attempt = 0; attempt < kMaxCandidateRetriesPerVertex; ++attempt) {
                                ++total_spawn_attempts;
                                const auto candidate = queue_item.select_candidate(ctx.rng(), edge_catalog);
                                const std::string candidate_name =
                                    candidate ? candidate->resolved_asset_name : std::string{"<none>"};
                                vibble::log::debug("[EdgeDetail] attempt room/trail='" + room.room_name +
                                                   "' candidate='" + candidate_name + "' pos=(" +
                                                   std::to_string(spawn_pos.x) + "," + std::to_string(spawn_pos.y) + ")");

                                if (!candidate || candidate->is_null || !candidate->info || candidate->resolved_asset_name.empty()) {
                                        vibble::log::debug("[EdgeDetail] rejected room/trail='" + room.room_name +
                                                           "' reason=null-or-unresolved-candidate");
                                        continue;
                                }
                                if (!attempted_candidates.insert(candidate->resolved_asset_name).second) {
                                        continue;
                                }

                                const auto spawn_gp = ctx.to_grid_point(spawn_pos);
                                if (ctx.checker().check(candidate->info,
                                                        spawn_gp,
                                                        ctx.exclusion_zones(),
                                                        ctx.all_assets(),
                                                        false,
                                                        queue_item.check_min_spacing,
                                                        false,
                                                        false,
                                                        5)) {
                                        vibble::log::debug("[EdgeDetail] rejected room/trail='" + room.room_name +
                                                           "' candidate='" + candidate_name + "' reason=spacing-check");
                                        continue;
                                }

                                Asset* result = ctx.spawnAsset(candidate->resolved_asset_name,
                                                               candidate->info,
                                                               expanded_area,
                                                               spawn_pos,
                                                               0,
                                                               queue_item.spawn_id,
                                                               "EdgeDetail");
                                if (!result) {
                                        vibble::log::debug("[EdgeDetail] rejected room/trail='" + room.room_name +
                                                           "' candidate='" + candidate_name + "' reason=spawn-failed");
                                        continue;
                                }
                                result->set_owning_room_name(room.room_name);

                                std::optional<Area> footprint = asset_footprint_area(*result, "edge_detail_footprint");
                                if (!footprint || !area_has_points(*footprint)) {
                                        footprint = make_bounds_area("edge_detail_footprint",
                                                                     spawn_pos.x - 8,
                                                                     spawn_pos.y - 8,
                                                                     spawn_pos.x + 8,
                                                                     spawn_pos.y + 8);
                                }

                                std::string rejection_reason;
                                if (footprint->intersects(original_area)) {
                                        rejection_reason = "intersects-original-pre-coarseness-area";
                                } else if (intersects_any_area_precisely(*footprint, original_spawn_exclusion_areas)) {
                                        rejection_reason = "intersects-another-room-or-trail-original-area";
                                        ++skipped_overlap_regions;
                                } else if (intersects_any_area_precisely(*footprint, exclusion_zones)) {
                                        rejection_reason = "overlaps-normal-spawned-asset";
                                } else {
                                        for (const auto& existing_edge : all_) {
                                                Asset* existing = existing_edge.get();
                                                if (!existing || existing == result || existing->spawn_method != "EdgeDetail") {
                                                        continue;
                                                }
                                                std::optional<Area> existing_footprint =
                                                    asset_footprint_area(*existing, "existing_edge_detail_footprint");
                                                if (existing_footprint && footprint->intersects(*existing_footprint)) {
                                                        rejection_reason = "overlaps-another-edge-detail-asset";
                                                        break;
                                                }
                                        }
                                }
                                if (rejection_reason.empty() && intersects_any_area_precisely(*footprint, claimed_edge_detail_regions)) {
                                        rejection_reason = "overlaps-claimed-edge-detail-region";
                                }

                                if (!rejection_reason.empty()) {
                                        vibble::log::debug("[EdgeDetail] rejected room/trail='" + room.room_name +
                                                           "' candidate='" + candidate_name + "' reason=" + rejection_reason +
                                                           " footprint=" + bounds_string(*footprint));
                                        if (!all_.empty() && all_.back().get() == result) {
                                                all_.pop_back();
                                        }
                                        continue;
                                }

                                claimed_edge_detail_regions.push_back(*footprint);
                                ctx.checker().register_asset(result, queue_item.check_min_spacing, true);
                                ++successful_spawns;
                                vibble::log::debug("[EdgeDetail] spawned room/trail='" + room.room_name +
                                                   "' candidate='" + candidate_name + "' footprint=" + bounds_string(*footprint));
                                break;
                        }
                        occupancy.set_occupied(vertex, true);
                }
                edge_checker.reset_session();
        }

        current_room_ = nullptr;
        const std::size_t final_edge_count = all_.size() - initial_edge_count;
        vibble::log::debug("[EdgeDetail] validation room/trail='" + room.room_name +
                           "' expanded_area_size=" + std::to_string(expanded_area.get_area()) +
                           " spawn_attempts=" + std::to_string(total_spawn_attempts) +
                           " successful_spawns=" + std::to_string(successful_spawns) +
                           " skipped_overlap_regions=" + std::to_string(skipped_overlap_regions) +
                           " final_count=" + std::to_string(final_edge_count));
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

