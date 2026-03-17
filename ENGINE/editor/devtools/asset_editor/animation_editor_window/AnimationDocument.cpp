#include "AnimationDocument.hpp"

#include "devtools/core/dev_json_store.hpp"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <optional>
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

    ensure_bool("flipped_source", false);
    ensure_bool("reverse_source", false);
    ensure_bool("locked", false);
    ensure_bool("loop", true);
    ensure_bool("rnd_start", false);

    bool derived_from_animation = (kind == "animation");
    bool derived_reverse = read_bool_field_like(payload, "reverse_source", false);
    bool derived_flip_x = read_bool_field_like(payload, "flipped_source", false);
    bool derived_flip_y = false;
    bool derived_flip_movement_x = false;
    bool derived_flip_movement_y = false;
    if (payload.contains("derived_modifiers") && payload["derived_modifiers"].is_object()) {
        const auto& modifiers = payload["derived_modifiers"];
        if (modifiers.contains("reverse")) {
            derived_reverse = read_bool_like(modifiers["reverse"], derived_reverse);
        }
        if (modifiers.contains("flipX")) {
            derived_flip_x = read_bool_like(modifiers["flipX"], derived_flip_x);
        }
        if (modifiers.contains("flipY")) {
            derived_flip_y = read_bool_like(modifiers["flipY"], false);
        }
        if (modifiers.contains("flipMovementX")) {
            derived_flip_movement_x = read_bool_like(modifiers["flipMovementX"], false);
        }
        if (modifiers.contains("flipMovementY")) {
            derived_flip_movement_y = read_bool_like(modifiers["flipMovementY"], false);
        }
    }

    bool inherit_source_movement = read_bool_field_like(payload, "inherit_source_movement", derived_from_animation);

    payload["inherit_source_movement"] = inherit_source_movement;

    if (derived_from_animation) {
        payload["derived_modifiers"] = nlohmann::json{{"reverse", derived_reverse},
                                                       {"flipX", derived_flip_x},
                                                       {"flipY", derived_flip_y},
                                                       {"flipMovementX", derived_flip_movement_x},
                                                       {"flipMovementY", derived_flip_movement_y}};

        if (inherit_source_movement) {
            payload.erase("movement");
            payload.erase("movement_total");
            payload.erase("movement_variants");
        }

        payload.erase("audio");
        payload.erase("locked");
        payload.erase("movement_preview_bounds");
    } else {
        payload.erase("derived_modifiers");
    }
    payload["reverse_source"] = derived_reverse;
    payload["flipped_source"] = derived_flip_x;

    payload.erase("fps");
    payload.erase("speed");
    payload.erase("speed_factor");
    payload.erase("speed_multiplier");

    int frames = read_int_like(payload.contains("number_of_frames") ? payload["number_of_frames"] : nlohmann::json(1), 1);
    if (frames < 1) frames = 1;
    payload["number_of_frames"] = frames;

    if (!derived_from_animation || (derived_from_animation && !inherit_source_movement)) {
        nlohmann::json movement = payload.contains("movement") && payload["movement"].is_array() ? payload["movement"] : nlohmann::json::array();
        if (!movement.is_array()) {
            movement = nlohmann::json::array();
        }
        if (movement.size() < static_cast<size_t>(frames)) {
            while (movement.size() < static_cast<size_t>(frames)) {
                movement.push_back(nlohmann::json::array({0, 0}));
            }
        } else if (movement.size() > static_cast<size_t>(frames)) {
            movement.erase(movement.begin() + frames, movement.end());
        }
        if (movement.empty()) {
            movement.push_back(nlohmann::json::array({0, 0}));
        }
        payload["movement"] = movement;

        auto read_component = [](const nlohmann::json& entry, int index) -> int {
            if (entry.is_array()) {
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
                const char* keys[] = {"dx", "dy", "dz"};
                const char* key = (index >= 0 && index < 3) ? keys[index] : nullptr;
                if (key && entry.contains(key)) {
                    return read_int_like(entry[key], 0);
                }
            }
            return 0;
};

        int total_dx = 0;
        int total_dy = 0;
        int total_dz = 0;
        for (std::size_t i = 1; i < movement.size(); ++i) {
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
    payload["on_end"] = on_end;


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

std::string serialize_payload(const nlohmann::json& payload) {
    return payload.dump();
}

nlohmann::json parse_payload(const std::string& payload_dump, const std::string& animation_id) {
    if (payload_dump.empty()) {
        return coerce_payload(animation_id, nlohmann::json::object());
    }
    nlohmann::json parsed = nlohmann::json::parse(payload_dump, nullptr, false);
    if (parsed.is_discarded()) {
        SDL_Log("AnimationDocument: failed to parse payload for '%s'", animation_id.c_str());
        return coerce_payload(animation_id, nlohmann::json::object());
    }
    return coerce_payload(animation_id, parsed);
}

std::string normalize_animation_id(std::string value) {
    std::string trimmed = animation_editor::strings::trim_copy(value);
    return animation_editor::strings::to_lower_copy(trimmed);
}

}

namespace animation_editor {

AnimationDocument::AnimationDocument() = default;

void AnimationDocument::load_from_file(const std::filesystem::path& info_path) {
    info_path_ = info_path;
    asset_root_ = info_path.empty() ? std::filesystem::path{} : info_path.parent_path();
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
                animations_[item.key()] = serialize_payload(coerce_payload(item.key(), item.value()));
            }
        }
    }

    ensure_document_initialized();
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
        animations_json[id] = parse_payload(payload_dump, id);
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
        // Force immediate persistence to disk so dev-mode edits are visible without debounce.
        devmode::core::DevJsonStore::instance().flush_all();
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
        nlohmann::json payload = parse_payload(entry.second, id);

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
            entry.second = serialize_payload(coerce_payload(id, payload));
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
    std::string normalized = serialize_payload(coerce_payload(animation_id, payload));
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
    auto it = animations_.find(animation_id);
    if (it == animations_.end()) {
        return std::nullopt;
    }
    return parse_payload(it->second, animation_id);
}

void AnimationDocument::ensure_document_initialized() {
    bool mutated = false;
    std::vector<std::string> ids;
    ids.reserve(animations_.size());

    for (auto& entry : animations_) {
        nlohmann::json normalized = parse_payload(entry.second, entry.first);
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

void AnimationDocument::mark_dirty() const { dirty_ = true; }

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
