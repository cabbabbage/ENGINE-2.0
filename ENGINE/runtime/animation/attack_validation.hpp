#pragma once

#include <optional>
#include <cstddef>
#include <string>
#include <vector>

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

    struct RankedAttackCandidate {
        std::string animation_id{};
        std::size_t path_index = 0;
        AttackWindowEvaluation evaluation{};
    };

    static std::optional<Attack> compute_attack_if_hit(const Asset& attacker,
                                                       const Asset& target);
    static AttackWindowEvaluation evaluate_attack_window(const Asset& attacker,
                                                         const Asset& target,
                                                         const std::string& attack_animation_id,
                                                         int horizon_frames);
    static AttackWindowEvaluation evaluate_attack_window(const Asset& attacker,
                                                         const Asset& target,
                                                         const std::string& attack_animation_id,
                                                         std::size_t path_index,
                                                         int horizon_frames);
    static std::optional<RankedAttackCandidate> rank_attack_candidates(
        const Asset& attacker,
        const Asset& target,
        const std::vector<std::string>& attack_animation_ids,
        int horizon_frames,
        bool require_clear_hit);
};

}  // namespace animation_update
