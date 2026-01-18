#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace vibble {

class RebuildQueueCoordinator {
public:
    RebuildQueueCoordinator();

    void request_full_asset_rebuild() const;
    void request_asset(const std::string& asset_name,
                       const std::vector<std::string>& animations = {}) const;
    void request_animation(const std::string& asset_name, const std::string& animation) const;
    void request_frame(const std::string& asset_name, const std::string& animation, int frame_index) const;

    bool has_pending_asset_work() const;

    bool run_asset_tool(const std::string& command_prefix = std::string()) const;
    bool validate_manifest_cache(const std::string& command_prefix = std::string()) const;

private:
    using json = nlohmann::json;

    std::filesystem::path repo_root_;
    std::filesystem::path manifest_path_;
    std::filesystem::path cache_root_;

    void mark_all_frames_for_rebuild() const;
    void mark_asset_for_rebuild(const std::string& asset_name) const;
    void mark_animation_for_rebuild(const std::string& asset_name, const std::string& animation) const;
    void mark_frame_for_rebuild(const std::string& asset_name, const std::string& animation, int frame_index) const;

    bool run_python_script(const std::filesystem::path& script, const std::vector<std::string>& args, const std::string& command_prefix) const;
    bool manifest_has_needs_rebuild() const;
};

}
