#include "asset_loader.hpp"
#include "asset_loader_internal.hpp"
#include <algorithm>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <cmath>
#include <cctype>
#include <stdexcept>
#include <chrono>
#include <filesystem>
#include <limits>
#include <cstdint>
#include <cstddef>
#include <string>
#include <cstdlib>
#include <utility>
#include <SDL3/SDL.h>
#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_library.hpp"
#include "assets/asset/asset_types.hpp"
#include "audio/audio_engine.hpp"
#include "gameplay/map_generation/room.hpp"
#include "utils/area.hpp"
#include "utils/map_grid_settings.hpp"
#include "gameplay/map_generation/generate_rooms.hpp"
#include "gameplay/map_generation/coarseness_system.hpp"
#include "gameplay/map_generation/map_graph.hpp"
#include "gameplay/map_generation/map_layers_geometry.hpp"
#include "gameplay/world/chunk.hpp"
#include "gameplay/world/world_grid.hpp"
#include "gameplay/spawn/runtime_candidates.hpp"
#include "gameplay/spawn/asset_spawner.hpp"
#include "utils/grid.hpp"
#include "core/tile_builder.hpp"
#include <nlohmann/json.hpp>
#include "utils/loading_status_notifier.hpp"
#include "utils/log.hpp"
using json = nlohmann::json;

namespace {

std::uint64_t mix_u64(std::uint64_t seed, std::uint64_t value) {
        seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
        return seed;
}

std::uint64_t build_map_coarseness_seed(const std::string& map_id, const nlohmann::json& manifest) {
        std::uint64_t seed = std::hash<std::string>{}(map_id);
        if (manifest.is_object()) {
                auto map_info_it = manifest.find("map_info");
                if (map_info_it != manifest.end() && map_info_it->is_object()) {
                        auto regen_it = map_info_it->find("regen_seed");
                        if (regen_it != map_info_it->end() && regen_it->is_number_integer()) {
                                seed = mix_u64(seed, static_cast<std::uint64_t>(regen_it->get<std::int64_t>()));
                        }
                }
        }
        return seed;
}

bool asset_info_has_default_frames(const std::shared_ptr<AssetInfo>& info) {
        if (!info) {
                return false;
        }

        auto it = info->animations.find("default");
        return it != info->animations.end() && it->second.has_frames();
}

std::unordered_set<std::string> collect_all_known_asset_names(const AssetLibrary* asset_library) {
        std::unordered_set<std::string> names;
        if (!asset_library) {
                return names;
        }

        for (const auto& [name, info] : asset_library->all()) {
                if (!name.empty() && info) {
                        names.insert(name);
                }
        }

        return names;
}

bool ensure_default_animation_frames_loaded_for_asset(SDL_Renderer* renderer,
                                                     AssetLibrary* asset_library,
                                                     const std::shared_ptr<AssetInfo>& info) {
        if (!info) {
                return false;
        }

        if (asset_info_has_default_frames(info)) {
                return true;
        }

        if (!renderer) {
                vibble::log::warn("[AssetLoader] Cannot lazy-load animations for '" + info->name +
                                  "': renderer is null.");
                return false;
        }

        try {
                if (asset_library && !info->name.empty()) {
                        std::unordered_set<std::string> one_asset;
                        one_asset.insert(info->name);
                        asset_library->loadAnimationsFor(renderer, one_asset);
                } else {
                        info->loadAnimations(renderer, true, true);
                }
        } catch (const std::exception& ex) {
                vibble::log::error("[AssetLoader] Lazy animation load failed for '" + info->name +
                                   "': " + ex.what());
                return false;
        } catch (...) {
                vibble::log::error("[AssetLoader] Lazy animation load failed for '" + info->name +
                                   "': unknown exception");
                return false;
        }

        return asset_info_has_default_frames(info);
}

} // namespace

AssetLoader::~AssetLoader() = default;

const std::vector<Room*>& AssetLoader::getRooms() const {
        static const std::vector<Room*> empty_rooms;
        return world_context_ ? world_context_->rooms() : empty_rooms;
}

std::shared_ptr<RuntimeWorldContext> AssetLoader::release_runtime_world_context() {
        return std::exchange(world_context_, nullptr);
}

AssetLoader::AssetLoader(const std::string& map_id,
                         const nlohmann::json& map_manifest,
                         SDL_Renderer* renderer,
                         std::string content_root,
                         devmode::core::ManifestStore* manifest_store,
                         AssetLibrary* shared_asset_library)
