#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "animation/controllers/shared/attack_payload.hpp"

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
    bool flatten_bottom_to_floor = false;
    FrameBoxRect rect{};
    float rotation_degrees = 0.0f;

    bool is_valid() const {
        return !name.empty() && !id.empty();
    }

    void set_rect(const FrameBoxRect& value) {
        rect = value.normalized_clamped();
    }

    static float sanitize_rotation_degrees(float value) {
        if (!std::isfinite(value)) {
            return 0.0f;
        }
        float normalized = std::fmod(value, 360.0f);
        if (normalized > 180.0f) {
            normalized -= 360.0f;
        } else if (normalized < -180.0f) {
            normalized += 360.0f;
        }
        return normalized;
    }

    float normalized_rotation_degrees() const {
        return sanitize_rotation_degrees(rotation_degrees);
    }

    void set_rotation_degrees(float value) {
        rotation_degrees = sanitize_rotation_degrees(value);
    }

    FrameBoxCorner corner(FrameBoxCornerId id) const {
        const auto corner_xy = corner_float(id);
        return FrameBoxCorner{
            std::max(0, static_cast<int>(std::lround(corner_xy[0]))),
            std::max(0, static_cast<int>(std::lround(corner_xy[1]))),
        };
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
        const float rotation = normalized_rotation_degrees();
        if (std::fabs(rotation) <= 1e-4f) {
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
            return;
        }

        auto opposite_corner_id = [](FrameBoxCornerId corner_id) {
            switch (corner_id) {
                case FrameBoxCornerId::TL:
                    return FrameBoxCornerId::BR;
                case FrameBoxCornerId::TR:
                    return FrameBoxCornerId::BL;
                case FrameBoxCornerId::BL:
                    return FrameBoxCornerId::TR;
                case FrameBoxCornerId::BR:
                default:
                    return FrameBoxCornerId::TL;
            }
        };

        auto corner_signs = [](FrameBoxCornerId corner_id, float& sx, float& sy) {
            switch (corner_id) {
                case FrameBoxCornerId::TL:
                    sx = -1.0f;
                    sy = -1.0f;
                    break;
                case FrameBoxCornerId::TR:
                    sx = 1.0f;
                    sy = -1.0f;
                    break;
                case FrameBoxCornerId::BL:
                    sx = -1.0f;
                    sy = 1.0f;
                    break;
                case FrameBoxCornerId::BR:
                default:
                    sx = 1.0f;
                    sy = 1.0f;
                    break;
            }
        };

        const auto opposite_corner = corner_float(opposite_corner_id(id));
        const float ox = opposite_corner[0];
        const float oy = opposite_corner[1];
        const float dx = static_cast<float>(x) - ox;
        const float dy = static_cast<float>(y) - oy;

        const float radians = rotation * static_cast<float>(3.14159265358979323846 / 180.0);
        const float cos_theta = std::cos(radians);
        const float sin_theta = std::sin(radians);
        const float ux = cos_theta;
        const float uy = sin_theta;
        const float vx = -sin_theta;
        const float vy = cos_theta;

        float sign_x = 1.0f;
        float sign_y = 1.0f;
        corner_signs(id, sign_x, sign_y);

        const float projected_u = (dx * ux) + (dy * uy);
        const float projected_v = (dx * vx) + (dy * vy);
        const float half_w = std::max(0.0f, (sign_x * projected_u) * 0.5f);
        const float half_h = std::max(0.0f, (sign_y * projected_v) * 0.5f);

        const float offset_x = (sign_x * half_w * ux) + (sign_y * half_h * vx);
        const float offset_y = (sign_x * half_w * uy) + (sign_y * half_h * vy);
        const float cx = ox + offset_x;
        const float cy = oy + offset_y;

        rect.left = static_cast<int>(std::lround(cx - half_w));
        rect.top = static_cast<int>(std::lround(cy - half_h));
        rect.right = static_cast<int>(std::lround(cx + half_w));
        rect.bottom = static_cast<int>(std::lround(cy + half_h));
        rect = rect.normalized_clamped();
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

private:
    std::array<float, 2> corner_float(FrameBoxCornerId id) const {
        const float center_x = (static_cast<float>(rect.left) + static_cast<float>(rect.right)) * 0.5f;
        const float center_y = (static_cast<float>(rect.top) + static_cast<float>(rect.bottom)) * 0.5f;
        const float half_w = static_cast<float>(std::max(0, rect.width())) * 0.5f;
        const float half_h = static_cast<float>(std::max(0, rect.height())) * 0.5f;

        float local_x = 0.0f;
        float local_y = 0.0f;
        switch (id) {
            case FrameBoxCornerId::TL:
                local_x = -half_w;
                local_y = -half_h;
                break;
            case FrameBoxCornerId::TR:
                local_x = half_w;
                local_y = -half_h;
                break;
            case FrameBoxCornerId::BL:
                local_x = -half_w;
                local_y = half_h;
                break;
            case FrameBoxCornerId::BR:
                local_x = half_w;
                local_y = half_h;
                break;
        }

        const float radians = normalized_rotation_degrees() * static_cast<float>(3.14159265358979323846 / 180.0);
        const float cos_theta = std::cos(radians);
        const float sin_theta = std::sin(radians);
        const float rotated_x = center_x + (local_x * cos_theta) - (local_y * sin_theta);
        const float rotated_y = center_y + (local_x * sin_theta) + (local_y * cos_theta);
        return std::array<float, 2>{rotated_x, rotated_y};
    }
};

struct FrameHitBox : FrameBoxBase {
};

struct FrameAttackBox : FrameBoxBase {
    AttackPayload payload{};
    int damage_amount = 0;
    std::string payload_id;
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
