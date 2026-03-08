#include "attack_helpers.hpp"

#include "animation/attack_validation.hpp"
#include "assets/Asset.hpp"

#include <algorithm>
#include <cmath>

namespace {

float sanitize_scale(float value) {
    return (std::isfinite(value) && value > 0.0f) ? value : 1.0f;
}

animation_update::GeometryContext geometry_for(const Asset& asset) {
    animation_update::GeometryContext context{};
    context.anchor = animation_update::detail::bottom_middle_for(asset, asset.world_xz_point());
    context.scale = sanitize_scale(asset.smoothed_scale());
    context.flipped = asset.flipped;
    context.plane = animation_update::CombatPlane::XY;
    return context;
}

animation_update::CombatantSnapshot snapshot_from_asset(const Asset& asset) {
    animation_update::CombatantSnapshot snapshot;
    snapshot.asset_id = asset.spawn_id.empty() ? (asset.info ? asset.info->name : std::string{}) : asset.spawn_id;
    snapshot.asset_name = asset.info ? asset.info->name : std::string{};
    snapshot.frame = asset.current_animation_frame();
    snapshot.transform = geometry_for(asset);
    return snapshot;
}

} // anonymous namespace

namespace animation_update {
namespace custom_controllers {
namespace attack_helpers {

void send_attack_if_hit(Asset* self, Asset* target) {
    if (!self || !target || target == self) {
        return;
    }

    if (!self->info || !self->current_animation_frame() || self->dead || !self->active) {
        return;
    }

    if (!target->info || !target->current_animation_frame() || target->dead || !target->active) {
        return;
    }

    // Create attacker snapshot
    CombatantSnapshot attacker_snapshot = snapshot_from_asset(*self);

    // Create target snapshot
    CombatantSnapshot target_snapshot = snapshot_from_asset(*target);

    // Check for attack hit
    auto attack_opt = AttackValidation::compute_attack_if_hit(
        attacker_snapshot, target_snapshot);

    if (attack_opt.has_value()) {
        target->send_attack(*attack_opt);
    }
}

} // namespace attack_helpers
} // namespace custom_controllers
} // namespace animation_update
