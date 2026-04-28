#pragma once

#include <optional>
#include <string>

#include "animation/attack.hpp"

class Asset;

namespace animation_update {

class AttackValidation {
public:
    enum class AttackWindowScore {
        Miss = 0,
        NearHit = 1,
        ClearHit = 2,
    };

    struct AttackWindowEvaluation {
        AttackWindowScore score = AttackWindowScore::Miss;
        std::optional<Attack> attack = std::nullopt;
    };

    static std::optional<Attack> compute_attack_if_hit(const Asset& attacker,
                                                       const Asset& target);
    static AttackWindowEvaluation evaluate_attack_window(const Asset& attacker,
                                                         const Asset& target,
                                                         const std::string& attack_animation_id,
                                                         int horizon_frames);
};

}  // namespace animation_update
