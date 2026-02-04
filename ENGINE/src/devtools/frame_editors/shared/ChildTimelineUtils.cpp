#include "devtools/frame_editors/shared/ChildTimelineUtils.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include "assets/animation.hpp"

namespace devmode::frame_editors::child_timelines {

namespace {

int read_int(const nlohmann::json& value, int fallback) {
    if (value.is_number_integer()) {
        try {
            return value.get<int>();
        } catch (...) {
        }
    } else if (value.is_number()) {
        try {
            return static_cast<int>(value.get<double>());
        } catch (...) {
        }
    } else if (value.is_string()) {
        try {
            return std::stoi(value.get<std::string>());
        } catch (...) {
        }
    }
    return fallback;
}

float read_float(const nlohmann::json& value, float fallback) {
    if (value.is_number()) {
        try {
            return static_cast<float>(value.get<double>());
        } catch (...) {
        }
    } else if (value.is_string()) {
        try {
            return std::stof(value.get<std::string>());
        } catch (...) {
        }
    }
    return fallback;
}

bool read_bool(const nlohmann::json& value, bool fallback) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<int>() != 0;
    }
    if (value.is_number()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (text == "true" || text == "1" || text == "yes" || text == "on") return true;
        if (text == "false" || text == "0" || text == "no" || text == "off") return false;
    }
    return fallback;
}

std::string to_lower(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (unsigned char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lowered;
}

nlohmann::json default_child_frame_json() {
    return nlohmann::json{{"dx", 0}, {"dy", 0}, {"dz", 0.0}, {"degree", 0.0}, {"visible", false}};
}

}  // namespace

bool timeline_entry_is_static(const nlohmann::json& entry) {
    if (!entry.is_object()) {
        return true;
    }
    if (entry.contains("mode") && entry["mode"].is_string()) {
        const std::string lowered = to_lower(entry["mode"].get<std::string>());
        if (lowered == "async" || lowered == "asynchronous") {
            return false;
        }
    }
    return true;
}

ChildFrameSample child_frame_from_json(const nlohmann::json& sample, int child_index) {
    ChildFrameSample child;
    child.child_index = child_index;
    child.dx = 0.0f;
    child.dy = 0.0f;
    child.dz = 0.0f;
    child.degree = 0.0f;
    child.visible = false;
    child.has_data = false;

    if (sample.is_object()) {
        if (sample.contains("px")) child.dx = read_float(sample["px"], 0.0f);
        if (sample.contains("py")) child.dy = read_float(sample["py"], 0.0f);
        if (sample.contains("pz")) child.dz = read_float(sample["pz"], 0.0f);
        if (sample.contains("degree")) {
            child.degree = read_float(sample["degree"], 0.0f);
        }
        if (sample.contains("visible")) child.visible = read_bool(sample["visible"], child.visible);
        child.has_data = true;
    }
    return child;
}

nlohmann::json child_frame_to_json(const ChildFrameSample& frame) {
    const bool has_sample = frame.has_data;
    nlohmann::json sample = nlohmann::json::object();
    sample["px"] = has_sample ? static_cast<double>(frame.dx) : 0.0;
    sample["py"] = has_sample ? static_cast<double>(frame.dy) : 0.0;
    sample["pz"] = has_sample ? static_cast<double>(frame.dz) : 0.0;
    sample["degree"] = has_sample ? static_cast<double>(frame.degree) : 0.0;
    sample["visible"] = has_sample ? frame.visible : false;
    return sample;
}