: map_id_(map_id),
map_path_(std::move(content_root)),
renderer_(renderer),
world_context_(std::make_shared<RuntimeWorldContext>()),
manifest_store_(manifest_store)
{
        vibble::log::info(std::string("[AssetLoader] Start for map '") + map_id_ + "' at root '" + map_path_ + "'.");
        using_shared_asset_library_ = (shared_asset_library != nullptr);
        if (using_shared_asset_library_) {
                asset_library_ = shared_asset_library;
        } else {
                owned_asset_library_ = std::make_unique<AssetLibrary>();
                asset_library_ = owned_asset_library_.get();
        }
        vibble::log::info(std::string("[AssetLoader] Asset library mode: ") + (using_shared_asset_library_ ? "shared" : "owned"));

        const auto overall_begin = std::chrono::steady_clock::now();

        const auto map_begin = std::chrono::steady_clock::now();
        loading_status::notify("Loading map data");
        load_from_manifest(map_manifest);
        const auto map_end = std::chrono::steady_clock::now();
        vibble::log::info(std::string("[AssetLoader] Map JSON parsed in ") + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(map_end - map_begin).count()) + "ms");

        const nlohmann::json& audio_manifest = map_manifest_json_.contains("audio") ? map_manifest_json_.at("audio") : nlohmann::json::object();
        try {
                const auto audio_begin = std::chrono::steady_clock::now();
                AudioEngine::instance().init(map_id_, audio_manifest, map_path_);
                const auto audio_end = std::chrono::steady_clock::now();
                vibble::log::info(std::string("[AssetLoader] Audio initialized in ") + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(audio_end - audio_begin).count()) + "ms");
        } catch (const std::exception& ex) {
                vibble::log::error(std::string("[AssetLoader] Audio init failed: ") + ex.what());
        } catch (...) {
                vibble::log::error("[AssetLoader] Audio init failed with unknown error.");
        }

        const auto library_begin = std::chrono::steady_clock::now();
        loading_status::notify("Loading assets");
        const auto library_end = std::chrono::steady_clock::now();
        if (asset_library_) {
                vibble::log::info(std::string("[AssetLoader] Asset library ready with ") + std::to_string(asset_library_->all().size()) + " known assets");
                vibble::log::debug(std::string("[AssetLoader] Asset library phase took ") + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(library_end - library_begin).count()) + "ms");
        }

        const auto rooms_begin = std::chrono::steady_clock::now();
        loading_status::notify("Creating map");
        try {
                loadRooms();
        } catch (const std::exception& ex) {
                vibble::log::error(std::string("[AssetLoader] loadRooms failed: ") + ex.what());
        } catch (...) {
                vibble::log::error("[AssetLoader] loadRooms failed with unknown error.");
        }
        const auto rooms_end = std::chrono::steady_clock::now();
        vibble::log::info(std::string("[AssetLoader] Rooms created: ") + std::to_string(getRooms().size()) + " in " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(rooms_end - rooms_begin).count()) + "ms");
        loading_status::notify("Loading assets");
        {
                const auto preload_begin = std::chrono::steady_clock::now();

                if (asset_library_ && renderer_) {
                        const std::unordered_set<std::string> all_asset_names =
                            collect_all_known_asset_names(asset_library_);

                        if (!all_asset_names.empty()) {
                                vibble::log::info(std::string("[AssetLoader] Loading runtime animations for all known assets (") +
                                                  std::to_string(all_asset_names.size()) + ")...");
                                asset_library_->loadAnimationsFor(renderer_, all_asset_names);

                                const auto preload_end = std::chrono::steady_clock::now();
                                const double preload_ms =
                                    std::chrono::duration_cast<std::chrono::milliseconds>(preload_end - preload_begin).count();

                                vibble::log::info(std::string("[AssetLoader] Runtime animation load completed for all known asset type(s): requested=") +
                                                  std::to_string(all_asset_names.size()) + " in " +
                                                  std::to_string(preload_ms) + "ms");
                        } else {
                                vibble::log::warn("[AssetLoader] Asset library has no known assets to preload.");
                        }
                } else if (!renderer_) {
                        vibble::log::warn("[AssetLoader] Renderer unavailable; skipping full runtime animation load.");
                } else {
                        vibble::log::warn("[AssetLoader] Asset library unavailable; skipping full runtime animation load.");
                }
        }

        loading_status::notify("Loading assets");
        vibble::log::info("[AssetLoader] Finalizing assets across rooms...");
        try {
                finalizeAssets();
        } catch (const std::exception& ex) {
                vibble::log::error(std::string("[AssetLoader] finalizeAssets threw: ") + ex.what());
        } catch (...) {
                vibble::log::error("[AssetLoader] finalizeAssets threw unknown error.");
        }
        vibble::log::info("[AssetLoader] Asset finalization completed; all assets are ready.");

        const auto overall_end = std::chrono::steady_clock::now();
        const double map_ms = std::chrono::duration_cast<std::chrono::milliseconds>(map_end - map_begin).count();
        const double library_ms = std::chrono::duration_cast<std::chrono::milliseconds>(library_end - library_begin).count();
        const double rooms_ms = std::chrono::duration_cast<std::chrono::milliseconds>(rooms_end - rooms_begin).count();
        const double total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(overall_end - overall_begin).count();
        vibble::log::info(std::string("[AssetLoader] Map metadata loaded in ") + std::to_string(map_ms) + "ms");
        vibble::log::info(std::string("[AssetLoader] Asset library ready in ") + std::to_string(library_ms) + "ms");
        vibble::log::info(std::string("[AssetLoader] Rooms built in ") + std::to_string(rooms_ms) + "ms");
        vibble::log::info(std::string("[AssetLoader] Initialization completed in ") + std::to_string(total_ms) + "ms");
        const auto distant_boundary = collectDistantAssets(150, 3000);
        vibble::log::info(std::string("[AssetLoader] Boundary startup filtering moved to runtime visibility culling. ")
                          + "distance_candidates=" + std::to_string(distant_boundary.size()) +
                          " hidden_applied=0");
}

