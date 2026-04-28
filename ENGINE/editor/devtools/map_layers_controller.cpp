#include "map_layers_controller.hpp"

#include "map_layers_common.hpp"
#include "gameplay/map_generation/map_layers_geometry.hpp"
#include "devtools/core/manifest_store.hpp"
#include "tag_utils.hpp"
#include "utils/display_color.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <sstream>
#include <SDL3/SDL_log.h>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {
constexpr int kDefaultRoomRangeMax = 64;
constexpr const char* kDefaultSpawnRoomName = "Spawn";
using map_layers::clamp_candidate_max;
using map_layers::clamp_candidate_min;

bool candidate_source_is_room_name(const json& candidate) {
    std::string source_type = candidate.value("source_type", std::string());
    if (source_type.empty()) {
        return true;
    }
    return source_type == "room_name";
}

std::string candidate_room_name_value(const json& candidate) {
    std::string value = candidate.value("value", std::string());
    if (!value.empty()) {
        return value;
    }
    return candidate.value("name", std::string());
}

std::vector<std::string> collect_room_tags(const json& room_entry) {
    std::vector<std::string> tags;
    auto add_tag = [&](const std::string& raw) {
        std::string normalized = tag_utils::normalize(raw);
        if (normalized.empty()) {
            return;
        }
        if (std::find(tags.begin(), tags.end(), normalized) == tags.end()) {
            tags.push_back(std::move(normalized));
        }
    };

    if (!room_entry.is_object()) {
        return tags;
    }

    auto add_from_array = [&](const json& arr) {
        if (!arr.is_array()) {
            return;
        }
        for (const auto& entry : arr) {
            if (entry.is_string()) {
                add_tag(entry.get<std::string>());
            }
        }
    };

    if (room_entry.contains("room_tags")) {
        add_from_array(room_entry["room_tags"]);
    }

    if (room_entry.contains("tags")) {
        const auto& tags_section = room_entry["tags"];
        if (tags_section.is_array()) {
            add_from_array(tags_section);
        } else if (tags_section.is_object()) {
            if (tags_section.contains("include")) add_from_array(tags_section["include"]);
            if (tags_section.contains("tags")) add_from_array(tags_section["tags"]);
        }
    }

    return tags;
}
}

namespace map_layers {
inline double min_edge_distance_from_map_info(const json& map_info) {

    if (!map_info.is_object()) {
        return static_cast<double>(kDefaultMinEdgeDistance);
    }
    const auto it = map_info.find("map_layers_settings");
    if (it == map_info.end() || !it->is_object()) {
        return static_cast<double>(kDefaultMinEdgeDistance);
    }
    int stored = it->value("min_edge_distance", kDefaultMinEdgeDistance);
    return static_cast<double>(stored);
}
}

void MapLayersController::bind(json* map_info, std::string map_path) {
    map_info_ = map_info;
    static_cast<void>(map_path);
    ensure_initialized();
    dirty_ = false;
    notify();
}

void MapLayersController::set_manifest_store(devmode::core::ManifestStore* store, std::string map_id) {
    manifest_store_ = store;
    map_id_ = std::move(map_id);
}

void MapLayersController::set_save_coordinator(devmode::core::DevSaveCoordinator* coordinator) {
    save_coordinator_ = coordinator;
}

void MapLayersController::set_save_manager(devmode::core::SaveManager* manager) {
    save_manager_ = manager;
}

MapLayersController::ListenerId MapLayersController::add_listener(Listener cb) {
    if (!cb) return 0;
    const ListenerId id = next_listener_id_++;
    listeners_.push_back(ListenerEntry{id, std::move(cb)});
    return id;
}

void MapLayersController::remove_listener(ListenerId id) {
    if (id == 0) {
        return;
    }
    auto it = std::remove_if(listeners_.begin(), listeners_.end(),
                             [id](const ListenerEntry& entry) { return entry.id == id; });
    listeners_.erase(it, listeners_.end());
}

void MapLayersController::clear_listeners() {
    listeners_.clear();
    next_listener_id_ = 1;
}

bool MapLayersController::save(devmode::core::DevSaveCoordinator::Priority priority) {
    if (!map_info_) return false;
    mark_dirty(priority);
    return true;
}

