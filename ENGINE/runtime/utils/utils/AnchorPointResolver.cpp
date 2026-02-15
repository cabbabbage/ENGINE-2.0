#include "AnchorPointResolver.hpp"

#include <cmath>
#include <limits>

#include "animation/animation_update.hpp"
#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/world/world_grid.hpp"

namespace {

int grid_floor_div(int numerator, int denominator) {
    if (denominator == 0) {
        return 0;
    }
    return static_cast<int>(std::floor(static_cast<double>(numerator) / static_cast<double>(denominator)));
}

struct LayerChoice {
    int layer = 0;
    SDL_Point snapped{0, 0};
    int error = std::numeric_limits<int>::max();
    int spacing = std::numeric_limits<int>::max();
};

struct AnchorComputation {
    SDL_Point world_px{0, 0};
    SDL_Point snapped_px{0, 0};
    int world_z = 0;
    int resolution_layer = 0;
    float rotation_deg = 0.0f;
};

LayerChoice choose_layer_for_anchor(const world::WorldGrid& grid, const SDL_Point& target) {
    LayerChoice best{};
    best.layer = grid.default_resolution_layer();
    best.snapped = target;
    best.error = std::numeric_limits<int>::max();
    best.spacing = std::numeric_limits<int>::max();

    const int max_layer = grid.max_resolution_layers();
    const auto origin = grid.origin();

    for (int layer = 0; layer <= max_layer; ++layer) {
        const int spacing = grid.grid_spacing_for_layer(layer);
        if (spacing <= 0) {
            continue;
        }

        const int snapped_x = origin.world_x() + grid_floor_div(target.x - origin.world_x(), spacing) * spacing;
        const int snapped_y = origin.world_y() + grid_floor_div(target.y - origin.world_y(), spacing) * spacing;
        const int error = std::abs(snapped_x - target.x) + std::abs(snapped_y - target.y);

        const bool better_error = (error < best.error);
        const bool equal_error_finer = (error == best.error && spacing < best.spacing);
        if (better_error || equal_error_finer) {
            best.layer = layer;
            best.snapped = SDL_Point{snapped_x, snapped_y};
            best.error = error;
            best.spacing = spacing;
            if (best.error == 0 && best.spacing == 1) {
                break; // can't do better than exact at finest layer
            }
        }
    }

    return best;
}

float anchor_height_px_fallback(const Asset& asset) {
    const float runtime_height = asset.runtime_height_px();
    if (std::isfinite(runtime_height) && runtime_height > 0.0f) {
        return runtime_height;
    }

    int height_px = asset.height();
    if (height_px <= 0) {
        if (SDL_Texture* tex = asset.get_current_frame()) {
            float fw = 0.0f;
            float fh = 0.0f;
            if (SDL_GetTextureSize(tex, &fw, &fh)) {
                height_px = static_cast<int>(std::lround(fh));
            }
        }
    }

    return (height_px > 0) ? static_cast<float>(height_px) : 0.0f;
}

AnchorComputation compute_anchor_world(const Asset& asset, const DisplacedAssetAnchorPoint& anchor) {
    AnchorComputation computed{};
    computed.rotation_deg = anchor.rotation_deg;

    const SDL_Point base = animation_update::detail::bottom_middle_for(asset, asset.world_point());
    const float     height_px = anchor_points::anchor_height_px(asset);
    const float     offset_x = std::isfinite(anchor.px) ? (anchor.px * height_px) : 0.0f;
    const float     offset_y = std::isfinite(anchor.py) ? (anchor.py * height_px) : 0.0f;
    const float     offset_z = std::isfinite(anchor.pz) ? (anchor.pz * height_px) : 0.0f;

    computed.world_px.x = base.x + static_cast<int>(std::lround(offset_x));
    computed.world_px.y = base.y + static_cast<int>(std::lround(offset_y));
    computed.world_z = asset.world_z() + static_cast<int>(std::lround(offset_z));

    computed.resolution_layer = asset.grid_resolution;
    computed.snapped_px = computed.world_px;

    if (auto* assets_owner = asset.get_assets()) {
        world::WorldGrid& grid = assets_owner->world_grid();
        const LayerChoice layer_choice = choose_layer_for_anchor(grid, computed.world_px);
        computed.resolution_layer = layer_choice.layer;
        computed.snapped_px = layer_choice.snapped;
    }

    return computed;
}

}  // namespace

namespace anchor_points {

float anchor_height_px(const Asset& asset) {
    return anchor_height_px_fallback(asset);
}

ResolvedAnchor resolve_anchor_point(const Asset& asset,
                                    const DisplacedAssetAnchorPoint& anchor,
                                    GridMaterialization grid_policy) {
    const AnchorComputation computed = compute_anchor_world(asset, anchor);

    ResolvedAnchor resolved{};
    resolved.world_px = computed.world_px;
    resolved.world_z = computed.world_z;
    resolved.resolution_layer = computed.resolution_layer;
    resolved.rotation_deg = computed.rotation_deg;

    if (auto* assets_owner = asset.get_assets()) {
        world::WorldGrid& grid = assets_owner->world_grid();
        const world::GridKey key{ computed.snapped_px.x, computed.snapped_px.y, computed.world_z, computed.resolution_layer };
        world::GridPoint* point = grid.find_grid_point_strict(key);
        if (!point && grid_policy == GridMaterialization::Ensure) {
            point = &grid.find_or_create_grid_point(key);
        }
        resolved.grid_point = point;
    }

    return resolved;
}

}
