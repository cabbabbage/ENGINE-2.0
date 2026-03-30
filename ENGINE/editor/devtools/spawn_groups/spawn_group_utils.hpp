#pragma once

#include <string>
#include <optional>

#include <nlohmann/json_fwd.hpp>

#include "gameplay/spawn/spawn_group_codec.hpp"

namespace devmode::spawn {

constexpr int kPerimeterRadiusDefault = vibble::spawn_group_codec::kPerimeterRadiusDefault;

std::string generate_spawn_id();

nlohmann::json& ensure_spawn_groups_array(nlohmann::json& root);

const nlohmann::json* find_spawn_groups_array(const nlohmann::json& root);

bool sanitize_perimeter_spawn_groups(nlohmann::json& groups);

bool sanitize_spawn_group_candidates(nlohmann::json& entry);

bool ensure_spawn_group_entry_defaults(nlohmann::json& entry, const std::string& default_display_name, std::optional<int> default_resolution = std::nullopt);

}
