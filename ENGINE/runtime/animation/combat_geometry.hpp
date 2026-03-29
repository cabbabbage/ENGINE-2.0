#pragma once

#include <algorithm>
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

enum class FrameBoxCornerId : std::uint8_t {
    TL = 0,
    TR = 1,
    BL = 2,
    BR = 3,
};

struct FrameBoxRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool is_valid() const {
        return left <= right && top <= bottom;
    }

    int width() const {
        return right - left;
    }

    int height() const {
        return bottom - top;
    }

    FrameBoxRect normalized_clamped() const {
        FrameBoxRect out = *this;
        if (out.left > out.right) {
            std::swap(out.left, out.right);
        }
        if (out.top > out.bottom) {
            std::swap(out.top, out.bottom);
        }
        out.left = std::max(0, out.left);
        out.top = std::max(0, out.top);
        out.right = std::max(0, out.right);
        out.bottom = std::max(0, out.bottom);
        if (out.right < out.left) {
            out.right = out.left;
        }
        if (out.bottom < out.top) {
            out.bottom = out.top;
        }
        return out;
    }

    static FrameBoxRect from_points(const std::vector<FrameBoxCorner>& points) {
        if (points.empty()) {
            return FrameBoxRect{};
        }
        int min_x = std::max(0, points.front().texture_x);
        int max_x = min_x;
        int min_y = std::max(0, points.front().texture_y);
        int max_y = min_y;
        for (const auto& point : points) {
            const int x = std::max(0, point.texture_x);
            const int y = std::max(0, point.texture_y);
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
        return FrameBoxRect{min_x, min_y, max_x, max_y}.normalized_clamped();
    }
};

struct FrameBoxBase {
    std::string id;
    std::string type;
    std::string name;
    bool enabled = true;
    int frame_start = -1;
    int frame_end = -1;
    std::string anchor_link;
    int extrusion_amount = 0;
    FrameBoxRect rect{};

    bool is_valid() const {
        return !name.empty() && !id.empty();
    }

    void set_rect(const FrameBoxRect& value) {
        rect = value.normalized_clamped();
    }

    FrameBoxCorner corner(FrameBoxCornerId id) const {
        switch (id) {
            case FrameBoxCornerId::TL:
                return FrameBoxCorner{rect.left, rect.top};
            case FrameBoxCornerId::TR:
                return FrameBoxCorner{rect.right, rect.top};
            case FrameBoxCornerId::BL:
                return FrameBoxCorner{rect.left, rect.bottom};
            case FrameBoxCornerId::BR:
                return FrameBoxCorner{rect.right, rect.bottom};
        }
        return FrameBoxCorner{};
    }

    static FrameBoxCornerId corner_id_from_runtime_index(std::size_t index) {
        switch (index % 4) {
            case 0:
                return FrameBoxCornerId::TL;
            case 1:
                return FrameBoxCornerId::TR;
            case 2:
                return FrameBoxCornerId::BR;
            default:
                return FrameBoxCornerId::BL;
        }
    }

    void set_corner_clamped(FrameBoxCornerId id, int texture_x, int texture_y) {
        const int x = std::max(0, texture_x);
        const int y = std::max(0, texture_y);
        switch (id) {
            case FrameBoxCornerId::TL:
                rect.left = std::min(x, rect.right);
                rect.top = std::min(y, rect.bottom);
                break;
            case FrameBoxCornerId::TR:
                rect.right = std::max(x, rect.left);
                rect.top = std::min(y, rect.bottom);
                break;
            case FrameBoxCornerId::BL:
                rect.left = std::min(x, rect.right);
                rect.bottom = std::max(y, rect.top);
                break;
            case FrameBoxCornerId::BR:
                rect.right = std::max(x, rect.left);
                rect.bottom = std::max(y, rect.top);
                break;
        }
    }

    void translate_clamped(int dx, int dy) {
        const int width = rect.width();
        const int height = rect.height();
        int next_left = rect.left + dx;
        int next_top = rect.top + dy;
        if (next_left < 0) {
            next_left = 0;
        }
        if (next_top < 0) {
            next_top = 0;
        }
        rect.left = next_left;
        rect.top = next_top;
        rect.right = rect.left + width;
        rect.bottom = rect.top + height;
    }

    std::array<FrameBoxCorner, 4> to_runtime_clockwise_points() const {
        return std::array<FrameBoxCorner, 4>{
            corner(FrameBoxCornerId::TL),
            corner(FrameBoxCornerId::TR),
            corner(FrameBoxCornerId::BR),
            corner(FrameBoxCornerId::BL),
        };
    }

    void set_position_and_size(int x, int y, int width, int height) {
        const int clamped_width = std::max(0, width);
        const int clamped_height = std::max(0, height);
        set_rect(FrameBoxRect{x, y, x + clamped_width, y + clamped_height});
    }
};

struct FrameHitBox : FrameBoxBase {
};

struct FrameAttackBox : FrameBoxBase {
    int damage_amount = 0;
    std::string meta_json;
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