std::vector<Asset*> AssetLoader::collectDistantAssets(int lock_threshold, int remove_threshold) {
        std::vector<Asset*> distant_assets;
        const std::vector<Room*>& rooms = getRooms();
        distant_assets.reserve(rooms.size() * 4);
        auto allZones = getAllRoomAndTrailAreas();
        auto zoneCache = asset_loader_internal::build_zone_cache(allZones);

        std::unordered_map<std::string, Room*> room_lookup;
        room_lookup.reserve(rooms.size());
        for (Room* room : rooms) {
                if (room) {
                        room_lookup.emplace(room->room_name, room);
                }
        }

        const double remove_distance = static_cast<double>(remove_threshold);
        const double lock_distance = static_cast<double>(lock_threshold);
        int considered = 0, skipped_type = 0, kept_in_room = 0, kept_in_zone = 0, removed = 0, locked = 0;

        for (Room* room : rooms) {
                for (auto& asset_up : room->assets) {
                        Asset* asset = asset_up.get();
            if (!asset->info || asset->info->type != asset_types::boundary) {
                    ++skipped_type;
                    continue;
            }
                        ++considered;
                        const SDL_Point asset_world_xz = asset->world_xz_point();

                        Room* owning_room = room;
                        const std::string& owner_name = asset->owning_room_name();
                        if (!owner_name.empty()) {
                                auto it = room_lookup.find(owner_name);
                                if (it != room_lookup.end() && it->second) {
                                        owning_room = it->second;
                                }
                        }

                        if (owning_room && owning_room->room_area && owning_room->room_area->contains_point(asset_world_xz)) {
                                ++kept_in_room;
                                continue;
                        }

                        if (asset_loader_internal::point_inside_any_zone(asset_world_xz, zoneCache)) {
                                ++kept_in_zone;
                                continue;
                        }
                        double minDistSq = asset_loader_internal::min_distance_sq_to_zones(asset_world_xz, zoneCache, remove_threshold);
                        double minDist = std::sqrt(minDistSq);

                        const bool should_lock = minDist > lock_distance;
                        const bool should_remove = minDist >= remove_distance;

                        if (asset && asset->info && asset->info->type == asset_types::player) {
                                asset->static_frame = false;
                        } else {
                                asset->static_frame = should_lock;
                        }
                        if (should_lock) ++locked;
                        if (should_remove) {
                                distant_assets.push_back(asset);
                                ++removed;
                                continue;
                        }
                }
        }

        vibble::log::debug(std::string("[AssetLoader] collectDistantAssets: considered=") + std::to_string(considered) + " removed=" + std::to_string(removed) + " locked=" + std::to_string(locked) + " kept_in_room=" + std::to_string(kept_in_room) + " kept_in_zone=" + std::to_string(kept_in_zone) + " skipped_non_boundary=" + std::to_string(skipped_type));

        return distant_assets;
}

