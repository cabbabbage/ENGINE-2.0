#include "rendering/render/debug_overlay_renderer.hpp"

#include "rendering/render/warped_screen_grid.hpp"
#include "utils/AnchorPointResolver.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame.hpp"
#include "animation/controllers/shared/anchor_bound_asset_helper.hpp"
#include "animation/controllers/shared/anchored_child_placement.hpp"
#include "animation/controllers/shared/oval_anchor_heading.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

constexpr float kMarkerMarginPx = 16.0f;

bool project_floor_point_to_screen(const WarpedScreenGrid& cam,
                                   float world_x,
                                   float world_z,
                                   SDL_FPoint& out_screen) {
    SDL_FPoint linear_screen{};
    if (!cam.project_world_point(SDL_FPoint{world_x, 0.0f}, world_z, linear_screen) ||
        !std::isfinite(linear_screen.x) ||
        !std::isfinite(linear_screen.y)) {
        return false;
    }
    linear_screen.y = cam.warp_floor_screen_y(0.0f, linear_screen.y);
    if (!std::isfinite(linear_screen.y)) {
        return false;
    }
    out_screen = linear_screen;
    return true;
}

bool is_debug_marker_in_bounds(const SDL_FPoint& point, int screen_width, int screen_height) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        return false;
    }
    return point.x >= -kMarkerMarginPx &&
           point.y >= -kMarkerMarginPx &&
           point.x <= static_cast<float>(screen_width) + kMarkerMarginPx &&
           point.y <= static_cast<float>(screen_height) + kMarkerMarginPx;
}

void draw_filled_debug_dot(SDL_Renderer* renderer,
                           const SDL_FPoint& center,
                           int radius_px,
                           const SDL_Color& color) {
    if (!renderer || radius_px <= 0) {
        return;
    }
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const int cx = static_cast<int>(std::lround(center.x));
    const int cy = static_cast<int>(std::lround(center.y));
    for (int dy = -radius_px; dy <= radius_px; ++dy) {
        const int inside = radius_px * radius_px - dy * dy;
        if (inside < 0) {
            continue;
        }
        const int dx = static_cast<int>(std::floor(std::sqrt(static_cast<float>(inside))));
        SDL_RenderLine(renderer,
                       static_cast<float>(cx - dx),
                       static_cast<float>(cy + dy),
                       static_cast<float>(cx + dx),
                       static_cast<float>(cy + dy));
    }
}

void draw_circle_outline(SDL_Renderer* renderer,
                         const SDL_FPoint& center,
                         float radius,
                         const SDL_Color& color) {
    if (!renderer || !std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(radius) || radius <= 0.0f) {
        return;
    }
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    constexpr int kSegments = 48;
    float prev_x = center.x + radius;
    float prev_y = center.y;
    for (int i = 1; i <= kSegments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSegments);
        const float angle = t * 6.28318530717958647692f;
        const float next_x = center.x + std::cos(angle) * radius;
        const float next_y = center.y + std::sin(angle) * radius;
        SDL_RenderLine(renderer, prev_x, prev_y, next_x, next_y);
        prev_x = next_x;
        prev_y = next_y;
    }
}

anchor_points::AnchorWorldPoint3 debug_child_anchor_world_displacement(const AnchorPoint& parent_anchor,
                                                                       Asset* owner_asset,
                                                                       const Asset* child_asset) {
    anchor_points::AnchorWorldPoint3 displacement{};
    if (!child_asset) {
        return displacement;
    }
    const Asset::CumulativeMovementDisplacement cumulative =
        child_asset->current_frame_cumulative_movement_displacement();
    if (!cumulative.valid) {
        return displacement;
    }

    float displacement_x = cumulative.dx;
    float displacement_y = cumulative.dy;
    float displacement_z = cumulative.dz;
    if (parent_anchor.flip_horizontal) {
        displacement_x = -displacement_x;
    }
    if (parent_anchor.flip_vertical) {
        displacement_y = -displacement_y;
    }
    const std::optional<float> heading_radians =
        oval_anchor_heading::resolve_effective_oval_heading_radians(owner_asset, parent_anchor);
    if (heading_radians.has_value()) {
        oval_anchor_heading::rotate_xz_about_world_y(*heading_radians, displacement_x, displacement_z);
    }

    displacement = anchor_points::AnchorWorldPoint3{
        displacement_x,
        displacement_y,
        displacement_z,
        std::isfinite(displacement_x) && std::isfinite(displacement_y) && std::isfinite(displacement_z)};
    return displacement;
}

} // namespace

