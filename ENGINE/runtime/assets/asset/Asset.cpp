#include "Asset.hpp"
#include "controller_factory.hpp"
#include "animation.hpp"
#include "core/AssetsManager.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/render.hpp"
#include "animation/animation_runtime.hpp"
#include "animation/animation_update.hpp"
#include "utils/area_helpers.hpp"
#include "assets/asset_filter_tags.hpp"
#include "asset_types.hpp"
#include "utils/grid.hpp"
#include "utils/transform_smoothing_settings.hpp"
#include "utils/log.hpp"
#include "gameplay/world/grid_point.hpp"
#include "gameplay/world/world_grid.hpp"
#include <iostream>
#include <random>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <exception>
#include <cassert>
#include <new>
#include <SDL3/SDL.h>
#include "utils/FramePointResolver.hpp"
#include "utils/AnchorPointResolver.hpp"
#include "animation/controllers/shared/anchor_bound_asset_helper.hpp"

#if !defined(NDEBUG)
namespace {
struct AnchorUpdateCounters {
        std::atomic<std::uint64_t> calls{0};
        std::atomic<std::uint64_t> cache_hits{0};
        std::atomic<std::uint64_t> recomputes{0};
};

AnchorUpdateCounters& anchor_update_counters() {
        static AnchorUpdateCounters counters{};
        return counters;
}
} // namespace
#endif
static std::mt19937& asset_rng()
{
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
}

static std::mutex& asset_rng_mutex()
{
        static std::mutex mutex;
        return mutex;
}

std::unordered_map<std::string, std::pair<bool,bool>> Asset::s_flip_overrides_{};
std::mutex Asset::s_flip_overrides_mutex_{};

namespace {

bool vibble_scale_trace_enabled() {
    static const bool enabled = [] {
        const char* raw = SDL_getenv("VIBBLE_SCALE_TRACE");
        if (!raw || !*raw) {
            return false;
        }
        const std::string value(raw);
        return value == "1" ||
               value == "true" ||
               value == "TRUE" ||
               value == "on" ||
               value == "ON";
    }();
    return enabled;
}

bool is_traced_asset_name(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    std::string lowered = name;
    for (char& ch : lowered) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered.find("vibble") != std::string::npos;
}

bool should_trace_asset_scale(const Asset& asset) {
    return vibble_scale_trace_enabled() &&
           asset.info &&
           is_traced_asset_name(asset.info->name);
}

bool should_emit_scale_trace_for_frame(const Asset& asset, std::uint32_t frame_id) {
    static std::unordered_map<const Asset*, std::uint32_t> last_logged_frame;
    auto it = last_logged_frame.find(&asset);
    if (it != last_logged_frame.end() && it->second == frame_id) {
        return false;
    }
    last_logged_frame[&asset] = frame_id;
    return true;
}

}

 #if !defined(ENGINE_WORLD_TESTS)
Asset::Asset(std::shared_ptr<AssetInfo> info_,
             const Area& spawn_area,
             SDL_Point start_pos,
             int depth_,
             const std::string& spawn_id_,
             const std::string& spawn_method_,
             int grid_resolution_)
: info(std::move(info_))
, current_animation()
, static_frame(false)
, active(false)
, provisional_grid_point_(GridPoint::make_virtual(start_pos.x, 0, start_pos.y, vibble::grid::clamp_resolution(grid_resolution_)))
, grid_point_(&provisional_grid_point_)
, grid_resolution(vibble::grid::clamp_resolution(grid_resolution_))
, depth(depth_)
, spawn_id(spawn_id_)
, spawn_method(spawn_method_)
, owning_room_name_(spawn_area.get_name())
{
	set_flip();

        try {
                if (info && asset_types::canonicalize(info->type) == asset_types::player) {
                        grid_resolution = 0;
                }
        } catch (...) {

        }
        std::string start_id = info->start_animation.empty() ? std::string{"default"} : info->start_animation;
        auto it = info->animations.find(start_id);
        if (it == info->animations.end()) {
                it = info->animations.find("default");
        }
        if (it != info->animations.end() && it->second.has_frames()) {
                current_animation = it->first;
                Animation& anim  = it->second;
                static_frame     = (anim.frame_count() == 1);
                current_frame    = anim.get_first_frame();
                if ((anim.randomize || anim.rnd_start) && anim.frame_count() > 1) {
                        std::uniform_int_distribution<int> d(0, int(anim.frame_count()) - 1);
                        int idx;
                        {
                                std::lock_guard<std::mutex> lock(asset_rng_mutex());
                                idx = d(asset_rng());
                        }
                        AnimationFrame* f = anim.get_first_frame();
                        while (idx-- > 0 && f && f->next) { f = f->next; }
                        current_frame = f;
                }
        }

        alpha_smoothing_.set_params(transform_smoothing::asset_alpha_params());

        alpha_smoothing_.reset(hidden ? 0.0f : 1.0f);
        refresh_filter_tags();
        initialize_anchor_registry_from_animations();
}

void Asset::refresh_filter_tags() {
    if (info) {
        filter_type_tag_ = asset_types::canonicalize(info->type);
    } else {
        filter_type_tag_.clear();
    }
    filter_method_tag_ = asset_filters::canonicalize_spawn_method(spawn_method);
}

 #if !defined(ENGINE_WORLD_TESTS)
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
        grid_point_ = &provisional_grid_point_;
}

#endif // !ENGINE_WORLD_TESTS

void Asset::clear_downscale_cache() {
    if (last_scaled_texture_) {
        SDL_DestroyTexture(last_scaled_texture_);
        last_scaled_texture_ = nullptr;
    }
    last_scaled_source_ = nullptr;
    last_scaled_w_ = 0;
    last_scaled_h_ = 0;
    last_scaled_camera_scale_ = -1.0f;
    downscale_cache_ready_revision_ = 0;
}

 #if !defined(ENGINE_WORLD_TESTS)
Asset::~Asset() {
        clear_render_caches();
        if (composite_texture_) {
                SDL_DestroyTexture(composite_texture_);
                composite_texture_ = nullptr;
        }
        visibility_stamp = 0;
}

#endif // !ENGINE_WORLD_TESTS

