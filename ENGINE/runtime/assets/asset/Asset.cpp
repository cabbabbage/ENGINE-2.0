//TODO We need to update this to completly remove "pos" and instead use grid point fully
//TODO find usage of pos in the code base replace with grid point usage
//TODO we need a good function for updating an assets grid point

//make sure that any asset can exist in a 3d space

//remove legacy old or unessesary data from this class

#include "Asset.hpp"
#include "controller_factory.hpp"
#include "animation.hpp"
#include "core/AssetsManager.hpp"
#include "core/asset_list.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/render.hpp"
#include "animation/animation_runtime.hpp"
#include "animation/animation_update.hpp"
#include "utils/area_helpers.hpp"
#include "assets/asset_filter_tags.hpp"
#include "assets/asset_types.hpp"
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
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <cassert>
#include <SDL3/SDL.h>
#include "utils/FramePointResolver.hpp"
#include "utils/AnchorPointResolver.hpp"

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


const DisplacedAssetAnchorPoint* find_anchor_with_frame_fallback(const Asset& asset,
                                                                  const AnimationFrame* current_frame,
                                                                  const std::string& name) {
        if (current_frame) {
                if (const auto* direct = current_frame->find_anchor(name)) {
                        return direct;
                }
        }

        if (!asset.info) {
                return nullptr;
        }
        auto anim_it = asset.info->animations.find(asset.current_animation);
        if (anim_it == asset.info->animations.end()) {
                return nullptr;
        }

        const Animation& anim = anim_it->second;
        const int current_index = current_frame ? current_frame->frame_index : 0;

        const DisplacedAssetAnchorPoint* best = nullptr;
        int best_distance = std::numeric_limits<int>::max();
        for (const AnimationFrame* frame : anim.frames) {
                if (!frame) {
                        continue;
                }
                const auto* candidate = frame->find_anchor(name);
                if (!candidate) {
                        continue;
                }
                const int distance = std::abs(frame->frame_index - current_index);
                if (!best || distance < best_distance) {
                        best = candidate;
                        best_distance = distance;
                        if (distance == 0) {
                                break;
                        }
                }
        }

        return best;
}

std::unordered_map<std::string, std::pair<bool,bool>> Asset::s_flip_overrides_{};
std::mutex Asset::s_flip_overrides_mutex_{};

Asset::Asset(std::shared_ptr<AssetInfo> info_,
             const Area& spawn_area,
             SDL_Point start_pos,
             int depth_,
             Asset* parent_,
             const std::string& spawn_id_,
             const std::string& spawn_method_,
             int grid_resolution_,
             std::optional<AnchorFollowTarget> anchor_follow)
