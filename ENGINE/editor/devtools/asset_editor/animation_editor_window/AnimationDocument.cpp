#include "AnimationDocument.hpp"

#include "devtools/core/dev_json_store.hpp"

#include <SDL3/SDL_log.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <nlohmann/json.hpp>

#include "assets/asset/animation.hpp"
#include "json_coercion.hpp"
#include "string_utils.hpp"

namespace {

using animation_editor::AnimationDocument;
using json_coercion::read_bool_field_like;
using json_coercion::read_bool_like;
using json_coercion::read_int_like;
using json_coercion::read_string_like;

namespace fs = std::filesystem;

std::string normalize_tag_value(std::string_view raw) {
    const auto begin = std::find_if_not(raw.begin(), raw.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto end = std::find_if_not(raw.rbegin(), raw.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (begin >= end) {
        return {};
    }

    std::string normalized(begin, end);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return normalized;
}

nlohmann::json canonicalize_tags_field(const nlohmann::json& payload) {
    nlohmann::json tags = nlohmann::json::array();
    std::unordered_set<std::string> seen;

    auto append_tag = [&](const nlohmann::json& node) {
        if (!node.is_string()) {
            return;
        }
        std::string normalized = normalize_tag_value(node.get<std::string>());
        if (normalized.empty()) {
            return;
        }
        if (seen.insert(normalized).second) {
            tags.push_back(std::move(normalized));
        }
    };

    if (payload.contains("tags")) {
        const auto& tag_node = payload["tags"];
        if (tag_node.is_array()) {
            for (const auto& entry : tag_node) {
                append_tag(entry);
            }
        } else {
            append_tag(tag_node);
        }
    }

    return tags;
}

nlohmann::json coerce_payload(const std::string& animation_id, const nlohmann::json& source_payload) {
    nlohmann::json payload = source_payload.is_object() ? source_payload : nlohmann::json::object();

    nlohmann::json source = payload.contains("source") && payload["source"].is_object() ? payload["source"] : nlohmann::json::object();
    std::string kind = read_string_like(source.contains("kind") ? source["kind"] : nlohmann::json{}, std::string{"folder"});
    std::string path = read_string_like(source.contains("path") ? source["path"] : nlohmann::json{},
                                        kind == "folder" ? animation_id : std::string{});
    nlohmann::json name_value;
    if (kind == "folder") {

        name_value = std::string{};
    } else {
        if (source.contains("name")) {
            name_value = read_string_like(source["name"], std::string{});
        } else {
            name_value = std::string{};
        }
    }
    payload["source"] = nlohmann::json{
        {"kind", kind},
        {"path", path},
        {"name", name_value},
};

    auto ensure_bool = [&](const char* key, bool fallback) {
        payload[key] = read_bool_like(payload.contains(key) ? payload[key] : nlohmann::json(fallback), fallback);
};

    ensure_bool("invert_x", false);
    ensure_bool("invert_y", false);
    ensure_bool("invert_z", false);
    ensure_bool("reverse_source", false);
    ensure_bool("locked", false);
    ensure_bool("rnd_start", false);
    payload.erase("loop");

    bool derived_from_animation = (kind == "animation");
    bool derived_reverse = read_bool_field_like(payload, "reverse_source", false);
    bool derived_invert_x = read_bool_field_like(payload, "invert_x", false);
    bool derived_invert_y = read_bool_field_like(payload, "invert_y", false);
    bool derived_invert_z = read_bool_field_like(payload, "invert_z", false);
    if (payload.contains("derived_modifiers") && payload["derived_modifiers"].is_object()) {
        const auto& modifiers = payload["derived_modifiers"];
        if (modifiers.contains("reverse")) {
            derived_reverse = read_bool_like(modifiers["reverse"], derived_reverse);
        }
    }

    const bool has_movement_json = payload.contains("movement") && payload["movement"].is_array();
    const bool has_anchor_points_json = payload.contains("anchor_points") && payload["anchor_points"].is_array();
    const bool has_hit_boxes_json = payload.contains("hit_boxes") && payload["hit_boxes"].is_array();
    const bool has_attack_boxes_json = payload.contains("attack_boxes") && payload["attack_boxes"].is_array();
    const bool has_any_local_frame_data =
        has_movement_json || has_anchor_points_json || has_hit_boxes_json || has_attack_boxes_json;
    const bool legacy_inherit_source_movement =
        read_bool_field_like(payload, "inherit_source_movement", derived_from_animation);
    const bool default_inherit_data =
        derived_from_animation &&
            legacy_inherit_source_movement &&
            !has_any_local_frame_data;
    bool inherit_data = default_inherit_data;
    if (payload.contains("inherit_data")) {
        inherit_data = read_bool_field_like(payload, "inherit_data", default_inherit_data);
    } else {
        inherit_data = read_bool_field_like(payload, "inherit_source_geometry", default_inherit_data);
    }
    inherit_data = derived_from_animation && inherit_data;
    if (!inherit_data) {
        derived_invert_x = false;
        derived_invert_y = false;
        derived_invert_z = false;
    }

    if (derived_from_animation) {
        payload["derived_modifiers"] = nlohmann::json{{"reverse", derived_reverse}};
        payload["inherit_data"] = inherit_data;

        if (inherit_data) {
            payload.erase("movement");
            payload.erase("movement_total");
            payload.erase("movement_variants");
            payload.erase("anchor_points");
            payload.erase("hit_boxes");
            payload.erase("attack_boxes");
            payload.erase("hit_geometry");
            payload.erase("attack_geometry");
        }

        payload.erase("audio");
        payload.erase("locked");
        payload.erase("movement_preview_bounds");
    } else {
        payload.erase("derived_modifiers");
        payload.erase("inherit_data");
        derived_invert_x = false;
        derived_invert_y = false;
        derived_invert_z = false;
    }
    payload["reverse_source"] = derived_reverse;
    payload["invert_x"] = derived_invert_x;
    payload["invert_y"] = derived_invert_y;
    payload["invert_z"] = derived_invert_z;
    if (derived_from_animation) {
        payload["invert_frames_horizontal"] = read_bool_field_like(payload, "invert_frames_horizontal", false);
        payload["invert_frames_vertical"] = false;
    } else {
        payload.erase("invert_frames_horizontal");
        payload.erase("invert_frames_vertical");
    }
    payload.erase("inherit_source_movement");
    payload.erase("flipped_source");
    payload.erase("flip_vertical_source");
    payload.erase("flip_movement_horizontal");
    payload.erase("flip_movement_vertical");
    if (payload.contains("derived_modifiers") && payload["derived_modifiers"].is_object()) {
        payload["derived_modifiers"].erase("flipX");
        payload["derived_modifiers"].erase("flipY");
        payload["derived_modifiers"].erase("flipMovementX");
        payload["derived_modifiers"].erase("flipMovementY");
    }

    payload.erase("fps");
    payload.erase("speed");
    payload.erase("speed_factor");
    payload.erase("speed_multiplier");
    payload["tags"] = canonicalize_tags_field(payload);

    int frames = read_int_like(payload.contains("number_of_frames") ? payload["number_of_frames"] : nlohmann::json(1), 1);
    if (frames < 1) frames = 1;
    payload["number_of_frames"] = frames;

    if (!derived_from_animation || (derived_from_animation && !inherit_data)) {
        nlohmann::json movement = payload.contains("movement") && payload["movement"].is_array() ? payload["movement"] : nlohmann::json::array();
        if (!movement.is_array()) {
            movement = nlohmann::json::array();
        }
        if (movement.size() < static_cast<size_t>(frames)) {
            while (movement.size() < static_cast<size_t>(frames)) {
                movement.push_back(nlohmann::json::array({0, 0, 0}));
            }
        } else if (movement.size() > static_cast<size_t>(frames)) {
            movement.erase(movement.begin() + frames, movement.end());
        }
        if (movement.empty()) {
            movement.push_back(nlohmann::json::array({0, 0, 0}));
        }
        payload["movement"] = movement;

        auto read_component = [](const nlohmann::json& entry, int index) -> int {
            if (entry.is_array()) {
                // Canonical arrays are [dx, y(height), z(depth)].
                // Legacy arrays may only include [dx, depth].
                if (index == 2 && entry.size() == 2 && entry[1].is_number()) {
                    return read_int_like(entry[1], 0);
                }
                if (index == 1 && entry.size() == 2 && entry[1].is_number()) {
                    return 0;
                }
                if (index < static_cast<int>(entry.size()) && entry[index].is_number()) {
                    try {
                        return entry[index].get<int>();
                    } catch (...) {
                    }
                    try {
                        return static_cast<int>(entry[index].get<double>());
                    } catch (...) {
                    }
                }
                return 0;
            }
            if (entry.is_object()) {
                if (index == 0) {
                    if (entry.contains("dx")) return read_int_like(entry["dx"], 0);
                    if (entry.contains("x")) return read_int_like(entry["x"], 0);
                    return 0;
                }
                if (index == 1) {
                    const bool has_xy_keys = entry.contains("x") || entry.contains("y") || entry.contains("z");
                    const bool has_dxyz_keys = entry.contains("dx") || entry.contains("dy") || entry.contains("dz");
                    const int legacy_height = entry.contains("dy")
                        ? read_int_like(entry["dy"], 0)
                        : (entry.contains("y") ? read_int_like(entry["y"], 0) : 0);
                    const int explicit_depth = entry.contains("dz")
                        ? read_int_like(entry["dz"], 0)
                        : (entry.contains("z") ? read_int_like(entry["z"], 0) : 0);
                    if (has_xy_keys && has_dxyz_keys && explicit_depth == 0 && legacy_height != 0) {
                        return 0;
                    }
                    if (entry.contains("dy")) return read_int_like(entry["dy"], 0);
                    if (entry.contains("y")) return read_int_like(entry["y"], 0);
                    return 0;
                }
                if (index == 2) {
                    const bool has_xy_keys = entry.contains("x") || entry.contains("y") || entry.contains("z");
                    const bool has_dxyz_keys = entry.contains("dx") || entry.contains("dy") || entry.contains("dz");
                    const int legacy_depth = entry.contains("dy")
                        ? read_int_like(entry["dy"], 0)
                        : (entry.contains("y") ? read_int_like(entry["y"], 0) : 0);
                    const int explicit_depth = entry.contains("dz")
                        ? read_int_like(entry["dz"], 0)
                        : (entry.contains("z") ? read_int_like(entry["z"], 0) : 0);
                    if (has_xy_keys && has_dxyz_keys && explicit_depth == 0 && legacy_depth != 0) {
                        return legacy_depth;
                    }
                    if (entry.contains("dz")) return read_int_like(entry["dz"], 0);
                    if (entry.contains("z")) return read_int_like(entry["z"], 0);
                    // Legacy object movement used `dy/y` for floor depth.
                    if (!entry.contains("dz") && !entry.contains("z")) {
                        if (entry.contains("dy")) return read_int_like(entry["dy"], 0);
                        if (entry.contains("y")) return read_int_like(entry["y"], 0);
                    }
                }
            }
            return 0;
};

        int total_dx = 0;
        int total_dy = 0;
        int total_dz = 0;
        for (std::size_t i = 0; i < movement.size(); ++i) {
            const nlohmann::json& entry = movement[i];
            total_dx += read_component(entry, 0);
            total_dy += read_component(entry, 1);
            total_dz += read_component(entry, 2);
        }
        payload["movement_total"] = nlohmann::json{{"dx", total_dx}, {"dy", total_dy}, {"dz", total_dz}};
    } else {
        payload.erase("movement");
        payload.erase("movement_total");
    }

    std::string on_end = "default";
    if (payload.contains("on_end")) {
        if (payload["on_end"].is_string()) {
            on_end = payload["on_end"].get<std::string>();
        } else if (payload["on_end"].is_null()) {
            on_end = "default";
        }
    }
    if (on_end.empty()) {
        on_end = "default";
    }
    std::string lowered_on_end = on_end;
    std::transform(lowered_on_end.begin(), lowered_on_end.end(), lowered_on_end.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lowered_on_end == "default" || lowered_on_end == "loop" || lowered_on_end == "kill" ||
        lowered_on_end == "lock" || lowered_on_end == "reverse") {
        payload["on_end"] = lowered_on_end;
    } else {
        payload["on_end"] = on_end;
    }


    if (!derived_from_animation) {
        if (payload.contains("audio") && payload["audio"].is_object()) {
            auto audio = payload["audio"];
            std::string name = read_string_like(audio.contains("name") ? audio["name"] : nlohmann::json{}, std::string{});
            int volume = std::clamp(read_int_like(audio.contains("volume") ? audio["volume"] : nlohmann::json(100), 100), 0, 100);
            bool effects = read_bool_like(audio.contains("effects") ? audio["effects"] : nlohmann::json(false), false);
            if (!name.empty()) {
                payload["audio"] = nlohmann::json{{"name", name}, {"volume", volume}, {"effects", effects}};
            } else {
                payload.erase("audio");
            }
        } else {
            payload.erase("audio");
        }
    } else {
        payload.erase("audio");
    }

    return payload;
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool payload_uses_animation_source(const nlohmann::json& payload) {
    return payload.contains("source") &&
           payload["source"].is_object() &&
           payload["source"].value("kind", std::string{}) == "animation";
}

std::string payload_source_animation_id(const nlohmann::json& payload) {
    if (!payload_uses_animation_source(payload)) {
        return {};
    }
    const auto& source = payload["source"];
    std::string reference =
        animation_editor::strings::trim_copy(source.value("name", std::string{}));
    if (!reference.empty()) {
        return reference;
    }
    return animation_editor::strings::trim_copy(source.value("path", std::string{}));
}

bool payload_has_local_movement_data(const nlohmann::json& payload) {
    return payload.contains("movement") && payload["movement"].is_array();
}

bool payload_has_local_non_movement_geometry(const nlohmann::json& payload) {
    return (payload.contains("anchor_points") && payload["anchor_points"].is_array()) ||
           (payload.contains("hit_boxes") && payload["hit_boxes"].is_array()) ||
           (payload.contains("attack_boxes") && payload["attack_boxes"].is_array());
}

bool payload_has_local_frame_data(const nlohmann::json& payload) {
    return payload_has_local_movement_data(payload) || payload_has_local_non_movement_geometry(payload);
}

bool payload_inherits_data(const nlohmann::json& payload) {
    if (!payload_uses_animation_source(payload)) {
        return false;
    }
    const bool legacy_inherit = read_bool_field_like(payload, "inherit_source_movement", true);
    const bool default_inherit = legacy_inherit && !payload_has_local_frame_data(payload);
    if (payload.contains("inherit_data")) {
        return read_bool_field_like(payload, "inherit_data", default_inherit);
    }
    return read_bool_field_like(payload, "inherit_source_geometry", default_inherit);
}

std::size_t payload_frame_count(const nlohmann::json& payload) {
    if (!payload.is_object()) {
        return 1;
    }
    if (payload.contains("number_of_frames") && payload["number_of_frames"].is_number_integer()) {
        return static_cast<std::size_t>(std::max(payload["number_of_frames"].get<int>(), 1));
    }
    if (payload.contains("number_of_frames") && payload["number_of_frames"].is_number()) {
        return static_cast<std::size_t>(
            std::max(static_cast<int>(payload["number_of_frames"].get<double>()), 1));
    }
    return 1;
}

nlohmann::json normalize_frame_array_key(const nlohmann::json& payload,
                                         const char* key,
                                         std::size_t frame_count) {
    nlohmann::json value =
        (payload.contains(key) && payload[key].is_array()) ? payload[key] : nlohmann::json::array();
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

int read_movement_component(const nlohmann::json& entry, int index) {
    auto read_number = [](const nlohmann::json& value) -> int {
        if (value.is_number_integer()) {
            return value.get<int>();
        }
        if (value.is_number()) {
            return static_cast<int>(std::lround(value.get<double>()));
        }
        return 0;
    };

    if (entry.is_array()) {
        if (index == 2 && entry.size() == 2 && entry[1].is_number()) {
            return read_number(entry[1]);
        }
        if (index == 1 && entry.size() == 2 && entry[1].is_number()) {
            return 0;
        }
        if (index < static_cast<int>(entry.size()) && entry[index].is_number()) {
            return read_number(entry[index]);
        }
        return 0;
    }

    if (!entry.is_object()) {
        return 0;
    }

    if (index == 0) {
        if (entry.contains("dx")) return read_number(entry["dx"]);
        if (entry.contains("x")) return read_number(entry["x"]);
        return 0;
    }

    if (index == 1) {
        const bool has_xy_keys = entry.contains("x") || entry.contains("y") || entry.contains("z");
        const bool has_dxyz_keys = entry.contains("dx") || entry.contains("dy") || entry.contains("dz");
        const int legacy_height = entry.contains("dy")
                                      ? read_number(entry["dy"])
                                      : (entry.contains("y") ? read_number(entry["y"]) : 0);
        const int explicit_depth = entry.contains("dz")
                                       ? read_number(entry["dz"])
                                       : (entry.contains("z") ? read_number(entry["z"]) : 0);
        if (has_xy_keys && has_dxyz_keys && explicit_depth == 0 && legacy_height != 0) {
            return 0;
        }
        if (entry.contains("dy")) return read_number(entry["dy"]);
        if (entry.contains("y")) return read_number(entry["y"]);
        return 0;
    }

    const bool has_xy_keys = entry.contains("x") || entry.contains("y") || entry.contains("z");
    const bool has_dxyz_keys = entry.contains("dx") || entry.contains("dy") || entry.contains("dz");
    const int legacy_depth = entry.contains("dy")
                                 ? read_number(entry["dy"])
                                 : (entry.contains("y") ? read_number(entry["y"]) : 0);
    const int explicit_depth = entry.contains("dz")
                                   ? read_number(entry["dz"])
                                   : (entry.contains("z") ? read_number(entry["z"]) : 0);
    if (has_xy_keys && has_dxyz_keys && explicit_depth == 0 && legacy_depth != 0) {
        return legacy_depth;
    }
    if (entry.contains("dz")) return read_number(entry["dz"]);
    if (entry.contains("z")) return read_number(entry["z"]);
    if (!entry.contains("dz") && !entry.contains("z")) {
        if (entry.contains("dy")) return read_number(entry["dy"]);
        if (entry.contains("y")) return read_number(entry["y"]);
    }
    return 0;
}

nlohmann::json canonical_movement_entry(const nlohmann::json& entry) {
    nlohmann::json result = nlohmann::json::array();
    result.push_back(read_movement_component(entry, 0));
    result.push_back(read_movement_component(entry, 1));
    result.push_back(read_movement_component(entry, 2));

    bool resort_z = false;
    bool has_resort = false;
    nlohmann::json preserved_color;
    bool has_color = false;

    if (entry.is_array()) {
        for (const auto& node : entry) {
            if (node.is_boolean()) {
                resort_z = node.get<bool>();
                has_resort = true;
            }
            const bool looks_color = node.is_array() &&
                                     node.size() == 3 &&
                                     node[0].is_number() &&
                                     node[1].is_number() &&
                                     node[2].is_number();
            if (looks_color && !has_color) {
                preserved_color = node;
                has_color = true;
            }
        }
    } else if (entry.is_object() && entry.contains("resort_z") && entry["resort_z"].is_boolean()) {
        resort_z = entry["resort_z"].get<bool>();
        has_resort = true;
    }

    if (has_resort) {
        result.push_back(resort_z);
    }
    if (has_color) {
        result.push_back(std::move(preserved_color));
    }
    return result;
}

std::vector<nlohmann::json> canonical_movement_frames(const nlohmann::json& payload,
                                                      std::size_t frame_count) {
    std::vector<nlohmann::json> movement(frame_count, nlohmann::json::array({0, 0, 0}));
    if (!payload.contains("movement") || !payload["movement"].is_array()) {
        return movement;
    }
    const auto& raw_movement = payload["movement"];
    const std::size_t limit = std::min(frame_count, raw_movement.size());
    for (std::size_t i = 0; i < limit; ++i) {
        movement[i] = canonical_movement_entry(raw_movement[i]);
    }
    return movement;
}

void set_movement_components(nlohmann::json& entry, int dx, int dy, int dz) {
    nlohmann::json updated = entry.is_array() ? entry : nlohmann::json::array();
    if (updated.size() < 3) {
        updated = nlohmann::json::array();
        updated.push_back(dx);
        updated.push_back(dy);
        updated.push_back(dz);
    } else {
        updated[0] = dx;
        updated[1] = dy;
        updated[2] = dz;
    }
    entry = std::move(updated);
}

template <typename T>
void resize_with_last(std::vector<T>& items, std::size_t frame_count, const T& fallback) {
    if (items.empty()) {
        items.resize(frame_count, fallback);
        return;
    }
    if (items.size() > frame_count) {
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(frame_count), items.end());
        return;
    }
    const T tail = items.back();
    items.resize(frame_count, tail);
}

void update_payload_movement_total(nlohmann::json& payload) {
    const auto movement = payload.contains("movement") && payload["movement"].is_array()
                              ? payload["movement"]
                              : nlohmann::json::array();
    int total_dx = 0;
    int total_dy = 0;
    int total_dz = 0;
    for (std::size_t i = 0; i < movement.size(); ++i) {
        total_dx += read_movement_component(movement[i], 0);
        total_dy += read_movement_component(movement[i], 1);
        total_dz += read_movement_component(movement[i], 2);
    }
    payload["movement_total"] =
        nlohmann::json{{"dx", total_dx}, {"dy", total_dy}, {"dz", total_dz}};
}

std::string serialize_payload(const nlohmann::json& payload) {
    return payload.dump();
}

std::string normalize_animation_id(std::string value) {
    std::string trimmed = animation_editor::strings::trim_copy(value);
    return animation_editor::strings::to_lower_copy(trimmed);
}

void bump_revision(std::uint64_t& revision) {
    if (revision == std::numeric_limits<std::uint64_t>::max()) {
        revision = 1;
        return;
    }
    ++revision;
}

}

namespace animation_editor {

AnimationDocument::AnimationDocument() = default;

std::optional<nlohmann::json> AnimationDocument::raw_animation_payload_json(
    const std::string& animation_id) const {
    auto it = animations_.find(animation_id);
    if (it == animations_.end()) {
        return std::nullopt;
    }
    if (it->second.empty()) {
        return nlohmann::json::object();
    }
    nlohmann::json parsed = nlohmann::json::parse(it->second, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        SDL_Log("AnimationDocument: failed to parse payload for '%s'", animation_id.c_str());
        return nlohmann::json::object();
    }
    return parsed;
}

nlohmann::json AnimationDocument::normalize_payload_for_storage(const std::string& animation_id,
                                                               const nlohmann::json& payload) const {
    nlohmann::json working = payload.is_object() ? payload : nlohmann::json::object();
    return coerce_payload(animation_id, working);
}

void AnimationDocument::load_from_file(const std::filesystem::path& info_path) {
    info_path_ = info_path;
    asset_root_ = info_path.empty() ? std::filesystem::path{} : info_path.parent_path();
    manifest_asset_key_debug_.clear();
    persist_callback_ = nullptr;

    nlohmann::json root = nlohmann::json::object();
    if (!info_path.empty()) {
        std::ifstream in(info_path);
        if (in.good()) {
            try {
                in >> root;
            } catch (const std::exception& ex) {
                SDL_Log("AnimationDocument: failed to parse %s: %s", info_path.string().c_str(), ex.what());
                root = nlohmann::json::object();
            }
        }
    }
    if (!root.is_object()) {
        root = nlohmann::json::object();
    }

    base_data_ = root;
    load_from_json_object(base_data_);
}

void AnimationDocument::load_from_manifest(const nlohmann::json& asset_json,
                                           const std::filesystem::path& asset_root,
                                           std::function<bool(const nlohmann::json&)> persist_callback) {
    info_path_.clear();
    asset_root_ = asset_root;
    persist_callback_ = std::move(persist_callback);
    base_data_ = asset_json.is_object() ? asset_json : nlohmann::json::object();
    load_from_json_object(base_data_);
}

void AnimationDocument::set_manifest_asset_key_debug(std::string key) {
    manifest_asset_key_debug_ = std::move(key);
}

void AnimationDocument::set_on_saved_callback(std::function<void()> callback) {
    on_saved_callback_ = std::move(callback);
}

void AnimationDocument::load_from_json_object(const nlohmann::json& root) {
    animations_.clear();
    start_animation_.reset();
    use_nested_container_ = false;
    container_metadata_.clear();
    dirty_ = false;

    nlohmann::json canonical = root.is_object() ? root : nlohmann::json::object();

    auto start_it = canonical.find("start");
    if (start_it != canonical.end() && start_it->is_string()) {
        std::string start_value = start_it->get<std::string>();
        if (!start_value.empty()) {
            start_animation_ = std::move(start_value);
        }
    }

    const auto animations_it = canonical.find("animations");
    if (animations_it != canonical.end()) {
        if (animations_it->is_object()) {
            const nlohmann::json* payloads = &(*animations_it);
            if (animations_it->contains("animations") && (*animations_it)["animations"].is_object()) {
                use_nested_container_ = true;
                nlohmann::json extras = *animations_it;
                extras.erase("animations");
                extras.erase("start");
                if (!extras.empty()) {
                    container_metadata_ = extras.dump();
                }
                payloads = &(*animations_it)["animations"];
                auto nested_start = animations_it->find("start");
                if (nested_start != animations_it->end() && nested_start->is_string()) {
                    std::string value = nested_start->get<std::string>();
                    if (!value.empty()) start_animation_ = std::move(value);
                }
            }

            for (const auto& item : payloads->items()) {
                if (!item.value().is_object()) {
                    if (item.key() == "start" && item.value().is_string()) {
                        std::string value = item.value().get<std::string>();
                        if (!value.empty()) start_animation_ = std::move(value);
                    }
                    continue;
                }
                animations_[item.key()] = serialize_payload(item.value());
            }
        }
    }

    ensure_document_initialized();
    bump_revision(revision_);
}

void AnimationDocument::save_to_file(bool fire_callback) const {
    (void)save_to_file_checked(fire_callback);
}

bool AnimationDocument::save_to_file_checked(bool fire_callback) const {
    nlohmann::json root;
    if (persist_callback_) {
        root = base_data_.is_object() ? base_data_ : nlohmann::json::object();
    } else {
        root = nlohmann::json::object();
        if (!info_path_.empty()) {
            std::ifstream in(info_path_);
            if (in.good()) {
                try {
                    in >> root;
                } catch (const std::exception& ex) {
                    SDL_Log("AnimationDocument: failed to parse %s for saving: %s", info_path_.string().c_str(), ex.what());
                    root = nlohmann::json::object();
                }
            }
        }
        if (!root.is_object()) {
            root = nlohmann::json::object();
        }
        if (base_data_.is_object()) {

            for (auto it = base_data_.begin(); it != base_data_.end(); ++it) {
                if (it.key() == "animations" || it.key() == "start") {
                    continue;
                }
                root[it.key()] = it.value();
            }
        }
    }

    nlohmann::json animations_json = nlohmann::json::object();
    for (const auto& [id, payload_dump] : animations_) {
        const auto raw_payload = raw_animation_payload_json(id);
        animations_json[id] = normalize_payload_for_storage(
            id,
            raw_payload.has_value() ? *raw_payload : nlohmann::json::object());
    }

    if (use_nested_container_) {
        nlohmann::json container = nlohmann::json::object();
        if (!container_metadata_.empty()) {
            nlohmann::json extras = nlohmann::json::parse(container_metadata_, nullptr, false);
            if (extras.is_object()) {
                for (auto& item : extras.items()) {
                    container[item.key()] = item.value();
                }
            }
        }
        container["animations"] = animations_json;
        container["start"] = start_animation_.has_value() ? *start_animation_ : std::string{};
        root["animations"] = container;
    } else {
        root["animations"] = animations_json;
        root["start"] = start_animation_.has_value() ? *start_animation_ : std::string{};
    }

    auto write_root_to_disk = [&](const std::filesystem::path& path) -> bool {
        if (path.empty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "AnimationDocument: no output path available for saving.");
            return false;
        }
        devmode::core::DevJsonStore::instance().submit(path, root, 4);
        return true;
    };

    bool saved = true;
    if (persist_callback_) {
        saved = persist_callback_(root);
        if (!saved) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "AnimationDocument: failed to persist manifest update.");
        } else {
            base_data_ = root;
        }
    } else {
        if (info_path_.empty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "AnimationDocument: no info path available for saving.");
            return false;
        }
        saved = write_root_to_disk(info_path_);
        base_data_ = root;
    }
    if (saved) {
        if (!persist_callback_ && !info_path_.empty()) {
            // Keep animation edits immediately durable without flushing unrelated pending files.
            devmode::core::DevJsonStore::instance().flush_path(info_path_);
        }
        dirty_ = false;
    }
    if (saved && fire_callback && on_saved_callback_) {
        on_saved_callback_();
    }
    return saved;
}

