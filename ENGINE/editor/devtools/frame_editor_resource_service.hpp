#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

class AssetInfo;

namespace devmode::frame_editor {

struct FrameEditorResourceResult {
    bool success = false;
    int frame_count = 0;
    std::filesystem::path raw_folder;
    std::string message;

    explicit operator bool() const { return success; }
};

struct FrameEditorResourceContext {
    std::string asset_name;
    std::filesystem::path asset_root;
    std::filesystem::path asset_directory;
    std::string animation_id;

    nlohmann::json* animation_payload = nullptr;
    std::function<nlohmann::json*()> animation_payload_provider;

    AssetInfo* asset_info = nullptr;
    std::shared_ptr<AssetInfo> shared_asset_info;

    std::function<bool()> save_manifest;
};

class FrameEditorResourceService {
public:
    explicit FrameEditorResourceService(FrameEditorResourceContext context);

    FrameEditorResourceResult delete_frames(const std::vector<int>& selected_indices);
    FrameEditorResourceResult duplicate_frames(const std::vector<int>& selected_indices);
    FrameEditorResourceResult reorder_frames(const std::vector<int>& selected_indices, int insertion_index);
    FrameEditorResourceResult insert_frame(int insertion_index, const std::filesystem::path& image_path);
    FrameEditorResourceResult replace_frame(int frame_index, const std::filesystem::path& image_path);

private:
    struct ResolvedAnimation;
    struct FramePlanEntry;

    nlohmann::json* payload() const;
    FrameEditorResourceResult fail(std::string message) const;
    std::optional<ResolvedAnimation> resolve_editable_animation(FrameEditorResourceResult& result) const;
    FrameEditorResourceResult apply_plan(const ResolvedAnimation& resolved,
                                         const std::vector<FramePlanEntry>& plan,
                                         const std::vector<std::filesystem::path>& external_images);

    FrameEditorResourceContext context_;
};

} // namespace devmode::frame_editor