DebugOverlayRenderer::DebugOverlayRenderer(SDL_Renderer* renderer)
    : renderer_(renderer) {}

void DebugOverlayRenderer::render_light_culling(const std::vector<render_debug::RuntimeLightDebugOverlayEntry>& entries) const {
    if (!renderer_ || entries.empty()) {
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    for (const auto& entry : entries) {
        const SDL_Color color = entry.rendered
            ? SDL_Color{96, 255, 128, 190}
            : SDL_Color{255, 96, 96, 160};
        draw_circle_outline(renderer_, entry.center, entry.radius, color);
        draw_filled_debug_dot(renderer_, entry.center, 2, color);
    }
}

void DebugOverlayRenderer::render_movement_debug(
    const WarpedScreenGrid& cam,
    int screen_width,
    int screen_height,
    const std::unordered_map<const Asset*, render_debug::MovementDebugAssetSnapshot>& snapshots,
    const std::vector<Asset*>& visible_assets) const {
    if (!renderer_ || snapshots.empty() || visible_assets.empty()) {
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    for (const Asset* asset : visible_assets) {
        if (!asset) {
            continue;
        }
        const auto it = snapshots.find(asset);
        if (it == snapshots.end()) {
            continue;
        }

        for (const auto& path : it->second.paths) {
            if (path.world_points.size() < 2) {
                continue;
            }
            SDL_SetRenderDrawColor(renderer_, path.color.r, path.color.g, path.color.b, path.color.a);
            for (std::size_t i = 1; i < path.world_points.size(); ++i) {
                SDL_FPoint a{};
                SDL_FPoint b{};
                if (!project_floor_point_to_screen(cam,
                                                   static_cast<float>(path.world_points[i - 1].x),
                                                   static_cast<float>(path.world_points[i - 1].y),
                                                   a) ||
                    !project_floor_point_to_screen(cam,
                                                   static_cast<float>(path.world_points[i].x),
                                                   static_cast<float>(path.world_points[i].y),
                                                   b)) {
                    continue;
                }
                if (!is_debug_marker_in_bounds(a, screen_width, screen_height) &&
                    !is_debug_marker_in_bounds(b, screen_width, screen_height)) {
                    continue;
                }
                SDL_RenderLine(renderer_, a.x, a.y, b.x, b.y);
            }
        }
    }
}

void DebugOverlayRenderer::render_anchor_debug(const WarpedScreenGrid& cam,
                                               int screen_width,
                                               int screen_height,
                                               const std::vector<Asset*>& visible_assets,
                                               bool /*dev_mode*/) const {
    if (!renderer_ || visible_assets.empty()) {
        return;
    }

    const SDL_Color flat_color{255, 32, 32, 220};
    const SDL_Color final_color{48, 128, 255, 255};
    const SDL_Color anchor_parity_color{255, 224, 64, 255};
    const SDL_Color child_parity_color{64, 255, 160, 255};
    const SDL_Color parity_line_color{255, 224, 64, 190};

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    for (Asset* asset : visible_assets) {
        if (!asset || asset->dead || !asset->current_frame) {
            continue;
        }

        for (const DisplacedAssetAnchorPoint& authored_anchor : asset->current_frame->anchor_points) {
            if (!authored_anchor.is_valid()) {
                continue;
            }

            anchor_points::FrameAnchorSample sample{};
            try {
                sample = anchor_points::resolve_frame_anchor_sample(
                    *asset,
                    authored_anchor,
                    anchor_points::GridMaterialization::None);
            } catch (const std::exception&) {
                continue;
            }

            if (sample.resolved.missing) {
                continue;
            }

            if (sample.has_flat_screen_px &&
                is_debug_marker_in_bounds(sample.flat_screen_px, screen_width, screen_height)) {
                draw_filled_debug_dot(renderer_, sample.flat_screen_px, 5, flat_color);
            }
            if (sample.has_final_screen_px &&
                is_debug_marker_in_bounds(sample.final_screen_px, screen_width, screen_height)) {
                draw_filled_debug_dot(renderer_, sample.final_screen_px, 3, final_color);
            }
        }
    }

    const auto bindings = anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().debug_bindings_snapshot();
    for (const auto& binding : bindings) {
        Asset* owner = binding.owner;
        Asset* child = binding.child_asset;
        if (!owner || !child || owner->dead || child->dead || binding.anchor_name.empty()) {
            continue;
        }

        const auto anchor = owner->anchor_state(binding.anchor_name,
                                                anchor_points::GridMaterialization::None,
                                                Asset::AnchorResolveMode::ForceRecompute);
        if (!anchor.has_value() || !anchor->is_active()) {
            continue;
        }

        anchored_child_placement::PlacementInput expected_input{};
        expected_input.parent.world_x = static_cast<float>(owner->world_x());
        expected_input.parent.world_y = static_cast<float>(owner->world_y());
        expected_input.parent.world_z = static_cast<float>(owner->world_z());
        expected_input.parent.resolution_layer =
            owner->grid_point() ? owner->grid_point()->resolution_layer() : owner->grid_resolution;
        expected_input.anchor_definition.anchor = *anchor;
        const anchor_points::AnchorWorldPoint3 anchor_displacement =
            debug_child_anchor_world_displacement(*anchor, owner, child);
        if (anchor_displacement.valid) {
            expected_input.anchor_world_displacement.x = anchor_displacement.x;
            expected_input.anchor_world_displacement.y = anchor_displacement.y;
            expected_input.anchor_world_displacement.z = anchor_displacement.z;
        }
        expected_input.sprite_transform.mirror_x = anchor->flip_horizontal;
        expected_input.sprite_transform.mirror_y = anchor->flip_vertical;
        expected_input.sprite_transform.rotation_degrees = anchor->rotation_degrees;
        expected_input.camera_state.camera = &cam;

        anchored_child_placement::PlacementOutput expected{};
        if (!anchored_child_placement::resolve_child_placement(expected_input, expected) ||
            !expected.has_child_screen_px) {
            continue;
        }

        SDL_FPoint actual_child_screen{};
        const bool have_actual_child = anchored_child_placement::project_child_pivot_screen(
            cam,
            child->smoothed_translation_x() + child->render_anchor_offset_x(),
            child->smoothed_translation_y() + child->render_anchor_offset_y(),
            static_cast<float>(child->world_z()) + child->world_z_offset() + child->render_anchor_offset_z(),
            actual_child_screen);
        if (!have_actual_child) {
            continue;
        }

        if (is_debug_marker_in_bounds(expected.child_screen_px, screen_width, screen_height)) {
            draw_filled_debug_dot(renderer_, expected.child_screen_px, 4, anchor_parity_color);
        }
        if (is_debug_marker_in_bounds(actual_child_screen, screen_width, screen_height)) {
            draw_filled_debug_dot(renderer_, actual_child_screen, 4, child_parity_color);
        }
        if (is_debug_marker_in_bounds(expected.child_screen_px, screen_width, screen_height) ||
            is_debug_marker_in_bounds(actual_child_screen, screen_width, screen_height)) {
            SDL_SetRenderDrawColor(renderer_,
                                   parity_line_color.r,
                                   parity_line_color.g,
                                   parity_line_color.b,
                                   parity_line_color.a);
            SDL_RenderLine(renderer_,
                           expected.child_screen_px.x,
                           expected.child_screen_px.y,
                           actual_child_screen.x,
                           actual_child_screen.y);
        }

    }
}

void DebugOverlayRenderer::render_impass_floor_debug(
    const WarpedScreenGrid& cam,
    int screen_width,
    int screen_height,
    const std::vector<render_debug::ImpassFloorDebugPolygon>& polygons) const {
    if (!renderer_ || polygons.empty()) {
        return;
    }

    const SDL_Color edge_color{255, 64, 180, 230};
    const SDL_Color point_color{255, 200, 80, 220};
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, edge_color.r, edge_color.g, edge_color.b, edge_color.a);
    for (const auto& polygon : polygons) {
        if (polygon.world_points.size() < 3) {
            continue;
        }
        std::vector<SDL_FPoint> screen_points;
        screen_points.reserve(polygon.world_points.size());
        for (const SDL_Point& pt : polygon.world_points) {
            SDL_FPoint projected{};
            if (project_floor_point_to_screen(cam, static_cast<float>(pt.x), static_cast<float>(pt.y), projected)) {
                screen_points.push_back(projected);
            }
        }
        if (screen_points.size() < 3) {
            continue;
        }
        for (std::size_t i = 0; i < screen_points.size(); ++i) {
            const SDL_FPoint& a = screen_points[i];
            const SDL_FPoint& b = screen_points[(i + 1) % screen_points.size()];
            if (!is_debug_marker_in_bounds(a, screen_width, screen_height) &&
                !is_debug_marker_in_bounds(b, screen_width, screen_height)) {
                continue;
            }
            SDL_RenderLine(renderer_, a.x, a.y, b.x, b.y);
            draw_filled_debug_dot(renderer_, a, 2, point_color);
        }
    }
}
