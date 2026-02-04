#pragma once

#include <cstddef>
#include <vector>

#include "animation/attack.hpp"

namespace animation_update {

struct FrameHitGeometry {
    struct HitBox {
        std::string type;
        float center_x   = 0.0f;
        float center_y   = 0.0f;
        float center_z   = 0.0f;
        float half_width = 0.0f;
        float half_height = 0.0f;
        float rotation_degrees = 0.0f;

        bool is_empty() const {
            return half_width <= 0.0f || half_height <= 0.0f;
        }
};

    std::vector<HitBox> boxes;

};

struct FrameAttackGeometry {
    using Vector = AttackVector;

    std::vector<Vector> vectors;

    Vector* vector_at(std::size_t index) {
        if (index >= vectors.size()) {
            return nullptr;
        }
        return &vectors[index];
    }

    const Vector* vector_at(std::size_t index) const {
        if (index >= vectors.size()) {
            return nullptr;
        }
        return &vectors[index];
    }

    Vector& add_vector(Vector vec = {}) {
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

}
