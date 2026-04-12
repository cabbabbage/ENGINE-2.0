#pragma once

#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace animation_update {

struct AttackPayload {
    int damage_amount = 0;
    std::string damage_type = "physical";

    bool hitback_enabled = true;
    float hitback_distance = 0.0f;
    int stun_frames = 0;

    std::string animation_trigger;
    std::string sound_effect;
    std::vector<std::string> status_effects;

    float critical_hit_chance = 0.0f;
    std::string element_type = "none";

    std::string payload_id;
};

AttackPayload make_default_attack_payload();
AttackPayload sanitize_attack_payload(AttackPayload payload);
AttackPayload attack_payload_from_json(const nlohmann::json& value);
nlohmann::json attack_payload_to_json(const AttackPayload& payload);

nlohmann::json parse_attack_meta_json(const std::string& raw_meta_json);
AttackPayload attack_payload_from_box(int fallback_damage_amount,
                                      const std::string& fallback_payload_id,
                                      const std::string& raw_meta_json);
std::string merge_attack_payload_into_meta_json(const std::string& raw_meta_json,
                                                const AttackPayload& payload);

} // namespace animation_update
