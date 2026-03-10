#pragma once

#include <optional>

#include "animation/attack.hpp"

class Asset;

namespace animation_update {

class AttackValidation {
public:
    static std::optional<Attack> compute_attack_if_hit(const Asset& attacker,
                                                       const Asset& target);
};

}  // namespace animation_update
