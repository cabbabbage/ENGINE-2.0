#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace animation_update {

struct AttackVector {
    std::string type;
    float start_x = 0.0f;
    float start_y = 0.0f;
    float start_z = 0.0f;

    float control_x = 0.0f;
    float control_y = 0.0f;
    float control_z = 0.0f;

    float end_x = 0.0f;
    float end_y = 0.0f;
    float end_z = 0.0f;

    int damage = 0;
};

struct Attack {
    std::string attacker_asset_id;
    std::string attacker_asset_name;
    std::string target_asset_id;
    std::string target_asset_name;

    int damage_amount = 0;

    float hit_x = 0.0f;
    float hit_y = 0.0f;
    float hit_z = 0.0f;

    int source_frame_index = -1;

    std::vector<AttackVector> vectors;

    std::uint64_t timestamp = 0;
    std::uint64_t tick = 0;

    AttackVector& add_vector(AttackVector vec = {}) {
        vectors.push_back(vec);
        return vectors.back();
    }

    bool erase_vector(std::size_t index) {
        if (index >= vectors.size()) {
            return false;
        }
        vectors.erase(vectors.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }
};

} // namespace animation_update