Asset::Asset(const Asset& o)
    : info(o.info)
    , current_animation(o.current_animation)
    , provisional_grid_point_(o.grid_point_
            ? GridPoint::make_virtual(o.grid_point_->world_x(),
                                      o.grid_point_->world_y(),
                                      o.grid_point_->world_z(),
                                      o.grid_point_->resolution_layer())
            : GridPoint::make_virtual(0, 0, 0, vibble::grid::clamp_resolution(o.grid_resolution)))
    , grid_point_(&provisional_grid_point_)
    , grid_resolution(vibble::grid::clamp_resolution(o.grid_resolution))
    , active(o.active)
    , flipped(o.flipped)
    , distance_from_camera(o.distance_from_camera)
    , angle_from_camera(o.angle_from_camera)
    , render_depth_bias_(o.render_depth_bias_)
    , depth(o.depth)
    , dead(o.dead)
    , static_frame(o.static_frame)
    , needs_target(o.needs_target)
    , cached_w(o.cached_w)
    , cached_h(o.cached_h)
    , window(o.window)
    , highlighted(o.highlighted)
    , hidden(o.hidden)
    , selected(o.selected)
    , current_frame(o.current_frame)
    , frame_progress(o.frame_progress)
    , assets_(o.assets_)
    , spawn_id(o.spawn_id)
    , spawn_method(o.spawn_method)
    , owning_room_name_(o.owning_room_name_)
    , controller_(nullptr)
    , tiling_info_(o.tiling_info_)
    , anim_(nullptr)
    , last_scaled_texture_(nullptr)
    , last_scaled_source_(nullptr)
    , last_scaled_w_(0)
    , last_scaled_h_(0)
    , last_scaled_camera_scale_(-1.0f)
    , last_scale_usage_()
    , last_rendered_frame_(nullptr)
    , scale_variant_state_(o.scale_variant_state_)
    , last_scale_base_input_(o.last_scale_base_input_)
    , last_scale_perspective_input_(o.last_scale_perspective_input_)
    , last_scale_camera_input_(o.last_scale_camera_input_)
    , last_scale_quality_cap_input_(o.last_scale_quality_cap_input_)
    , grid_id_(o.grid_id_)
    , composite_texture_(nullptr)
    , composite_dirty_(true)
    , composite_rect_({0, 0, 0, 0})
    , composite_scale_(o.composite_scale_)
    , composite_depth_cue_settings_version_(o.composite_depth_cue_settings_version_)
    , world_z_offset_(o.world_z_offset_)
    , render_anchor_offset_x_(o.render_anchor_offset_x_)
    , render_anchor_offset_y_(o.render_anchor_offset_y_)
    , render_anchor_offset_z_(o.render_anchor_offset_z_)
{

        clear_render_caches();
        last_scale_usage_ = o.last_scale_usage_;
        scale_variant_state_ = o.scale_variant_state_;
        cached_grid_residency_    = o.cached_grid_residency_;
        has_cached_grid_residency_ = o.has_cached_grid_residency_;
        alpha_smoothing_          = o.alpha_smoothing_;
        finalized_                = o.finalized_;
        refresh_filter_tags();
        initialize_anchor_registry_from_animations();
}

Asset& Asset::operator=(const Asset& o) {
        if (this == &o) return *this;

        clear_render_caches();
        info                 = o.info;
        current_animation    = o.current_animation;
        if (o.grid_point_) {
                set_provisional_grid_point(o.grid_point_->world_x(),
                                           o.grid_point_->world_y(),
                                           o.grid_point_->world_z(),
                                           o.grid_point_->resolution_layer());
        } else {
                set_provisional_grid_point(0, 0, 0, vibble::grid::clamp_resolution(o.grid_resolution));
        }
    grid_resolution      = vibble::grid::clamp_resolution(o.grid_resolution);
	active               = o.active;
        flipped              = o.flipped;
        distance_from_camera = o.distance_from_camera;
        angle_from_camera = o.angle_from_camera;
        render_depth_bias_   = o.render_depth_bias_;
	depth                = o.depth;
	dead                 = o.dead;
	static_frame         = o.static_frame;
        needs_target        = o.needs_target;
	cached_w             = o.cached_w;
	cached_h             = o.cached_h;
	window               = o.window;
        highlighted          = o.highlighted;
        hidden               = o.hidden;
        selected             = o.selected;
        current_frame        = o.current_frame;
        frame_progress       = o.frame_progress;
        last_rendered_frame_   = nullptr;
        assets_              = o.assets_;
        spawn_id             = o.spawn_id;
        spawn_method         = o.spawn_method;
        owning_room_name_    = o.owning_room_name_;
        controller_.reset();
        children_.clear();
        anim_.reset();
        tiling_info_         = o.tiling_info_;
        last_scaled_texture_      = nullptr;
        last_scaled_source_       = nullptr;
        last_scaled_w_            = 0;
        last_scaled_h_            = 0;
        last_scaled_camera_scale_ = -1.0f;
        last_scale_usage_         = o.last_scale_usage_;
        scale_variant_state_      = o.scale_variant_state_;
        last_scale_base_input_    = o.last_scale_base_input_;
        last_scale_perspective_input_ = o.last_scale_perspective_input_;
        last_scale_camera_input_  = o.last_scale_camera_input_;
        last_scale_quality_cap_input_ = o.last_scale_quality_cap_input_;
        cached_grid_residency_    = o.cached_grid_residency_;
        has_cached_grid_residency_ = o.has_cached_grid_residency_;
        alpha_smoothing_          = o.alpha_smoothing_;
        finalized_                = o.finalized_;
        grid_id_                  = o.grid_id_;
        composite_texture_        = nullptr;
        composite_dirty_          = true;
        composite_rect_           = {0, 0, 0, 0};
        composite_scale_          = o.composite_scale_;
        composite_depth_cue_settings_version_ = o.composite_depth_cue_settings_version_;
        world_z_offset_           = o.world_z_offset_;
        render_anchor_offset_x_   = o.render_anchor_offset_x_;
        render_anchor_offset_y_   = o.render_anchor_offset_y_;
        render_anchor_offset_z_   = o.render_anchor_offset_z_;
        anchor_handles_.clear();
        anchor_points_.clear();
        anchor_name_to_index_.clear();
        current_hit_box_volumes_.clear();
        current_attack_box_volumes_.clear();
        runtime_hit_box_lookup_.clear();
        runtime_attack_box_lookup_.clear();
        anchors_initialized_ = false;
        refresh_filter_tags();
        initialize_anchor_registry_from_animations();
        return *this;
}

void Asset::add_child(Asset* child) {
    if (!child || child == this) {
        return;
    }
    if (std::find(children_.begin(), children_.end(), child) != children_.end()) {
        return;
    }
    children_.push_back(child);
}

void Asset::remove_child(Asset* child) {
    if (!child) {
        return;
    }
    auto it = std::remove(children_.begin(), children_.end(), child);
    if (it == children_.end()) {
        return;
    }
    children_.erase(it, children_.end());
}

bool Asset::has_child(const Asset* child) const {
    return child && std::find(children_.begin(), children_.end(), child) != children_.end();
}

void Asset::initialize_anchor_registry_from_animations() {
        if (anchors_initialized_) {
                return;
        }

        anchor_handles_.clear();
        anchor_points_.clear();
        anchor_name_to_index_.clear();

        if (!info) {
                anchors_initialized_ = true;
                return;
        }

        std::unordered_set<std::string> unique_names;
        auto collect_from_frame = [&](const AnimationFrame* frame) {
                if (!frame) {
                        return;
                }
                for (const auto& anchor : frame->anchor_points) {
                        if (anchor.is_valid()) {
                                unique_names.insert(anchor.name);
                        }
                }
        };

        for (const auto& [anim_id, anim] : info->animations) {
                (void)anim_id;
                const std::size_t path_count = anim.movement_path_count();
                for (std::size_t i = 0; i < path_count; ++i) {
                        const auto& path = anim.movement_path(i);
                        for (const auto& frame : path) {
                                collect_from_frame(&frame);
                        }
                }
        }

        if (!unique_names.empty()) {
                std::vector<std::string> sorted_names(unique_names.begin(), unique_names.end());
                std::sort(sorted_names.begin(), sorted_names.end());
                anchor_handles_.reserve(sorted_names.size());
                anchor_points_.reserve(sorted_names.size());
                for (const auto& anchor_name : sorted_names) {
                        AnchorHandle handle{};
                        handle.name = anchor_name;
                        handle.owner = this;
                        anchor_name_to_index_[anchor_name] = anchor_handles_.size();
                        anchor_handles_.push_back(std::move(handle));
                        AnchorPoint point{};
                        point.name = anchor_name;
                        anchor_points_.push_back(std::move(point));
                }
        }

        anchors_initialized_ = true;
}

