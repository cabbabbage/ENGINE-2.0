#include "FramePointResolver.hpp"

#include <algorithm>
#include <cmath>

#include "animation_update/animation_update.hpp"
#include "asset/Asset.hpp"

namespace devmode::frame_editors {

FramePointResolver::FramePointResolver(const Asset* asset) : asset_(asset) {}

SDL_Point FramePointResolver::anchor_world() const {
    if (!asset_) {
        return SDL_Point{0, 0};
    }
    return animation_update::detail::bottom_middle_for(*asset_, asset_->pos);
}

float FramePointResolver::base_world_z() const {
    if (!asset_) {
        return 0.0f;
    }
    return asset_->world_z_offset();
}

float FramePointResolver::parent_height_px() const {
    if (!asset_) {
        return 0.0f;
    }
    int h = asset_->cached_h;
    if (h <= 0 && asset_->info) {
        h = asset_->info->original_canvas_height;
    }
    float scale = std::isfinite(asset_->current_scale) && asset_->current_scale > 0.0f
                      ? asset_->current_scale
                      : 1.0f;
    float height_px = static_cast<float>(std::max(h, 0)) * scale;
    if (!std::isfinite(height_px) || height_px < 0.0f) {
        height_px = 0.0f;
    }
    return height_px;
}

float FramePointResolver::to_world_z(float z_percent) const {
    const float height = parent_height_px();
    const float clamped = std::clamp(z_percent, 0.0f, 1.0f);
    if (height <= 0.0f || !std::isfinite(height)) {
        return base_world_z();
    }
    return base_world_z() + height * clamped;
}

float FramePointResolver::to_percent(float world_z) const {
    const float height = parent_height_px();
    if (height <= 0.0f || !std::isfinite(height)) {
        return 0.0f;
    }
    const float percent = (world_z - base_world_z()) / height;
    if (!std::isfinite(percent)) {
        return 0.0f;
    }
    return std::clamp(percent, 0.0f, 1.0f);
}

FramePointResolver::WorldPoint FramePointResolver::resolve_from_anchor_offset(
    const SDL_FPoint& offset_from_anchor,
    float z_percent) const {
    SDL_Point anchor = anchor_world();
    WorldPoint out;
    out.xy = SDL_FPoint{
        static_cast<float>(anchor.x) + offset_from_anchor.x,
        static_cast<float>(anchor.y) + offset_from_anchor.y};
    out.z = to_world_z(z_percent);
    return out;
}

}  // namespace devmode::frame_editors
