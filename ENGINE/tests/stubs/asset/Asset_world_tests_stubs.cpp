#include "assets/Asset.hpp"
#include "assets/asset/asset_info.hpp"

#include <utility>

#if defined(ENGINE_WORLD_TESTS)

// Minimal, non-rendering Asset/AssetInfo implementations for engine_world_tests.

AssetInfo::AssetInfo(const std::string& asset_folder_name)
    : AssetInfo(asset_folder_name, nlohmann::json::object()) {}

AssetInfo::AssetInfo(const std::string& asset_folder_name, const nlohmann::json& metadata) {
    name = asset_folder_name;
    type = "object";
    start_animation = "default";
    passable = true;
    min_same_type_distance = 0;
    min_distance_all = 0;
    scale_factor = 1.0f;
    smooth_scaling = true;
    original_canvas_width = metadata.value("original_canvas_width", 1);
    original_canvas_height = metadata.value("original_canvas_height", 1);
    flipable = true;
    tillable = false;
}

AssetInfo::~AssetInfo() = default;

std::shared_ptr<AssetInfo> AssetInfo::from_manifest_entry(const std::string& asset_folder_name,
                                                         const nlohmann::json& metadata) {
    return std::make_shared<AssetInfo>(asset_folder_name, metadata);
}

bool AssetInfo::has_tag(const std::string&) const { return false; }
void AssetInfo::loadAnimations(SDL_Renderer*) {}
bool AssetInfo::commit_manifest() { return false; }
nlohmann::json AssetInfo::manifest_payload() const { return nlohmann::json::object(); }
void AssetInfo::set_asset_type(const std::string& t) { type = t; }

Asset::Asset(std::shared_ptr<AssetInfo> info_in,
             const Area& spawn_area,
             SDL_Point start_pos,
             int depth_in,
             Asset* parent_in,
             const std::string& spawn_id_in,
             const std::string& spawn_method_in,
             int grid_resolution_in,
             std::optional<AnchorFollowTarget> anchor_follow)
    : parent(parent_in)
    , info(std::move(info_in))
    , current_animation()
    , pos_(nullptr)
    , initial_world_pos_(start_pos)
    , grid_resolution(vibble::grid::clamp_resolution(grid_resolution_in))
    , depth(depth_in)
    , spawn_id(spawn_id_in)
    , spawn_method(spawn_method_in)
    , owning_room_name_(spawn_area.get_name())
    , follow_anchor_(std::move(anchor_follow)) {
    current_scale = 1.0f;
    cached_w = info ? info->original_canvas_width : 0;
    cached_h = info ? info->original_canvas_height : 0;
    alpha_smoothing_.reset(1.0f);
}

Asset::~Asset() = default;

float Asset::smoothed_translation_x() const {
    return pos_ ? static_cast<float>(pos_->world_x()) : static_cast<float>(initial_world_pos_.x);
}

float Asset::smoothed_translation_y() const {
    return pos_ ? static_cast<float>(pos_->world_y()) : static_cast<float>(initial_world_pos_.y);
}

float Asset::smoothed_scale() const {
    return current_scale;
}

void Asset::set_assets(Assets* a) { assets_ = a; }
void Asset::sync_transform_to_position() {}

#endif // ENGINE_WORLD_TESTS