Asset::AnchorHandle* Asset::find_anchor_handle(const std::string& name) {
        if (!anchors_initialized_) {
                initialize_anchor_registry_from_animations();
        }
        auto it = anchor_name_to_index_.find(name);
        if (it == anchor_name_to_index_.end()) {
                return nullptr;
        }
        if (it->second >= anchor_handles_.size()) {
                return nullptr;
        }
        return &anchor_handles_[it->second];
}

void Asset::finalize_setup() {
        if (finalized_) {
                return;
        }
        if (!info) return;
        if (!anchors_initialized_) {
                initialize_anchor_registry_from_animations();
        }
        if (current_animation.empty() ||
        !info->animations[current_animation].has_frames())
        {
		std::string start_id = info->start_animation.empty() ? std::string{"default"} : info->start_animation;
		auto it = info->animations.find(start_id);
		if (it == info->animations.end()) it = info->animations.find("default");
		if (it == info->animations.end()) it = info->animations.begin();
		if (it != info->animations.end() && it->second.has_frames()) {
			current_animation = it->first;
			Animation& anim = it->second;
                        anim.change(current_frame, static_frame);
                        frame_progress = 0.0f;
                        if ((anim.randomize || anim.rnd_start) && anim.frame_count() > 1) {
                                std::uniform_int_distribution<int> dist(0, int(anim.frame_count()) - 1);
                                int idx;
                                {
                                        std::lock_guard<std::mutex> lock(asset_rng_mutex());
                                        idx = dist(asset_rng());
                                }
                                AnimationFrame* f = anim.get_first_frame();
                                while (idx-- > 0 && f && f->next) { f = f->next; }
                                current_frame = f;
                        }
                }
	}
        if (!current_animation.empty() && info->animations.count(current_animation) &&
            info->animations[current_animation].has_frames()) {
            ensure_animation_runtime(false);
        } else {
            vibble::log::warn("[Asset] Cannot create animation runtime: animation '" + current_animation + "' is missing or has no frames");
        }
        if (assets_ && !controller_) {
                ControllerFactory cf(assets_);
                controller_ = cf.create_for_asset(this);
        }
        NeighborSearchRadius = info->NeighborSearchRadius;
        refresh_cached_dimensions();

        finalized_ = true;
}
void Asset::update_scale_values(bool force) {
    const std::uint32_t frame_id = assets_ ? assets_->frame_id() : 0;
    const std::uint64_t camera_state_version = assets_ ? assets_->getView().camera_state_version() : 0;
    if (!force &&
        last_scale_update_frame_id_ == frame_id &&
        last_scale_update_camera_state_version_ == camera_state_version) {
        return;
    }
    last_scale_update_frame_id_ = frame_id;
    last_scale_update_camera_state_version_ = camera_state_version;

    const PerspectiveSample perspective_sample = runtime_perspective_sample();
    const float base_scale =
        (info && std::isfinite(info->scale_factor) && info->scale_factor > 0.0f) ? info->scale_factor : 1.0f;
    const float perspective_scale = perspective_sample.scale;
    const char* perspective_source = perspective_source_label(perspective_sample.source);

    float camera_scale = 1.0f;
    if (assets_) {
        camera_scale = static_cast<float>(std::max(0.0001, assets_->getView().get_scale()));
    } else if (window) {
        camera_scale = static_cast<float>(std::max(0.0001, window->get_scale()));
    }
    float quality_cap = render_pipeline::ScalingLogic::QualityCap();
    if (!std::isfinite(quality_cap) || quality_cap <= 0.0f) {
        quality_cap = 1.0f;
    }

    const float prospective_scale = base_scale * perspective_scale;
    constexpr float kScaleEpsilon = 1e-4f;
    const float scale_delta = prospective_scale - current_scale;
    const bool trace_scale = should_trace_asset_scale(*this) &&
                             should_emit_scale_trace_for_frame(*this, frame_id);
    if (trace_scale) {
        vibble::log::debug(std::string("[ScaleTrace][Asset] asset='") +
                           (info ? info->name : std::string{"<unknown>"}) +
                           "' frame=" + std::to_string(frame_id) +
                           " source=" + perspective_source +
                           " perspective=" + std::to_string(perspective_scale) +
                           " base=" + std::to_string(base_scale) +
                           " scale=" + std::to_string(prospective_scale) +
                           " delta=" + std::to_string(scale_delta));
    }

    if (std::fabs(prospective_scale - current_scale) < kScaleEpsilon &&
        std::fabs(base_scale - last_scale_base_input_) < kScaleEpsilon &&
        std::fabs(perspective_scale - last_scale_perspective_input_) < kScaleEpsilon &&
        std::fabs(camera_scale - last_scale_camera_input_) < kScaleEpsilon &&
        std::fabs(quality_cap - last_scale_quality_cap_input_) < kScaleEpsilon) {
        return;
    }

    current_scale = prospective_scale;
    last_scale_base_input_ = base_scale;
    last_scale_perspective_input_ = perspective_scale;
    last_scale_camera_input_ = camera_scale;
    last_scale_quality_cap_input_ = quality_cap;

    // Select variants against the actual render scale so we prefer downscaling
    // a larger source texture, and only upscale when no larger variant exists.
    float desired_variant_scale = current_scale;
    if (!std::isfinite(desired_variant_scale) || desired_variant_scale <= 0.0f) {
        desired_variant_scale = 1.0f;
    }

    const auto& steps = render_pipeline::ScalingLogic::DefaultScaleSteps();

    render_pipeline::ScalingLogic::HysteresisState hysteresis_state{};
    hysteresis_state.last_index = scale_variant_state_.last_variant_index;
    hysteresis_state.min_scale = scale_variant_state_.hysteresis_min;
    hysteresis_state.max_scale = scale_variant_state_.hysteresis_max;

    auto selection = render_pipeline::ScalingLogic::Choose(
        desired_variant_scale,
        steps,
        hysteresis_state,
        desired_variant_scale,
        render_pipeline::ScalingLogic::HysteresisOptions{});

    current_nearest_variant_scale = selection.stored_scale;
    current_variant_index = selection.index;

    scale_variant_state_.last_variant_index = selection.index;
    scale_variant_state_.hysteresis_min = selection.hysteresis_min;
    scale_variant_state_.hysteresis_max = selection.hysteresis_max;

    if (current_nearest_variant_scale > 0.0f) {
        current_remaining_scale_adjustment = current_scale / current_nearest_variant_scale;
    } else {
        current_remaining_scale_adjustment = 1.0f;
    }

    last_scale_usage_.requested_scale = current_scale;
    last_scale_usage_.texture_scale = current_nearest_variant_scale;
    last_scale_usage_.remainder_scale = current_remaining_scale_adjustment;
    last_scale_usage_.variant_index = current_variant_index;

    // Variant/remainder changes alter package dimensions and source texture selection.
    mark_composite_dirty();
    mark_anchors_dirty();
    mark_mesh_dirty();
}

 #if !defined(ENGINE_WORLD_TESTS)
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
#endif // !ENGINE_WORLD_TESTS

 #if !defined(ENGINE_WORLD_TESTS)
