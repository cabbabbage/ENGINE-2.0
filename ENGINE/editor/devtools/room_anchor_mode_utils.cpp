#include "room_anchor_mode_utils.hpp"

#include <algorithm>
#include <cctype>
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

std::string trim_copy(std::string_view value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    std::string out(value);
    out.erase(out.begin(), std::find_if(out.begin(), out.end(), [&](unsigned char ch) {
        return !is_space(ch);
    }));
    out.erase(std::find_if(out.rbegin(), out.rend(), [&](unsigned char ch) {
        return !is_space(ch);
    }).base(), out.end());
    return out;
}

std::string make_unique_anchor_name(const std::string& desired_name,
                                    const std::vector<std::string>& existing_names,
                                    const std::string& excluded_name) {
    const std::string trimmed = trim_copy(desired_name);
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
        }));
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
