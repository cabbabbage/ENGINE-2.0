#include "animation_update.hpp"

#include <string>

#include "animation_runtime.hpp"
#include "animation_tag_utils.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_types.hpp"

AnimationUpdate::AutoMoveCombatOptions AnimationUpdate::resolve_auto_move_combat_options(AutoMoveCombatOverrides overrides) const {
    AutoMoveCombatOptions options;
    if (!self_ || !self_->info) return options;
    bool has_attack_animation = false;
    for (const auto& [animation_id, animation] : self_->info->animations) { (void)animation_id; if (animation_update::tag_utils::has_normalized_tag(animation.tags, "attack")) { has_attack_animation = true; break; } }
    options.attacking_enabled = (asset_types::canonicalize(self_->info->type) == asset_types::enemy && has_attack_animation);
    if (overrides.attacking_enabled.has_value()) options.attacking_enabled = *overrides.attacking_enabled;
    return options;
}

bool AnimationUpdate::should_defer_auto_move_for_committed_attack() const { return runtime_ && runtime_->auto_attack_commitment_active(); }