Asset::PerspectiveSample Asset::runtime_perspective_sample() const {
    PerspectiveSample sample{};
    sample.scale = 1.0f;
    sample.resolution_layer = grid_point_ ? grid_point_->resolution_layer() : grid_resolution;
    sample.source = PerspectiveSource::Default;

    const world::GridPoint* traversal_gp = nullptr;
    if (assets_) {
        traversal_gp = assets_->getView().grid_point_for_asset(this);
    } else if (window) {
        traversal_gp = window->grid_point_for_asset(this);
    }

    if (traversal_gp &&
        std::isfinite(traversal_gp->perspective_scale()) &&
        traversal_gp->perspective_scale() > 0.0001f) {
        sample.scale = std::max(0.0001f, traversal_gp->perspective_scale());
        sample.resolution_layer = traversal_gp->resolution_layer();
        sample.source = PerspectiveSource::CameraTraversal;
        return sample;
    }

    if (grid_point_ &&
        std::isfinite(grid_point_->perspective_scale()) &&
        grid_point_->perspective_scale() > 0.0001f) {
        sample.scale = std::max(0.0001f, grid_point_->perspective_scale());
        sample.resolution_layer = grid_point_->resolution_layer();
        sample.source = PerspectiveSource::AssetGridPoint;
        return sample;
    }

    if (std::isfinite(last_scale_perspective_input_) &&
        last_scale_perspective_input_ > 0.0001f) {
        sample.scale = std::max(0.0001f, last_scale_perspective_input_);
        sample.source = PerspectiveSource::CachedLastFrame;
        return sample;
    }

    return sample;
}
#endif // !ENGINE_WORLD_TESTS

SDL_Texture* Asset::get_current_variant_texture() const {
    if (!current_frame) return nullptr;
    return current_frame->get_base_texture(current_variant_index);
}

SDL_Texture* Asset::get_texture()
{
	if (current_frame) {
		return current_frame->get_base_texture(current_variant_index);
	}
	return nullptr;
}

SDL_Texture* Asset::get_current_frame() const
{
	if (current_frame) {
		return current_frame->get_base_texture(current_variant_index);
	}
	return nullptr;
}

void Asset::set_current_animation(const std::string& name)
{
	if (info == nullptr) {
		return;
	}

	auto it = info->animations.find(name);
        if (it == info->animations.end() && assets_) {
                if (SDL_Renderer* renderer = assets_->renderer()) {
                        info->loadAnimations(renderer, true);
                        it = info->animations.find(name);
                }
        }
	if (it != info->animations.end()) {
		current_animation = name;
		Animation& anim = it->second;
		anim.change(current_frame, static_frame);
		frame_progress = 0.0f;
		refresh_cached_dimensions();
                mark_anchors_dirty();
        }
}

void Asset::update() {
    if (!info) return;

#if !defined(NDEBUG)
    const SDL_Point anchor_debug_start_world{world_x(), world_y()};
    const int anchor_debug_start_world_z = world_z();
    const int anchor_debug_start_layer = grid_point() ? grid_point()->resolution_layer() : grid_resolution;
    const std::uint64_t anchor_debug_start_revision = anchor_world_revision_;
#endif

    // Detect external transform/frame changes before we do any work so bound children can react immediately.
    const bool external_world_changed = update_anchor_basis_if_needed();

    const bool controller_suppressed_for_frame_editor =
        assets_ && assets_->is_frame_editor_target_active(this);

    if (controller_) {
        if (!controller_suppressed_for_frame_editor && assets_) {
            if (Input* in = assets_->get_input()) {
                controller_->update(*in);
            }
        }
        controller_->process_pending_attacks(*this);
    }

    if (anim_) {
        auto iti = info->animations.find(current_animation);
        if (iti == info->animations.end()) {

            auto def = info->animations.find("default");
            if (def == info->animations.end()) def = info->animations.begin();
            if (def != info->animations.end()) {
                if (anim_) {
                    anim_->move(SDL_Point{ 0, 0 }, def->first);
                } else {
                    current_animation = def->first;
                    Animation& anim   = def->second;
                    current_frame     = anim.get_first_frame();
                    frame_progress    = 0.0f;
                    if (info && info->type == asset_types::player) {
                        static_frame = false;
                    } else {
                        static_frame = anim.is_frozen() || anim.locked;
                    }
                }
            }
        } else {
            Animation& anim = iti->second;
            if (anim.index_of(current_frame) < 0) {
                std::size_t path_index = anim_ ? anim_->path_index_for(current_animation) : 0;
                current_frame = anim.get_first_frame(path_index);
                frame_progress = 0.0f;
                if (info && info->type == asset_types::player) {
                    static_frame = false;
                } else {
                    static_frame = anim.is_frozen() || anim.locked;
                }
            }
        }
    }

    const bool can_advance_animation = !assets_ || assets_->should_advance_animation_for(this);

    if (!dead && anim_runtime_ && can_advance_animation) {
        anim_runtime_->update();
    }

    // Re-check anchor basis after any movement/animation/scale changes we just applied.
    const bool post_world_changed = update_anchor_basis_if_needed();

    if (assets_ && (external_world_changed || post_world_changed)) {
        assets_->mark_collision_context_dirty();
    }

    const float alpha_target = hidden ? 0.0f : 1.0f;
    alpha_smoothing_.reset(alpha_target);

#if !defined(NDEBUG)
    const bool anchor_debug_world_changed =
        anchor_debug_start_world.x != world_x() ||
        anchor_debug_start_world.y != world_y() ||
        anchor_debug_start_world_z != world_z() ||
        anchor_debug_start_layer != (grid_point() ? grid_point()->resolution_layer() : grid_resolution);
    if (anchor_debug_world_changed && anchor_world_revision_ == anchor_debug_start_revision) {
        vibble::log::warn("[Asset] anchor_world_revision did not advance after world transform change for asset '" +
                          (info ? info->name : std::string{"<unknown>"}) + "'");
        assert(anchor_world_revision_ != anchor_debug_start_revision);
    }
#endif
}

void Asset::refresh_anchor_point_cache_from_frame() {
    if (!anchors_initialized_) {
        initialize_anchor_registry_from_animations();
    }

    if (anchor_handles_.empty() || anchor_points_.empty()) {
        return;
    }

    const SDL_Point origin_world = world_xy_point();
    const std::size_t count = std::min(anchor_handles_.size(), anchor_points_.size());
    for (std::size_t idx = 0; idx < count; ++idx) {
        AnchorPoint& resolved = anchor_points_[idx];
        const DisplacedAssetAnchorPoint* frame_anchor =
            current_frame ? current_frame->find_anchor(resolved.name) : nullptr;

        resolved.name = anchor_handles_[idx].name;
        resolved.frame_index = current_frame ? current_frame->frame_index : -1;
        resolved.exists = frame_anchor && frame_anchor->is_valid();
        resolved.depth_offset = frame_anchor ? frame_anchor->depth_offset : 0;

        if (!resolved.exists) {
            resolved.relative_pos_2d = Vec2{};
            resolved.world_pos_2d = Vec2{};
            resolved.world_exact_pos_2d = Vec2{};
            resolved.world_quantized_px = SDL_Point{0, 0};
            resolved.world_z = world_z();
            resolved.world_exact_z = static_cast<float>(world_z());
            resolved.world_depth = static_cast<float>(world_z());
            resolved.resolution_layer = grid_point() ? grid_point()->resolution_layer() : grid_resolution;
            continue;
        }

        AnchorHandle& handle = anchor_handles_[idx];
        handle.owner = this;
        handle.update(anchor_points::GridMaterialization::None);

        apply_anchor_runtime_state(resolved, handle, frame_anchor);
        if (resolved.exists) {
            resolved.relative_pos_2d = Vec2{
                resolved.world_pos_2d.x - static_cast<float>(origin_world.x),
                resolved.world_pos_2d.y - static_cast<float>(origin_world.y)};
        }
    }
}

