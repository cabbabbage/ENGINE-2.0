#include "devtools/room_box_payload_utils.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace devmode::room_box_payload {

namespace {

std::string trim_copy(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string sanitize_box_name(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) != 0 || ch == '_' || ch == '-' || ch == '.') {
            out.push_back(ch);
        } else if (std::isspace(uch) != 0) {
            out.push_back('_');
        }
    }
    return trim_copy(out);
}

nlohmann::json normalize_box_frame_array(const nlohmann::json& payload,
                                         const char* key,
                                         std::size_t frame_count) {
    nlohmann::json value = (payload.contains(key) && payload[key].is_array())
                               ? payload[key]
                               : nlohmann::json::array();
    if (value.size() < frame_count) {
        value.get_ref<nlohmann::json::array_t&>().resize(frame_count, nlohmann::json::array());
    } else if (value.size() > frame_count) {
        value.erase(value.begin() + static_cast<std::ptrdiff_t>(frame_count), value.end());
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (!value[i].is_array()) {
            value[i] = nlohmann::json::array();
        }
    }
    return value;
}

nlohmann::json serialize_hit_boxes(const std::vector<animation_update::FrameHitBox>& boxes) {
    nlohmann::json serialized = nlohmann::json::array();
    for (const auto& box : boxes) {
        nlohmann::json node = nlohmann::json::object();
        node["name"] = box.name;
        node["extrusion_amount"] = std::max(0, box.extrusion_amount);
        nlohmann::json corners = nlohmann::json::array();
        for (std::size_t corner_index = 0; corner_index < 4; ++corner_index) {
            const auto& corner = box.corners[corner_index];
            corners.push_back(nlohmann::json::object({
                {"texture_x", std::max(0, corner.texture_x)},
                {"texture_y", std::max(0, corner.texture_y)},
            }));
        }
        node["corners"] = std::move(corners);
        serialized.push_back(std::move(node));
    }
    return serialized;
}

nlohmann::json serialize_attack_boxes(const std::vector<animation_update::FrameAttackBox>& boxes) {
    nlohmann::json serialized = nlohmann::json::array();
    for (const auto& box : boxes) {
        nlohmann::json node = nlohmann::json::object();
        node["name"] = box.name;
        node["extrusion_amount"] = std::max(0, box.extrusion_amount);
        node["damage_amount"] = box.damage_amount;
        nlohmann::json corners = nlohmann::json::array();
        for (std::size_t corner_index = 0; corner_index < 4; ++corner_index) {
            const auto& corner = box.corners[corner_index];
            corners.push_back(nlohmann::json::object({
                {"texture_x", std::max(0, corner.texture_x)},
                {"texture_y", std::max(0, corner.texture_y)},
            }));
        }
        node["corners"] = std::move(corners);
        serialized.push_back(std::move(node));
    }
    return serialized;
}

bool write_box_frame_to_payload(nlohmann::json& animation_payload,
                                const char* key,
                                std::size_t frame_count,
                                std::size_t frame_index,
                                const nlohmann::json& serialized_boxes) {
    if (frame_count == 0 || frame_index >= frame_count) {
        return false;
    }
    if (!animation_payload.is_object()) {
        animation_payload = nlohmann::json::object();
    }
    nlohmann::json frame_array = normalize_box_frame_array(animation_payload, key, frame_count);
    frame_array[frame_index] = serialized_boxes;
    animation_payload[key] = std::move(frame_array);
    animation_payload.erase("hit_geometry");
    animation_payload.erase("attack_geometry");
    return true;
}

std::vector<std::string> existing_names_from(const std::vector<std::string>& existing_names) {
    std::vector<std::string> out;
    out.reserve(existing_names.size());
    for (const auto& name : existing_names) {
        const std::string cleaned = sanitize_box_name(name);
        if (!cleaned.empty()) {
            out.push_back(cleaned);
        }
    }
    return out;
}

template <typename TBox>
TBox make_default_box_with_name(const std::string& name) {
    TBox box{};
    box.name = name;
    box.extrusion_amount = 0;
    box.corners[0].texture_x = 0;
    box.corners[0].texture_y = 0;
    box.corners[1].texture_x = 16;
    box.corners[1].texture_y = 0;
    box.corners[2].texture_x = 16;
    box.corners[2].texture_y = 16;
    box.corners[3].texture_x = 0;
    box.corners[3].texture_y = 16;
    return box;
}

}  // namespace

std::string make_unique_box_name(const std::string& desired_name,
                                 const std::vector<std::string>& existing_names,
                                 const std::string& fallback_base,
                                 const std::string& excluded_name) {
    std::unordered_set<std::string> used;
    for (const auto& name : existing_names_from(existing_names)) {
        if (!excluded_name.empty() && name == excluded_name) {
            continue;
        }
        used.insert(name);
    }

    const std::string cleaned_desired = sanitize_box_name(desired_name);
    const std::string cleaned_fallback = sanitize_box_name(fallback_base);
    const std::string base = !cleaned_desired.empty() ? cleaned_desired :
                             (!cleaned_fallback.empty() ? cleaned_fallback : std::string{"box"});
    if (used.find(base) == used.end()) {
        return base;
    }

    int suffix = 2;
    while (true) {
        const std::string candidate = base + "_" + std::to_string(suffix++);
        if (used.find(candidate) == used.end()) {
            return candidate;
        }
    }
}

animation_update::FrameHitBox make_default_hit_box(const std::vector<std::string>& existing_names) {
    const std::string name = make_unique_box_name({}, existing_names, "hit_box");
    return make_default_box_with_name<animation_update::FrameHitBox>(name);
}

animation_update::FrameAttackBox make_default_attack_box(const std::vector<std::string>& existing_names) {
    animation_update::FrameAttackBox box =
        make_default_box_with_name<animation_update::FrameAttackBox>(
            make_unique_box_name({}, existing_names, "attack_box"));
    box.damage_amount = 0;
    return box;
}

bool write_hit_box_frame_to_payload(nlohmann::json& animation_payload,
                                    std::size_t frame_count,
                                    std::size_t frame_index,
                                    const std::vector<animation_update::FrameHitBox>& boxes) {
    return write_box_frame_to_payload(animation_payload,
                                      "hit_boxes",
                                      frame_count,
                                      frame_index,
                                      serialize_hit_boxes(boxes));
}

bool write_attack_box_frame_to_payload(nlohmann::json& animation_payload,
                                       std::size_t frame_count,
                                       std::size_t frame_index,
                                       const std::vector<animation_update::FrameAttackBox>& boxes) {
    return write_box_frame_to_payload(animation_payload,
                                      "attack_boxes",
                                      frame_count,
                                      frame_index,
                                      serialize_attack_boxes(boxes));
}

}  // namespace devmode::room_box_payload
