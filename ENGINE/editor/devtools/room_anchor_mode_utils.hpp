#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <nlohmann/json_fwd.hpp>

#include "assets/asset/anchor_point.hpp"

namespace devmode::room_anchor_mode {

int wrap_index(int index, int count);

std::string trim_copy(std::string_view value);

std::string make_unique_anchor_name(const std::string& desired_name,
                                    const std::vector<std::string>& existing_names,
                                    const std::string& excluded_name = {});

std::string next_default_anchor_name(const std::vector<std::string>& existing_names);

SDL_Point default_anchor_position_for_frame(int frame_width, int frame_height);

DisplacedAssetAnchorPoint make_default_anchor_for_frame(const std::string& name,
                                                        int frame_width,
                                                        int frame_height);

nlohmann::json serialize_anchor_frame(const std::vector<DisplacedAssetAnchorPoint>& anchors);

void normalize_anchor_points_payload(nlohmann::json& animation_payload, std::size_t frame_count);

bool write_anchor_frame_to_payload(nlohmann::json& animation_payload,
                                   std::size_t frame_count,
                                   std::size_t frame_index,
                                   const std::vector<DisplacedAssetAnchorPoint>& anchors);

}  // namespace devmode::room_anchor_mode