void AssetLoader::loadRooms() {
        vibble::log::info("[AssetLoader] Starting room generation for map '" + map_id_ + "'");
        const double min_edge_distance = map_layers::min_edge_distance_from_map_manifest(map_manifest_json_);
        GenerateRooms generator(map_layers_, map_center_x_, map_center_y_, map_id_, map_manifest_json_, min_edge_distance, manifest_store_);
        nlohmann::json empty_rooms    = nlohmann::json::object();
        nlohmann::json empty_trails   = nlohmann::json::object();
        map_grid_settings_ = MapGridSettings::from_json(map_manifest_json_.contains("map_grid_settings") ? &map_manifest_json_["map_grid_settings"] : nullptr);
        MapGridSettings grid_settings = map_grid_settings_;
        auto room_ptrs = generator.build( asset_library_, map_radius_, layer_radii_, rooms_data_        ? *rooms_data_        : empty_rooms, trails_data_       ? *trails_data_       : empty_trails, grid_settings);
        world_context_->adopt_rooms(std::move(room_ptrs));
        {
                auto rooms = getRooms();

                std::unordered_map<Room*, Area> original_room_areas;
                original_room_areas.reserve(rooms.size());
                std::vector<Area> normal_spawned_occupancy;
                auto add_asset_claim = [&normal_spawned_occupancy](Asset* asset) {
                        if (!asset) return;
                        std::optional<Area> footprint = AssetSpawner::asset_footprint_area(*asset, "normal_asset_footprint");
                        if (footprint) {
                                normal_spawned_occupancy.push_back(*footprint);
                        }
                };
                for (Room* room : rooms) {
                        if (!room || !room->room_area) continue;
                        original_room_areas.emplace(room, *room->room_area);
                        for (auto& asset_up : room->assets) {
                                add_asset_claim(asset_up.get());
                        }
                }

                const std::uint64_t coarseness_seed = build_map_coarseness_seed(map_id_, map_manifest_json_);
                vibble::mapgen::coarseness::apply_coarseness_expansion(rooms, coarseness_seed);

                const nlohmann::json* edge_detail_candidates = nullptr;
                auto edge_detail_it = map_manifest_json_.find("edge_detail_candidates");
                if (edge_detail_it != map_manifest_json_.end() && edge_detail_it->is_object()) {
                        edge_detail_candidates = &(*edge_detail_it);
                }

                std::vector<Area> edge_detail_claimed;
                for (Room* room : rooms) {
                        if (!room || !room->coarseness_added_area || !edge_detail_candidates) continue;
                        if (room->coarseness_added_area->get_points().empty() || room->coarseness_added_area->get_area() <= 0.0) {
                                continue;
                        }
                        auto original_it = original_room_areas.find(room);
                        const Area& original_area = original_it != original_room_areas.end()
                            ? original_it->second
                            : *room->room_area;
                        std::vector<Area> other_original_areas;
                        other_original_areas.reserve(original_room_areas.size());
                        for (const auto& [other_room, other_area] : original_room_areas) {
                                if (other_room && other_room != room) {
                                        other_original_areas.push_back(other_area);
                                }
                        }

                        AssetSpawner edge_spawner(asset_library_, normal_spawned_occupancy);
                        edge_spawner.set_map_grid_settings(room->map_grid_settings());
                        edge_spawner.spawn_edge_detail_candidates(*room,
                                                                  *room->room_area,
                                                                  original_area,
                                                                  other_original_areas,
                                                                  *edge_detail_candidates,
                                                                  edge_detail_claimed);
                }
        }
        if (getRooms().empty()) {
                throw std::runtime_error("[AssetLoader] Room generation produced zero rooms after manifest normalization.");
        }

        vibble::log::info("[AssetLoader] Room generation completed successfully: " + std::to_string(getRooms().size()) + " rooms created");
        vibble::log::debug(std::string("[AssetLoader] loadRooms: rooms=") + std::to_string(getRooms().size()));
}

