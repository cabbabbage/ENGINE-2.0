#include "animation/controllers/shared/attack_payload.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace animation_update {

namespace {

bool has_non_whitespace(const std::string& value) {
    for (char ch : value) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            return true;
        }
    }
    return false;
}

std::string trim_copy(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string read_string_field(const nlohmann::json& node,
                              const char* key,
                              const std::string& fallback) {
    if (!node.is_object() || !node.contains(key)) {
        return fallback;
    }
    const auto& value = node[key];
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int>());
    }
    if (value.is_number_float()) {
        return std::to_string(value.get<double>());
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    return fallback;
}

int read_int_field(const nlohmann::json& node, const char* key, int fallback) {
    if (!node.is_object() || !node.contains(key)) {
        return fallback;
    }
    const auto& value = node[key];
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_number_float()) {
        return static_cast<int>(std::lround(value.get<double>()));
    }
    if (value.is_string()) {
        try {
            return std::stoi(value.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

float read_float_field(const nlohmann::json& node, const char* key, float fallback) {
    if (!node.is_object() || !node.contains(key)) {
        return fallback;
    }
    const auto& value = node[key];
    if (value.is_number()) {
        return static_cast<float>(value.get<double>());
    }
    if (value.is_string()) {
        try {
            return std::stof(value.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

bool read_bool_field(const nlohmann::json& node, const char* key, bool fallback) {
    if (!node.is_object() || !node.contains(key)) {
        return fallback;
    }
    const auto& value = node[key];
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<int>() != 0;
    }
    if (value.is_string()) {
        const std::string raw = trim_copy(value.get<std::string>());
        if (raw == "true" || raw == "1" || raw == "yes" || raw == "on") {
            return true;
        }
        if (raw == "false" || raw == "0" || raw == "no" || raw == "off") {
            return false;
        }
    }
    return fallback;
}

std::vector<std::string> read_status_effects(const nlohmann::json& node,
                                             const std::vector<std::string>& fallback) {
    if (!node.is_object() || !node.contains("status_effects")) {
        return fallback;
    }
    const auto& value = node["status_effects"];
    std::vector<std::string> effects;
    if (value.is_array()) {
        effects.reserve(value.size());
        for (const auto& entry : value) {
            if (!entry.is_string()) {
                continue;
            }
            const std::string trimmed = trim_copy(entry.get<std::string>());
            if (!trimmed.empty()) {
                effects.push_back(trimmed);
            }
        }
        return effects;
    }
    if (value.is_string()) {
        const std::string csv = value.get<std::string>();
        std::size_t cursor = 0;
        while (cursor <= csv.size()) {
            std::size_t comma = csv.find(',', cursor);
            const std::string token = trim_copy(csv.substr(cursor, comma - cursor));
            if (!token.empty()) {
                effects.push_back(token);
            }
            if (comma == std::string::npos) {
                break;
            }
            cursor = comma + 1;
        }
        return effects;
    }
    return fallback;
}

bool object_has_any_payload_fields(const nlohmann::json& node) {
    if (!node.is_object()) {
        return false;
    }
    static const char* kPayloadKeys[] = {
        "damage_amount",
        "damage_type",
        "hitback_enabled",
        "hitback_distance",
        "stun_frames",
        "animation_trigger",
        "sound_effect",
        "status_effects",
        "critical_hit_chance",
        "element_type",
        "recharge_seconds",
        "recharge_random_weight",
        "payload_id",
    };
    for (const char* key : kPayloadKeys) {
        if (node.contains(key)) {
            return true;
        }
    }
    return false;
}

void apply_payload_fields(const nlohmann::json& source, AttackPayload& target) {
    if (!source.is_object()) {
        return;
    }
    if (source.contains("damage_amount")) {
        target.damage_amount = read_int_field(source, "damage_amount", target.damage_amount);
    }
    if (source.contains("damage_type")) {
        target.damage_type = read_string_field(source, "damage_type", target.damage_type);
    }
    if (source.contains("hitback_enabled")) {
        target.hitback_enabled = read_bool_field(source, "hitback_enabled", target.hitback_enabled);
    }
    if (source.contains("hitback_distance")) {
        target.hitback_distance = read_float_field(source, "hitback_distance", target.hitback_distance);
    }
    if (source.contains("stun_frames")) {
        target.stun_frames = read_int_field(source, "stun_frames", target.stun_frames);
    }
    if (source.contains("animation_trigger")) {
        target.animation_trigger = read_string_field(source, "animation_trigger", target.animation_trigger);
    }
    if (source.contains("sound_effect")) {
        target.sound_effect = read_string_field(source, "sound_effect", target.sound_effect);
    }
    if (source.contains("status_effects")) {
        target.status_effects = read_status_effects(source, target.status_effects);
    }
    if (source.contains("critical_hit_chance")) {
        target.critical_hit_chance = read_float_field(source, "critical_hit_chance", target.critical_hit_chance);
    }
    if (source.contains("element_type")) {
        target.element_type = read_string_field(source, "element_type", target.element_type);
    }
    if (source.contains("recharge_seconds")) {
        target.recharge_seconds = read_float_field(source, "recharge_seconds", target.recharge_seconds);
    }
    if (source.contains("recharge_random_weight")) {
        target.recharge_random_weight =
            read_float_field(source, "recharge_random_weight", target.recharge_random_weight);
    }
    if (source.contains("payload_id")) {
        target.payload_id = read_string_field(source, "payload_id", target.payload_id);
    }
}

} // namespace

AttackPayload make_default_attack_payload() {
    return AttackPayload{};
}

AttackPayload sanitize_attack_payload(AttackPayload payload) {
    payload.damage_amount = std::max(0, payload.damage_amount);
    if (!std::isfinite(payload.hitback_distance) || payload.hitback_distance < 0.0f) {
        payload.hitback_distance = 0.0f;
    }
    payload.stun_frames = std::max(0, payload.stun_frames);
    if (!std::isfinite(payload.critical_hit_chance)) {
        payload.critical_hit_chance = 0.0f;
    }
    payload.critical_hit_chance = std::clamp(payload.critical_hit_chance, 0.0f, 1.0f);
    if (!std::isfinite(payload.recharge_seconds) || payload.recharge_seconds < 0.0f) {
        payload.recharge_seconds = 0.0f;
    }
    if (!std::isfinite(payload.recharge_random_weight) || payload.recharge_random_weight <= 0.0f) {
        payload.recharge_random_weight = 1.0f;
    }

    payload.damage_type = trim_copy(payload.damage_type);
    if (!has_non_whitespace(payload.damage_type)) {
        payload.damage_type = "physical";
    }
    payload.element_type = trim_copy(payload.element_type);
    if (!has_non_whitespace(payload.element_type)) {
        payload.element_type = "none";
    }
    payload.animation_trigger = trim_copy(payload.animation_trigger);
    payload.sound_effect = trim_copy(payload.sound_effect);
    payload.payload_id = trim_copy(payload.payload_id);

    std::vector<std::string> filtered_effects;
    filtered_effects.reserve(payload.status_effects.size());
    for (const std::string& effect : payload.status_effects) {
        const std::string trimmed = trim_copy(effect);
        if (!trimmed.empty()) {
            filtered_effects.push_back(trimmed);
        }
    }
    payload.status_effects = std::move(filtered_effects);
    return payload;
}

AttackPayload attack_payload_from_json(const nlohmann::json& value) {
    AttackPayload payload = make_default_attack_payload();
    apply_payload_fields(value, payload);
    return sanitize_attack_payload(std::move(payload));
}

nlohmann::json attack_payload_to_json(const AttackPayload& payload) {
    const AttackPayload sanitized = sanitize_attack_payload(payload);
    nlohmann::json json = nlohmann::json::object();
    json["damage_amount"] = sanitized.damage_amount;
    json["damage_type"] = sanitized.damage_type;
    json["hitback_enabled"] = sanitized.hitback_enabled;
    json["hitback_distance"] = sanitized.hitback_distance;
    json["stun_frames"] = sanitized.stun_frames;
    json["animation_trigger"] = sanitized.animation_trigger;
    json["sound_effect"] = sanitized.sound_effect;
    json["status_effects"] = sanitized.status_effects;
    json["critical_hit_chance"] = sanitized.critical_hit_chance;
    json["element_type"] = sanitized.element_type;
    json["recharge_seconds"] = sanitized.recharge_seconds;
    json["recharge_random_weight"] = sanitized.recharge_random_weight;
    json["payload_id"] = sanitized.payload_id;
    return json;
}

nlohmann::json parse_attack_meta_json(const std::string& raw_meta_json) {
    if (raw_meta_json.empty()) {
        return nlohmann::json::object();
    }
    nlohmann::json parsed = nlohmann::json::parse(raw_meta_json, nullptr, false);
    if (!parsed.is_object()) {
        return nlohmann::json::object();
    }
    return parsed;
}

AttackPayload attack_payload_from_box(int fallback_damage_amount,
                                      const std::string& fallback_payload_id,
                                      const std::string& raw_meta_json) {
    AttackPayload payload = make_default_attack_payload();
    payload.damage_amount = std::max(0, fallback_damage_amount);
    payload.payload_id = fallback_payload_id;

    const nlohmann::json meta = parse_attack_meta_json(raw_meta_json);
    if (meta.contains("attack_payload") && meta["attack_payload"].is_object()) {
        apply_payload_fields(meta["attack_payload"], payload);
    } else if (object_has_any_payload_fields(meta)) {
        // Backward compatibility for payload fields that were previously stored at root.
        apply_payload_fields(meta, payload);
    }

    if (payload.payload_id.empty()) {
        payload.payload_id = fallback_payload_id;
    }
    return sanitize_attack_payload(std::move(payload));
}

std::string merge_attack_payload_into_meta_json(const std::string& raw_meta_json,
                                                const AttackPayload& payload) {
    nlohmann::json meta = parse_attack_meta_json(raw_meta_json);
    if (!meta.is_object()) {
        meta = nlohmann::json::object();
    }
    meta["attack_payload"] = attack_payload_to_json(payload);
    return meta.dump();
}

} // namespace animation_update
