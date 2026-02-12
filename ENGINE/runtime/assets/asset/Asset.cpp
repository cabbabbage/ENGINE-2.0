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
#include "assets/asset_types.hpp"
#include "utils/grid.hpp"
#include "utils/transform_smoothing_settings.hpp"
#include "utils/log.hpp"
#include "gameplay/world/grid_point.hpp"
#include "gameplay/world/world_grid.hpp"
#include <iostream>
#include <random>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <SDL3/SDL.h>
#include "utils/FramePointResolver.hpp"
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

static std::vector<std::string> collect_animation_child_names(const AssetInfo& info) {
        std::vector<std::string> names;
        std::unordered_set<std::string> seen;
        auto add = [&](const std::string& name) {
                if (name.empty()) {
                        return;
                }
                if (seen.insert(name).second) {
                        names.push_back(name);
                }
};

        for (const auto& name : info.animation_children) {
                add(name);
        }

        for (const auto& entry : info.animations) {
                for (const auto& child_name : entry.second.child_assets()) {
                        add(child_name);
                }
        }

        return names;
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
             int grid_resolution_)
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
    , animation_children_(o.animation_children_)
    , child_creation_requested_(o.child_creation_requested_)
    , is_child_timeline_asset_(o.is_child_timeline_asset_)
    , child_timeline_index_(o.child_timeline_index_)
{

        clear_render_caches();
        last_scale_usage_ = o.last_scale_usage_;
        scale_variant_state_ = o.scale_variant_state_;
        cached_grid_residency_    = o.cached_grid_residency_;
        has_cached_grid_residency_ = o.has_cached_grid_residency_;
        alpha_smoothing_          = o.alpha_smoothing_;
        finalized_                = o.finalized_;
        for (auto& slot : animation_children_) {
                slot.timeline = nullptr;
        }
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
        animation_children_       = o.animation_children_;
        child_creation_requested_ = o.child_creation_requested_;
        is_child_timeline_asset_ = o.is_child_timeline_asset_;
        child_timeline_index_ = o.child_timeline_index_;
        for (auto& slot : animation_children_) {
                slot.timeline = nullptr;
        }
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
	}
}

void Asset::update() {
    if (!info) return;


    const int previous_x = pos_ ? pos_->world_x() : 0;
    const int previous_y = pos_ ? pos_->world_y() : 0;
    const int previous_z = pos_ ? pos_->world_z() : 0;

    if (controller_) {
        if (assets_) {
            if (Input* in = assets_->get_input()) {
                controller_->update(*in);
            }
        }
        controller_->process_pending_attacks(*this);
    }

    const int current_x = pos_ ? pos_->world_x() : 0;
    const int current_y = pos_ ? pos_->world_y() : 0;
    const int current_z = pos_ ? pos_->world_z() : 0;
    const bool moved = (current_x != previous_x || current_y != previous_y || current_z != previous_z);

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

    if (!dead && anim_runtime_) {
        anim_runtime_->update();
    }

    request_child_timeline_creation_if_needed();


    



    if (info->moving_asset && moved) {
        update_neighbor_lists(true);
    }

    const float alpha_target = hidden ? 0.0f : 1.0f;
    alpha_smoothing_.reset(alpha_target);
}

std::string Asset::get_current_animation() const { return current_animation; }

const std::vector<Asset::AnimationChildAttachment>& Asset::animation_children() const {
    return animation_children_;
}

std::vector<Asset::AnimationChildAttachment>& Asset::animation_children() {
    return animation_children_;
}

void Asset::request_child_timeline_creation_if_needed() {
    if (!assets_ || parent || dead || !active) {
        return;
    }
    if (child_creation_requested_) {
        return;
    }
    const bool has_child_slots = std::any_of(animation_children_.begin(),
                                             animation_children_.end(),
                                             [](const AnimationChildAttachment& slot) {
                                                 return slot.child_index >= 0 && slot.timeline;
                                             });
    if (!has_child_slots) {
        return;
    }
    child_creation_requested_ = true;
    assets_->request_child_timeline_creation(this);
}



void Asset::stop_all_child_async() {
}

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
        child_creation_requested_ = false;
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

void Asset::on_scale_factor_changed() {

        last_scale_usage_ = {};

        refresh_cached_dimensions();

        mark_composite_dirty();
        mark_mesh_dirty();

        if (assets_) {
                assets_->queue_asset_dimension_update(this);
        }
}

void Asset::set_hidden(bool state){ hidden = state; }
bool  Asset::is_hidden() const { return hidden; }



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
}

void Asset::set_world_z(int world_z) {
    if (!pos_ || !assets_) return;

    // Move to same XY but different Z
    move_to_world_position(pos_->world_x(), pos_->world_y(), world_z);
}
