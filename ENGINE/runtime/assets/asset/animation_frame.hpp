#pragma once

#include <vector>
#include <unordered_map>
#include <SDL3/SDL.h>

#include "animation/combat_geometry.hpp"
#include "animation_frame_variant.hpp"
#include "anchor_point.hpp"

class AnimationFrame {
public:
    // Movement in world pixels (for movement frames)
    int dx = 0;
    int dy = 0;
    int dz = 0;
    bool z_resort = true;
    SDL_Color rgb{255, 255, 255, 255};
    int frame_index = -1;
    AnimationFrame* prev = nullptr;
    AnimationFrame* next = nullptr;
    bool is_last = false;
    bool is_first = false;

    std::vector<FrameVariant> variants;
    std::vector<DisplacedAssetAnchorPoint> anchor_points;

    SDL_Texture* get_base_texture(int index) const {
        return variants[index].get_base_texture();
    }

    SDL_Texture* get_foreground_texture(int index) const {
        return variants[index].get_foreground_texture();
    }

    SDL_Texture* get_background_texture(int index) const {
        return variants[index].get_background_texture();
    }


    animation_update::FrameHitGeometry hit_geometry;
    animation_update::FrameAttackGeometry attack_geometry;

    const animation_update::FrameHitGeometry& get_hit_geometry() const {
        return hit_geometry;
    }
    animation_update::FrameHitGeometry& mutable_hit_geometry() {
        return hit_geometry;
    }

    const animation_update::FrameAttackGeometry& get_attack_geometry() const {
        return attack_geometry;
    }
    animation_update::FrameAttackGeometry& mutable_attack_geometry() {
        return attack_geometry;
    }

    const DisplacedAssetAnchorPoint* find_anchor(const std::string& name) const {
        if (anchor_lookup_.empty() && !anchor_points.empty()) {
            const_cast<AnimationFrame*>(this)->rebuild_anchor_lookup();
        }
        auto it = anchor_lookup_.find(name);
        if (it != anchor_lookup_.end() && it->second < anchor_points.size()) {
            return &anchor_points[it->second];
        }
        return nullptr;
    }

    void rebuild_anchor_lookup() {
        anchor_lookup_.clear();
        for (std::size_t i = 0; i < anchor_points.size(); ++i) {
            const auto& anchor = anchor_points[i];
            if (!anchor.name.empty()) {
                anchor_lookup_[anchor.name] = i;
            }
        }
    }

    void set_anchor_points(std::vector<DisplacedAssetAnchorPoint> anchors) {
        anchor_points = std::move(anchors);
        rebuild_anchor_lookup();
    }

private:
    std::unordered_map<std::string, std::size_t> anchor_lookup_;
};