void Asset::refresh_runtime_box_cache_from_frame() {
    current_hit_box_volumes_.clear();
    current_attack_box_volumes_.clear();
    runtime_hit_box_lookup_.clear();
    runtime_attack_box_lookup_.clear();

    if (!assets_ || !current_frame) {
        return;
    }

    auto build_volume = [&](const std::string& name,
                            int extrusion_amount,
                            int damage_amount,
                            const std::array<animation_update::FrameBoxCorner, 4>& corners,
                            RuntimeBoxVolume& out_volume) -> bool {
        out_volume = RuntimeBoxVolume{};
        out_volume.name = name;
        out_volume.frame_index = current_frame ? current_frame->frame_index : -1;
        out_volume.extrusion_amount = std::max(0, extrusion_amount);
        out_volume.damage_amount = damage_amount;

        float sum_x = 0.0f;
        float sum_y = 0.0f;
        float sum_z = 0.0f;
        const float extrusion = static_cast<float>(out_volume.extrusion_amount);
        for (std::size_t corner_index = 0; corner_index < corners.size(); ++corner_index) {
            const auto& corner = corners[corner_index];
            DisplacedAssetAnchorPoint sample_anchor{};
            sample_anchor.name = "__box_corner";
            sample_anchor.texture_x = std::max(0, corner.texture_x);
            sample_anchor.texture_y = std::max(0, corner.texture_y);
            sample_anchor.depth_offset = 0;

            const anchor_points::FrameAnchorSample sample =
                anchor_points::resolve_frame_anchor_sample(*this, sample_anchor, anchor_points::GridMaterialization::None);
            if (sample.resolved.missing || !sample.flat_relative_pixel_point.valid) {
                return false;
            }

            anchor_points::AnchorWorldPoint3 near_point{};
            anchor_points::AnchorWorldPoint3 far_point{};
            if (!anchor_points::build_symmetric_camera_ray_extrusion(*this,
                                                                     sample.flat_relative_pixel_point,
                                                                     extrusion,
                                                                     near_point,
                                                                     far_point) ||
                !near_point.valid ||
                !far_point.valid) {
                return false;
            }

            const RuntimeBoxPoint3 near_world{
                near_point.x,
                near_point.y,
                near_point.z};
            const RuntimeBoxPoint3 far_world{
                far_point.x,
                far_point.y,
                far_point.z};
            out_volume.world_points[corner_index] = near_world;
            out_volume.world_points[corner_index + corners.size()] = far_world;
            sum_x += near_world.x + far_world.x;
            sum_y += near_world.y + far_world.y;
            sum_z += near_world.z + far_world.z;
        }

        out_volume.centroid = RuntimeBoxPoint3{sum_x / 8.0f, sum_y / 8.0f, sum_z / 8.0f};
        out_volume.valid = true;
        return true;
    };

    current_hit_box_volumes_.reserve(current_frame->hit_boxes.boxes.size());
    for (const auto& box : current_frame->hit_boxes.boxes) {
        if (!box.is_valid()) {
            continue;
        }
        RuntimeBoxVolume volume{};
        const auto runtime_corners = box.to_runtime_clockwise_points();
        if (!build_volume(box.name, box.extrusion_amount, 0, runtime_corners, volume)) {
            continue;
        }
        runtime_hit_box_lookup_.emplace(volume.name, current_hit_box_volumes_.size());
        current_hit_box_volumes_.push_back(std::move(volume));
    }

    current_attack_box_volumes_.reserve(current_frame->attack_boxes.boxes.size());
    for (const auto& box : current_frame->attack_boxes.boxes) {
        if (!box.is_valid()) {
            continue;
        }
        RuntimeBoxVolume volume{};
        const auto runtime_corners = box.to_runtime_clockwise_points();
        if (!build_volume(box.name, box.extrusion_amount, box.damage_amount, runtime_corners, volume)) {
            continue;
        }
        runtime_attack_box_lookup_.emplace(volume.name, current_attack_box_volumes_.size());
        current_attack_box_volumes_.push_back(std::move(volume));
    }
}

const Asset::RuntimeBoxVolume* Asset::find_hit_box_volume(const std::string& name) const {
    auto it = runtime_hit_box_lookup_.find(name);
    if (it == runtime_hit_box_lookup_.end() || it->second >= current_hit_box_volumes_.size()) {
        return nullptr;
    }
    return &current_hit_box_volumes_[it->second];
}

const Asset::RuntimeBoxVolume* Asset::find_attack_box_volume(const std::string& name) const {
    auto it = runtime_attack_box_lookup_.find(name);
    if (it == runtime_attack_box_lookup_.end() || it->second >= current_attack_box_volumes_.size()) {
        return nullptr;
    }
    return &current_attack_box_volumes_[it->second];
}

std::string Asset::get_current_animation() const { return current_animation; }
bool Asset::is_current_animation_locked_in_progress() const {
        if (!info || !current_frame) return false;
        if (info->type == asset_types::player) return false;
        auto it = info->animations.find(current_animation);
        if (it == info->animations.end()) return false;
        const Animation& anim = it->second;
        if (!anim.locked) return false;
        return !current_frame->is_last;
}

bool Asset::is_current_animation_last_frame() const {
        if (!current_frame) return false;
        return current_frame->is_last;
}

bool Asset::is_current_animation_looping() const {
	if (!info) return false;
	auto it = info->animations.find(current_animation);
	if (it == info->animations.end()) return false;
	const Animation& anim = it->second;
	return anim.loop;
}

float Asset::frame_delta_seconds_clamped() const {
    if (!assets_) {
        constexpr float kFallbackDt = 1.0f / 60.0f;
        return kFallbackDt;
    }
    return assets_->frame_delta_seconds_clamped();
}

 #if !defined(ENGINE_WORLD_TESTS)
void Asset::set_assets(Assets* a) {
    assets_ = a;
    if (assets_) {
        assets_->track_asset_for_grid(this);
    }
    ensure_animation_runtime(false);
    if (!controller_ && assets_) {
            ControllerFactory cf(assets_);
            controller_ = cf.create_for_asset(this);
    }

}
 
#endif // !ENGINE_WORLD_TESTS

void Asset::set_tiling_info(std::optional<TilingInfo> info) {
    tiling_info_ = std::move(info);
}

void Asset::set_owning_room_name(std::string name) {
    owning_room_name_ = std::move(name);
}

void Asset::rebuild_animation_runtime() {
    ensure_animation_runtime(true);
}



 #if !defined(ENGINE_WORLD_TESTS)
void Asset::Delete() {
    if (dead) {
        return;
    }
    dead = true;
    active = false;
    if (assets_) {
        assets_->schedule_removal(this);
    }
}

#endif // !ENGINE_WORLD_TESTS

void Asset::ensure_animation_runtime(bool force_recreate) {
    if (!assets_) {
        return;
    }
    if (!force_recreate && anim_ && anim_runtime_) {
        return;
    }
    anim_runtime_.reset();
    anim_.reset();
    anim_runtime_ = std::make_unique<AnimationRuntime>(this, assets_);
    anim_ = std::make_unique<AnimationUpdate>(this, assets_);
    if (anim_runtime_) anim_runtime_->set_planner(anim_.get());
    if (anim_) anim_->set_runtime(anim_runtime_.get());
}

