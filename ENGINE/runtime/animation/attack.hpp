#pragma once

#include <cstdint>
#include <string>

#include "animation/controllers/shared/attack_payload.hpp"

namespace animation_update {

struct Attack {
    std::string attacker_asset_id;
    std::string attacker_asset_name;
    std::string target_asset_id;
    std::string target_asset_name;
    std::string attack_type;
    std::string attack_payload_id;

    int damage_amount = 0;
    AttackPayload payload{};

    float hit_x = 0.0f;
    float hit_y = 0.0f;
    float hit_z = 0.0f;

    int source_frame_index = -1;

    std::uint64_t timestamp = 0;
    std::uint64_t tick = 0;
};

} // namespace animation_update
