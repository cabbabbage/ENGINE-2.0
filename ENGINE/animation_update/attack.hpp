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
    std::string damage_type;

    float hit_x = 0.0f;
    float hit_y = 0.0f;
    float hit_z = 0.0f;

    int source_frame_index = -1;

    std::vector<AttackVector> vectors;

    std::uint64_t timestamp = 0;
    std::uint64_t tick = 0;

    std::size_t count_for_type(const std::string& type) const {
        std::size_t count = 0;
        for (const auto& vector : vectors) {
            if (vector.type == type) {
                ++count;
            }
        }
        return count;
    }

    AttackVector* vector_at(const std::string& type, std::size_t type_index) {
        std::size_t seen = 0;
        for (auto& vector : vectors) {
            if (vector.type != type) continue;
            if (seen == type_index) {
                return &vector;
            }
            ++seen;
        }
        return nullptr;
    }

    const AttackVector* vector_at(const std::string& type, std::size_t type_index) const {
        std::size_t seen = 0;
        for (const auto& vector : vectors) {
            if (vector.type != type) continue;
            if (seen == type_index) {
                return &vector;
            }
            ++seen;
        }
        return nullptr;
    }

    AttackVector& add_vector(AttackVector vec = {}) {
        vectors.push_back(vec);
        return vectors.back();
    }

    bool erase_vector(const std::string& type, std::size_t type_index) {
        std::size_t seen = 0;
        for (auto it = vectors.begin(); it != vectors.end(); ++it) {
            if (it->type != type) continue;
            if (seen == type_index) {
                vectors.erase(it);
                return true;
            }
            ++seen;
        }
        return false;
    }
};

} // namespace animation_update