void AssetLoader::finalizeAssets() {
        std::size_t room_index         = 0;
        std::size_t total_assets       = 0;
        std::size_t finalized_assets   = 0;
        std::size_t skipped_assets     = 0;
        std::size_t skipped_missing_default_count = 0;
        std::size_t lazy_loaded_asset_count = 0;

        std::vector<std::string> skipped_missing_default_names;
        skipped_missing_default_names.reserve(16);

        for (Room* room : getRooms()) {
                if (!room) {
                        ++room_index;
                        continue;
                }

                const std::size_t room_total = room->assets.size();
                std::size_t room_finalized = 0;
                std::size_t room_skipped = 0;

                for (auto& asset_up : room->assets) {
                        ++total_assets;

                        Asset* asset = asset_up.get();
                        if (!asset || !asset->info) {
                                ++skipped_assets;
                                ++room_skipped;
                                continue;
                        }

                        const std::string name = asset->info->name;

                        const bool had_default_frames = asset_info_has_default_frames(asset->info);
                        if (!had_default_frames &&
                            ensure_default_animation_frames_loaded_for_asset(renderer_, asset_library_, asset->info)) {
                                ++lazy_loaded_asset_count;
                        }

                        if (!asset_info_has_default_frames(asset->info)) {
                                vibble::log::error(std::string("[AssetLoader] finalizeAssets: asset '") +
                                                   name +
                                                   "' is missing default animation frames after map-scoped lazy load; skipping.");

                                ++skipped_missing_default_count;
                                if (skipped_missing_default_names.size() < 16) {
                                        skipped_missing_default_names.push_back(name);
                                }

                                asset_up.reset();
                                ++skipped_assets;
                                ++room_skipped;
                                continue;
                        }

                        try {
                                asset->finalize_setup();
                                ++finalized_assets;
                                ++room_finalized;
                        } catch (const std::exception& ex) {
                                vibble::log::error(std::string("[AssetLoader] finalizeAssets: exception during finalize_setup for '") +
                                                   name +
                                                   "': " +
                                                   ex.what() +
                                                   ". Skipping asset.");

                                asset_up.reset();
                                ++skipped_assets;
                                ++room_skipped;
                                continue;
                        } catch (...) {
                                vibble::log::error(std::string("[AssetLoader] finalizeAssets: unknown exception during finalize_setup for '") +
                                                   name +
                                                   "'. Skipping asset.");

                                asset_up.reset();
                                ++skipped_assets;
                                ++room_skipped;
                                continue;
                        }
                }

                if (room_total > 0) {
                        std::string msg =
                            std::string("[AssetLoader] finalizeAssets: room=") +
                            std::to_string(room_index) +
                            " finalized " +
                            std::to_string(room_finalized) +
                            "/" +
                            std::to_string(room_total);

                        if (room_skipped > 0) {
                                msg += std::string(" (skipped ") + std::to_string(room_skipped) + ")";
                        }

                        vibble::log::debug(msg);
                }

                ++room_index;
        }

        std::string msg =
            std::string("[AssetLoader] finalizeAssets complete: ") +
            std::to_string(finalized_assets) +
            "/" +
            std::to_string(total_assets) +
            " assets ready";

        if (skipped_assets > 0) {
                msg += std::string(" (") + std::to_string(skipped_assets) + " skipped)";
        }

        msg += std::string(", lazy_loaded_asset_types=") + std::to_string(lazy_loaded_asset_count);
        vibble::log::info(msg);

        if (skipped_missing_default_count > 0) {
                vibble::log::warn(std::string("[AssetLoader] finalizeAssets missing-default skip count after lazy load: ") +
                                  std::to_string(skipped_missing_default_count));
        }

        if (!skipped_missing_default_names.empty()) {
                std::string sampled = skipped_missing_default_names.front();
                for (std::size_t i = 1; i < skipped_missing_default_names.size(); ++i) {
                        sampled += ", " + skipped_missing_default_names[i];
                }

                vibble::log::error(std::string("[AssetLoader] finalizeAssets high-severity: missing default frames after map-scoped lazy load. Sample assets: ") +
                                   sampled);
        }
}