bool MapLayersController::reload() {
    if (!map_info_) return false;
    if (!manifest_store_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[MapLayersController] Cannot reload map info: manifest store is not available.");
        return false;
    }
    if (map_id_.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[MapLayersController] Cannot reload map info: map identifier is empty.");
        return false;
    }
    const nlohmann::json* entry = manifest_store_->find_map_entry(map_id_);
    if (!entry) {
        std::cerr << "[MapLayersController] Map '" << map_id_ << "' not found in manifest\n";
        return false;
    }
    *map_info_ = *entry;
    ensure_initialized();
    mark_clean();
    notify();
    return true;
}

void MapLayersController::mark_clean() {
    dirty_ = false;
}

int MapLayersController::layer_count() const {
    if (!map_info_) return 0;
    const json* layers = map_info_->contains("map_layers") ? &(*map_info_)["map_layers"] : nullptr;
    if (!layers || !layers->is_array()) return 0;
    return static_cast<int>(layers->size());
}

const json* MapLayersController::layer(int index) const {
    if (!map_info_) return nullptr;
    const auto& arr = layers();
    if (!arr.is_array() || index < 0 || index >= static_cast<int>(arr.size())) return nullptr;
    return &arr[index];
}

json* MapLayersController::layer(int index) {
    if (!map_info_) return nullptr;
    auto& arr = (*map_info_)["map_layers"];
    if (!arr.is_array() || index < 0 || index >= static_cast<int>(arr.size())) return nullptr;
    return &arr[index];
}

const json& MapLayersController::layers() const {
    static json empty = json::array();
    if (!map_info_) return empty;
    const auto it = map_info_->find("map_layers");
    if (it == map_info_->end() || !it->is_array()) return empty;
    return *it;
}

