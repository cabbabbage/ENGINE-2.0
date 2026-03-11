#include "spawn_group_utils.hpp"

#include "gameplay/spawn/spawn_group_codec.hpp"

namespace devmode::spawn {

std::string generate_spawn_id() {
    return vibble::spawn_group_codec::generate_spawn_id();
}

nlohmann::json& ensure_spawn_groups_array(nlohmann::json& root) {
    return vibble::spawn_group_codec::ensure_spawn_groups_array(root);
}

const nlohmann::json* find_spawn_groups_array(const nlohmann::json& root) {
    return vibble::spawn_group_codec::find_spawn_groups_array(root);
}

bool sanitize_perimeter_spawn_groups(nlohmann::json& groups) {
    if (!groups.is_array()) {
        return false;
    }
    bool changed = false;
    for (auto& entry : groups) {
        if (!entry.is_object()) {
            continue;
        }
        changed = vibble::spawn_group_codec::sanitize_perimeter_edge_fields(entry) || changed;
    }
    return changed;
}

bool sanitize_spawn_group_candidates(nlohmann::json& entry) {
    return vibble::spawn_group_codec::sanitize_spawn_group_candidates(entry);
}

bool ensure_spawn_group_entry_defaults(nlohmann::json& entry,
                                       const std::string& default_display_name,
                                       std::optional<int> default_resolution) {
    vibble::spawn_group_codec::EntryDefaults defaults{};
    defaults.display_name = default_display_name;
    defaults.resolution = default_resolution;
    return vibble::spawn_group_codec::ensure_spawn_group_entry_defaults(entry, defaults);
}

} // namespace devmode::spawn
