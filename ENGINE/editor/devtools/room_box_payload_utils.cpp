#include "devtools/room_box_payload_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

#include <SDL3/SDL.h>
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

std::string sanitize_box_id(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) != 0 || ch == '_' || ch == '-' || ch == '.') {
            out.push_back(static_cast<char>(std::tolower(uch)));
        } else if (std::isspace(uch) != 0) {
            out.push_back('_');
        }
    }
    return trim_copy(out);
}

bool box_trace_enabled() {
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

std::string ensure_box_id(const std::string& raw_id,
                          const std::string& fallback_prefix,
                          std::size_t ordinal) {
    const std::string sanitized = sanitize_box_id(raw_id);
    if (!sanitized.empty()) {
        return sanitized;
    }
    return sanitize_box_id(fallback_prefix + "_" + std::to_string(ordinal + 1));
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

nlohmann::json frame_range_json(const animation_update::FrameBoxBase& box, std::size_t frame_index) {
    const int fallback_frame = static_cast<int>(frame_index);
    int frame_start = box.frame_start;
    int frame_end = box.frame_end;
    if (frame_start < 0) {
        frame_start = fallback_frame;
    }
    if (frame_end < frame_start) {
        frame_end = frame_start;
    }
    return nlohmann::json::object({
        {"start", frame_start},
        {"end", frame_end},
    });
}

template <typename TBox>
nlohmann::json serialize_box_base(const TBox& box,
                                  std::size_t ordinal,
                                  std::size_t frame_index,
                                  const char* default_type) {
    nlohmann::json node = nlohmann::json::object();
    const std::string box_id = ensure_box_id(box.id, default_type, ordinal);
    node["id"] = box_id;
    node["type"] = std::string{default_type};
    node["name"] = box.name;
    node["enabled"] = box.enabled;
    node["frame_range"] = frame_range_json(box, frame_index);
    node["position"] = nlohmann::json::object({
        {"x", box.rect.left},
        {"y", box.rect.top},
    });
    node["size"] = nlohmann::json::object({
        {"w", std::max(0, box.rect.width())},
        {"h", std::max(0, box.rect.height())},
    });
    node["rotation_degrees"] = static_cast<double>(
        animation_update::FrameBoxBase::sanitize_rotation_degrees(box.rotation_degrees));
    node["anchor_link"] = box.anchor_link;
    node["extrusion_amount"] = std::max(0, box.extrusion_amount);
    nlohmann::json corners = nlohmann::json::array();
    const auto runtime_corners = box.to_runtime_clockwise_points();
    for (std::size_t corner_index = 0; corner_index < runtime_corners.size(); ++corner_index) {
        const auto& corner = runtime_corners[corner_index];
        corners.push_back(nlohmann::json::object({
            {"texture_x", std::max(0, corner.texture_x)},
            {"texture_y", std::max(0, corner.texture_y)},
        }));
    }
    node["corners"] = std::move(corners);
    return node;
}

nlohmann::json parse_meta_json(const std::string& raw_meta) {
    if (raw_meta.empty()) {
        return nlohmann::json::object();
    }
    nlohmann::json parsed = nlohmann::json::parse(raw_meta, nullptr, false);
    if (!parsed.is_object()) {
        return nlohmann::json::object();
    }
    return parsed;
}

nlohmann::json serialize_hit_boxes(const std::vector<animation_update::FrameHitBox>& boxes,
                                   std::size_t frame_index) {
    nlohmann::json serialized = nlohmann::json::array();
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const auto& box = boxes[i];
        nlohmann::json node = serialize_box_base(box, i, frame_index, "hitbox");
        serialized.push_back(std::move(node));
    }
    return serialized;
}

nlohmann::json serialize_attack_boxes(const std::vector<animation_update::FrameAttackBox>& boxes,
                                      std::size_t frame_index) {
    nlohmann::json serialized = nlohmann::json::array();
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const auto& box = boxes[i];
        nlohmann::json node = serialize_box_base(box, i, frame_index, "attack_box");
        node["damage_amount"] = box.damage_amount;
        node["meta"] = parse_meta_json(box.meta_json);
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
    animation_payload["box_schema_version"] = 1;
    animation_payload.erase("hit_geometry");
    animation_payload.erase("attack_geometry");
    if (box_trace_enabled()) {
        SDL_Log("[BoxFlow][serialize] key=%s frame=%zu count=%zu", key, frame_index, serialized_boxes.size());
    }
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
TBox make_default_box_with_name(const std::string& name, int frame_width, int frame_height) {
    TBox box{};
    box.id = sanitize_box_id(name + "_id");
    box.type = "hitbox";
    box.name = name;
    box.enabled = true;
    box.frame_start = -1;
    box.frame_end = -1;
    box.anchor_link.clear();
    box.extrusion_amount = 0;
    const int max_x = std::max(0, frame_width - 1);
    const int max_y = std::max(0, frame_height - 1);
    box.set_rect(animation_update::FrameBoxRect{0, 0, max_x, max_y});
    return box;
}

}  // namespace

std::string make_unique_box_name(const std::string& desired_name,
                                 const std::vector<std::string>& existing_names,
                                 const std::string& fallback_base,
                                 const std::string& excluded_name) {
    std::unordered_set<std::string> used;
    const std::string sanitized_excluded = sanitize_box_name(excluded_name);
    for (const auto& name : existing_names_from(existing_names)) {
        if (!sanitized_excluded.empty() && name == sanitized_excluded) {
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

animation_update::FrameHitBox make_default_hit_box(const std::vector<std::string>& existing_names,
                                                   int frame_width,
                                                   int frame_height) {
    const std::string name = make_unique_box_name({}, existing_names, "hit_box");
    return make_default_box_with_name<animation_update::FrameHitBox>(name, frame_width, frame_height);
}

animation_update::FrameAttackBox make_default_attack_box(const std::vector<std::string>& existing_names,
                                                         int frame_width,
                                                         int frame_height) {
    animation_update::FrameAttackBox box =
        make_default_box_with_name<animation_update::FrameAttackBox>(
            make_unique_box_name({}, existing_names, "attack_box"),
            frame_width,
            frame_height);
    box.type = "attack_box";
    box.damage_amount = 0;
    box.meta_json = "{}";
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
                                      serialize_hit_boxes(boxes, frame_index));
}

bool write_attack_box_frame_to_payload(nlohmann::json& animation_payload,
                                       std::size_t frame_count,
                                       std::size_t frame_index,
                                       const std::vector<animation_update::FrameAttackBox>& boxes) {
    return write_box_frame_to_payload(animation_payload,
                                      "attack_boxes",
                                      frame_count,
                                      frame_index,
                                      serialize_attack_boxes(boxes, frame_index));
}

}  // namespace devmode::room_box_payload
