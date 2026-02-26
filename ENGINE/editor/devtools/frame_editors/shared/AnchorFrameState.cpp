#include "AnchorFrameState.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <unordered_set>

namespace devmode::frame_editors {

namespace {

float read_float(const nlohmann::json& v, float fallback = 0.0f) {
    if (v.is_number()) {
        try {
            return static_cast<float>(v.get<double>());
        } catch (...) {
        }
    }
    if (v.is_string()) {
        try {
            return std::stof(v.get<std::string>());
        } catch (...) {
        }
    }
    return fallback;
}

std::size_t read_positive_size(const nlohmann::json& v) {
    const float parsed = read_float(v, 0.0f);
    if (!std::isfinite(parsed)) {
        return 0;
    }
    const int rounded = static_cast<int>(std::lround(parsed));
    return rounded > 0 ? static_cast<std::size_t>(rounded) : 0;
}

std::size_t declared_frame_count(const nlohmann::json& payload) {
    if (payload.contains("number_of_frames")) {
        return read_positive_size(payload["number_of_frames"]);
    }
    return 0;
}

std::size_t inferred_frame_count(const nlohmann::json& payload) {
    const std::size_t declared = declared_frame_count(payload);
    const std::size_t movement = (payload.contains("movement") && payload["movement"].is_array())
                                     ? payload["movement"].size()
                                     : 0;
    const std::size_t frames = (payload.contains("frames") && payload["frames"].is_array())
                                   ? payload["frames"].size()
                                   : 0;
    const std::size_t anchors = (payload.contains("anchor_points") && payload["anchor_points"].is_array())
                                    ? payload["anchor_points"].size()
                                    : 0;
    return std::max<std::size_t>(1, std::max({declared, movement, frames, anchors}));
}

bool read_anchor(const nlohmann::json& node, FrameAnchorPoint& pt) {
    if (!node.is_object()) return false;
    if (!node.contains("name") || !node["name"].is_string()) return false;
    if (!node.contains("texture_x") || !node["texture_x"].is_number_integer()) return false;
    if (!node.contains("texture_y") || !node["texture_y"].is_number_integer()) return false;
    if (!node.contains("in_front") || !node["in_front"].is_boolean()) return false;

    pt.name = node["name"].get<std::string>();
    if (pt.name.empty()) return false;
    pt.texture_x = node["texture_x"].get<int>();
    pt.texture_y = node["texture_y"].get<int>();
    if (pt.texture_x < 0 || pt.texture_y < 0) return false;
    pt.in_front = node["in_front"].get<bool>();
    return true;
}

}  // namespace

std::vector<AnchorFrame> parse_anchor_frames_from_payload(const nlohmann::json& payload) {
    const std::size_t frame_count = std::max<std::size_t>(1, inferred_frame_count(payload));
    std::vector<AnchorFrame> frames(frame_count);

    if (!payload.is_object()) {
        return frames;
    }

    if (!payload.contains("anchor_points") || !payload["anchor_points"].is_array()) {
        return frames;
    }

    const auto& anchor_json = payload["anchor_points"];

    const std::size_t limit = std::min<std::size_t>(frame_count, anchor_json.size());
    for (std::size_t i = 0; i < limit; ++i) {
        const auto& entry = anchor_json[i];
        if (!entry.is_array()) {
            continue;
        }
        std::unordered_set<std::string> names;
        for (const auto& node : entry) {
            FrameAnchorPoint pt{};
            if (!read_anchor(node, pt)) continue;
            if (!names.insert(pt.name).second) continue;
            frames[i].anchors.push_back(pt);
        }
    }

    return frames;
}

nlohmann::json build_payload_with_anchors(const std::vector<AnchorFrame>& frames, const nlohmann::json& existing_payload) {
    nlohmann::json payload = existing_payload.is_object() ? existing_payload : nlohmann::json::object();
    const std::size_t declared_frame_count =
        payload.contains("number_of_frames") ? read_positive_size(payload["number_of_frames"]) : 0;
    const std::size_t frame_count = std::max<std::size_t>(1, std::max(declared_frame_count, frames.size()));
    nlohmann::json anchor_points = nlohmann::json::array();
    anchor_points.get_ref<nlohmann::json::array_t&>().resize(frame_count, nlohmann::json::array());

    const std::size_t frames_to_write = std::min<std::size_t>(frames.size(), frame_count);
    for (std::size_t i = 0; i < frames_to_write; ++i) {
        nlohmann::json frame_array = nlohmann::json::array();
        std::unordered_set<std::string> names;
        const auto& anchor_frame = frames[i];
        for (const auto& anchor : anchor_frame.anchors) {
            if (anchor.name.empty()) continue;
            if (!names.insert(anchor.name).second) {
                continue;
            }
            nlohmann::json entry{
                {"name", anchor.name},
                {"texture_x", std::max(0, anchor.texture_x)},
                {"texture_y", std::max(0, anchor.texture_y)},
                {"in_front", anchor.in_front},
            };
            frame_array.push_back(std::move(entry));
        }
        anchor_points[static_cast<nlohmann::json::array_t::size_type>(i)] = std::move(frame_array);
    }

    payload["anchor_points"] = std::move(anchor_points);

    return payload;
}


void apply_anchor_scope(AnchorFrame& target, const AnchorFrame& source, AnchorConflictPolicy policy) {
    if (policy != AnchorConflictPolicy::SyncExact) {
        return;
    }
    target = source;
}

}  // namespace devmode::frame_editors