bool AnimationDocument::consume_dirty_flag() const {
    if (!dirty_) {
        return false;
    }
    dirty_ = false;
    return true;
}

bool AnimationDocument::clear_dirty_if_revision_not_newer(std::uint64_t revision) const {
    if (!dirty_) {
        return false;
    }
    if (revision_ > revision) {
        return false;
    }
    dirty_ = false;
    return true;
}

void AnimationDocument::create_animation(const std::string& animation_id) {
    std::string base = normalize_animation_id(animation_id.empty() ? std::string{"animation"} : animation_id);
    std::string candidate = base;
    int suffix = 2;
    while (animations_.count(candidate) != 0) {
        candidate = base + "_" + std::to_string(suffix++);
    }

    nlohmann::json payload = coerce_payload(candidate, nlohmann::json::object({
                                                    {"source", nlohmann::json::object({
                                                                    {"kind", "folder"},
                                                                    {"path", candidate},
                                                                    {"name", nullptr},
                                                                })},
                                                }));
    animations_[candidate] = serialize_payload(payload);
    if (!start_animation_.has_value() || start_animation_->empty()) {
        start_animation_ = candidate;
    }
    rebuild_animation_cache();
    mark_dirty();
}

void AnimationDocument::delete_animation(const std::string& animation_id) {
    if (animation_id.empty()) return;
    auto it = animations_.find(animation_id);
    if (it == animations_.end()) return;
    animations_.erase(it);

    if (start_animation_ && *start_animation_ == animation_id) {
        auto ids = animation_ids();
        if (!ids.empty()) {
            start_animation_ = ids.front();
        } else {
            start_animation_.reset();
        }
    }
    mark_dirty();
}

