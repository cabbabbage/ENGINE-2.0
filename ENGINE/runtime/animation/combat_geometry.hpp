#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace animation_update {

struct FrameBoxCorner {
    int texture_x = 0;
    int texture_y = 0;

    bool is_valid() const {
        return texture_x >= 0 && texture_y >= 0;
    }
};

struct FrameBoxBase {
    std::string name;
    int extrusion_amount = 0;
    std::array<FrameBoxCorner, 4> corners{};

    bool is_valid() const {
        return !name.empty();
    }

    FrameBoxCorner* corner_at(std::size_t index) {
        if (index >= corners.size()) {
            return nullptr;
        }
        return &corners[index];
    }

    const FrameBoxCorner* corner_at(std::size_t index) const {
        if (index >= corners.size()) {
            return nullptr;
        }
        return &corners[index];
    }
};

struct FrameHitBox : FrameBoxBase {
};

struct FrameAttackBox : FrameBoxBase {
    int damage_amount = 0;
};

struct FrameHitBoxes {
    std::vector<FrameHitBox> boxes;

    FrameHitBox* box_at(std::size_t index) {
        if (index >= boxes.size()) {
            return nullptr;
        }
        return &boxes[index];
    }

    const FrameHitBox* box_at(std::size_t index) const {
        if (index >= boxes.size()) {
            return nullptr;
        }
        return &boxes[index];
    }
};

struct FrameAttackBoxes {
    std::vector<FrameAttackBox> boxes;

    FrameAttackBox* box_at(std::size_t index) {
        if (index >= boxes.size()) {
            return nullptr;
        }
        return &boxes[index];
    }

    const FrameAttackBox* box_at(std::size_t index) const {
        if (index >= boxes.size()) {
            return nullptr;
        }
        return &boxes[index];
    }
};

}  // namespace animation_update