nlohmann::json build_child_timelines_payload(
    const nlohmann::json& existing_payload,
    const std::vector<std::vector<ChildFrameSample>>& static_frames_by_child,
    const std::vector<std::string>& child_assets,
    const std::vector<AnimationChildMode>& child_modes,
    const std::vector<std::vector<ChildFrameSample>>& async_timelines_by_child,
    const std::vector<float>& async_start_times,
    const std::vector<bool>& async_has_start) {

    nlohmann::json normalized = nlohmann::json::array();
    if (child_assets.empty()) {
        return normalized;
    }

    std::unordered_map<std::string, std::string> animation_overrides;
    auto it = existing_payload.find("child_timelines");
    if (it != existing_payload.end() && it->is_array()) {
        for (const auto& entry : *it) {
            if (!entry.is_object()) {
                continue;
            }
            std::string asset = entry.value("asset", std::string{});
            if (asset.empty()) {
                int idx = entry.value("child", entry.value("child_index", -1));
                if (idx >= 0 && static_cast<std::size_t>(idx) < child_assets.size()) {
                    asset = child_assets[static_cast<std::size_t>(idx)];
                }
            }
            if (asset.empty()) {
                continue;
            }
            if (!entry.contains("animation") || !entry["animation"].is_string()) {
                continue;
            }
            animation_overrides.emplace(asset, entry["animation"].get<std::string>());
        }
    }

    normalized.get_ref<nlohmann::json::array_t&>().reserve(child_assets.size());
    for (std::size_t child_idx = 0; child_idx < child_assets.size(); ++child_idx) {
        const std::string& asset_name = child_assets[child_idx];
        nlohmann::json entry = nlohmann::json::object();
        entry["child"] = static_cast<int>(child_idx);
        entry["child_index"] = static_cast<int>(child_idx);
        entry["asset"] = asset_name;
        auto animation_override = animation_overrides.find(asset_name);
        entry["animation"] = (animation_override != animation_overrides.end()) ? animation_override->second : std::string{};
        const bool is_static = (child_idx < child_modes.size()) ? child_modes[child_idx] != AnimationChildMode::Async : true;
        entry["mode"] = is_static ? "static" : "async";
        if (is_static) {
            nlohmann::json frames = nlohmann::json::array();
            const auto& timeline = (child_idx < static_frames_by_child.size()) ? static_frames_by_child[child_idx] : std::vector<ChildFrameSample>{};
            frames.get_ref<nlohmann::json::array_t&>().reserve(std::max<std::size_t>(timeline.size(), 1));
            for (const auto& sample : timeline) {
                ChildFrameSample entry_sample = sample;
                entry_sample.child_index = static_cast<int>(child_idx);
                frames.push_back(child_frame_to_json(entry_sample));
            }
            if (frames.empty()) {
                ChildFrameSample fallback{};
                fallback.child_index = static_cast<int>(child_idx);
                fallback.visible = false;
                frames.push_back(child_frame_to_json(fallback));
            }
            entry["frames"] = std::move(frames);
        } else {
            int start_frame = 0;
            float start_time = 0.0f;
            bool has_start = (child_idx < async_has_start.size()) ? async_has_start[child_idx] : false;
            if (has_start && child_idx < async_start_times.size()) {
                start_time = async_start_times[child_idx];
            }
            if (child_idx < async_timelines_by_child.size()) {
                nlohmann::json frames = nlohmann::json::array();
                for (const auto& sample : async_timelines_by_child[child_idx]) {
                    ChildFrameSample entry_sample = sample;
                    entry_sample.child_index = static_cast<int>(child_idx);
                    frames.push_back(child_frame_to_json(entry_sample));
                }
                if (frames.empty()) {
                    ChildFrameSample fallback{};
                    fallback.child_index = static_cast<int>(child_idx);
                    fallback.visible = false;
                    frames.push_back(child_frame_to_json(fallback));
                }
                entry["frames"] = std::move(frames);
            } else {
                ChildFrameSample fallback{};
                fallback.child_index = static_cast<int>(child_idx);
                fallback.visible = false;
                entry["frames"] = nlohmann::json::array();
                entry["frames"].push_back(child_frame_to_json(fallback));
            }
            if (has_start) {
                if (start_frame <= 0 && start_time > 0.0f) {
                    start_frame = static_cast<int>(std::lround(start_time * static_cast<float>(kBaseAnimationFps)));
                } else if (start_frame < 0) {
                    start_frame = 0;
                }
                if (start_time <= 0.0f && start_frame > 0) {
                    start_time = static_cast<float>(start_frame) / static_cast<float>(kBaseAnimationFps);
                }
                entry["start_frame"] = start_frame;
                entry["start_time"] = start_time;
                if (!entry.contains("auto_start")) {
                    entry["auto_start"] = true;
                }
                if (!entry.contains("autostart")) {
                    entry["autostart"] = entry.value("auto_start", true);
                }
            }
        }
        normalized.push_back(std::move(entry));
    }
    return normalized;
}

}  // namespace devmode::frame_editors::child_timelines
