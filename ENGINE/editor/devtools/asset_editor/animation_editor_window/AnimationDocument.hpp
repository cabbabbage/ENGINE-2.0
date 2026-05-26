#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace animation_editor {

class AnimationDocument {
  public:
    enum class CreateAnimationResult {
        Created,
        AlreadyExists,
        InvalidName,
        Error,
    };

    enum class StructureChangeKind {
        Created,
        Deleted,
        Renamed,
    };

    struct StructureChangeEvent {
        StructureChangeKind kind = StructureChangeKind::Created;
        std::string animation_id;
        std::string previous_animation_id;
    };

    struct LoadReport {
        int parse_failures = 0;
        int normalization_failures = 0;
    };

    AnimationDocument();

    void load_from_file(const std::filesystem::path& info_path);
    void load_from_manifest(const nlohmann::json& asset_json,
                            const std::filesystem::path& asset_root,
                            std::function<bool(const nlohmann::json&)> persist_callback);
    void save_to_file(bool fire_callback = true) const;
    bool save_to_file_checked(bool fire_callback = true) const;

    bool consume_dirty_flag() const;
    bool clear_dirty_if_revision_not_newer(std::uint64_t revision) const;

    CreateAnimationResult create_animation(const std::string& animation_id);
    void delete_animation(const std::string& animation_id);

    std::vector<std::string> animation_ids() const;
    std::optional<std::string> start_animation() const;
    void set_start_animation(const std::string& animation_id);

    void rename_animation(const std::string& old_id, const std::string& new_id);
    void replace_animation_payload(const std::string& animation_id, const std::string& payload_json);
    bool update_animation_payload(const std::string& animation_id, const nlohmann::json& payload);
    bool save_animation_payload_immediately(const std::string& animation_id, const nlohmann::json& payload);
    std::optional<std::string> animation_payload(const std::string& animation_id) const;
    std::optional<nlohmann::json> animation_payload_json(const std::string& animation_id) const;
    const std::filesystem::path& info_path() const { return info_path_; }
    const std::filesystem::path& asset_root() const { return asset_root_; }
    std::uint64_t revision() const { return revision_; }
    const LoadReport& last_load_report() const { return last_load_report_; }
    void set_manifest_asset_key_debug(std::string key);
    const std::string& manifest_asset_key_debug() const { return manifest_asset_key_debug_; }

    void set_on_saved_callback(std::function<void()> callback);
    void set_on_structure_changed_callback(std::function<void(const StructureChangeEvent&)> callback);

    double scale_percentage() const;

  private:
    std::optional<nlohmann::json> raw_animation_payload_json(const std::string& animation_id) const;
    nlohmann::json normalize_payload_for_storage(const std::string& animation_id,
                                                 const nlohmann::json& payload) const;
    void load_from_json_object(const nlohmann::json& root);
    void ensure_document_initialized(bool track_load_diagnostics = false);
    void rebuild_animation_cache();
    void mark_dirty() const;

  private:
    std::filesystem::path info_path_;
    std::filesystem::path asset_root_;
    std::unordered_map<std::string, std::string> animations_;
    std::optional<std::string> start_animation_;
    bool use_nested_container_ = false;
    std::string container_metadata_;
    mutable bool dirty_ = false;
    mutable std::uint64_t revision_ = 1;
    mutable nlohmann::json base_data_;
    std::string manifest_asset_key_debug_;
    LoadReport last_load_report_;
    std::function<bool(const nlohmann::json&)> persist_callback_;
    mutable std::unordered_set<std::string> touched_animation_ids_;
    std::function<void()> on_saved_callback_;
    std::function<void(const StructureChangeEvent&)> on_structure_changed_callback_;
};

}