: parent(parent_)
, info(std::move(info_))
, current_animation()
, static_frame(false)
, active(false)
, pos_(nullptr)
, initial_world_pos_(start_pos)
, grid_resolution(vibble::grid::clamp_resolution(grid_resolution_))
, depth(depth_)
, spawn_id(spawn_id_)
, spawn_method(spawn_method_)
, owning_room_name_(spawn_area.get_name())
, follow_anchor_(std::move(anchor_follow))
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
        if (it != info->animations.end() && !it->second.frames.empty()) {
                current_animation = it->first;
                Animation& anim  = it->second;
                static_frame     = (anim.frames.size() == 1);
                current_frame    = anim.get_first_frame();
                if ((anim.randomize || anim.rnd_start) && anim.frames.size() > 1) {
                        std::uniform_int_distribution<int> d(0, int(anim.frames.size()) - 1);
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
}

void Asset::refresh_filter_tags() {
    if (info) {
        filter_type_tag_ = asset_types::canonicalize(info->type);
    } else {
        filter_type_tag_.clear();
    }
    filter_method_tag_ = asset_filters::canonicalize_spawn_method(spawn_method);
}

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

Asset::~Asset() {
        clear_render_caches();
        if (composite_texture_) {
                SDL_DestroyTexture(composite_texture_);
                composite_texture_ = nullptr;
        }
        visibility_stamp = 0;
}

Asset::Asset(const Asset& o)
    : parent(o.parent)
    , info(o.info)
    , current_animation(o.current_animation)
    , pos_(nullptr)
    , initial_world_pos_(o.initial_world_pos_)
    , grid_resolution(vibble::grid::clamp_resolution(o.grid_resolution))
    , active(o.active)
    , flipped(o.flipped)
    , distance_from_camera(o.distance_from_camera)
    , angle_from_camera(o.angle_from_camera)
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
    , grid_id_(o.grid_id_)
    , composite_texture_(nullptr)
    , composite_dirty_(true)
    , composite_rect_({0, 0, 0, 0})
    , composite_scale_(o.composite_scale_)
    , follow_anchor_(o.follow_anchor_)
    , last_follow_world_(o.last_follow_world_)
    , last_follow_world_z_(o.last_follow_world_z_)
    , follow_initialized_(o.follow_initialized_)
{

        clear_render_caches();
        last_scale_usage_ = o.last_scale_usage_;
        scale_variant_state_ = o.scale_variant_state_;
        cached_grid_residency_    = o.cached_grid_residency_;
        has_cached_grid_residency_ = o.has_cached_grid_residency_;
        alpha_smoothing_          = o.alpha_smoothing_;
        finalized_                = o.finalized_;
        refresh_filter_tags();
}

Asset& Asset::operator=(const Asset& o) {
        if (this == &o) return *this;

        clear_render_caches();
        parent               = o.parent;
        info                 = o.info;
        current_animation    = o.current_animation;
    pos_                 = nullptr;
    grid_resolution      = vibble::grid::clamp_resolution(o.grid_resolution);
	active               = o.active;
        flipped              = o.flipped;
        distance_from_camera = o.distance_from_camera;
        angle_from_camera = o.angle_from_camera;
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
        merged_from_neighbors_ = o.merged_from_neighbors_;
        current_frame        = o.current_frame;
        frame_progress       = o.frame_progress;
        last_rendered_frame_   = nullptr;
        assets_              = o.assets_;
        spawn_id             = o.spawn_id;
        spawn_method         = o.spawn_method;
        owning_room_name_    = o.owning_room_name_;
        controller_.reset();
        anim_.reset();
        neighbors.reset();
        impassable_naighbors.reset();
        neighbor_lists_initialized_ = false;
        last_neighbor_origin_ = SDL_Point{ std::numeric_limits<int>::min(), std::numeric_limits<int>::min() };
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
        cached_grid_residency_    = o.cached_grid_residency_;
        has_cached_grid_residency_ = o.has_cached_grid_residency_;
        alpha_smoothing_          = o.alpha_smoothing_;
        finalized_                = o.finalized_;
        grid_id_                  = o.grid_id_;
        composite_texture_        = nullptr;
        composite_dirty_          = true;
        composite_rect_           = {0, 0, 0, 0};
        composite_scale_          = o.composite_scale_;
        follow_anchor_            = o.follow_anchor_;
        last_follow_world_        = o.last_follow_world_;
        last_follow_world_z_      = o.last_follow_world_z_;
        follow_initialized_       = o.follow_initialized_;
        anchor_handles_.clear();
        anchor_lookup_.clear();
        refresh_filter_tags();
        return *this;
}

void Asset::finalize_setup() {
        if (finalized_) {
                return;
        }
        if (!info) return;
        if (current_animation.empty() ||
        info->animations[current_animation].frames.empty())
        {
		std::string start_id = info->start_animation.empty() ? std::string{"default"} : info->start_animation;
		auto it = info->animations.find(start_id);
		if (it == info->animations.end()) it = info->animations.find("default");
		if (it == info->animations.end()) it = info->animations.begin();
		if (it != info->animations.end() && !it->second.frames.empty()) {
			current_animation = it->first;
			Animation& anim = it->second;
                        anim.change(current_frame, static_frame);
                        frame_progress = 0.0f;
                        if ((anim.randomize || anim.rnd_start) && anim.frames.size() > 1) {
                                std::uniform_int_distribution<int> dist(0, int(anim.frames.size()) - 1);
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
            !info->animations[current_animation].frames.empty()) {
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



void Asset::update_scale_values() {
    const float base_scale =
        (info && std::isfinite(info->scale_factor) && info->scale_factor > 0.0f) ? info->scale_factor : 1.0f;

    float perspective_scale = 1.0f;
    const char* perspective_source = "default";
    // Try multiple sources for perspective scale to handle movement transitions
    if (pos_ && pos_->perspective_scale > 0.0001f) {
        // Primary: use current GridPoint directly
        perspective_scale = pos_->perspective_scale;
        perspective_source = "grid-point";
    } else if (window) {
        if (auto* gp = window->grid_point_for_asset(this)) {
            perspective_scale = std::max(0.0001f, gp->perspective_scale);
            perspective_source = "window-grid";
        } else if (last_scale_perspective_input_ > 0.0001f) {
            // Use cached value from last frame during movement transition
            perspective_scale = last_scale_perspective_input_;
            perspective_source = "cached-last-frame";
        }
    } else if (last_scale_perspective_input_ > 0.0001f) {
        // Absolute fallback: use last known value
        perspective_scale = last_scale_perspective_input_;
        perspective_source = "cached-last-frame";
    }

    float camera_scale = 1.0f;
    if (assets_) {
        camera_scale = static_cast<float>(std::max(0.0001, assets_->getView().get_scale()));
    } else if (window) {
        camera_scale = static_cast<float>(std::max(0.0001, window->get_scale()));
    }

    const float prospective_scale = base_scale * perspective_scale;
    constexpr float kScaleEpsilon = 1e-4f;
    if (std::fabs(prospective_scale - current_scale) < kScaleEpsilon &&
        std::fabs(base_scale - last_scale_base_input_) < kScaleEpsilon &&
        std::fabs(perspective_scale - last_scale_perspective_input_) < kScaleEpsilon &&
        std::fabs(camera_scale - last_scale_camera_input_) < kScaleEpsilon) {
        return;
    }

    current_scale = prospective_scale;
    last_scale_base_input_ = base_scale;
    last_scale_perspective_input_ = perspective_scale;
    last_scale_camera_input_ = camera_scale;

    float desired_variant_scale = current_scale / camera_scale;
    if (!std::isfinite(desired_variant_scale) || desired_variant_scale <= 0.0f) {
        desired_variant_scale = current_scale;
    }

    const auto& steps = (info && !info->scale_variants.empty())
        ? static_cast<const std::vector<float>&>(info->scale_variants)
        : render_pipeline::ScalingLogic::DefaultScaleSteps();

    render_pipeline::ScalingLogic::HysteresisState hysteresis_state{};
    hysteresis_state.last_index = scale_variant_state_.last_variant_index;
    hysteresis_state.min_scale = scale_variant_state_.hysteresis_min;
    hysteresis_state.max_scale = scale_variant_state_.hysteresis_max;

    auto selection = render_pipeline::ScalingLogic::Choose(
        desired_variant_scale,
        steps,
        hysteresis_state,
        current_scale,
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

    mark_anchors_dirty();
    mark_mesh_dirty();
}

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
	if (it != info->animations.end()) {
		current_animation = name;
		Animation& anim = it->second;
		anim.change(current_frame, static_frame);
		frame_progress = 0.0f;
		refresh_cached_dimensions();
                mark_anchors_dirty();
                apply_anchor_follow_target();
	}
}

void Asset::update() {
    if (!info) return;

#if !defined(NDEBUG)
    const SDL_Point anchor_debug_start_world{world_x(), world_y()};
    const int anchor_debug_start_world_z = world_z();
    const int anchor_debug_start_layer = pos_ ? pos_->resolution_layer() : grid_resolution;
    const std::uint64_t anchor_debug_start_revision = anchor_world_revision_;
#endif

    // Detect external transform/frame changes before we do any work so bound children can react immediately.
    const bool external_world_changed = update_anchor_basis_if_needed();

    apply_anchor_follow_target();

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

    apply_anchor_follow_target();

    update_scale_values();

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

    if (info->moving_asset && (external_world_changed || post_world_changed)) {
        update_neighbor_lists(true);
    }

    const float alpha_target = hidden ? 0.0f : 1.0f;
    alpha_smoothing_.reset(alpha_target);

#if !defined(NDEBUG)
    const bool anchor_debug_world_changed =
        anchor_debug_start_world.x != world_x() ||
        anchor_debug_start_world.y != world_y() ||
        anchor_debug_start_world_z != world_z() ||
        anchor_debug_start_layer != (pos_ ? pos_->resolution_layer() : grid_resolution);
    if (anchor_debug_world_changed && anchor_world_revision_ == anchor_debug_start_revision) {
        vibble::log::warn("[Asset] anchor_world_revision did not advance after world transform change for asset '" +
                          (info ? info->name : std::string{"<unknown>"}) + "'");
        assert(anchor_world_revision_ != anchor_debug_start_revision);
    }
#endif
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
    neighbors.reset();
    impassable_naighbors.reset();
    neighbor_lists_initialized_ = false;
    last_neighbor_origin_ = SDL_Point{ std::numeric_limits<int>::min(), std::numeric_limits<int>::min() };

}

void Asset::set_tiling_info(std::optional<TilingInfo> info) {
    tiling_info_ = std::move(info);
}

void Asset::set_owning_room_name(std::string name) {
    owning_room_name_ = std::move(name);
}

void Asset::rebuild_animation_runtime() {
    ensure_animation_runtime(true);
}



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

AssetList* Asset::get_neighbors_list() { return neighbors.get(); }
const AssetList* Asset::get_neighbors_list() const { return neighbors.get(); }
AssetList* Asset::get_impassable_naighbors() { return impassable_naighbors.get(); }
const AssetList* Asset::get_impassable_naighbors() const { return impassable_naighbors.get(); }

void Asset::update_neighbor_lists(bool force_update) {
    if (!assets_ || !info || !info->moving_asset) {
        return;
    }

    auto base_filter = [this](const Asset* candidate) {
        if (!candidate || candidate == this || !candidate->info) {
            return false;
        }
        if (candidate->info->type == asset_types::texture) {
            return false;
        }
        return true;
};

    auto impassable_filter = [this](const Asset* candidate) {
        if (!candidate || candidate == this || !candidate->info) {
            return false;
        }
        if (candidate->info->type == asset_types::texture) {
            return false;
        }
        const std::string canonical_type = asset_types::canonicalize(candidate->info->type);
        if (canonical_type == asset_types::player) {
            return false;
        }
        if (canonical_type == asset_types::boundary) {
            return true;
        }
        if (canonical_type == asset_types::enemy || canonical_type == asset_types::npc) {
            return true;
        }
        if (candidate->info->moving_asset) {
            return true;
        }
        return !candidate->info->passable;
};

    const auto& candidates = assets_->getActiveRaw();
    if (candidates.empty()) {
        neighbors.reset();
        impassable_naighbors.reset();
        neighbor_lists_initialized_ = false;
        return;
    }

    const bool needs_rebuild = force_update || !neighbors || !neighbor_lists_initialized_ ||
                               last_neighbor_origin_.x != (pos_ ? pos_->world_x() : 0) ||
                               last_neighbor_origin_.y != (pos_ ? pos_->world_y() : 0);
    if (!needs_rebuild) {
        return;
    }

    impassable_naighbors.reset();
    neighbors = std::make_unique<AssetList>(
        candidates,
        this,
        info->NeighborSearchRadius,
        std::vector<std::string>{},
        std::vector<std::string>{},
        std::vector<std::string>{},
        base_filter);

    if (neighbors) {
        auto imp_child = std::make_unique<AssetList>(
            *neighbors,
            this,
            info->NeighborSearchRadius,
            std::vector<std::string>{},
            std::vector<std::string>{},
            std::vector<std::string>{},
            impassable_filter,
            true);
        impassable_naighbors = std::move(imp_child);
    }

    last_neighbor_origin_ = pos_ ? SDL_Point{pos_->world.x, pos_->world.y} : SDL_Point{0, 0};
    neighbor_lists_initialized_ = true;
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

        SDL_Point world_pos = pos_ ? SDL_Point{pos_->world.x, pos_->world.y} : SDL_Point{0, 0};
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

float Asset::runtime_height_px() const {
        const float base_height = static_cast<float>(height());
        if (!(base_height > 0.0f)) {
                return 0.0f;
        }
        float remainder = last_scale_usage_.remainder_scale;
        if (!std::isfinite(remainder) || remainder <= 0.0f) {
                remainder = 1.0f;
        }
        return base_height * remainder;
}

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

void Asset::cache_grid_residency(const world::GridPoint& point) {
        cached_grid_residency_    = point.key();
        has_cached_grid_residency_ = true;
}

void Asset::clear_grid_residency_cache() {
        cached_grid_residency_    = world::GridKey{ std::numeric_limits<int>::min(), std::numeric_limits<int>::min(), 0, 0 };
        has_cached_grid_residency_ = false;
}

void Asset::sync_transform_to_position() {
}

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

void Asset::mark_anchors_dirty() {
        for (auto& handle : anchor_handles_) {
                handle.dirty = true;
        }
        ++anchor_world_revision_;
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

        const bool has_pos = (pos_ != nullptr);
        const float perspective = (has_pos && pos_->perspective_scale > 0.0001f)
                ? pos_->perspective_scale
                : (last_scale_perspective_input_ > 0.0001f ? last_scale_perspective_input_ : 1.0f);
        sig.perspective_scale = perspective;

        sig.world_z_offset = world_z_offset_;
        sig.resolution_layer = has_pos ? pos_->resolution_layer() : grid_resolution;
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

void Asset::set_anchor_follow_target(std::optional<AnchorFollowTarget> follow) {
        follow_anchor_ = std::move(follow);
        follow_initialized_ = false;
        follow_missing_ = false;
        last_follow_world_ = SDL_Point{0, 0};
        last_follow_world_z_ = 0;
        last_follow_source_revision_ = 0;
        if (follow_anchor_) {
                apply_anchor_follow_target();
        } else {
                set_anchor_hidden(false);
        }
}

void Asset::bind_child_to_anchor(Asset* child, const std::string& anchor_name) {
        if (!child || anchor_name.empty()) {
                throw std::runtime_error("bind_child_to_anchor requires non-null child and non-empty anchor name");
        }

        Assets* owner_assets = assets_ ? assets_ : child->assets_;
        if (!owner_assets) {
                throw std::runtime_error("bind_child_to_anchor requires Assets owner to update binding graph");
        }
        world::WorldGrid& grid = owner_assets->world_grid();

        child->set_anchor_hidden(false);
        AnchorFollowTarget follow = child->anchor_follow_target().value_or(AnchorFollowTarget{});
        follow.source = this;
        follow.anchor_name = anchor_name;
        child->set_anchor_follow_target(std::move(follow));
        child->last_follow_source_revision_ = this->anchor_world_revision();
        mark_anchors_dirty();
#ifndef NDEBUG
        {
                const auto before = grid.children_of(this);
                SDL_assert(std::count(before.begin(), before.end(), child) == 0);
        }
#endif
        grid.update_asset_parent(child, this);
#ifndef NDEBUG
        {
                const auto after = grid.children_of(this);
                SDL_assert(std::count(after.begin(), after.end(), child) == 1);
        }
#endif
}

void Asset::unbind_child_from_anchor(Asset* child) {
        if (!child) {
                return;
        }
        Assets* owner_assets = assets_ ? assets_ : child->assets_;
        if (owner_assets) {
                owner_assets->world_grid().unbind_child(child);
#ifndef NDEBUG
                SDL_assert(owner_assets->world_grid().parent_of(child) == nullptr);
#endif
        } else {
                child->parent = nullptr;
                child->set_anchor_follow_target(std::nullopt);
        }
}

void Asset::refresh_bound_children_anchor_follows() {
        // Anchor-follow propagation now runs in a global Assets update phase.
        // Keep this helper for compatibility with existing call sites.
}

void Asset::apply_anchor_follow_target() {
        if (!follow_anchor_ || !follow_anchor_->valid()) {
                return;
        }
        AnchorFollowTarget& follow = *follow_anchor_;
        Asset* source = follow.source;
        if (!source) {
                throw std::runtime_error("Anchor follow failed: missing controller source for anchor '" + follow.anchor_name + "'");
        }

        const std::uint64_t source_anchor_revision = source->anchor_world_revision();
        if (source_anchor_revision == last_follow_source_revision_ && (follow_initialized_ || follow_missing_)) {
                return;
        }

        auto resolved = source->anchor_state(follow.anchor_name, anchor_points::GridMaterialization::Ensure, follow.depth_policy);
        if (!resolved.has_value() || resolved->missing) {
                follow_initialized_ = false;
                follow_missing_ = true;
                set_anchor_hidden(true);
                last_follow_source_revision_ = source_anchor_revision;
                ++anchor_world_revision_;
                return;
        }
        follow_error_reported_ = false;
        follow_missing_ = false;
        set_anchor_hidden(false);

        SDL_Point target_px = resolved->world_px;
        int target_z = resolved->world_z;
        int target_layer = resolved->resolution_layer;

        if (follow.layer_policy.has_value() &&
            follow.layer_policy.value() == AnchorFollowTarget::LayerPolicy::MatchControllerAsset) {
                if (auto* source_gp = source->grid_point()) {
                        target_layer = source_gp->resolution_layer();
                } else {
                        target_layer = source->grid_resolution;
                }
        }

        if (!resolved->grid_point && source) {
                if (auto* source_gp = source->grid_point()) {
                        target_layer = source_gp->resolution_layer();
                } else {
                        target_layer = source->grid_resolution;
                }
        }

        grid_resolution = target_layer;

        const bool had_follow_position = follow_initialized_;
        const SDL_Point previous_follow_world = last_follow_world_;
        const int previous_follow_world_z = last_follow_world_z_;

        const bool unchanged = had_follow_position &&
                               previous_follow_world.x == target_px.x &&
                               previous_follow_world.y == target_px.y &&
                               previous_follow_world_z == target_z &&
                               world_x() == target_px.x &&
                               world_y() == target_px.y &&
                               world_z() == target_z &&
                               (!pos_ || pos_->resolution_layer() == target_layer);

        last_follow_world_ = target_px;
        last_follow_world_z_ = target_z;
        follow_initialized_ = true;
        last_follow_source_revision_ = source_anchor_revision;

        if (unchanged) {
                return;
        }

        ++anchor_world_revision_;

        if (!assets_) {
                initial_world_pos_ = target_px;
                return;
        }

        world::WorldGrid& grid = assets_->world_grid();
        world::GridPoint& target = world::GridPoint::from_world(target_px.x, target_px.y, target_z, target_layer, grid);
        if (pos_) {
                grid.move_asset(this, *pos_, target);
        } else {
                const int start_x = target_px.x + 1;
                const int start_y = target_px.y + 1;
                world::GridPoint virtual_start = world::GridPoint::make_virtual(start_x, start_y, target_z, target_layer);
                grid.move_asset(this, virtual_start, target);
        }
        mark_anchors_dirty();
}

Asset::AnchorHandle& Asset::get_anchor_point(const std::string& name) {
        auto it = anchor_lookup_.find(name);
        if (it != anchor_lookup_.end() && it->second < anchor_handles_.size()) {
                return anchor_handles_[it->second];
        }

        AnchorHandle handle;
        handle.name  = name;
        handle.owner = this;
        anchor_handles_.push_back(std::move(handle));
        anchor_lookup_[name] = anchor_handles_.size() - 1;
        return anchor_handles_.back();
}

std::optional<ResolvedAnchor> Asset::anchor_state(const std::string& name,
                                                  anchor_points::GridMaterialization grid_policy,
                                                  std::optional<anchor_points::AnchorDepthPolicy> depth_policy) {
        AnchorHandle& handle = get_anchor_point(name);
        handle.update(grid_policy, depth_policy);
        ResolvedAnchor resolved{};
        resolved.world_px    = handle.world_px;
        resolved.world_z     = handle.world_z;
        resolved.resolution_layer = handle.resolution_layer;
        resolved.source_texture_px = handle.source_texture_px;
        resolved.has_canonical_texture_source = handle.has_canonical_texture_source;
        resolved.grid_point  = handle.grid;
        resolved.missing     = handle.missing;
        resolved.in_front    = handle.in_front;
        if (!resolved.missing && !resolved.has_canonical_texture_source) {
                throw std::runtime_error("Anchor invariant failure: resolved anchor missing canonical texture source");
        }
        return resolved;
}

void Asset::AnchorHandle::update(anchor_points::GridMaterialization grid_policy,
                                 std::optional<anchor_points::AnchorDepthPolicy> depth_policy) {
#if !defined(NDEBUG)
        auto& counters = anchor_update_counters();
        ++counters.calls;
#endif

        const bool cache_hit = !dirty && last_update_key_.matches(grid_policy, depth_policy);
        if (cache_hit) {
#if !defined(NDEBUG)
                ++counters.cache_hits;
#endif
                return;
        }

#if !defined(NDEBUG)
        ++counters.recomputes;
#endif
        last_update_key_.set(grid_policy, depth_policy);

        if (!owner) {
                dirty = false;
                return;
        }
        const AnimationFrame* frame = owner->current_frame;
        const DisplacedAssetAnchorPoint* anchor = find_anchor_with_frame_fallback(*owner, frame, name);

        if (!anchor) {
                grid = nullptr;
                world_px = SDL_Point{0, 0};
                world_z = 0;
                resolution_layer = 0;
                missing = true;
                in_front = true;
                source_texture_px = SDL_Point{0, 0};
                has_canonical_texture_source = false;
                dirty = false;
                return;
        }

        const anchor_points::AnchorDepthPolicy resolved_depth = depth_policy.value_or(
                anchor->in_front ? anchor_points::AnchorDepthPolicy::InFront : anchor_points::AnchorDepthPolicy::Behind);

        const auto resolved = anchor_points::resolve_frame_anchor_sample(
                *owner,
                *anchor,
                resolved_depth,
                grid_policy);

        grid = resolved.resolved.grid_point;
        world_px = resolved.resolved.world_px;
        world_z = resolved.resolved.world_z;
        resolution_layer = resolved.resolved.resolution_layer;
        missing = resolved.resolved.missing;
        in_front = resolved.resolved.in_front;
        source_texture_px = resolved.resolved.source_texture_px;
        has_canonical_texture_source = resolved.resolved.has_canonical_texture_source;
        if (!missing && !resolved.resolved.has_canonical_texture_source) {
                throw std::runtime_error("Anchor invariant failure: resolved anchor missing canonical texture source");
        }
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

void Asset::clear_grid_id() {
        grid_id_ = 0;
}

void Asset::set_composite_texture(SDL_Texture* tex) {
    if (composite_texture_ && composite_texture_ != tex) {
        SDL_DestroyTexture(composite_texture_);
    }
    composite_texture_ = tex;
}

float Asset::smoothed_translation_x() const {
    return pos_ ? static_cast<float>(pos_->world_x()) : 0.0f;
}

float Asset::smoothed_translation_y() const {
    return pos_ ? static_cast<float>(pos_->world_y()) : 0.0f;
}

float Asset::smoothed_scale() const {
    return current_scale;
}

float Asset::smoothed_alpha() const {
        float value = alpha_smoothing_.value_for_render();
        if (!std::isfinite(value)) {
                value = hidden ? 0.0f : 1.0f;
        }
        return std::clamp(value, 0.0f, 1.0f);
}

void Asset::move_to_world_position(int world_x, int world_y, int world_z) {
    if (!assets_) return;

    world::WorldGrid& grid = assets_->world_grid();
    const int resolved_layer = pos_ ? pos_->resolution_layer() : grid_resolution;
    world::GridPoint& target = world::GridPoint::from_world(world_x, world_y, world_z, resolved_layer, grid);

    if (pos_) {
        grid.move_asset(this, *pos_, target);
    } else {
        const int start_x = world_x + 1; // force placement when no previous grid residency
        const int start_y = world_y + 1;
        world::GridPoint virtual_start = world::GridPoint::make_virtual(start_x, start_y, world_z, grid_resolution);
        grid.move_asset(this, virtual_start, target);
    }

    mark_anchors_dirty();
}

void Asset::set_world_z(int world_z) {
    if (!pos_ || !assets_) return;

    // Move to same XY but different Z
    move_to_world_position(pos_->world_x(), pos_->world_y(), world_z);
}
