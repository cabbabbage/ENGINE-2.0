#pragma once

class Asset;
class Assets;

namespace animation_update::custom_controllers {

class AttackDetectionHelper {
public:
    static void send_attack_if_hit(Asset* attacker, Asset* target);
    static void send_attacks_to_active_targets(Asset* attacker, Assets* assets);
    static void process_pending_attacks_default(Asset* self);
};

} // namespace animation_update::custom_controllers
