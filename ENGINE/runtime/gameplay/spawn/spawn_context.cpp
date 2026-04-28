#include "spawn_context.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_map>
#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_library.hpp"
#include "gameplay/spawn/asset_spawn_planner.hpp"
#include "gameplay/spawn/asset_spawner.hpp"
#include "utils/area.hpp"
#include "utils/area_helpers.hpp"
#include "utils/log.hpp"
#include "utils/map_grid_settings.hpp"
#include "gameplay/world/grid_point.hpp"
SpawnContext::SpawnContext(std::mt19937& rng,
                           Check& checker,
                           std::vector<Area>& exclusion_zones,
                           const std::unordered_map<std::string, std::shared_ptr<AssetInfo>>& asset_info_library,
                           std::vector<std::unique_ptr<Asset>>& all,
                           AssetLibrary* asset_library,
                           vibble::grid::Grid& grid,
                           vibble::grid::Occupancy* occupancy)
: rng_(rng),
checker_(checker),
exclusion_zones_(exclusion_zones),
asset_info_library_(asset_info_library),
all_(all),
asset_library_(asset_library),
grid_(grid),
occupancy_(occupancy),
spawn_resolution_(occupancy ? occupancy->resolution() : grid_.default_resolution()),
map_grid_settings_(MapGridSettings::defaults())
{}

SpawnContext::Point SpawnContext::get_area_center(const Area& area) const {
	return area.get_center();
}

SpawnContext::Point SpawnContext::get_point_within_area(const Area& area) {
        auto [minx, miny, maxx, maxy] = area.get_bounds();
        for (int i = 0; i < 100; ++i) {
                int x = std::uniform_int_distribution<int>(minx, maxx)(rng_);
                int y = std::uniform_int_distribution<int>(miny, maxy)(rng_);
                if (area.contains_point(SDL_Point{ x, y })) return SDL_Point{ x, y };
        }
        return SDL_Point{0, 0};
}

Asset* SpawnContext::spawnAssetInternal(const std::string& name,
                                        const std::shared_ptr<AssetInfo>& info,
                                        const Area& area,
                                        SDL_Point pos,
                                        int depth,
                                        const std::string& spawn_id,
                                        const std::string& spawn_method,
                                        const std::optional<Asset::TilingInfo>& tiling)
{

        if (clip_area_ && !position_allowed(*clip_area_, pos)) {
                return nullptr;
        }
        auto assetPtr = std::make_unique<Asset>(info, area, pos, depth, spawn_id, spawn_method, spawn_resolution_);
        Asset* raw = assetPtr.get();
        all_.push_back(std::move(assetPtr));
        if (tiling && raw) {
                raw->set_tiling_info(*tiling);
        } else if (raw) {
                raw->set_tiling_info(std::nullopt);
        }
        return raw;
}

Asset* SpawnContext::spawnAsset(const std::string& name,
                                const std::shared_ptr<AssetInfo>& info,
                                const Area& area,
                                SDL_Point pos,
                                int depth,
                                const std::string& spawn_id,
                                const std::string& spawn_method)
{
        if (info && info->tillable) {
                return spawnTiledAsset(name, info, area, pos, depth, spawn_id, spawn_method);
        }
        return spawnAssetInternal(name, info, area, pos, depth, spawn_id, spawn_method, std::nullopt);
}

