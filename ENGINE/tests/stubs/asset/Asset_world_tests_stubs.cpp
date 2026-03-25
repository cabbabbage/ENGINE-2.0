#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

#if defined(ENGINE_WORLD_TESTS)

class AnimationRuntime {
public:
    AnimationRuntime() = default;
    ~AnimationRuntime() = default;
};

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
void AssetInfo::loadAnimations(SDL_Renderer*, bool) {}
bool AssetInfo::commit_manifest() { return false; }
nlohmann::json AssetInfo::manifest_payload() const { return nlohmann::json::object(); }
void AssetInfo::set_asset_type(const std::string& t) { type = t; }

Asset::Asset(std::shared_ptr<AssetInfo> info_in,
             const Area& spawn_area,
             SDL_Point start_pos,
             int depth_in,
             const std::string& spawn_id_in,
             const std::string& spawn_method_in,
             int grid_resolution_in)
    : info(std::move(info_in))
    , current_animation()
    , provisional_grid_point_(GridPoint::make_virtual(start_pos.x,
                                                      0,
                                                      start_pos.y,
                                                      vibble::grid::clamp_resolution(grid_resolution_in)))
    , pos_(&provisional_grid_point_)
    , grid_resolution(vibble::grid::clamp_resolution(grid_resolution_in))
    , depth(depth_in)
    , spawn_id(spawn_id_in)
    , spawn_method(spawn_method_in)
    , owning_room_name_(spawn_area.get_name()) {
    current_scale = 1.0f;
    cached_w = info ? info->original_canvas_width : 0;
    cached_h = info ? info->original_canvas_height : 0;
    alpha_smoothing_.reset(1.0f);
}

Asset::~Asset() = default;

float Asset::smoothed_translation_x() const {
    return pos_ ? static_cast<float>(pos_->world_x()) : 0.0f;
}

float Asset::smoothed_translation_y() const {
    return pos_ ? static_cast<float>(pos_->world_y()) : 0.0f;
}

float Asset::smoothed_scale() const {
    return current_scale;
}

float Asset::runtime_height_px() const {
    return (cached_h > 0) ? static_cast<float>(cached_h) : 0.0f;
}

Asset::PerspectiveSample Asset::runtime_perspective_sample() const {
    PerspectiveSample sample{};
    sample.scale = 1.0f;
    sample.resolution_layer = pos_ ? pos_->resolution_layer() : grid_resolution;
    sample.source = PerspectiveSource::Default;

    if (pos_ && std::isfinite(pos_->perspective_scale()) && pos_->perspective_scale() > 0.0001f) {
        sample.scale = std::max(0.0001f, pos_->perspective_scale());
        sample.resolution_layer = pos_->resolution_layer();
        sample.source = PerspectiveSource::AssetGridPoint;
        return sample;
    }

    if (std::isfinite(last_scale_perspective_input_) && last_scale_perspective_input_ > 0.0001f) {
        sample.scale = std::max(0.0001f, last_scale_perspective_input_);
        sample.source = PerspectiveSource::CachedLastFrame;
        return sample;
    }

    return sample;
}

const char* Asset::perspective_source_label(PerspectiveSource source) {
    switch (source) {
    case PerspectiveSource::CameraTraversal:
        return "camera-traversal";
    case PerspectiveSource::AssetGridPoint:
        return "asset-grid-point";
    case PerspectiveSource::CachedLastFrame:
        return "cached-last-frame";
    case PerspectiveSource::Default:
    default:
        return "default";
    }
}

void Asset::set_assets(Assets* a) { assets_ = a; }
void Asset::sync_transform_to_position() {}
void Asset::clear_grid_id() { grid_id_ = 0; }
void Asset::mark_anchors_dirty() {}

void Asset::set_provisional_grid_point(const world::GridPoint& point) {
    set_provisional_grid_point(point.world_x(),
                               point.world_y(),
                               point.world_z(),
                               point.resolution_layer());
}

void Asset::set_provisional_grid_point(int world_x, int world_y, int world_z, int resolution_layer) {
    provisional_grid_point_.~GridPoint();
    new (&provisional_grid_point_) GridPoint(
        GridPoint::make_virtual(world_x,
                                world_y,
                                world_z,
                                vibble::grid::clamp_resolution(resolution_layer)));
    pos_ = &provisional_grid_point_;
}

void Asset::cache_grid_residency(const world::GridPoint& point) {
    has_cached_grid_residency_ = true;
    cached_grid_residency_ = point.key();
}

#endif // ENGINE_WORLD_TESTS
