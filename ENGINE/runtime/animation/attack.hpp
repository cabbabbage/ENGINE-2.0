#pragma once

#include <cstdint>
#include <string>

namespace animation_update {

struct Attack {
    std::string attacker_asset_id;
    std::string attacker_asset_name;
    std::string target_asset_id;
    std::string target_asset_name;
    std::string attack_type;

    int damage_amount = 0;

    float hit_x = 0.0f;
    float hit_y = 0.0f;
    float hit_z = 0.0f;

    int source_frame_index = -1;

    std::uint64_t timestamp = 0;
    std::uint64_t tick = 0;
};

} // namespace animation_update