std::vector<std::unique_ptr<Asset>> AssetLoader::extract_all_assets() {
        std::vector<std::unique_ptr<Asset>> out;
        const std::vector<Room*>& rooms = getRooms();
        out.reserve(rooms.size() * 4);
        std::size_t hidden_skipped_total = 0;
        std::size_t hidden_skipped_boundary = 0;
        for (Room* room : rooms) {
                if (!room) continue;
                auto& assets = room->assets;
                for (auto it = assets.begin(); it != assets.end();) {
                        std::unique_ptr<Asset>& aup = *it;
                        Asset* asset = aup.get();
                        if (!asset) {
                                it = assets.erase(it);
                                continue;
                        }
                        if (asset->is_hidden()) {
                                ++hidden_skipped_total;
                                if (asset->info &&
                                    asset_types::canonicalize(asset->info->type) == std::string(asset_types::boundary)) {
                                        ++hidden_skipped_boundary;
                                }
                                ++it;
                                continue;
                        }
                        out.push_back(std::move(aup));
                        it = assets.erase(it);
                }
        }
        if (hidden_skipped_total > 0) {
                vibble::log::info(std::string("[AssetLoader] extract_all_assets skipped hidden entries. total=") +
                                  std::to_string(hidden_skipped_total) +
                                  " boundary=" + std::to_string(hidden_skipped_boundary));
        }
        return out;
}

void AssetLoader::createAssets(world::WorldGrid& grid) {
        const auto t0 = std::chrono::steady_clock::now();

        const int requested = std::max(0, map_grid_settings_.grid_resolution);
        grid.set_grid_resolution(requested);
        vibble::log::debug(std::string("[AssetLoader] createAssets: requested grid_resolution=") + std::to_string(requested));

        auto extracted_assets = extract_all_assets();
        std::size_t extracted_boundary_assets = 0;
        for (const auto& asset_up : extracted_assets) {
                if (!asset_up || !asset_up->info) {
                        continue;
                }
                if (asset_types::canonicalize(asset_up->info->type) == std::string(asset_types::boundary)) {
                        ++extracted_boundary_assets;
                }
        }
        std::vector<Asset*> registered_assets;
        registered_assets.reserve(extracted_assets.size());
        vibble::log::info(std::string("[AssetLoader] Extracted ") + std::to_string(extracted_assets.size()) + " visible assets from rooms");
        vibble::log::info(std::string("[AssetLoader] Boundary startup visibility summary: extracted=") +
                          std::to_string(extracted_boundary_assets));

        for (auto& asset_up : extracted_assets) {
                if (!asset_up) continue;
                Asset* asset = grid.create_asset_at_point(std::move(asset_up));
                if (asset) {
                        registered_assets.push_back(asset);
                        //vibble::log::info(std::string("[AssetLoader] Registered asset: ") + (asset->info ? asset->info->name : std::string{"<null>"}));
                }
        }
        std::size_t registered_boundary_assets = 0;
        for (Asset* asset : registered_assets) {
                if (!asset || !asset->info) {
                        continue;
                }
                if (asset_types::canonicalize(asset->info->type) == std::string(asset_types::boundary)) {
                        ++registered_boundary_assets;
                }
        }
        vibble::log::info(std::string("[AssetLoader] Boundary startup visibility summary: registered=") +
                          std::to_string(registered_boundary_assets) +
                          " filtered=" + std::to_string(extracted_boundary_assets - std::min(extracted_boundary_assets, registered_boundary_assets)));
        vibble::log::debug(std::string("[AssetLoader] Registered assets: total=") + std::to_string(registered_assets.size()));

        {
            std::vector<Asset*> finalized_assets;
            finalized_assets.reserve(registered_assets.size());
            for (Asset* asset : registered_assets) {
                    if (!asset) {
                            continue;
                    }
                    if (!asset->is_finalized()) {
                            vibble::log::error(std::string("[AssetLoader] createAssets: skipping unfinalized asset '") +
                                               (asset->info ? asset->info->name : std::string{"<null>"}) +
                                               "'. Loader lifecycle requires finalized assets before grid tile build.");
                            continue;
                    }
                    finalized_assets.push_back(asset);
            }
            loader_tiles::build_grid_tiles(renderer_, grid, map_grid_settings_, finalized_assets);
        }

        const auto t1 = std::chrono::steady_clock::now();
        vibble::log::debug(std::string("[AssetLoader] createAssets total ") + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()) + "ms");
}

std::vector<const Area*> AssetLoader::getAllRoomAndTrailAreas() const {
        std::vector<const Area*> areas;
        const std::vector<Room*>& rooms = getRooms();
        areas.reserve(rooms.size());
        for (const Room* r : rooms) {
                if (r && r->room_area) {
                        areas.push_back(r->room_area.get());
                }
        }
        return areas;
}