std::vector<std::string> MapLayersController::available_rooms() const {
    std::vector<std::string> result;
    if (!map_info_) return result;
    const auto rooms_it = map_info_->find("rooms_data");
    if (rooms_it == map_info_->end() || !rooms_it->is_object()) return result;
    result.reserve(rooms_it->size());
    for (auto it = rooms_it->begin(); it != rooms_it->end(); ++it) {
        result.push_back(it.key());
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> MapLayersController::available_room_tags() const {
    std::vector<std::string> result;
    if (!map_info_) return result;
    const auto rooms_it = map_info_->find("rooms_data");
    if (rooms_it == map_info_->end() || !rooms_it->is_object()) return result;

    for (auto it = rooms_it->begin(); it != rooms_it->end(); ++it) {
        const std::vector<std::string> room_tags = collect_room_tags(it.value());
        for (const std::string& tag : room_tags) {
            if (std::find(result.begin(), result.end(), tag) == result.end()) {
                result.push_back(tag);
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

double MapLayersController::min_edge_distance() const {
    if (!map_info_) {
        return static_cast<double>(map_layers::kDefaultMinEdgeDistance);
    }
    return map_layers::min_edge_distance_from_map_manifest(*map_info_);
}

bool MapLayersController::set_min_edge_distance(double value) {
    if (!map_info_) {
        return false;
    }
    ensure_map_settings();
    const double clamped = std::clamp(value, 0.0, map_layers::kMinEdgeDistanceMax);
    int stored = static_cast<int>(std::lround(clamped));
    auto& settings = (*map_info_)["map_layers_settings"];
    if (!settings.is_object()) {
        settings = json::object();
    }
    int current = settings.value("min_edge_distance", stored);
    if (current == stored) {
        return false;
    }
    settings["min_edge_distance"] = stored;
    mark_dirty();
    notify();
    return true;
}

int MapLayersController::create_layer(const std::string& display_name) {
    if (!map_info_) return -1;
    ensure_initialized();
    auto& arr = (*map_info_)["map_layers"];
    const int idx = static_cast<int>(arr.size());
    json layer = {
        {"level", idx},
        {"name", display_name.empty() ? std::string("layer_") + std::to_string(idx) : display_name},
        {"min_rooms", 0},
        {"max_rooms", 0},
        {"rooms", json::array()}
};
    arr.push_back(std::move(layer));
    ensure_layer_indices();
    mark_dirty();
    notify();
    return idx;
}

bool MapLayersController::delete_layer(int index) {
    if (!map_info_) return false;
    if (index == 0) return false;
    auto& arr = (*map_info_)["map_layers"];
    if (!arr.is_array() || index < 0 || index >= static_cast<int>(arr.size())) return false;
    arr.erase(arr.begin() + index);
    ensure_layer_indices();
    mark_dirty();
    notify();
    return true;
}

bool MapLayersController::reorder_layer(int from, int to) {
    if (!map_info_) return false;
    auto& arr = (*map_info_)["map_layers"];
    if (!arr.is_array() || arr.empty()) return false;
    const int count = static_cast<int>(arr.size());
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) return false;
    if (from == 0 || to == 0) return false;
    json layer = arr[from];
    arr.erase(arr.begin() + from);
    arr.insert(arr.begin() + to, std::move(layer));
    ensure_layer_indices();
    mark_dirty();
    notify();
    return true;
}

int MapLayersController::duplicate_layer(int index) {
    if (!map_info_) return -1;
    auto& arr = (*map_info_)["map_layers"];
    if (!arr.is_array() || index < 0 || index >= static_cast<int>(arr.size())) return -1;

    ensure_initialized();

    json copy = arr[index];
    if (!copy.is_object()) copy = json::object();

    auto name_exists = [&](const std::string& candidate) {
        return std::any_of(arr.begin(), arr.end(), [&](const json& entry) {
            return entry.is_object() && entry.value("name", std::string()) == candidate;
        });
};

    std::string base_name = copy.value("name", std::string());
    if (base_name.empty()) {
        base_name = std::string("layer_") + std::to_string(index);
    }
    std::string candidate = base_name + " Copy";
    int suffix = 2;
    while (name_exists(candidate)) {
        candidate = base_name + " Copy " + std::to_string(suffix++);
    }
    copy["name"] = candidate;

    auto& rooms = copy["rooms"];
    if (!rooms.is_array()) {
        rooms = json::array();
    }
    for (auto& entry : rooms) {
        normalize_candidate_shape(entry);
    }

    const int insert_index = std::min(index + 1, static_cast<int>(arr.size()));
    arr.insert(arr.begin() + insert_index, std::move(copy));
    ensure_layer_indices();
    mark_dirty();
    notify();
    return insert_index;
}

bool MapLayersController::rename_layer(int index, const std::string& name) {
    if (!validate_layer_index(index)) return false;
    auto* layer_json = layer(index);
    if (!layer_json) return false;
    std::string trimmed = name;
    trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), trimmed.end());
    if (trimmed.empty()) return false;
    (*layer_json)["name"] = trimmed;
    mark_dirty();
    notify();
    return true;
}

void MapLayersController::normalize_candidate_shape(json& candidate) {
    if (!candidate.is_object()) {
        candidate = json::object();
    }
    std::string source_type = candidate.value("source_type", std::string());
    if (source_type != "room_name" && source_type != "room_tag") {
        source_type = "room_name";
    }
    std::string value = candidate.value("value", std::string());
    if (value.empty()) {
        value = candidate.value("name", std::string());
    }
    if (!candidate.contains("min_instances")) candidate["min_instances"] = 0;
    if (!candidate.contains("max_instances")) candidate["max_instances"] = 1;
    if (!candidate.contains("required_children") || !candidate["required_children"].is_array()) {
        candidate["required_children"] = json::array();
    }
    candidate["source_type"] = source_type;
    candidate["value"] = value;
    candidate["name"] = value; // Backward-compat mirror.
}

bool MapLayersController::add_candidate_internal(int layer_index,
                                                 const std::string& source_type,
                                                 const std::string& value) {
    if (!validate_layer_index(layer_index)) return false;
    if (value.empty()) return false;
    if (layer_index == 0 && source_type != "room_name") return false;
    auto* layer_json = layer(layer_index);
    if (!layer_json) return false;
    auto& rooms = (*layer_json)["rooms"];
    if (!rooms.is_array()) rooms = json::array();

    if (layer_index == 0) {
        std::string previous_name;
        if (!rooms.empty() && rooms[0].is_object() && candidate_source_is_room_name(rooms[0])) {
            previous_name = candidate_room_name_value(rooms[0]);
        }

        json candidate = {
            {"source_type", source_type},
            {"value", value},
            {"name", value},
            {"min_instances", 1},
            {"max_instances", 1},
            {"required_children", json::array()}
        };

        if (!rooms.empty() && rooms[0].is_object()) {
            json& spawn_entry = rooms[0];
            auto& required = spawn_entry["required_children"];
            if (!required.is_array()) {
                required = json::array();
            }
            candidate["required_children"] = required;
        }

        rooms = json::array({candidate});
        clamp_layer_counts(*layer_json);
        if (source_type == "room_name" && !previous_name.empty() && previous_name != value) {
            if (map_info_) {
                map_layers::rename_room_references_in_layers(*map_info_, previous_name, value);
            }
        }
        if (source_type == "room_name") {
            ensure_spawn_room_data(previous_name);
        }
        mark_dirty();
        notify();
        return true;
    }

    json candidate = {
        {"source_type", source_type},
        {"value", value},
        {"name", value},
        {"min_instances", 0},
        {"max_instances", 1},
        {"required_children", json::array()}
    };
    rooms.push_back(std::move(candidate));
    clamp_layer_counts(*layer_json);
    mark_dirty();
    notify();
    return true;
}

bool MapLayersController::add_candidate(int layer_index, const std::string& room_name) {
    return add_candidate_internal(layer_index, "room_name", room_name);
}

bool MapLayersController::add_candidate_tag(int layer_index, const std::string& room_tag) {
    return add_candidate_internal(layer_index, "room_tag", room_tag);
}

bool MapLayersController::remove_candidate(int layer_index, int candidate_index) {
    if (!validate_layer_index(layer_index)) return false;
    if (layer_index == 0) return false;
    auto* layer_json = layer(layer_index);
    if (!layer_json) return false;
    auto& rooms = (*layer_json)["rooms"];
    if (!rooms.is_array() || candidate_index < 0 || candidate_index >= static_cast<int>(rooms.size())) return false;
    rooms.erase(rooms.begin() + candidate_index);
    clamp_layer_counts(*layer_json);
    mark_dirty();
    notify();
    return true;
}

bool MapLayersController::set_candidate_instance_range(int layer_index,
                                                       int candidate_index,
                                                       int min_instances,
                                                       int max_instances) {
    if (!validate_layer_index(layer_index)) return false;
    if (layer_index == 0) return false;
    auto* layer_json = layer(layer_index);
    if (!layer_json) return false;
    auto& rooms = (*layer_json)["rooms"];
    if (!rooms.is_array() || candidate_index < 0 || candidate_index >= static_cast<int>(rooms.size())) return false;
    auto& candidate = rooms[candidate_index];
    int clamped_min = clamp_candidate_min(min_instances);
    int clamped_max = clamp_candidate_max(clamped_min, max_instances);
    bool changed = false;
    if (candidate.value("min_instances", -1) != clamped_min) {
        candidate["min_instances"] = clamped_min;
        changed = true;
    }
    if (candidate.value("max_instances", -1) != clamped_max) {
        candidate["max_instances"] = clamped_max;
        changed = true;
    }
    clamp_layer_counts(*layer_json);
    if (changed) {
        mark_dirty();
        notify();
    }
    return changed;
}

bool MapLayersController::set_candidate_instance_count(int layer_index, int candidate_index, int max_instances) {
    if (!validate_layer_index(layer_index)) return false;
    if (layer_index == 0) return false;
    auto* layer_json = layer(layer_index);
    if (!layer_json) return false;
    auto& rooms = (*layer_json)["rooms"];
    if (!rooms.is_array() || candidate_index < 0 || candidate_index >= static_cast<int>(rooms.size())) return false;
    auto& candidate = rooms[candidate_index];
    int current_min = clamp_candidate_min(candidate.value("min_instances", 0));
    return set_candidate_instance_range(layer_index, candidate_index, current_min, max_instances);
}

bool MapLayersController::add_candidate_child(int layer_index, int candidate_index, const std::string& child_room) {
    if (!validate_layer_index(layer_index)) return false;
    auto* layer_json = layer(layer_index);
    if (!layer_json) return false;
    auto& rooms = (*layer_json)["rooms"];
    if (!rooms.is_array() || candidate_index < 0 || candidate_index >= static_cast<int>(rooms.size())) return false;
    if (child_room.empty()) return false;
    auto& candidate = rooms[candidate_index];
    auto& required = candidate["required_children"];
    if (!required.is_array()) required = json::array();
    bool changed = false;
    if (std::find(required.begin(), required.end(), child_room) == required.end()) {
        required.push_back(child_room);
        changed = true;
    }

    auto& layers_arr = (*map_info_)["map_layers"];
    if (!layers_arr.is_array()) layers_arr = json::array();
    int child_layer_index = layer_index + 1;
    bool layer_added = false;

    if (child_layer_index >= static_cast<int>(layers_arr.size())) {
        int new_level = static_cast<int>(layers_arr.size());
        json child_layer = {
            {"level", new_level},
            {"name", std::string("layer_") + std::to_string(new_level)},
            {"max_rooms", 0},
            {"rooms", json::array()}
};
        layers_arr.push_back(std::move(child_layer));
        child_layer_index = new_level;
        layer_added = true;
    }

    json& child_layer = layers_arr[child_layer_index];
    if (!child_layer.is_object()) child_layer = json::object();
    auto& child_rooms = child_layer["rooms"];
    if (!child_rooms.is_array()) child_rooms = json::array();

    bool child_layer_changed = false;
    auto child_it = std::find_if(child_rooms.begin(), child_rooms.end(), [&](const json& entry) {
        if (!entry.is_object()) return false;
        const std::string source = entry.value("source_type", std::string("room_name"));
        if (source != "room_name") return false;
        const std::string value = entry.value("value", entry.value("name", std::string()));
        return value == child_room;
    });
    if (child_it == child_rooms.end()) {
        json child_candidate = {
            {"source_type", "room_name"},
            {"value", child_room},
            {"name", child_room},
            {"min_instances", 0},
            {"max_instances", 1},
            {"required_children", json::array()}
};
        child_rooms.push_back(std::move(child_candidate));
        child_layer_changed = true;
    } else {
        json& entry = *child_it;
        int current_min = clamp_candidate_min(entry.value("min_instances", 0));
        int current_max = clamp_candidate_max(current_min, entry.value("max_instances", 1));
        if (entry.value("min_instances", -1) != current_min) {
            entry["min_instances"] = current_min;
            child_layer_changed = true;
        }
        if (entry.value("max_instances", -1) != current_max) {
            entry["max_instances"] = current_max;
            child_layer_changed = true;
        }
    }

    clamp_layer_counts(child_layer);
    if (layer_added) {
        ensure_layer_indices();
    }

    if (child_layer_changed) changed = true;
    if (layer_added) changed = true;

    clamp_layer_counts(*layer_json);

    if (changed) {
        mark_dirty();
        notify();
    }
    return changed;
}

bool MapLayersController::remove_candidate_child(int layer_index, int candidate_index, const std::string& child_room) {
    if (!validate_layer_index(layer_index)) return false;
    auto* layer_json = layer(layer_index);
    if (!layer_json) return false;
    auto& rooms = (*layer_json)["rooms"];
    if (!rooms.is_array() || candidate_index < 0 || candidate_index >= static_cast<int>(rooms.size())) return false;
    auto& candidate = rooms[candidate_index];
    auto& required = candidate["required_children"];
    if (!required.is_array()) return false;
    auto it = std::find(required.begin(), required.end(), child_room);
    if (it == required.end()) return false;
    required.erase(it);
    mark_dirty();
    notify();
    return true;
}

void MapLayersController::ensure_initialized() {
    if (!map_info_) return;
    ensure_map_settings();
    if (!map_info_->contains("map_layers") || !(*map_info_)["map_layers"].is_array()) {
        (*map_info_)["map_layers"] = json::array();
    }
    auto map_radius_it = map_info_->find("map_radius");
    if (map_radius_it != map_info_->end()) {
        map_info_->erase(map_radius_it);
    }
    ensure_layer_indices();

    auto rooms_it = map_info_->find("rooms_data");
    if (rooms_it != map_info_->end()) {
        if (!rooms_it->is_object()) {
            *rooms_it = json::object();
        }
        std::vector<SDL_Color> colors = utils::display_color::collect(*rooms_it);
        bool mutated = false;
        for (auto entry_it = rooms_it->begin(); entry_it != rooms_it->end(); ++entry_it) {
            if (!entry_it->is_object()) {
                *entry_it = json::object();
            }
            bool entry_mutated = false;
            utils::display_color::ensure(*entry_it, colors, &entry_mutated);
            mutated = mutated || entry_mutated;
        }
        if (mutated) {
            mark_dirty();
        }
    }
}

void MapLayersController::ensure_map_settings() {
    if (!map_info_) {
        return;
    }
    double sanitized = map_layers::min_edge_distance_from_map_manifest(*map_info_);
    auto& settings = (*map_info_)["map_layers_settings"];
    if (!settings.is_object()) {
        settings = json::object();
    }
    settings["min_edge_distance"] = static_cast<int>(std::lround(sanitized));
}

void MapLayersController::ensure_layer_indices() {
    if (!map_info_) return;
    auto& arr = (*map_info_)["map_layers"];
    if (!arr.is_array()) {
        arr = json::array();
        return;
    }
    for (size_t i = 0; i < arr.size(); ++i) {
        auto& layer_json = arr[i];
        if (!layer_json.is_object()) layer_json = json::object();
        layer_json["level"] = static_cast<int>(i);
        if (!layer_json.contains("name")) {
            std::ostringstream oss;
            oss << "layer_" << i;
            layer_json["name"] = oss.str();
        }
        if (!layer_json.contains("min_rooms")) layer_json["min_rooms"] = 0;
        if (!layer_json.contains("max_rooms")) layer_json["max_rooms"] = 0;
        if (!layer_json.contains("rooms") || !layer_json["rooms"].is_array()) {
            layer_json["rooms"] = json::array();
        }
        auto radius_it = layer_json.find("radius");
        if (radius_it != layer_json.end()) {
            layer_json.erase(radius_it);
        }
        clamp_layer_counts(layer_json);
        auto& rooms = layer_json["rooms"];
        for (auto& candidate : rooms) {
            normalize_candidate_shape(candidate);
            int min_inst = clamp_candidate_min(candidate.value("min_instances", 0));
            int max_inst = clamp_candidate_max(min_inst, candidate.value("max_instances", min_inst));
            candidate["min_instances"] = min_inst;
            candidate["max_instances"] = max_inst;
        }
    }
}

bool MapLayersController::validate_layer_index(int index) const {
    if (!map_info_) return false;
    const auto& arr = layers();
    return arr.is_array() && index >= 0 && index < static_cast<int>(arr.size());
}

bool MapLayersController::validate_candidate_index(const json& layer, int candidate_index) const {
    if (!layer.is_object()) return false;
    const auto it = layer.find("rooms");
    if (it == layer.end() || !it->is_array()) return false;
    return candidate_index >= 0 && candidate_index < static_cast<int>(it->size());
}

void MapLayersController::mark_dirty(devmode::core::DevSaveCoordinator::Priority priority) {
    dirty_ = true;
    if (dirty_callback_) {
        dirty_callback_(priority);
    }
}

void MapLayersController::notify() {
    auto it = listeners_.begin();
    while (it != listeners_.end()) {
        if (!it->callback) {
            it = listeners_.erase(it);
            continue;
        }
        it->callback();
        ++it;
    }
}

void MapLayersController::clamp_layer_counts(json& layer) const {
    if (!layer.is_object()) return;

    int level = layer.value("level", -1);
    auto& rooms_ref = layer["rooms"];
    if (!rooms_ref.is_array()) {
        rooms_ref = json::array();
    }
    if (level == 0) {
        std::string previous_name;
        if (!rooms_ref.empty()) {
            if (rooms_ref[0].is_object() && candidate_source_is_room_name(rooms_ref[0])) {
                previous_name = candidate_room_name_value(rooms_ref[0]);
            } else if (rooms_ref[0].is_string()) {
                previous_name = rooms_ref[0].get<std::string>();
            }
        }
        if (rooms_ref.empty() || !rooms_ref[0].is_object()) {
            json candidate = {
                {"source_type", "room_name"},
                {"value", kDefaultSpawnRoomName},
                {"name", kDefaultSpawnRoomName},
                {"min_instances", 1},
                {"max_instances", 1},
                {"required_children", json::array()}
};
            rooms_ref = json::array({candidate});
        }
        json& spawn_entry = rooms_ref[0];
        normalize_candidate_shape(spawn_entry);
        spawn_entry["source_type"] = "room_name";
        if (spawn_entry.value("value", std::string()).empty()) {
            spawn_entry["value"] = kDefaultSpawnRoomName;
            spawn_entry["name"] = kDefaultSpawnRoomName;
        }
        spawn_entry["min_instances"] = 1;
        spawn_entry["max_instances"] = 1;
        auto& required = spawn_entry["required_children"];
        if (!required.is_array()) {
            required = json::array();
        }
        if (rooms_ref.size() > 1) {
            rooms_ref.erase(rooms_ref.begin() + 1, rooms_ref.end());
        }
        layer["min_rooms"] = 1;
        layer["max_rooms"] = 1;
        std::string current_name;
        if (candidate_source_is_room_name(spawn_entry)) {
            current_name = candidate_room_name_value(spawn_entry);
        }
        if (!previous_name.empty() && !current_name.empty() && previous_name != current_name) {
            if (map_info_) {
                map_layers::rename_room_references_in_layers(*map_info_, previous_name, current_name);
            }
        }
        if (candidate_source_is_room_name(spawn_entry)) {
            ensure_spawn_room_data(previous_name);
        }
        return;
    }

    int min_sum = 0;
    int max_sum = 0;
    if (rooms_ref.is_array()) {
        for (auto& candidate : rooms_ref) {
            if (!candidate.is_object()) continue;
            int min_inst = clamp_candidate_min(candidate.value("min_instances", 0));
            int max_inst = clamp_candidate_max(min_inst, candidate.value("max_instances", min_inst));
            candidate["min_instances"] = min_inst;
            candidate["max_instances"] = max_inst;
            min_sum += min_inst;
            max_sum += max_inst;
        }
    }

    int derived_min = std::min(min_sum, kDefaultRoomRangeMax);
    int derived_max = std::max(min_sum, max_sum);
    derived_max = std::min(derived_max, kDefaultRoomRangeMax);

    layer["min_rooms"] = derived_min;
    layer["max_rooms"] = derived_max;
}

void MapLayersController::ensure_spawn_room_data(const std::string& previous_name) const {
    if (!map_info_) {
        return;
    }

    if (!map_info_->is_object()) {
        *map_info_ = json::object();
    }

    json& rooms_data = (*map_info_)["rooms_data"];
    if (!rooms_data.is_object()) {
        rooms_data = json::object();
    }

    std::string spawn_room_name = kDefaultSpawnRoomName;
    const auto layers_it = map_info_->find("map_layers");
    if (layers_it != map_info_->end() && layers_it->is_array() && !layers_it->empty() && (*layers_it)[0].is_object()) {
        const auto rooms_it = (*layers_it)[0].find("rooms");
        if (rooms_it != (*layers_it)[0].end() && rooms_it->is_array() && !rooms_it->empty() && (*rooms_it)[0].is_object()) {
            const json& candidate = (*rooms_it)[0];
            if (candidate_source_is_room_name(candidate)) {
                std::string candidate_name = candidate_room_name_value(candidate);
                if (!candidate_name.empty()) {
                    spawn_room_name = candidate_name;
                }
            }
        }
    }

    const std::string prev = previous_name;
    auto ensure_room_entry = [&](const std::string& key) {
        json& entry = rooms_data[key];
        if (!entry.is_object()) {
            entry = json::object();
        }
        entry["name"] = key;
        entry.erase("is_spawn");
        if (!entry.contains("room_tags") || !entry["room_tags"].is_array()) {
            entry["room_tags"] = json::array();
        }
        if (!entry.contains("spawn_groups") || !entry["spawn_groups"].is_array()) {
            entry["spawn_groups"] = json::array();
        }
    };

    if (!prev.empty() && prev != spawn_room_name && rooms_data.contains(prev) && rooms_data[prev].is_object() &&
        !rooms_data.contains(spawn_room_name)) {
        rooms_data[spawn_room_name] = rooms_data[prev];
        rooms_data.erase(prev);
    }

    ensure_room_entry(spawn_room_name);
}
