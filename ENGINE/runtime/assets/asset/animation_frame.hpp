#pragma once

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <cassert>
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
    float rotation_degrees = 0.0f;
    bool z_resort = true;
    SDL_Color rgb{255, 255, 255, 255};
    int frame_index = -1;
    AnimationFrame* prev = nullptr;
    AnimationFrame* next = nullptr;
    bool is_last = false;
    bool is_first = false;

    FrameVariant variant;
    std::vector<DisplacedAssetAnchorPoint> anchor_points;

    // Single texture per frame — GPU mipmaps handle scaling
    SDL_Texture* get_base_texture(int /*index*/ = 0) const {
        return variant.get_base_texture();
    }


    animation_update::FrameHitBoxes hit_boxes;
    animation_update::FrameAttackBoxes attack_boxes;

    const animation_update::FrameHitBoxes& get_hit_boxes() const {
        return hit_boxes;
    }
    animation_update::FrameHitBoxes& mutable_hit_boxes() {
        return hit_boxes;
    }

    const animation_update::FrameAttackBoxes& get_attack_boxes() const {
        return attack_boxes;
    }
    animation_update::FrameAttackBoxes& mutable_attack_boxes() {
        return attack_boxes;
    }

    const animation_update::FrameHitBox* find_hit_box(const std::string& name) const {
        if (hit_box_lookup_.empty() && !hit_boxes.boxes.empty()) {
            const_cast<AnimationFrame*>(this)->rebuild_hit_box_lookup();
        }
        auto it = hit_box_lookup_.find(name);
        if (it != hit_box_lookup_.end() && it->second < hit_boxes.boxes.size()) {
            return &hit_boxes.boxes[it->second];
        }
        return nullptr;
    }

    const animation_update::FrameAttackBox* find_attack_box(const std::string& name) const {
        if (attack_box_lookup_.empty() && !attack_boxes.boxes.empty()) {
            const_cast<AnimationFrame*>(this)->rebuild_attack_box_lookup();
        }
        auto it = attack_box_lookup_.find(name);
        if (it != attack_box_lookup_.end() && it->second < attack_boxes.boxes.size()) {
            return &attack_boxes.boxes[it->second];
        }
        return nullptr;
    }

    void rebuild_hit_box_lookup() {
        hit_box_lookup_.clear();
        for (std::size_t i = 0; i < hit_boxes.boxes.size(); ++i) {
            const auto& box = hit_boxes.boxes[i];
            if (!box.name.empty()) {
                hit_box_lookup_[box.name] = i;
            }
        }
    }

    void rebuild_attack_box_lookup() {
        attack_box_lookup_.clear();
        for (std::size_t i = 0; i < attack_boxes.boxes.size(); ++i) {
            const auto& box = attack_boxes.boxes[i];
            if (!box.name.empty()) {
                attack_box_lookup_[box.name] = i;
            }
        }
    }

    void set_hit_boxes(std::vector<animation_update::FrameHitBox> boxes) {
#if !defined(NDEBUG)
        std::unordered_set<std::string> seen_ids;
        for (const auto& box : boxes) {
            if (!box.id.empty()) {
                assert(seen_ids.insert(box.id).second && "Hit box IDs must be unique per frame");
            }
        }
#endif
        hit_boxes.boxes = std::move(boxes);
        rebuild_hit_box_lookup();
        if (box_trace_enabled()) {
            SDL_Log("[BoxFlow][set_hit_boxes] frame=%d count=%zu ids=%s",
                    frame_index,
                    hit_boxes.boxes.size(),
                    box_ids_csv(hit_boxes.boxes).c_str());
        }
    }

    void set_attack_boxes(std::vector<animation_update::FrameAttackBox> boxes) {
#if !defined(NDEBUG)
        std::unordered_set<std::string> seen_ids;
        for (const auto& box : boxes) {
            if (!box.id.empty()) {
                assert(seen_ids.insert(box.id).second && "Attack box IDs must be unique per frame");
            }
        }
#endif
        attack_boxes.boxes = std::move(boxes);
        rebuild_attack_box_lookup();
        if (box_trace_enabled()) {
            SDL_Log("[BoxFlow][set_attack_boxes] frame=%d count=%zu ids=%s",
                    frame_index,
                    attack_boxes.boxes.size(),
                    box_ids_csv(attack_boxes.boxes).c_str());
        }
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
    static bool box_trace_enabled() {
        static const bool enabled = [] {
            const char* raw = SDL_getenv("VIBBLE_BOX_TRACE");
            if (!raw || !*raw) {
                return false;
            }
            const std::string value(raw);
            return value == "1" || value == "true" || value == "TRUE" || value == "on" || value == "ON";
        }();
        return enabled;
    }

    template <typename TBox>
    static std::string box_ids_csv(const std::vector<TBox>& boxes) {
        std::ostringstream out;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (i > 0) {
                out << ",";
            }
            out << boxes[i].id;
        }
        return out.str();
    }

    std::unordered_map<std::string, std::size_t> anchor_lookup_;
    std::unordered_map<std::string, std::size_t> hit_box_lookup_;
    std::unordered_map<std::string, std::size_t> attack_box_lookup_;
};
