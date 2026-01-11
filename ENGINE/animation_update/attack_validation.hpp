#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <SDL.h>

#include "animation_update/attack.hpp"
#include "animation_update/combat_geometry.hpp"

class AnimationFrame;

namespace animation_update {

/// Specifies which plane (XY or XZ) is used when interpreting planar coordinates.
enum class CombatPlane {
    XY,
    XZ,
};

/// Describes how to map local attack/hit coordinates into world space.
struct GeometryContext {
    SDL_Point anchor{0, 0};
    float scale = 1.0f;
    bool flipped = false;
    CombatPlane plane = CombatPlane::XY;
};

/// A frame snapshot plus the world transform needed to convert its geometry.
struct CombatantSnapshot {
    std::string asset_id;
    std::string asset_name;
    const AnimationFrame* frame = nullptr;
    GeometryContext transform{};
};

/// Describes leaf attachments that also emit attack geometry.
struct ChildAttachmentSnapshot {
    std::string asset_id;
    std::string asset_name;
    const AnimationFrame* frame = nullptr;
    GeometryContext transform{};
};

class AttackValidation {
public:
    static constexpr std::size_t kDefaultAttackSegments = 12;

    /// Samples a quadratic attack vector in world space using the supplied transform.
    static std::vector<SDL_FPoint> attack_vector_path(const FrameAttackGeometry::Vector& vector,
                                                      const GeometryContext& context,
                                                      std::size_t segments = kDefaultAttackSegments);

    /// Returns the four world-space corners for the given hit box.
    static std::vector<SDL_FPoint> hitbox_polygon(const FrameHitGeometry::HitBox& box,
                                                  const GeometryContext& context);

    /// Checks attacker + child snapshots against target hitboxes and returns the first matching Attack.
    static std::optional<Attack> compute_attack_if_hit(const CombatantSnapshot& attacker,
                                                       const CombatantSnapshot& target,
                                                       const std::vector<ChildAttachmentSnapshot>& child_frames,
                                                       std::size_t path_segments = kDefaultAttackSegments);
};

} // namespace animation_update
