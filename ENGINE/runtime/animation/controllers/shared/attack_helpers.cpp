#include "attack_helpers.hpp"

#include "attack_detection_helper.hpp"

namespace animation_update {
namespace custom_controllers {
namespace attack_helpers {

void send_attack_if_hit(Asset* self, Asset* target) {
    AttackDetectionHelper::send_attack_if_hit(self, target);
}

} // namespace attack_helpers
} // namespace custom_controllers
} // namespace animation_update