void AssetLoader::load_from_manifest(const nlohmann::json& map_manifest) {
        if (!map_manifest.is_object()) {
                map_manifest_json_ = nlohmann::json::object();
                vibble::log::warn(std::string("[AssetLoader] Invalid manifest payload for '") + map_id_ + "'. Using empty object.");
        } else {
                map_manifest_json_ = map_manifest;
        }

        auto bind_object_section = [this](const char* key) -> nlohmann::json* {
                auto it = map_manifest_json_.find(key);
                if (it != map_manifest_json_.end() && it->is_object()) {
                        return &(*it);
                }
                vibble::log::warn(std::string("[AssetLoader] Missing or invalid manifest section '") + key +
                                  "' for map '" + map_id_ + "'.");
                return nullptr;
        };

        rooms_data_        = bind_object_section("rooms_data");
        trails_data_       = bind_object_section("trails_data");

        map_graph::MapGraphPlan graph_plan = map_graph::build_map_graph_plan(&map_manifest_json_);
        for (const std::string& diagnostic : graph_plan.diagnostics) {
                if (diagnostic.rfind("error:", 0) == 0) {
                        vibble::log::error(std::string("[AssetLoader] map_graph: ") + diagnostic);
                } else {
                        vibble::log::warn(std::string("[AssetLoader] map_graph: ") + diagnostic);
                }
        }
        if (!graph_plan.valid) {
                throw std::runtime_error(
                    std::string("[AssetLoader] map_graph planning failed for map '") + map_id_ + "'.");
        }
        rooms_data_        = bind_object_section("rooms_data");
        trails_data_       = bind_object_section("trails_data");

        map_layers_ = graph_plan.resolved_layers;
        nlohmann::json resolved_layers_json = nlohmann::json::array();
        for (const LayerSpec& layer_spec : map_layers_) {
                nlohmann::json layer_entry = nlohmann::json::object();
                layer_entry["level"] = layer_spec.level;
                layer_entry["max_rooms"] = layer_spec.max_rooms;
                layer_entry["rooms"] = nlohmann::json::array();
                for (const RoomSpec& room_spec : layer_spec.rooms) {
                        layer_entry["rooms"].push_back(nlohmann::json::object({
                            {"name", room_spec.name},
                            {"max_instances", room_spec.max_instances}
                        }));
                }
                resolved_layers_json.push_back(std::move(layer_entry));
        }

        map_layers::LayerRadiiResult radii_result;
        const nlohmann::json* rooms_data_ptr = rooms_data_;
        const double min_edge = map_layers::min_edge_distance_from_map_manifest(map_manifest_json_);
        radii_result = map_layers::compute_layer_radii(resolved_layers_json, rooms_data_ptr, min_edge);

        map_radius_   = radii_result.map_radius;
        map_center_x_ = map_center_y_ = map_radius_;
        layer_radii_  = radii_result.layer_radii;
        auto layers_it = map_manifest_json_.find("map_layers");
        if (layers_it != map_manifest_json_.end() && layers_it->is_array()) {
                for (std::size_t idx = 0; idx < layers_it->size(); ++idx) {
                        auto& layer_entry = (*layers_it)[idx];
                        if (!layer_entry.is_object()) {
                                continue;
                        }
                        const double ring_radius = idx < radii_result.layer_radii.size() ? radii_result.layer_radii[idx] : 0.0;
                        const double extent_value = idx < radii_result.layer_extents.size() ? radii_result.layer_extents[idx] : 0.0;
                        layer_entry["ring_radius"] = ring_radius;
                        layer_entry["bounding_extent"] = extent_value;
                }
        }
        auto layer_settings_it = map_manifest_json_.find("map_layers_settings");
        if (layer_settings_it != map_manifest_json_.end() && layer_settings_it->is_object()) {
                (*layer_settings_it)["min_edge_distance"] = radii_result.min_edge_distance;
        } else {
                vibble::log::warn(std::string("[AssetLoader] Missing or invalid 'map_layers_settings' section for map '") +
                                  map_id_ + "'.");
        }

        vibble::log::debug(std::string("[AssetLoader] load_from_manifest: map_radius_=") + std::to_string(map_radius_) + " layers=" + std::to_string(map_layers_.size()));
}
