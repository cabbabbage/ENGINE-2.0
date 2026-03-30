#include "devtools/room_editor_drag_solver.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "assets/asset/Asset.hpp"
#include "utils/utils/AnchorPointResolver.hpp"

namespace devmode::room_editor_drag_solver {

bool solve_texture_point_for_world_target(const Asset& asset,
                                          const DisplacedAssetAnchorPoint& anchor_template,
                                          float desired_world_x,
                                          float desired_world_y,
                                          int initial_texture_x,
                                          int initial_texture_y,
                                          int& out_texture_x,
                                          int& out_texture_y) {
    int tex_x = initial_texture_x;
    int tex_y = initial_texture_y;

    auto sample_flat_world = [&](int x, int y, anchor_points::AnchorWorldPoint3& out_flat_world) {
        DisplacedAssetAnchorPoint sample_anchor = anchor_template;
        sample_anchor.texture_x = x;
        sample_anchor.texture_y = y;
        const auto sample = anchor_points::resolve_frame_anchor_sample(
            asset,
            sample_anchor,
            anchor_points::GridMaterialization::None);
        out_flat_world = sample.flat_relative_pixel_point;
        return out_flat_world.valid &&
               std::isfinite(out_flat_world.x) &&
               std::isfinite(out_flat_world.y);
    };

    constexpr int kSolveIterations = 8;
    constexpr float kMaxSolveStepPixels = 256.0f;
    for (int iter = 0; iter < kSolveIterations; ++iter) {
        anchor_points::AnchorWorldPoint3 base{};
        anchor_points::AnchorWorldPoint3 plus_x{};
        anchor_points::AnchorWorldPoint3 plus_y{};
        if (!sample_flat_world(tex_x, tex_y, base) ||
            !sample_flat_world(tex_x + 1, tex_y, plus_x) ||
            !sample_flat_world(tex_x, tex_y + 1, plus_y)) {
            break;
        }

        const float vx_x = plus_x.x - base.x;
        const float vx_y = plus_x.y - base.y;
        const float vy_x = plus_y.x - base.x;
        const float vy_y = plus_y.y - base.y;
        const float det = vx_x * vy_y - vx_y * vy_x;
        if (std::fabs(det) < 1e-5f) {
            break;
        }

        const float rx = desired_world_x - base.x;
        const float ry = desired_world_y - base.y;
        float dx = (rx * vy_y - ry * vy_x) / det;
        float dy = (ry * vx_x - rx * vx_y) / det;
        if (!std::isfinite(dx) || !std::isfinite(dy)) {
            break;
        }
        dx = std::clamp(dx, -kMaxSolveStepPixels, kMaxSolveStepPixels);
        dy = std::clamp(dy, -kMaxSolveStepPixels, kMaxSolveStepPixels);

        const int next_x = static_cast<int>(std::lround(static_cast<float>(tex_x) + dx));
        const int next_y = static_cast<int>(std::lround(static_cast<float>(tex_y) + dy));
        if (next_x == tex_x && next_y == tex_y) {
            break;
        }
        tex_x = next_x;
        tex_y = next_y;
    }

    int best_x = tex_x;
    int best_y = tex_y;
    float best_dist_sq = std::numeric_limits<float>::max();
    auto consider_candidate = [&](int candidate_x, int candidate_y) {
        anchor_points::AnchorWorldPoint3 flat_world{};
        if (!sample_flat_world(candidate_x, candidate_y, flat_world)) {
            return;
        }
        const float diff_x = flat_world.x - desired_world_x;
        const float diff_y = flat_world.y - desired_world_y;
        const float dist_sq = diff_x * diff_x + diff_y * diff_y;
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_x = candidate_x;
            best_y = candidate_y;
        }
    };

    constexpr int kLocalSearchRadius = 3;
    for (int dy = -kLocalSearchRadius; dy <= kLocalSearchRadius; ++dy) {
        for (int dx = -kLocalSearchRadius; dx <= kLocalSearchRadius; ++dx) {
            consider_candidate(tex_x + dx, tex_y + dy);
        }
    }

    if (!std::isfinite(best_dist_sq)) {
        return false;
    }

    out_texture_x = best_x;
    out_texture_y = best_y;
    return true;
}

}  // namespace devmode::room_editor_drag_solver

