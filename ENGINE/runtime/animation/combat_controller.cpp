#include "animation_update.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>

#include "animation_runtime.hpp"
#include "animation_tag_utils.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_types.hpp"
#include "utils/utils/string_utils.hpp"

AnimationUpdate::AutoMoveCombatOptions AnimationUpdate::resolve_auto_move_combat_options(AutoMoveCombatOverrides overrides) const {
    AutoMoveCombatOptions options;
    if (!self_ || !self_->info) return options;
    bool has_attack_animation = false;
    for (const auto& [animation_id, animation] : self_->info->animations) { (void)animation_id; if (animation_update::tag_utils::has_normalized_tag(animation.tags, "attack")) { has_attack_animation = true; break; } }
    options.attacking_enabled = (asset_types::canonicalize(self_->info->type) == asset_types::enemy && has_attack_animation);
    if (overrides.force_attacking_enabled.has_value()) {
        options.attacking_enabled = *overrides.force_attacking_enabled && has_attack_animation;
    }
    if (overrides.attacking_enabled.has_value()) options.attacking_enabled = *overrides.attacking_enabled;
    return options;
}

MovementTagFilter AnimationUpdate::resolve_movement_tag_filter(const AutoMoveCombatOverrides& overrides) const {
    MovementTagFilter filter{};
    std::unordered_set<std::string> seen_required;
    std::unordered_set<std::string> seen_excluded;

    for (const std::string& tag : overrides.required_movement_tags) {
        const std::string normalized =
            vibble::strings::to_lower_copy(vibble::strings::trim_copy(tag));
        if (!normalized.empty() && seen_required.insert(normalized).second) {
            filter.required_tags.push_back(normalized);
        }
    }

    for (const std::string& tag : overrides.excluded_movement_tags) {
        const std::string normalized =
            vibble::strings::to_lower_copy(vibble::strings::trim_copy(tag));
        if (!normalized.empty() && seen_excluded.insert(normalized).second) {
            filter.excluded_tags.push_back(normalized);
        }
    }

    // Default behavior keeps locomotion and combat tags separate.
    const bool attack_explicitly_required =
        std::find(filter.required_tags.begin(), filter.required_tags.end(), "attack") != filter.required_tags.end();
    if (!attack_explicitly_required &&
        seen_excluded.insert("attack").second) {
        filter.excluded_tags.push_back("attack");
    }

    return filter;
}

bool AnimationUpdate::should_defer_auto_move_for_committed_attack() const { return runtime_ && runtime_->auto_attack_commitment_active(); }
