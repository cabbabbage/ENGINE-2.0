#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class AssetInfo;
class Assets;
struct SDL_Renderer;

namespace devmode::core {
class ManifestStore;
}

namespace devmode::animation_import {

struct ImportResult {
    bool success = false;
    int frames_written = 0;
    std::string asset_key;
    std::vector<std::string> animation_ids;
    std::string message;
    std::vector<std::string> warnings;
};

struct CreateAssetRequest {
    std::string asset_name;
    std::vector<std::filesystem::path> input_paths;
    devmode::core::ManifestStore* manifest_store = nullptr;
    Assets* assets = nullptr;
};

nlohmann::json build_folder_animation_payload(const std::string& animation_id,
                                              int frame_count);

nlohmann::json build_default_asset_manifest(const std::string& asset_name,
                                            const std::filesystem::path& asset_dir,
                                            int frame_count);

ImportResult import_frames_to_animation_folder(const std::filesystem::path& asset_root,
                                               const std::string& animation_id,
                                               const std::vector<std::filesystem::path>& input_paths);

bool delete_asset_cache(const std::string& asset_key, std::string& error_message);

bool verify_animation_files(const std::filesystem::path& asset_root,
                            const std::string& animation_id,
                            int expected_frames,
                            std::string& error_message);

bool verify_runtime_textures(const std::shared_ptr<AssetInfo>& info,
                             const std::string& animation_id,
                             int expected_frames,
                             std::string& error_message);

ImportResult reload_and_verify_runtime(Assets* assets,
                                       const std::string& asset_key,
                                       const std::string& animation_id,
                                       int expected_frames);

ImportResult create_asset_from_frames(const CreateAssetRequest& request);

} // namespace devmode::animation_import
