#pragma once

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace world { struct GridPoint; }

enum class AnchorScalingMethod {
    Parent = 0,
    Real3DPoint,
    Relative2DAnchorPoint,
    Real3DFloorPoint,
};

namespace anchor_points {

inline constexpr std::string_view anchor_scaling_method_to_token(AnchorScalingMethod method) {
    switch (method) {
        case AnchorScalingMethod::Parent:
            return "parent";
        case AnchorScalingMethod::Real3DPoint:
            return "real_3d_point";
        case AnchorScalingMethod::Relative2DAnchorPoint:
            return "relative_2d_anchor_point";
        case AnchorScalingMethod::Real3DFloorPoint:
            return "real_3d_floor_point";
    }
    return "parent";
}

inline bool anchor_scaling_method_token_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ac = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const char bc = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ac != bc) {
            return false;
        }
    }
    return true;
}

inline AnchorScalingMethod anchor_scaling_method_from_token(std::string_view token,
                                                            AnchorScalingMethod fallback = AnchorScalingMethod::Parent) {
    if (anchor_scaling_method_token_equals(token, "parent")) {
        return AnchorScalingMethod::Parent;
    }
    if (anchor_scaling_method_token_equals(token, "real_3d_point")) {
        return AnchorScalingMethod::Real3DPoint;
    }
    if (anchor_scaling_method_token_equals(token, "relative_2d_anchor_point")) {
        return AnchorScalingMethod::Relative2DAnchorPoint;
    }
    if (anchor_scaling_method_token_equals(token, "real_3d_floor_point")) {
        return AnchorScalingMethod::Real3DFloorPoint;
    }
    return fallback;
}

}

// Lightweight 2D vector used by runtime anchor results.
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
};

// Canonical anchor contract:
// - texture_x/texture_y name an integer pixel in the un-flipped source texture.
// - The anchor refers to the *center* of that pixel: (x + 0.5, y + 0.5).
// - Coordinates may be outside frame bounds for off-sprite anchor placements.
// - +X goes right, +Y goes down in texture space; Z is derived at runtime.
// - Horizontal flips are applied after converting to UV: u = 1 - u when flipped.
// - The same contract is consumed by runtime projection and the editor preview.
struct DisplacedAssetAnchorPoint {
    std::string name;
    int         texture_x = 0;   // Pixel coordinate on the sprite texture (X axis)
    int         texture_y = 0;   // Pixel coordinate on the sprite texture (vertical axis)
    float       depth_offset = 0.0f; // Signed world-pixel offset along camera->anchor ray from the flat texture point (+farther, -closer)
    // Transform inheritance parity:
    // - true  => preserve parent axis orientation
    // - false => invert parent axis orientation
    bool        flip_horizontal = true;
    bool        flip_vertical = true;
    float       rotation_degrees = 0.0f;
    bool        hidden = false;
    bool        resolve_x = true;
    AnchorScalingMethod scaling_method = AnchorScalingMethod::Parent;

    DisplacedAssetAnchorPoint() = default;
    DisplacedAssetAnchorPoint(std::string name_,
                              int tex_x,
                              int tex_y,
                              float depth_offset_px = 0.0f,
                              bool flip_horizontal_ = true,
                              bool flip_vertical_ = true,
                              float rotation_degrees_ = 0.0f,
                              bool hidden_ = false,
                              bool resolve_x_ = true,
                              AnchorScalingMethod scaling_method_ = AnchorScalingMethod::Parent)
        : name(std::move(name_))
        , texture_x(tex_x)
        , texture_y(tex_y)
        , depth_offset(depth_offset_px)
        , flip_horizontal(flip_horizontal_)
        , flip_vertical(flip_vertical_)
        , rotation_degrees(rotation_degrees_)
        , hidden(hidden_)
        , resolve_x(resolve_x_)
        , scaling_method(scaling_method_) {}

    bool is_valid() const {
        return !name.empty();
    }
};

struct ResolvedAnchor {
    Vec2             world_exact_pos_2d{};
    float            world_exact_z = 0.0f;
    Vec2             flat_world_exact_pos_2d{};
    float            flat_world_exact_z = 0.0f;
    float            flat_perspective_scale = 1.0f;
    bool             has_flat_perspective_scale = false;
    SDL_Point        world_px{0, 0};
    int              world_z = 0;
    float            world_depth = 0.0f;
    int              resolution_layer = 0;
    SDL_Point        source_texture_px{0, 0};
    bool             has_canonical_texture_source = false;
    world::GridPoint* grid_point = nullptr;
    bool             missing = false;
    float            depth_offset = 0.0f;
};

// Runtime-facing anchor state used by animation, rendering, and binding helpers.
struct AnchorPoint {
    std::string name;
    int frame_index = -1;
    bool exists = false;
    float depth_offset = 0.0f;
    bool flip_horizontal = false;
    bool flip_vertical = false;
    float rotation_degrees = 0.0f;
    bool hidden = false;
    bool resolve_x = true;
    AnchorScalingMethod scaling_method = AnchorScalingMethod::Parent;
    SDL_FPoint screen_pos_2d{0.0f, 0.0f};
    Vec2 relative_pos_2d{};
    Vec2 world_pos_2d{}; // exact world-space anchor position (render-facing)
    Vec2 world_exact_pos_2d{};
    Vec2 flat_world_pos_2d{}; // exact flat texture/world point (pre-depth-offset)
    Vec2 flat_world_exact_pos_2d{};
    SDL_Point world_quantized_px{0, 0};
    int world_z = 0;
    float world_exact_z = 0.0f;
    float flat_world_exact_z = 0.0f;
    float world_depth = 0.0f;
    int resolution_layer = 0;
    float flat_perspective_scale = 1.0f;
    bool has_flat_perspective_scale = false;

    bool is_active() const { return exists; }
    const Vec2& world_pos() const { return world_pos_2d; }
};

namespace anchor_points {

enum class GridMaterialization {
    None,
    Ensure
};

// Convert a canonical anchor pixel to UV, applying optional horizontal flip.
// UV may fall outside [0, 1] when anchors are authored beyond frame bounds.
inline SDL_FPoint anchor_pixel_to_uv(SDL_Point texture_px,
                                     int texture_w,
                                     int texture_h,
                                     SDL_FlipMode flip = SDL_FLIP_NONE) {
    auto to_unit = [](int pixel, int dimension) {
        if (dimension <= 0) {
            return 0.5f;
        }
        return (static_cast<float>(pixel) + 0.5f) / static_cast<float>(dimension);
    };
    const float frame_u = to_unit(texture_px.x, texture_w);
    const float frame_v = to_unit(texture_px.y, texture_h);
    const bool flip_h = (flip & SDL_FLIP_HORIZONTAL) != 0;
    return SDL_FPoint{flip_h ? (1.0f - frame_u) : frame_u, frame_v};
}

}
