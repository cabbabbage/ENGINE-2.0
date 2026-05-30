#include "animation/controllers/shared/attack_detection_helper.hpp"

#include "animation/attack_validation.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/log.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

namespace animation_update::custom_controllers {

namespace {

struct XzBounds {
    float min_x = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
    bool valid = false;
};

double attack_detail_warning_ms() {
    const char* raw = SDL_getenv("VIBBLE_ATTACK_DETAIL_WARN_MS");
    if (!raw || !*raw) {
        return 2.0;
    }
    const double parsed = std::atof(raw);
    if (!std::isfinite(parsed)) {
        return 2.0;
    }
    return std::clamp(parsed, 0.1, 5000.0);
}

void include_point(XzBounds& bounds, float x, float z) {
    if (!std::isfinite(x) || !std::isfinite(z)) {
        return;
    }
    bounds.min_x = std::min(bounds.min_x, x);
    bounds.max_x = std::max(bounds.max_x, x);
    bounds.min_z = std::min(bounds.min_z, z);
    bounds.max_z = std::max(bounds.max_z, z);
    bounds.valid = true;
}

XzBounds bounds_for_volumes(const std::vector<Asset::RuntimeBoxVolume>& volumes) {
    XzBounds bounds{};
    for (const Asset::RuntimeBoxVolume& volume : volumes) {
        if (!volume.enabled || !volume.valid) {
            continue;
        }
        for (const Asset::RuntimeBoxPoint3& point : volume.world_points) {
            include_point(bounds, point.x, point.z);
        }
    }
    return bounds;
}

XzBounds expanded_bounds(XzBounds bounds, float padding) {
    if (!bounds.valid) {
        return bounds;
    }
    const float safe_padding = std::max(0.0f, std::isfinite(padding) ? padding : 0.0f);
    bounds.min_x -= safe_padding;
    bounds.max_x += safe_padding;
    bounds.min_z -= safe_padding;
    bounds.max_z += safe_padding;
    return bounds;
}

bool intersects(const XzBounds& lhs, const XzBounds& rhs) {
    return lhs.valid &&
           rhs.valid &&
           lhs.max_x >= rhs.min_x &&
           lhs.min_x <= rhs.max_x &&
           lhs.max_z >= rhs.min_z &&
           lhs.min_z <= rhs.max_z;
}

bool can_send_attacks_from(const Asset* attacker) {
    return attacker &&
           attacker->info &&
           attacker->isAttackBoxEnabled() &&
           attacker->current_animation_frame() &&
           !attacker->dead &&
           attacker->active &&
           !attacker->current_attack_box_volumes().empty();
}

bool can_receive_attacks_from(const Asset* attacker, const Asset* target) {
    if (!attacker || !target || attacker == target) {
        return false;
    }

    if (!target->info || !target->current_animation_frame() || target->dead || !target->active) {
        return false;
    }
    if (!target->isHitboxEnabled()) {
        return false;
    }

    if (target->current_hit_box_volumes().empty()) {
        return false;
    }

    // Attached children should never damage their owning parent.
    if (target->has_child(attacker)) {
        return false;
    }

    return true;
}

bool may_overlap_attack_bounds(const XzBounds& attack_bounds, const Asset* target) {
    if (!target) {
        return false;
    }
    const XzBounds target_bounds = expanded_bounds(bounds_for_volumes(target->current_hit_box_volumes()), 8.0f);
    return intersects(attack_bounds, target_bounds);
}

} // namespace

bool AttackDetectionHelper::send_attack_if_hit(Asset* attacker, Asset* target) {
    if (!can_send_attacks_from(attacker) || !can_receive_attacks_from(attacker, target)) {
        return false;
    }

    const auto attack_opt = AttackValidation::compute_attack_if_hit(*attacker, *target);
    if (attack_opt.has_value()) {
        target->send_attack(*attack_opt);
        return true;
    }
    return false;
}

int AttackDetectionHelper::send_attacks_to_active_targets(Asset* attacker, Assets* assets) {
    if (!can_send_attacks_from(attacker) || !assets) {
        return 0;
    }

    const std::uint64_t freq = SDL_GetPerformanceFrequency();
    const std::uint64_t begin = SDL_GetPerformanceCounter();
    std::size_t considered = 0;
    std::size_t narrowed = 0;
    std::size_t validated = 0;
    std::size_t hits = 0;
    const XzBounds attack_bounds = expanded_bounds(bounds_for_volumes(attacker->current_attack_box_volumes()), 16.0f);
    if (!attack_bounds.valid) {
        return 0;
    }

    const auto& active_assets = assets->getActive();
    for (Asset* target : active_assets) {
        ++considered;
        if (!can_receive_attacks_from(attacker, target)) {
            continue;
        }
        if (!may_overlap_attack_bounds(attack_bounds, target)) {
            continue;
        }
        ++narrowed;

        ++validated;
        const auto attack_opt = AttackValidation::compute_attack_if_hit(*attacker, *target);
        if (attack_opt.has_value()) {
            target->send_attack(*attack_opt);
            ++hits;
        }
    }

    if (freq != 0) {
        const std::uint64_t end = SDL_GetPerformanceCounter();
        if (end > begin) {
            const double elapsed_ms =
                static_cast<double>(end - begin) * 1000.0 / static_cast<double>(freq);
            if (elapsed_ms >= attack_detail_warning_ms()) {
                const std::string attacker_name =
                    (attacker->info && !attacker->info->name.empty()) ? attacker->info->name : std::string{"<unknown>"};
                vibble::log::warn("[AttackDetection] Slow active target scan attacker='" + attacker_name +
                                  "' ms=" + std::to_string(elapsed_ms) +
                                  " active=" + std::to_string(active_assets.size()) +
                                  " considered=" + std::to_string(considered) +
                                  " narrowed=" + std::to_string(narrowed) +
                                  " validated=" + std::to_string(validated) +
                                  " hits=" + std::to_string(hits));
            }
        }
    }
    return static_cast<int>(hits);
}

} // namespace animation_update::custom_controllers