void Asset::set_flip() {
        if (!info || !info->flipable) return;

        bool use_override = false;
        bool override_value = false;
        if (!spawn_id.empty()) {
                std::lock_guard<std::mutex> lock(s_flip_overrides_mutex_);
                auto it = s_flip_overrides_.find(spawn_id);
                if (it != s_flip_overrides_.end() && it->second.first) {
                        use_override = true;
                        override_value = it->second.second;
                }
        }
        if (use_override) {
                flipped = override_value;
                return;
        }
        std::uniform_int_distribution<int> dist(0, 1);
        bool should_flip;
        {
                std::lock_guard<std::mutex> lock(asset_rng_mutex());
                should_flip = (dist(asset_rng()) == 1);
        }
        flipped = should_flip;
}

void Asset::SetFlipOverrideForSpawnId(const std::string& id, bool enabled, bool flipped) {
        if (id.empty()) return;
        std::lock_guard<std::mutex> lock(s_flip_overrides_mutex_);
        s_flip_overrides_[id] = std::make_pair(enabled, flipped);
}

void Asset::ClearFlipOverrideForSpawnId(const std::string& id) {
        if (id.empty()) return;
        std::lock_guard<std::mutex> lock(s_flip_overrides_mutex_);
        s_flip_overrides_.erase(id);
}

Area Asset::get_area(const std::string& name) const {
        if (!info) {
                return Area(name, 0);
        }

        Area* base = info->find_area(name);
        if (!base) {
                base = info->find_area(name + "_area");
        }
        if (!base) {
                return Area(name, 0);
        }

        SDL_Point world_pos = SDL_Point{world_x(), world_z()};
        return area_helpers::make_world_area(*info, *base, world_pos, flipped);
}

void Asset::deactivate() {
        active = false;
        hidden = true;
        clear_render_caches();
        visibility_stamp = 0;
        if (assets_) {
                assets_->mark_active_assets_dirty();
        }
}

void Asset::clear_render_caches() {
    mesh_dirty_ = true;
    for (auto& obj : render_package) {
        obj.mesh_dirty = true;
    }
}

void Asset::invalidate_downscale_cache() {
        last_scaled_texture_      = nullptr;
        last_scaled_source_       = nullptr;
        last_scaled_w_            = 0;
        last_scaled_h_            = 0;
        last_scaled_camera_scale_ = -1.0f;
        last_scale_usage_         = {};

        downscale_cache_ready_revision_ = 0;
}

void Asset::refresh_cached_dimensions() {
        int width = 0;
        int height = 0;

        if ((width <= 0 || height <= 0)) {
                SDL_Texture* frame = get_current_variant_texture();
                if (frame) {
                        float wf = 0.0f;
                        float hf = 0.0f;
                        if (SDL_GetTextureSize(frame, &wf, &hf)) {
                                width = static_cast<int>(std::lround(wf));
                                height = static_cast<int>(std::lround(hf));
                        }
                }
        }

        if ((width <= 0 || height <= 0) && info) {
                width  = info->original_canvas_width;
                height = info->original_canvas_height;
        }

        cached_w = (width > 0) ? width : 0;
        cached_h = (height > 0) ? height : 0;
}

void Asset::refresh_frame_texture_bindings() {
        if (!current_frame) {
                last_rendered_frame_ = nullptr;
                return;
        }
        current_frame->rebuild_anchor_lookup();
        last_rendered_frame_ = current_frame;
        refresh_cached_dimensions();
        mark_composite_dirty();
        mark_mesh_dirty();
        mark_anchors_dirty();
}

float Asset::runtime_scale_remainder() const {
        float remainder = last_scale_usage_.remainder_scale;
        if (!std::isfinite(remainder) || remainder <= 0.0f) {
                remainder = current_remaining_scale_adjustment;
        }
        if (!std::isfinite(remainder) || remainder <= 0.0f) {
                remainder = 1.0f;
        }
        return remainder;
}

float Asset::runtime_resolved_scale() const {
        float texture_scale = last_scale_usage_.texture_scale;
        if (!std::isfinite(texture_scale) || texture_scale <= 0.0f) {
                texture_scale = current_nearest_variant_scale;
        }
        if (!std::isfinite(texture_scale) || texture_scale <= 0.0f) {
                texture_scale = 1.0f;
        }
        return texture_scale * runtime_scale_remainder();
}

float Asset::runtime_width_px() const {
        const float base_width = static_cast<float>(width());
        if (!(base_width > 0.0f)) {
                return 0.0f;
        }
        return base_width * runtime_scale_remainder();
}

 #if !defined(ENGINE_WORLD_TESTS)
float Asset::runtime_height_px() const {
        const float base_height = static_cast<float>(height());
        if (!(base_height > 0.0f)) {
                return 0.0f;
        }
        return base_height * runtime_scale_remainder();
}
#endif // !ENGINE_WORLD_TESTS

void Asset::on_scale_factor_changed() {

        last_scale_usage_ = {};

        refresh_cached_dimensions();

        mark_composite_dirty();
        mark_mesh_dirty();
        mark_anchors_dirty();

        if (assets_) {
                assets_->queue_asset_dimension_update(this);
        }
}

void Asset::set_hidden(bool state){ hidden = state; }
bool  Asset::is_hidden() const { return hidden; }

void Asset::set_anchor_hidden(bool state){ anchor_hidden_ = state; }
bool  Asset::is_anchor_hidden() const { return anchor_hidden_; }



void Asset::set_highlighted(bool state){ highlighted = state; }
bool  Asset::is_highlighted(){ return highlighted; }

void Asset::set_selected(bool state){ selected = state; }
bool  Asset::is_selected(){ return selected; }

 #if !defined(ENGINE_WORLD_TESTS)
void Asset::cache_grid_residency(const world::GridPoint& point) {
        cached_grid_residency_    = point.key();
        has_cached_grid_residency_ = true;
}
#endif // !ENGINE_WORLD_TESTS

void Asset::clear_grid_residency_cache() {
        cached_grid_residency_    = world::GridKey{ std::numeric_limits<int>::min(), std::numeric_limits<int>::min(), 0, 0 };
        has_cached_grid_residency_ = false;
}

 #if !defined(ENGINE_WORLD_TESTS)
void Asset::sync_transform_to_position() {
}
#endif // !ENGINE_WORLD_TESTS

void Asset::send_attack(const animation_update::Attack& attack) {
        std::lock_guard<std::mutex> lock(pending_attacks_mutex_);
        pending_attacks_.push_back(attack);
}

std::vector<animation_update::Attack> Asset::process_pending_attacks() {
        std::lock_guard<std::mutex> lock(pending_attacks_mutex_);
        std::vector<animation_update::Attack> attacks;
        attacks.swap(pending_attacks_);
        return attacks;
}

 #if !defined(ENGINE_WORLD_TESTS)
void Asset::mark_anchors_dirty() {
        for (auto& handle : anchor_handles_) {
                handle.dirty = true;
        }
        current_hit_box_volumes_.clear();
        current_attack_box_volumes_.clear();
        runtime_hit_box_lookup_.clear();
        runtime_attack_box_lookup_.clear();
        ++anchor_world_revision_;
        anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().notify_anchor_changed(
            this,
            std::string{});
}

