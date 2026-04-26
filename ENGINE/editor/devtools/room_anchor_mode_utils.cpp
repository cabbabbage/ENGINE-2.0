#include "room_anchor_mode_utils.hpp"

#include <algorithm>
#include <cctype>
#include "utils/string_utils.hpp"
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace devmode::room_anchor_mode {

int wrap_index(int index, int count) {
    if (count <= 0) {
        return 0;
    }
    const int wrapped = index % count;
    return wrapped < 0 ? (wrapped + count) : wrapped;
}

AnchorPointOwner owner_from_light_mode(bool light_mode) {
    return light_mode ? AnchorPointOwner::Light : AnchorPointOwner::NonLight;
}

bool anchor_owned_by_mode(const DisplacedAssetAnchorPoint& anchor, AnchorPointOwner owner) {
    if (!anchor.is_valid()) {
        return false;
    }
    if (owner == AnchorPointOwner::Light) {
        return anchor.has_light_data;
    }
    return !anchor.has_light_data;
}

bool anchor_visible_in_mode(const DisplacedAssetAnchorPoint& anchor,
                            AnchorPointOwner owner,
                            const std::function<bool(const std::string&)>& is_reserved_anchor_name) {
    if (!anchor.is_valid() || !anchor_owned_by_mode(anchor, owner)) {
        return false;
    }
    return !is_reserved_anchor_name || !is_reserved_anchor_name(anchor.name);
}

bool anchor_mutable_in_mode(const DisplacedAssetAnchorPoint& anchor,
                            AnchorPointOwner owner,
                            const std::function<bool(const std::string&)>& is_reserved_anchor_name) {
    return anchor_visible_in_mode(anchor, owner, is_reserved_anchor_name);
}

const DisplacedAssetAnchorPoint* find_anchor_in_mode(
    const std::vector<DisplacedAssetAnchorPoint>& anchors,
    const std::string& name,
    AnchorPointOwner owner,
    const std::function<bool(const std::string&)>& is_reserved_anchor_name) {
    if (name.empty()) {
        return nullptr;
    }
    auto it = std::find_if(anchors.begin(),
                           anchors.end(),
                           [&](const DisplacedAssetAnchorPoint& anchor) {
                               return anchor.name == name &&
                                      anchor_mutable_in_mode(anchor, owner, is_reserved_anchor_name);
                           });
    return it == anchors.end() ? nullptr : &(*it);
}

DisplacedAssetAnchorPoint* find_anchor_in_mode_mutable(
    std::vector<DisplacedAssetAnchorPoint>& anchors,
    const std::string& name,
    AnchorPointOwner owner,
    const std::function<bool(const std::string&)>& is_reserved_anchor_name) {
    if (name.empty()) {
        return nullptr;
    }
    auto it = std::find_if(anchors.begin(),
                           anchors.end(),
                           [&](const DisplacedAssetAnchorPoint& anchor) {
                               return anchor.name == name &&
                                      anchor_mutable_in_mode(anchor, owner, is_reserved_anchor_name);
                           });
    return it == anchors.end() ? nullptr : &(*it);
}

bool rename_anchor_in_mode(std::vector<DisplacedAssetAnchorPoint>& anchors,
                           const std::string& old_name,
                           const std::string& new_name,
                           AnchorPointOwner owner,
                           const std::function<bool(const std::string&)>& is_reserved_anchor_name) {
    if (old_name.empty() || new_name.empty() || old_name == new_name) {
        return false;
    }
    bool changed = false;
    for (auto& anchor : anchors) {
        if (anchor.name != old_name || !anchor_mutable_in_mode(anchor, owner, is_reserved_anchor_name)) {
            continue;
        }
        anchor.name = new_name;
        changed = true;
    }
    return changed;
}

bool delete_anchor_in_mode(std::vector<DisplacedAssetAnchorPoint>& anchors,
                           const std::string& name,
                           AnchorPointOwner owner,
                           const std::function<bool(const std::string&)>& is_reserved_anchor_name) {
    if (name.empty()) {
        return false;
    }
    const auto erase_it = std::remove_if(
        anchors.begin(),
        anchors.end(),
        [&](const DisplacedAssetAnchorPoint& anchor) {
            if (anchor.name != name || !anchor_mutable_in_mode(anchor, owner, is_reserved_anchor_name)) {
                return false;
            }
            if (owner == AnchorPointOwner::Light && !anchor.has_light_data) {
                return false;
            }
            return true;
        });
    if (erase_it == anchors.end()) {
        return false;
    }
    anchors.erase(erase_it, anchors.end());
    return true;
}

std::string make_unique_anchor_name(const std::string& desired_name,
                                    const std::vector<std::string>& existing_names,
                                    const std::string& excluded_name) {
    const std::string trimmed = vibble::strings::trim_copy(desired_name);
    const std::string base = trimmed.empty() ? std::string("anchor") : trimmed;

    std::unordered_set<std::string> used;
    used.reserve(existing_names.size());
    for (const std::string& name : existing_names) {
        if (name.empty() || name == excluded_name) {
            continue;
        }
        used.insert(name);
    }

    if (used.find(base) == used.end()) {
        return base;
    }

    int suffix = 2;
    std::string candidate = base + "_" + std::to_string(suffix);
    while (used.find(candidate) != used.end()) {
        ++suffix;
        candidate = base + "_" + std::to_string(suffix);
    }
    return candidate;
}

std::string next_default_anchor_name(const std::vector<std::string>& existing_names) {
    std::unordered_set<std::string> used;
    used.reserve(existing_names.size());
    for (const std::string& name : existing_names) {
        if (!name.empty()) {
            used.insert(name);
        }
    }

    int suffix = 1;
    std::string candidate = "anchor_" + std::to_string(suffix);
    while (used.find(candidate) != used.end()) {
        ++suffix;
        candidate = "anchor_" + std::to_string(suffix);
    }
    return candidate;
}

SDL_Point default_anchor_position_for_frame(int frame_width, int frame_height) {
    const int clamped_w = std::max(1, frame_width);
    const int clamped_h = std::max(1, frame_height);
    return SDL_Point{clamped_w / 2, clamped_h - 1};
}

DisplacedAssetAnchorPoint make_default_anchor_for_frame(const std::string& name,
                                                        int frame_width,
                                                        int frame_height) {
    const SDL_Point pos = default_anchor_position_for_frame(frame_width, frame_height);
    return DisplacedAssetAnchorPoint{name, pos.x, pos.y, 0};
}

nlohmann::json serialize_anchor_frame(const std::vector<DisplacedAssetAnchorPoint>& anchors) {
    nlohmann::json frame_json = nlohmann::json::array();
    for (const auto& anchor : anchors) {
        if (!anchor.is_valid()) {
            continue;
        }
        frame_json.push_back(nlohmann::json::object({
            {"name", anchor.name},
            {"texture_x", anchor.texture_x},
            {"texture_y", anchor.texture_y},
            {"depth_offset", anchor.depth_offset},
            {"flip_horizontal", anchor.flip_horizontal},
            {"flip_vertical", anchor.flip_vertical},
            {"rotation_degrees", anchor.rotation_degrees},
            {"hidden", anchor.hidden},
            {"resolve_x", anchor.resolve_x},
            {"scaling_method", std::string(anchor_points::anchor_scaling_method_to_token(anchor.scaling_method))},
        }));
        if (anchor.has_light_data) {
            nlohmann::json light_json = nlohmann::json::object();
            light_json["enabled"] = anchor.light.enabled;
            light_json["color"] = nlohmann::json::array(
                {anchor.light.color_r, anchor.light.color_g, anchor.light.color_b});
            light_json["opacity"] = anchor.light.opacity;
            light_json["intensity"] = anchor.light.intensity;
            light_json["radius"] = anchor.light.radius;
            light_json["falloff"] = anchor.light.falloff;
            light_json["shadow_strength"] = anchor.light.shadow_strength;
            light_json["cast_shadows"] = anchor.light.cast_shadows;
            frame_json.back()["light"] = std::move(light_json);
        }
    }
    return frame_json;
}

void normalize_anchor_points_payload(nlohmann::json& animation_payload, std::size_t frame_count) {
    if (!animation_payload.is_object()) {
        animation_payload = nlohmann::json::object();
    }

    nlohmann::json anchor_points = nlohmann::json::array();
    auto existing_it = animation_payload.find("anchor_points");
    if (existing_it != animation_payload.end() && existing_it->is_array()) {
        anchor_points = *existing_it;
    }

    nlohmann::json normalized = nlohmann::json::array();
    for (std::size_t i = 0; i < frame_count; ++i) {
        if (i < anchor_points.size() && anchor_points[i].is_array()) {
            normalized.push_back(anchor_points[i]);
        } else {
            normalized.push_back(nlohmann::json::array());
        }
    }

    animation_payload["anchor_points"] = std::move(normalized);
}

bool write_anchor_frame_to_payload(nlohmann::json& animation_payload,
                                   std::size_t frame_count,
                                   std::size_t frame_index,
                                   const std::vector<DisplacedAssetAnchorPoint>& anchors) {
    if (frame_count == 0 || frame_index >= frame_count) {
        return false;
    }

    normalize_anchor_points_payload(animation_payload, frame_count);
    auto it = animation_payload.find("anchor_points");
    if (it == animation_payload.end() || !it->is_array() || frame_index >= it->size()) {
        return false;
    }

    (*it)[frame_index] = serialize_anchor_frame(anchors);
    return true;
}

}  // namespace devmode::room_anchor_mode