std::vector<std::string> AnimationDocument::animation_ids() const {
    std::vector<std::string> ids;
    ids.reserve(animations_.size());
    for (const auto& entry : animations_) {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::optional<std::string> AnimationDocument::start_animation() const {
    if (!start_animation_ || start_animation_->empty()) return std::nullopt;
    if (animations_.count(*start_animation_) == 0) return std::nullopt;
    return start_animation_;
}

void AnimationDocument::set_start_animation(const std::string& animation_id) {
    if (animation_id.empty()) {
        if (start_animation_) {
            start_animation_.reset();
            mark_dirty();
        }
        return;
    }
    if (animations_.count(animation_id) == 0) {
        return;
    }
    if (!start_animation_ || *start_animation_ != animation_id) {
        start_animation_ = animation_id;
        mark_dirty();
    }
}

void AnimationDocument::rename_animation(const std::string& old_id, const std::string& new_id) {
    if (old_id.empty() || new_id.empty()) return;
    std::string normalized = normalize_animation_id(new_id);
    if (normalized.empty() || normalized == old_id) return;
    auto it = animations_.find(old_id);
    if (it == animations_.end()) return;

    std::string base = normalized;
    std::string candidate = base;
    int suffix = 2;
    while (animations_.count(candidate) != 0 && candidate != old_id) {
        candidate = base + "_" + std::to_string(suffix++);
    }

    if (candidate == old_id) {
        return;
    }

#if defined(__cpp_lib_node_extract)
    auto node = animations_.extract(old_id);
    node.key() = candidate;
    animations_.insert(std::move(node));
#else
    std::string payload = it->second;
    animations_.erase(it);
    animations_[candidate] = payload;
#endif

    if (start_animation_ && *start_animation_ == old_id) {
        start_animation_ = candidate;
    }

    for (auto& entry : animations_) {
        const std::string& id = entry.first;
        const auto raw_payload = raw_animation_payload_json(id);
        nlohmann::json payload =
            raw_payload.has_value() ? *raw_payload : nlohmann::json::object();

        bool changed = false;

        auto trim_copy = [](std::string s) {
            auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
            s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
            return s;
};

        if (payload.contains("source") && payload["source"].is_object()) {
            nlohmann::json& src = payload["source"];
            std::string kind = read_string_like(src.contains("kind") ? src["kind"] : nlohmann::json{}, std::string{"folder"});
            if (kind == std::string{"animation"}) {

                if (src.contains("name")) {
                    if (src["name"].is_string()) {
                        std::string name = trim_copy(src["name"].get<std::string>());
                        if (name == old_id) {
                            src["name"] = candidate;
                            changed = true;
                        }
                    } else if (src["name"].is_null()) {

                    }
                }

                if (src.contains("path") && src["path"].is_string()) {
                    std::string path = trim_copy(src["path"].get<std::string>());
                    if (path == old_id) {
                        src["path"] = candidate;
                        changed = true;
                    }
                }
            }
        }

        if (payload.contains("on_end") && payload["on_end"].is_string()) {
            std::string oe = trim_copy(payload["on_end"].get<std::string>());
            if (oe == old_id) {
                payload["on_end"] = candidate;
                changed = true;
            }
        }

        if (payload.contains("movement_variants")) {
            nlohmann::json& mv = payload["movement_variants"];

            std::function<void(nlohmann::json&)> rewrite_strings = [&](nlohmann::json& node) {
                if (node.is_string()) {
                    try {
                        std::string v = node.get<std::string>();
                        if (trim_copy(v) == old_id) {
                            node = candidate;
                            changed = true;
                        }
                    } catch (...) {
                    }
                    return;
                }
                if (node.is_array()) {
                    for (auto& item : node) rewrite_strings(item);
                    return;
                }
                if (node.is_object()) {
                    for (auto it2 = node.begin(); it2 != node.end(); ++it2) {
                        rewrite_strings(it2.value());
                    }
                    return;
                }
};
            rewrite_strings(mv);
        }

        if (changed) {
            entry.second = serialize_payload(normalize_payload_for_storage(id, payload));
        }
    }

    mark_dirty();
    rebuild_animation_cache();
}

void AnimationDocument::replace_animation_payload(const std::string& animation_id, const std::string& payload_json) {
    auto it = animations_.find(animation_id);
    if (it == animations_.end()) return;
    nlohmann::json parsed = nlohmann::json::parse(payload_json, nullptr, false);
    if (parsed.is_discarded()) {
        SDL_Log("AnimationDocument: ignoring invalid payload for '%s'", animation_id.c_str());
        return;
    }
    (void)update_animation_payload(animation_id, parsed);
}

bool AnimationDocument::update_animation_payload(const std::string& animation_id, const nlohmann::json& payload) {
    auto it = animations_.find(animation_id);
    if (it == animations_.end()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "AnimationDocument: missing animation '%s' for update.", animation_id.c_str());
        return false;
    }
    if (!payload.is_object()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "AnimationDocument: payload update for '%s' is not an object.", animation_id.c_str());
        return false;
    }
    std::string normalized = serialize_payload(normalize_payload_for_storage(animation_id, payload));
    if (it->second == normalized) {
        return false;
    }
    it->second = std::move(normalized);
    mark_dirty();
    return true;
}

