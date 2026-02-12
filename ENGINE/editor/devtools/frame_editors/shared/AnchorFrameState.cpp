#include "AnchorFrameState.hpp"

#include <algorithm>
#include <cmath>
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

std::size_t inferred_frame_count(const nlohmann::json& payload) {
    if (payload.contains("movement") && payload["movement"].is_array()) {
        return payload["movement"].size();
    }
    if (payload.contains("frames") && payload["frames"].is_array()) {
        return payload["frames"].size();
    }
    return 1;
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
            if (!node.is_object()) continue;
            FrameAnchorPoint pt{};
            pt.name = node.value("name", std::string{});
            if (pt.name.empty()) continue;
            pt.px = read_float(node.value("px", 0.0f));
            pt.py = read_float(node.value("py", 0.0f));
            pt.pz = std::clamp(read_float(node.value("pz", 0.0f)), 0.0f, 1.0f);
            pt.rotation = read_float(node.value("rotation", node.value("rotation_deg", 0.0f)));
            if (!std::isfinite(pt.px) || !std::isfinite(pt.py) || !std::isfinite(pt.pz) || !std::isfinite(pt.rotation)) {
                continue;
            }
            if (!names.insert(pt.name).second) {
                continue;
            }
            frames[i].anchors.push_back(pt);
        }
    }

    return frames;
}

nlohmann::json build_payload_with_anchors(const std::vector<AnchorFrame>& frames, const nlohmann::json& existing_payload) {
    nlohmann::json payload = existing_payload.is_object() ? existing_payload : nlohmann::json::object();
    const std::size_t frame_count = std::max<std::size_t>(1, frames.size());

    nlohmann::json anchor_points = (payload.contains("anchor_points") && payload["anchor_points"].is_array())
                                       ? payload["anchor_points"]
                                       : nlohmann::json::array();
    anchor_points.get_ref<nlohmann::json::array_t&>().resize(frame_count, nlohmann::json::array());

    for (std::size_t i = 0; i < frame_count; ++i) {
        nlohmann::json frame_array = nlohmann::json::array();
        std::unordered_set<std::string> names;
        const auto& anchor_frame = frames[i];
        for (const auto& anchor : anchor_frame.anchors) {
            if (anchor.name.empty()) continue;
            if (!std::isfinite(anchor.px) || !std::isfinite(anchor.py) || !std::isfinite(anchor.pz) ||
                !std::isfinite(anchor.rotation)) {
                continue;
            }
            if (!names.insert(anchor.name).second) {
                continue;
            }
            frame_array.push_back(nlohmann::json{
                {"name", anchor.name},
                {"px", anchor.px},
                {"py", anchor.py},
                {"pz", std::clamp(anchor.pz, 0.0f, 1.0f)},
                {"rotation", anchor.rotation},
            });
        }
        anchor_points[static_cast<nlohmann::json::array_t::size_type>(i)] = std::move(frame_array);
    }

    payload["anchor_points"] = std::move(anchor_points);
    return payload;
}

}  // namespace devmode::frame_editors