#endif // !ENGINE_WORLD_TESTS

void Asset::assert_unique_anchor_names_for_frame() const {
#if !defined(NDEBUG)
        if (!current_frame) {
                return;
        }
        std::unordered_set<std::string> seen;
        for (const auto& anchor : current_frame->anchor_points) {
                if (!anchor.is_valid()) {
                        continue;
                }
                if (!seen.insert(anchor.name).second) {
                        const std::string asset_name = info ? info->name : std::string{"<unknown>"};
                        vibble::log::warn("[AnchorDebug] Duplicate anchor name '" +
                                          anchor.name +
                                          "' on asset '" +
                                          asset_name +
                                          "' (frame " +
                                          std::to_string(current_frame->frame_index) +
                                          ")");
                        SDL_assert(!"Anchor names must be unique per asset frame");
                        break;
                }
        }
#endif
}

Asset::AnchorBasisSignature Asset::compute_anchor_basis_signature() const {
        AnchorBasisSignature sig{};
        sig.world_x = world_x();
        sig.world_y = world_y();
        sig.world_z = world_z();
        sig.frame_index = current_frame ? current_frame->frame_index : std::numeric_limits<int>::min();
        sig.variant_index = current_variant_index;
        sig.flipped = flipped;

        float remainder = current_remaining_scale_adjustment;
        if (!std::isfinite(remainder) || remainder <= 0.0f) {
                remainder = 1.0f;
        }
        sig.remainder_scale = remainder;

        const PerspectiveSample perspective_sample = runtime_perspective_sample();
        sig.perspective_scale = perspective_sample.scale;

        sig.world_z_offset = world_z_offset_;
        sig.resolution_layer = perspective_sample.resolution_layer;
        sig.camera_state_version = (assets_ ? assets_->getView().camera_state_version() : 0);
        return sig;
}

void Asset::capture_anchor_basis_snapshot(const AnchorBasisSignature& signature) {
        last_anchor_basis_signature_ = signature;
        anchor_basis_initialized_ = true;
}

bool Asset::update_anchor_basis_if_needed() {
        const AnchorBasisSignature signature = compute_anchor_basis_signature();

        const auto almost_equal = [](float a, float b) {
                constexpr float kEpsilon = 1e-4f;
                return std::fabs(a - b) < kEpsilon;
        };

        const bool world_changed = !anchor_basis_initialized_ ||
                                   signature.world_x != last_anchor_basis_signature_.world_x ||
                                   signature.world_y != last_anchor_basis_signature_.world_y ||
                                   signature.world_z != last_anchor_basis_signature_.world_z;

        const bool signature_changed =
                world_changed ||
                signature.frame_index != last_anchor_basis_signature_.frame_index ||
                signature.variant_index != last_anchor_basis_signature_.variant_index ||
                signature.flipped != last_anchor_basis_signature_.flipped ||
                signature.resolution_layer != last_anchor_basis_signature_.resolution_layer ||
                signature.camera_state_version != last_anchor_basis_signature_.camera_state_version ||
                !almost_equal(signature.remainder_scale, last_anchor_basis_signature_.remainder_scale) ||
                !almost_equal(signature.perspective_scale, last_anchor_basis_signature_.perspective_scale) ||
                !almost_equal(signature.world_z_offset, last_anchor_basis_signature_.world_z_offset);

        if (!signature_changed) {
                return false;
        }

        capture_anchor_basis_snapshot(signature);
        mark_anchors_dirty();
        return world_changed;
}

AnchorPoint* Asset::get_anchor_point(const std::string& name) {
        auto resolved = anchor_state(name);
        if (!resolved.has_value()) {
                return nullptr;
        }
        auto it = anchor_name_to_index_.find(name);
        if (it == anchor_name_to_index_.end() || it->second >= anchor_points_.size()) {
                return nullptr;
        }
        return &anchor_points_[it->second];
}

std::optional<std::string> Asset::anchor_name_for_index(std::size_t index) const {
        if (index >= anchor_points_.size()) {
                return std::nullopt;
        }
        return anchor_points_[index].name;
}

void Asset::apply_anchor_runtime_state(AnchorPoint& resolved,
                                     const AnchorHandle& handle,
                                     const DisplacedAssetAnchorPoint* frame_anchor) const {
        const bool anchor_present = frame_anchor && frame_anchor->is_valid();
        resolved.exists = anchor_present && !handle.missing && handle.has_canonical_texture_source;
        resolved.depth_offset = anchor_present ? frame_anchor->depth_offset : handle.depth_offset;

        if (resolved.exists) {
                resolved.world_pos_2d = handle.world_exact_pos_2d;
                resolved.world_exact_pos_2d = handle.world_exact_pos_2d;
                resolved.world_quantized_px = handle.world_px;
                const SDL_Point origin_world = world_xy_point();
                resolved.relative_pos_2d = Vec2{
                        resolved.world_pos_2d.x - static_cast<float>(origin_world.x),
                        resolved.world_pos_2d.y - static_cast<float>(origin_world.y)};
                resolved.screen_pos_2d = handle.screen_px;
        } else {
                resolved.world_pos_2d = Vec2{};
                resolved.world_exact_pos_2d = Vec2{};
                resolved.world_quantized_px = SDL_Point{0, 0};
                resolved.relative_pos_2d = Vec2{};
                resolved.screen_pos_2d = SDL_FPoint{0.0f, 0.0f};
        }
        resolved.world_z = handle.world_z;
        resolved.world_exact_z = handle.world_exact_z;
        resolved.world_depth = handle.world_depth;
        resolved.resolution_layer = handle.resolution_layer;
}

AnchorPoint& Asset::resolve_anchor_point_entry(std::size_t index,
                                               anchor_points::GridMaterialization grid_policy,
                                               const DisplacedAssetAnchorPoint* frame_anchor,
                                               AnchorResolveMode resolve_mode) {
        assert(index < anchor_handles_.size());
        assert(index < anchor_points_.size());

#if !defined(NDEBUG)
        assert_unique_anchor_names_for_frame();
#endif

        AnchorHandle& handle = anchor_handles_[index];
        handle.owner = this;
        handle.update(grid_policy, resolve_mode == AnchorResolveMode::ForceRecompute);

        AnchorPoint& resolved = anchor_points_[index];
        resolved.name = handle.name;
        resolved.frame_index = current_frame ? current_frame->frame_index : -1;
        apply_anchor_runtime_state(resolved, handle, frame_anchor);

        return resolved;
}

std::optional<AnchorPoint> Asset::anchor_state(const std::string& name,
                                               anchor_points::GridMaterialization grid_policy,
                                               AnchorResolveMode resolve_mode) {
        if (!anchors_initialized_) {
                initialize_anchor_registry_from_animations();
        }
        auto it = anchor_name_to_index_.find(name);
        if (it == anchor_name_to_index_.end() ||
            it->second >= anchor_handles_.size() ||
            it->second >= anchor_points_.size()) {
                return std::nullopt;
        }

        const DisplacedAssetAnchorPoint* frame_anchor =
                current_frame ? current_frame->find_anchor(name) : nullptr;

        AnchorPoint& resolved = resolve_anchor_point_entry(it->second,
                                                           grid_policy,
                                                           frame_anchor,
                                                           resolve_mode);
        return resolved;
}

#endif // !ENGINE_WORLD_TESTS