Asset* SpawnContext::spawnTiledAsset(const std::string& name,
                                     const std::shared_ptr<AssetInfo>& info,
                                     const Area& area,
                                     SDL_Point pos,
                                     int depth,
                                     const std::string& spawn_id,
                                     const std::string& spawn_method)
{
        if (!info) {
                return spawnAssetInternal(name, info, area, pos, depth, spawn_id, spawn_method, std::nullopt);
        }

        Asset::TilingInfo tiling{};
        tiling.enabled = true;

        const int raw_w = std::max(1, info->original_canvas_width);
        const int raw_h = std::max(1, info->original_canvas_height);
        double scale = 1.0;
        if (std::isfinite(info->scale_factor) && info->scale_factor > 0.0f) {
                scale = static_cast<double>(info->scale_factor);
        }
        const int fallback_step = std::max(
            1,
            static_cast<int>(std::lround(
                static_cast<double>(std::max(raw_w, raw_h)) * scale)));
        const int tile_step = resolve_tiled_asset_step_px(map_grid_settings_, fallback_step);
        int tile_w = tile_step;
        int tile_h = tile_step;

        if (tile_w <= 0 || tile_h <= 0) {
                return spawnAssetInternal(name, info, area, pos, depth, spawn_id, spawn_method, std::nullopt);
        }

        const auto bounds = area.get_bounds();
        const int min_x = std::get<0>(bounds);
        const int min_y = std::get<1>(bounds);
        const int max_x = std::get<2>(bounds);
        const int max_y = std::get<3>(bounds);

        auto align_down = [](int value, int step) {
                if (step <= 0) {
                        return value;
                }
                const double scaled = std::floor(static_cast<double>(value) / static_cast<double>(step));
                return static_cast<int>(scaled * static_cast<double>(step));
};
        auto align_up = [](int value, int step) {
                if (step <= 0) {
                        return value;
                }
                const double scaled = std::ceil(static_cast<double>(value) / static_cast<double>(step));
                return static_cast<int>(scaled * static_cast<double>(step));
};

        const int origin_x = align_down(min_x, tile_w);
        const int origin_y = align_down(min_y, tile_h);
        const int limit_x  = align_up(max_x, tile_w);
        const int limit_y  = align_up(max_y, tile_h);

        SDL_Point aligned_pos = pos;
        aligned_pos.x = align_down(pos.x, tile_w) + tile_w / 2;
        aligned_pos.y = align_down(pos.y, tile_h) + tile_h / 2;

        tiling.grid_origin = SDL_Point{ origin_x, origin_y };
        tiling.tile_size   = SDL_Point{ tile_w, tile_h };
        tiling.anchor      = aligned_pos;

        int coverage_w = std::max(tile_w, limit_x - origin_x);
        int coverage_h = std::max(tile_h, limit_y - origin_y);
        tiling.coverage = SDL_Rect{ origin_x, origin_y, coverage_w, coverage_h };

        return spawnAssetInternal(name, info, area, aligned_pos, depth, spawn_id, spawn_method, tiling);
}

bool SpawnContext::point_overlaps_trail(SDL_Point pt, const Area* ignore) const {
        for (const Area* trail : trail_areas_) {
                if (!trail) {
                        continue;
                }
                if (ignore && trail == ignore) {
                        continue;
                }
                if (trail->contains_point(pt)) {
                        return true;
                }
        }
        return false;
}

void SpawnContext::set_map_grid_settings(const MapGridSettings& settings) {
        map_grid_settings_ = settings;
        map_grid_settings_.clamp();
        spawn_resolution_ = occupancy_ ? occupancy_->resolution() : vibble::grid::clamp_resolution(map_grid_settings_.grid_resolution);
}

void SpawnContext::set_floor_point_resolver(FloorPointResolver resolver) {
        floor_point_resolver_ = std::move(resolver);
}

world::GridPoint SpawnContext::resolve_floor_point(SDL_Point pos) const {
        if (floor_point_resolver_) {
                return floor_point_resolver_(pos, spawn_resolution_);
        }
    // `pos.y` represents depth (the forward/back axis on the ground plane).
    // Layer height is calculated separately.
        return world::GridPoint::make_virtual(pos.x, 0, pos.y, spawn_resolution_);
}

world::GridPoint SpawnContext::to_grid_point(SDL_Point pos) const {
        return resolve_floor_point(pos);
}

void SpawnContext::set_spacing_filter(std::unordered_set<std::string> names) {
        spacing_filter_storage_ = std::move(names);
        spacing_filter_ = &spacing_filter_storage_;
}

void SpawnContext::set_spacing_filter(const std::unordered_set<std::string>* names) {
        if (names) {
                spacing_filter_storage_.clear();
        }
        spacing_filter_ = names;
}

bool SpawnContext::track_spacing_for(const std::shared_ptr<AssetInfo>& info,
                                     bool enforce_spacing,
                                     bool default_track) const {
        if (!default_track) {
            return false;
        }
        if (enforce_spacing) {
            return true;
        }
        if (!spacing_filter_) {
            return default_track;
        }
        if (!info) {
            return false;
        }
        return spacing_filter_->count(info->name) > 0;
}

bool SpawnContext::position_allowed(const Area& area, SDL_Point pos) const {
        if (area.contains_point(pos)) {
                return true;
        }
        if (!allow_partial_clip_overlap_ || !occupancy_) {
                return false;
        }
        return occupancy_->cell_overlaps(area, pos);
}
