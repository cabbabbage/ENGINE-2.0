#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "animation/combat_geometry.hpp"

namespace devmode::room_box_payload {

std::string make_unique_box_name(const std::string& desired_name,
                                 const std::vector<std::string>& existing_names,
                                 const std::string& fallback_base,
                                 const std::string& excluded_name = {});

animation_update::FrameHitBox make_default_hit_box(const std::vector<std::string>& existing_names,
                                                   int frame_width,
                                                   int frame_height);
animation_update::FrameAttackBox make_default_attack_box(const std::vector<std::string>& existing_names,
                                                         int frame_width,
                                                         int frame_height);

bool write_hit_box_frame_to_payload(nlohmann::json& animation_payload,
                                    std::size_t frame_count,
                                    std::size_t frame_index,
                                    const std::vector<animation_update::FrameHitBox>& boxes);

bool write_attack_box_frame_to_payload(nlohmann::json& animation_payload,
                                       std::size_t frame_count,
                                       std::size_t frame_index,
                                       const std::vector<animation_update::FrameAttackBox>& boxes);

}  // namespace devmode::room_box_payload