void Asset::AnchorHandle::update(anchor_points::GridMaterialization grid_policy, bool force_recompute) {
#if !defined(NDEBUG)
        auto& counters = anchor_update_counters();
        ++counters.calls;
#endif

        const std::uint64_t camera_state_version =
                (owner && owner->assets_) ? owner->assets_->getView().camera_state_version() : 0;
        const bool cache_hit = !force_recompute && !dirty && last_update_key_.matches(grid_policy, camera_state_version);
        if (cache_hit) {
#if !defined(NDEBUG)
                ++counters.cache_hits;
#endif
                return;
        }

#if !defined(NDEBUG)
        ++counters.recomputes;
#endif
        last_update_key_.set(grid_policy, camera_state_version);

        if (!owner) {
                dirty = false;
                return;
        }
        const AnimationFrame* frame = owner->current_frame;
        auto mark_missing = [&](const std::string& reason, bool keep_dirty = true) {
                (void)reason;
                grid = nullptr;
                world_exact_pos_2d = Vec2{};
                world_exact_z = 0.0f;
                world_px = SDL_Point{0, 0};
                world_z = 0;
                world_depth = 0.0f;
                resolution_layer = 0;
                missing = true;
                depth_offset = 0;
                source_texture_px = SDL_Point{0, 0};
                screen_px = SDL_FPoint{0.0f, 0.0f};
                has_canonical_texture_source = false;
                dirty = keep_dirty;
        };

        if (!frame) {
                mark_missing("no current frame");
                return;
        }

        const DisplacedAssetAnchorPoint* anchor = frame->find_anchor(name);

        if (!anchor) {
                mark_missing("anchor not present on current frame");
                return;
        }

        if (!anchor->is_valid()) {
                mark_missing("anchor data invalid");
                return;
        }

        anchor_points::FrameAnchorSample resolved_sample{};
        try {
                resolved_sample = anchor_points::resolve_frame_anchor_sample(
                        *owner,
                        *anchor,
                        grid_policy);
        } catch (const std::exception& ex) {
                mark_missing(std::string("invalid anchor data: ") + ex.what());
                return;
        }

        if (resolved_sample.resolved.missing || !resolved_sample.resolved.has_canonical_texture_source) {
                mark_missing(resolved_sample.resolved.missing
                        ? std::string("resolver marked anchor missing")
                        : std::string("missing canonical texture source"));
                return;
        }

        grid = resolved_sample.resolved.grid_point;
        world_exact_pos_2d = resolved_sample.resolved.world_exact_pos_2d;
        world_exact_z = resolved_sample.resolved.world_exact_z;
        world_px = resolved_sample.resolved.world_px;
        world_z = resolved_sample.resolved.world_z;
        world_depth = resolved_sample.resolved.world_depth;
        resolution_layer = resolved_sample.resolved.resolution_layer;
        missing = resolved_sample.resolved.missing;
        depth_offset = resolved_sample.resolved.depth_offset;
        source_texture_px = resolved_sample.resolved.source_texture_px;
        screen_px = resolved_sample.screen_px;
        has_canonical_texture_source = resolved_sample.resolved.has_canonical_texture_source;
        dirty = false;
}



bool Asset::has_grid_residency_cache() const {
        return has_cached_grid_residency_;
}

world::GridKey Asset::grid_residency_cache() const {
        return cached_grid_residency_;
}

void Asset::set_grid_id(std::uint64_t id) {
        grid_id_ = id;
}

#if !defined(ENGINE_WORLD_TESTS)
void Asset::clear_grid_id() {
        grid_id_ = 0;
}
#endif // !ENGINE_WORLD_TESTS

void Asset::set_render_anchor_offset(float x, float y, float z) {
        if (!std::isfinite(x)) {
                x = 0.0f;
        }
        if (!std::isfinite(y)) {
                y = 0.0f;
        }
        if (!std::isfinite(z)) {
                z = 0.0f;
        }

        constexpr float kEpsilon = 1e-5f;
        if (std::fabs(render_anchor_offset_x_ - x) < kEpsilon &&
            std::fabs(render_anchor_offset_y_ - y) < kEpsilon &&
            std::fabs(render_anchor_offset_z_ - z) < kEpsilon) {
                return;
        }

        render_anchor_offset_x_ = x;
        render_anchor_offset_y_ = y;
        render_anchor_offset_z_ = z;
        mark_composite_dirty();
        mark_mesh_dirty();
}

void Asset::clear_render_anchor_offset() {
        set_render_anchor_offset(0.0f, 0.0f, 0.0f);
}

void Asset::set_composite_texture(SDL_Texture* tex) {
    if (composite_texture_ && composite_texture_ != tex) {
        SDL_DestroyTexture(composite_texture_);
    }
    composite_texture_ = tex;
}

 #if !defined(ENGINE_WORLD_TESTS)
float Asset::smoothed_translation_x() const {
    return static_cast<float>(world_x());
}

float Asset::smoothed_translation_y() const {
    return static_cast<float>(world_y());
}

float Asset::smoothed_scale() const {
    return current_scale;
}

#endif // !ENGINE_WORLD_TESTS

float Asset::smoothed_alpha() const {
        float value = alpha_smoothing_.value_for_render();
        if (!std::isfinite(value)) {
                value = hidden ? 0.0f : 1.0f;
        }
        return std::clamp(value, 0.0f, 1.0f);
}

 #if !defined(ENGINE_WORLD_TESTS)
void Asset::move_to_world_position(int world_x,
                                   int world_y,
                                   int world_z,
                                   std::optional<int> resolution_layer_override) {
    if (!assets_) return;

    const int previous_world_x = this->world_x();
    const int previous_world_y = this->world_y();
    const int previous_world_z = this->world_z();
    const int previous_layer = grid_point() ? grid_point()->resolution_layer() : grid_resolution;

    world::WorldGrid& grid = assets_->world_grid();
    world::GridPoint* current_point = grid.point_for_asset(this);
    const int resolved_layer = resolution_layer_override.has_value()
        ? vibble::grid::clamp_resolution(*resolution_layer_override)
        : (current_point ? current_point->resolution_layer() : grid_resolution);
    world::GridPoint& target = world::GridPoint::from_world(world_x, world_y, world_z, resolved_layer, grid);

    if (current_point) {
        grid.move_asset(this, *current_point, target);
    } else {
        set_provisional_grid_point(world_x, world_y, world_z, resolved_layer);
    }

    grid_resolution = resolved_layer;
    const bool point_changed =
        previous_world_x != this->world_x() ||
        previous_world_y != this->world_y() ||
        previous_world_z != this->world_z() ||
        previous_layer != grid_resolution;

    if (point_changed) {
        mark_composite_dirty();
        mark_mesh_dirty();
    }

    mark_anchors_dirty();
    assets_->mark_collision_context_dirty();
}

#endif // !ENGINE_WORLD_TESTS

void Asset::set_world_z(int world_z) {
    if (!assets_) return;
    move_to_world_position(world_x(), world_y(), world_z);
}

 #if !defined(ENGINE_WORLD_TESTS)
void Asset::set_render_depth_bias(double bias) {
    if (!std::isfinite(bias)) {
        bias = 0.0;
    }
    // Allow anchored chains to stack several one-pixel offsets without clipping.
    constexpr double kMaxBiasMagnitude = 8.0;
    render_depth_bias_ = std::clamp(bias, -kMaxBiasMagnitude, kMaxBiasMagnitude);
}

#endif // !ENGINE_WORLD_TESTS