bool AnimationDocument::save_animation_payload_immediately(const std::string& animation_id, const nlohmann::json& payload) {
    // First update the payload using the regular update method
    bool update_success = update_animation_payload(animation_id, payload);
    if (!update_success) {
        return false;
    }

    // Immediately persist the changes to disk
    return save_to_file_checked(true);
}

std::optional<std::string> AnimationDocument::animation_payload(const std::string& animation_id) const {
    auto it = animations_.find(animation_id);
    if (it == animations_.end()) return std::nullopt;
    return it->second;
}

std::optional<nlohmann::json> AnimationDocument::animation_payload_json(const std::string& animation_id) const {
    const auto raw_payload = raw_animation_payload_json(animation_id);
    if (!raw_payload.has_value()) {
        return std::nullopt;
    }
    return normalize_payload_for_storage(animation_id, *raw_payload);
}

void AnimationDocument::ensure_document_initialized() {
    bool mutated = false;
    std::vector<std::string> ids;
    ids.reserve(animations_.size());

    for (auto& entry : animations_) {
        const auto raw_payload = raw_animation_payload_json(entry.first);
        nlohmann::json normalized = normalize_payload_for_storage(
            entry.first,
            raw_payload.has_value() ? *raw_payload : nlohmann::json::object());
        std::string serialized = serialize_payload(normalized);
        if (serialized != entry.second) {
            entry.second = std::move(serialized);
            mutated = true;
        }
        ids.push_back(entry.first);
    }

    if (ids.empty()) {

        nlohmann::json payload = coerce_payload("default", nlohmann::json::object({
                                                           {"source", nlohmann::json{{"kind", "folder"},
                                                                                       {"path", "default"},
                                                                                       {"name", ""}}},
                                                       }));
        animations_["default"] = serialize_payload(payload);
        ids.push_back("default");
        start_animation_ = std::string{"default"};
        mutated = true;
    }

    if (start_animation_ && animations_.count(*start_animation_) == 0) {
        start_animation_.reset();
        mutated = true;
    }

    if (!start_animation_ && !ids.empty()) {
        std::sort(ids.begin(), ids.end());
        auto preferred = std::find(ids.begin(), ids.end(), std::string{"default"});
        start_animation_ = (preferred != ids.end()) ? *preferred : ids.front();
        mutated = true;
    }

    if (mutated) {
        mark_dirty();
    }
}

void AnimationDocument::rebuild_animation_cache() {
    ensure_document_initialized();
}

void AnimationDocument::mark_dirty() const {
    dirty_ = true;
    bump_revision(revision_);
}

double AnimationDocument::scale_percentage() const {
    try {
        if (!base_data_.is_object()) return 100.0;
        const auto it = base_data_.find("size_settings");
        if (it == base_data_.end() || !it->is_object()) return 100.0;
        const auto& ss = *it;
        if (ss.contains("scale_percentage")) {
            const auto& v = ss["scale_percentage"];
            if (v.is_number()) {
                double pct = v.get<double>();
                if (!std::isfinite(pct) || pct <= 0.0) return 100.0;
                return pct;
            }
        }
    } catch (...) {
    }
    return 100.0;
}

}
