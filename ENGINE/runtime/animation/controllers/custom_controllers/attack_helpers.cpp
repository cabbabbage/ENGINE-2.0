#include "attack_helpers.hpp"

#include "animation/attack_validation.hpp"
#include "assets/asset/Asset.hpp"

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

    auto attack_opt = AttackValidation::compute_attack_if_hit(*self, *target);

    if (attack_opt.has_value()) {
        target->send_attack(*attack_opt);
    }
}

} // namespace attack_helpers
} // namespace custom_controllers
} // namespace animation_update
