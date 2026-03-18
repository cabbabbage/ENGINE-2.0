#include "rendering/render/dynamic_boundary_system.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/grid_overlay.hpp"
#include "gameplay/world/world_grid.hpp"
#include "assets/asset/asset_library.hpp"
#include "assets/asset/asset_info.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "assets/asset/animation.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/map_generation/room.hpp"
#include "utils/area.hpp"
#include "utils/log.hpp"
#include "utils/grid.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <cstdint>
#include <tuple>

namespace {
constexpr float kDefaultAnimationFrameMs = 1000.0f / 24.0f;
constexpr long long kMaxBoundaryCells = 250000;

inline bool is_trail_string(const std::string& text) {
    if (text.size() != 5) return false;
    return std::tolower(static_cast<unsigned char>(text[0])) == 't' &&
           std::tolower(static_cast<unsigned char>(text[1])) == 'r' &&
           std::tolower(static_cast<unsigned char>(text[2])) == 'a' &&
           std::tolower(static_cast<unsigned char>(text[3])) == 'i' &&
           std::tolower(static_cast<unsigned char>(text[4])) == 'l';
}

inline float make_positive_scale(float value, float fallback = 1.0f) {
    if (std::isfinite(value) && value > 0.0f) {
        return static_cast<float>(value);
    }
    return fallback;
}

inline SDL_Point rounded_world_point(const SDL_FPoint& point) {
    return SDL_Point{
        static_cast<int>(std::lround(point.x)),
        static_cast<int>(std::lround(point.y))
    };
}

struct BoundaryScaleResult {
    float remainder_scale = 1.0f;
    int variant_index = 0;
};

BoundaryScaleResult compute_boundary_asset_scale(DynamicBoundarySystem::BoundaryCandidate& candidate,
                                                 const WarpedScreenGrid& cam,
                                                 const Assets* assets,
                                                 const SDL_FPoint& world_pos,
                                                 int world_z) {
    (void)assets;
    if (!candidate.info) {
        return BoundaryScaleResult{};
    }

    const SDL_Point world_pt = rounded_world_point(world_pos);
    const WarpedScreenGrid::RenderEffects effects =
        cam.compute_render_effects(world_pt, 0.0f, 0.0f, WarpedScreenGrid::RenderSmoothingKey{}, world_z);

    const float perspective_scale = make_positive_scale(effects.distance_scale);
    const float base_scale = (std::isfinite(candidate.info->scale_factor) && candidate.info->scale_factor > 0.0f)
        ? candidate.info->scale_factor
        : 1.0f;
    const float current_scale = base_scale * perspective_scale;

    // Match runtime assets: pick the nearest larger variant for this draw scale
    // and only upscale if every variant is smaller than the target.
    float desired_variant_scale = current_scale;
    if (!std::isfinite(desired_variant_scale) || desired_variant_scale <= 0.0f) {
        desired_variant_scale = 1.0f;
    }

    const auto& steps = (!candidate.info->scale_variants.empty())
        ? candidate.info->scale_variants
        : render_pipeline::ScalingLogic::DefaultScaleSteps();

    auto selection = render_pipeline::ScalingLogic::Choose(desired_variant_scale,
                                                           steps,
                                                           candidate.hysteresis_state,
                                                           desired_variant_scale,
                                                           render_pipeline::ScalingLogic::HysteresisOptions{});
    candidate.hysteresis_state.last_index = selection.index;
    candidate.hysteresis_state.min_scale = selection.hysteresis_min;
    candidate.hysteresis_state.max_scale = selection.hysteresis_max;

    float stored_scale = selection.stored_scale;
    if (!std::isfinite(stored_scale) || stored_scale <= 0.0f) {
        stored_scale = 1.0f;
    }

    float remainder_scale = current_scale / stored_scale;
    if (!std::isfinite(remainder_scale) || remainder_scale <= 0.0f) {
        remainder_scale = 1.0f;
    }
    return BoundaryScaleResult{remainder_scale, selection.index};
}



}

using render_overlay::mix_uint64;

DynamicBoundarySystem::DynamicBoundarySystem() = default;

DynamicBoundarySystem::~DynamicBoundarySystem() {
    clear_runtime_caches();
}

bool DynamicBoundarySystem::initialize(SDL_Renderer* renderer, AssetLibrary* asset_library) {
    if (!renderer || !asset_library) {
        vibble::log::warn("[DynamicBoundarySystem] Renderer or AssetLibrary is null; cannot initialize");
        return false;
    }
    renderer_ = renderer;
    asset_library_ = asset_library;
    initialized_ = true;
    config_dirty_ = true;
    boundary_types_.clear();
    clear_runtime_caches();
    return true;
}

void DynamicBoundarySystem::update(const WarpedScreenGrid& cam,
                                   world::WorldGrid& grid,
                                   Assets* assets,
                                   float delta_ms) {
    static int s_debug_frame_counter = 0;
    active_boundary_sprites_.clear();
    if (!initialized_ || !renderer_ || !asset_library_ || !assets) {
        return;
    }
    if (delta_ms < 0.0f) {
        delta_ms = 0.0f;
    }

    const nlohmann::json& map_info = assets->map_info_json();
    const bool config_changed = refresh_boundary_config_revision(map_info);
    if (config_dirty_ || config_changed) {
        auto boundary_it = map_info.find("map_boundary_data");
        if (boundary_it != map_info.end() && boundary_it->is_object()) {
            parse_boundary_config(*boundary_it);
        } else {
            parse_boundary_config(nlohmann::json::object());
        }
    }

    if (boundary_types_.empty()) {
        return;
    }

    if (warning_revision_ != config_revision_) {
        warning_revision_ = config_revision_;
        dense_type_warnings_.clear();
    }

    SDL_FPoint view_center = cam.get_view_center_f();
    double view_height = cam.view_height_world();
    const float margin = static_cast<float>(view_height) * 0.5f;
    SDL_FRect visible_bounds{
        static_cast<float>(view_center.x - view_height - margin),
        static_cast<float>(view_center.y - view_height - margin),
        static_cast<float>(view_height * 2.0 + margin * 2.0),
        static_cast<float>(view_height * 2.0 + margin * 2.0)
    };

    const world::GridPoint grid_origin = grid.origin();
    const float spacing_multiplier = render_overlay::clamp_spacing_multiplier(config().grid_spacing_multiplier);
    const std::vector<Room*>& rooms = assets->rooms();
    const std::size_t rooms_hash = compute_rooms_topology_hash(assets);
    ensure_region_cache_valid(grid, rooms, rooms_hash, spacing_multiplier);

    StaticAssignmentFingerprint next_static{};
    next_static.config_revision = config_revision_;
    next_static.rooms_hash = rooms_hash;
    next_static.origin_x = grid_origin.world_x();
    next_static.origin_z = grid_origin.world_z();
    next_static.origin_layer = grid_origin.resolution_layer();
    next_static.grid_resolution = grid.grid_resolution();
    next_static.spacing_multiplier = spacing_multiplier;

    next_static.min_grid_x = static_cast<int>(std::floor(visible_bounds.x));
    next_static.max_grid_x = static_cast<int>(std::ceil(visible_bounds.x + visible_bounds.w));
    next_static.min_grid_y = static_cast<int>(std::floor(visible_bounds.y));
    next_static.max_grid_y = static_cast<int>(std::ceil(visible_bounds.y + visible_bounds.h));

    if (static_assignment_fingerprint_ != next_static) {
        static_assignments_.clear();
        for (size_t type_idx = 0; type_idx < boundary_types_.size(); ++type_idx) {
            BoundaryType& btype = boundary_types_[type_idx];
            const int resolution_layer = vibble::grid::clamp_resolution(btype.grid_resolution);
            const int base_spacing = render_overlay::spacing_for_resolution(resolution_layer);
            if (base_spacing <= 0) {
                continue;
            }
            int grid_spacing = render_overlay::scaled_spacing(base_spacing, spacing_multiplier);
            if (grid_spacing <= 0) {
                continue;
            }

            const float max_random_jitter = render_overlay::clamp_random_jitter(static_cast<float>(btype.jitter));

            const float min_x = visible_bounds.x;
            const float max_x = visible_bounds.x + visible_bounds.w;
            const float min_y = visible_bounds.y;
            const float max_y = visible_bounds.y + visible_bounds.h;

            auto compute_span = [&](int spacing, int& start_x, int& end_x, int& start_y, int& end_y) {
                start_x = static_cast<int>(std::floor((min_x - static_cast<float>(grid_origin.world_x())) / spacing));
                end_x = static_cast<int>(std::ceil((max_x - static_cast<float>(grid_origin.world_x())) / spacing));
                start_y = static_cast<int>(std::floor((min_y - static_cast<float>(grid_origin.world_z())) / spacing));
                end_y = static_cast<int>(std::ceil((max_y - static_cast<float>(grid_origin.world_z())) / spacing));
            };

            int start_idx_x = 0;
            int end_idx_x = 0;
            int start_idx_y = 0;
            int end_idx_y = 0;
            compute_span(grid_spacing, start_idx_x, end_idx_x, start_idx_y, end_idx_y);
            const long long count_x = static_cast<long long>(end_idx_x) - static_cast<long long>(start_idx_x) + 1;
            const long long count_y = static_cast<long long>(end_idx_y) - static_cast<long long>(start_idx_y) + 1;
            long long total_cells = (count_x > 0 && count_y > 0) ? count_x * count_y : 0;
            if (total_cells > kMaxBoundaryCells) {
                const double scale = std::sqrt(static_cast<double>(total_cells) / static_cast<double>(kMaxBoundaryCells));
                const int spacing_scale = std::max(1, static_cast<int>(std::ceil(scale)));
                const int adjusted_spacing = grid_spacing * spacing_scale;
                if (adjusted_spacing > grid_spacing) {
                    grid_spacing = adjusted_spacing;
                    compute_span(grid_spacing, start_idx_x, end_idx_x, start_idx_y, end_idx_y);
                    const long long new_count_x = static_cast<long long>(end_idx_x) - static_cast<long long>(start_idx_x) + 1;
                    const long long new_count_y = static_cast<long long>(end_idx_y) - static_cast<long long>(start_idx_y) + 1;
                    total_cells = (new_count_x > 0 && new_count_y > 0) ? new_count_x * new_count_y : 0;
                }
            }
            if (total_cells > kMaxBoundaryCells) {
                if (dense_type_warnings_.insert(static_cast<int>(type_idx)).second) {
                    vibble::log::warn(std::string{"[DynamicBoundarySystem] Skipping dense boundary type '"} +
                                      btype.display_name + "' (grid_resolution=" +
                                      std::to_string(btype.grid_resolution) +
                                      ") to avoid excessive cells.");
                }
                continue;
            }

            for (int ix = start_idx_x; ix <= end_idx_x; ++ix) {
                const int world_x = grid_origin.world_x() + ix * grid_spacing;
                for (int iy = start_idx_y; iy <= end_idx_y; ++iy) {
                    const int world_z = grid_origin.world_z() + iy * grid_spacing;
                    const BoundaryKey key = make_key(static_cast<int>(type_idx), resolution_layer, ix, iy, world_z);
                    const int candidate_idx = select_candidate_for_key(key, btype);
                    if (candidate_idx < 0 || candidate_idx >= static_cast<int>(btype.candidates.size())) {
                        continue;
                    }
                    BoundaryCandidate& candidate = btype.candidates[candidate_idx];
                    if (candidate.is_null || candidate.frames.empty()) {
                        continue;
                    }

                    const SDL_FPoint jitter = sample_jitter_offset(key, max_random_jitter);
                    const float jittered_z = static_cast<float>(world_z) + jitter.y;
                    SDL_FPoint world_pos{
                        static_cast<float>(world_x) + jitter.x,
                        0.0f
                    };
                    const SDL_Point world_pt{static_cast<int>(std::lround(world_pos.x)), static_cast<int>(std::lround(jittered_z))};
                    const auto& region_entry = resolve_region_cache(world_pt, rooms);
                    if (region_entry.region_kind != RegionKind::Boundary || region_entry.blocked) {
                        continue;
                    }

                    static_assignments_.push_back(StaticCellAssignment{key, candidate_idx, static_cast<int>(type_idx), world_pos, static_cast<int>(std::lround(jittered_z))});
                }
            }
        }
        static_assignment_fingerprint_ = next_static;
    }

    for (const StaticCellAssignment& assignment : static_assignments_) {
        if (assignment.boundary_type_index < 0 || assignment.boundary_type_index >= static_cast<int>(boundary_types_.size())) {
            continue;
        }
        BoundaryType& btype = boundary_types_[assignment.boundary_type_index];
        if (assignment.candidate_index < 0 || assignment.candidate_index >= static_cast<int>(btype.candidates.size())) {
            continue;
        }
        BoundaryCandidate& candidate = btype.candidates[assignment.candidate_index];
        if (candidate.is_null || candidate.frames.empty()) {
            continue;
        }

        const world::GridPoint virtual_point =
            world::GridPoint::make_virtual(static_cast<int>(std::lround(assignment.world_pos.x)),
                                           static_cast<int>(std::lround(assignment.world_pos.y)),
                                           assignment.world_z,
                                           assignment.key.resolution_layer);
        const world::GridKey virtual_key =
            grid.grid_key_from_world(virtual_point, assignment.key.resolution_layer);
        if (world::GridPoint* gp = grid.find_grid_point(virtual_key)) {
            gp->region_kind = world::GridPoint::RegionKind::Boundary;
            gp->region_owner = nullptr;
        }

        SDL_FPoint screen_pos{};
        if (!cam.project_world_point(assignment.world_pos, static_cast<float>(assignment.world_z), screen_pos)) {
            continue;
        }
        if (!std::isfinite(screen_pos.x) || !std::isfinite(screen_pos.y)) {
            continue;
        }

        auto& frame_state = animation_states_[assignment.key];
        frame_state.elapsed_ms += delta_ms;
        const int total_frames = static_cast<int>(candidate.frames.size());
        if (total_frames <= 0) {
            continue;
        }

        int current_index = frame_state.frame_index % total_frames;
        float frame_duration = candidate.frames[current_index].duration_ms;
        if (!(frame_duration > 0.0f)) {
            frame_duration = kDefaultAnimationFrameMs;
        }
        while (frame_state.elapsed_ms >= frame_duration && total_frames > 0) {
            frame_state.elapsed_ms -= frame_duration;
            current_index = (current_index + 1) % total_frames;
            frame_state.frame_index = current_index;
            frame_duration = candidate.frames[current_index].duration_ms;
            if (!(frame_duration > 0.0f)) {
                frame_duration = kDefaultAnimationFrameMs;
            }
        }

        frame_state.frame_index = current_index;
        const BoundaryFrame& active_frame = candidate.frames[current_index];
        const BoundaryScaleResult scale_result =
            compute_boundary_asset_scale(candidate, cam, assets, assignment.world_pos, assignment.world_z);
        const int variant_index = scale_result.variant_index;
        const auto& variants = active_frame.variants;
        const BoundaryFrameVariant* frame_variant = nullptr;
        if (!variants.empty()) {
            const int clamped_index = std::clamp(variant_index, 0, static_cast<int>(variants.size()) - 1);
            if (variants[clamped_index].texture) {
                frame_variant = &variants[clamped_index];
            } else {
                for (const auto& variant : variants) {
                    if (variant.texture) {
                        frame_variant = &variant;
                        break;
                    }
                }
            }
        }
        if (!frame_variant || !frame_variant->texture) {
            continue;
        }

        SDL_Texture* texture = frame_variant->texture;
        BoundarySprite sprite;
        sprite.texture = texture;
        sprite.spawn_id = btype.spawn_id;
        sprite.world_pos = assignment.world_pos;
        sprite.screen_pos = screen_pos;
        sprite.world_z = assignment.world_z;
        sprite.texture_w = frame_variant->width;
        sprite.texture_h = frame_variant->height;
        if (sprite.texture_w <= 0 || sprite.texture_h <= 0) {
            float texture_w = 0.0f;
            float texture_h = 0.0f;
            if (SDL_GetTextureSize(texture, &texture_w, &texture_h)) {
                sprite.texture_w = static_cast<int>(std::lround(texture_w));
                sprite.texture_h = static_cast<int>(std::lround(texture_h));
            }
        }
        if (sprite.texture_w <= 0 || sprite.texture_h <= 0) {
            continue;
        }
        const float config_scale = base_size_scale();
        sprite.asset_scale = scale_result.remainder_scale;
        sprite.world_width = static_cast<float>(sprite.texture_w) * scale_result.remainder_scale * config_scale;
        sprite.world_height = static_cast<float>(sprite.texture_h) * scale_result.remainder_scale * config_scale;
        if (!std::isfinite(sprite.world_width) || sprite.world_width <= 0.0f ||
            !std::isfinite(sprite.world_height) || sprite.world_height <= 0.0f) {
            continue;
        }
        sprite.boundary_type_index = assignment.boundary_type_index;
        sprite.candidate_index = assignment.candidate_index;
        sprite.current_frame_index = current_index;
        sprite.total_frames = total_frames;
        sprite.frame_duration_ms = frame_duration;
        sprite.frame_elapsed_ms = frame_state.elapsed_ms;

        active_boundary_sprites_.push_back(sprite);
    }

    const double anchor_depth = cam.anchor_world_z();
    std::sort(active_boundary_sprites_.begin(), active_boundary_sprites_.end(),
        [anchor_depth](const BoundarySprite& a, const BoundarySprite& b) {
            const double da = render_depth::depth_from_anchor(anchor_depth, static_cast<double>(a.world_z));
            const double db = render_depth::depth_from_anchor(anchor_depth, static_cast<double>(b.world_z));
            if (da != db) return da > db;
            return a.world_pos.x < b.world_pos.x;
        });

    ++s_debug_frame_counter;
    if ((s_debug_frame_counter % 120) == 0) {
        vibble::log::info(std::string{"[DynamicBoundarySystem] frame stats: types="} +
                          std::to_string(boundary_types_.size()) +
                          " static_assignments=" + std::to_string(static_assignments_.size()) +
                          " active_sprites=" + std::to_string(active_boundary_sprites_.size()));
    }
}

std::size_t DynamicBoundarySystem::BoundaryKeyHash::operator()(const DynamicBoundarySystem::BoundaryKey& key) const noexcept {
    return static_cast<std::size_t>(
        render_overlay::hash_grid_cell(key.grid_x,
                                       key.grid_y,
                                       key.world_z,
                                       key.resolution_layer,
                                       key.group));
}

DynamicBoundarySystem::BoundaryKey DynamicBoundarySystem::make_key(int group_idx, int resolution_layer, int grid_x, int grid_y, int world_z) const {
    return DynamicBoundarySystem::BoundaryKey{
        group_idx,
        resolution_layer,
        grid_x,
        grid_y,
        world_z
    };
}

std::uint64_t DynamicBoundarySystem::hash_key(const BoundaryKey& key) const {
    return render_overlay::hash_grid_cell(key.grid_x,
                                          key.grid_y,
                                          key.world_z,
                                          key.resolution_layer,
                                          key.group,
                                          boundary_regen_seed_);
}

int DynamicBoundarySystem::select_candidate_for_key(const BoundaryKey& key, const BoundaryType& btype) {
    auto it = boundary_assignments_.find(key);
    if (it != boundary_assignments_.end()) {
        return it->second;
    }
    if (btype.total_chance <= 0 || btype.candidates.empty()) {
        boundary_assignments_.emplace(key, -1);
        return -1;
    }
    const int roll = render_overlay::hashed_roll(hash_key(key), btype.total_chance);
    if (roll < 0) {
        boundary_assignments_.emplace(key, -1);
        return -1;
    }

    int cumulative = 0;
    for (int idx = 0; idx < static_cast<int>(btype.candidates.size()); ++idx) {
        cumulative += btype.candidates[idx].chance;
        if (roll < cumulative) {
            boundary_assignments_.emplace(key, idx);
            return idx;
        }
    }

    boundary_assignments_.emplace(key, static_cast<int>(btype.candidates.size()) - 1);
    return static_cast<int>(btype.candidates.size()) - 1;
}

SDL_FPoint DynamicBoundarySystem::sample_jitter_offset(const BoundaryKey& key, float max_jitter) const {
    return render_overlay::jitter_from_hash(hash_key(key), max_jitter);
}

void DynamicBoundarySystem::build_candidate_frames(BoundaryCandidate& candidate) {
    candidate.frames.clear();
    candidate.is_null = false;
    if (candidate.asset_name.empty() || candidate.asset_name == "null") {
        candidate.is_null = true;
        return;
    }
    if (!asset_library_) {
        candidate.is_null = true;
        return;
    }

    auto info = asset_library_->get(candidate.asset_name);
    if (!info) {
        auto normalize = [](std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        };
        const std::string wanted = normalize(candidate.asset_name);
        for (const auto& [asset_key, asset_info] : asset_library_->all()) {
            if (!asset_info) {
                continue;
            }
            const std::string key_name = normalize(asset_key);
            const std::string display_name = normalize(asset_info->name);
            if (wanted == key_name || (!display_name.empty() && wanted == display_name)) {
                info = asset_info;
                candidate.asset_name = asset_key;
                break;
            }
        }
    }
    if (!info) {
        candidate.is_null = true;
        return;
    }
    candidate.info = info;

    std::string animation_id = !info->start_animation.empty() ? info->start_animation : "default";
    auto anim_it = info->animations.find(animation_id);
    if (anim_it == info->animations.end() && !info->animations.empty()) {
        anim_it = info->animations.begin();
    }

    if (anim_it != info->animations.end()) {
        const Animation& animation = anim_it->second;
        for (const AnimationFrame* frame : animation.frames) {
            if (!frame || frame->variants.empty()) {
                continue;
            }
            BoundaryFrame boundary_frame;
            boundary_frame.duration_ms = kDefaultAnimationFrameMs;
            boundary_frame.variants.reserve(frame->variants.size());
            bool has_texture = false;
            for (const FrameVariant& variant : frame->variants) {
                if (!variant.base_texture) {
                    boundary_frame.variants.push_back(BoundaryFrameVariant{nullptr, 0, 0});
                    continue;
                }
                BoundaryFrameVariant frame_variant;
                frame_variant.texture = variant.base_texture;
                float wf = 0.0f;
                float hf = 0.0f;
                if (SDL_GetTextureSize(frame_variant.texture, &wf, &hf)) {
                    frame_variant.width = static_cast<int>(std::lround(wf));
                    frame_variant.height = static_cast<int>(std::lround(hf));
                }
                boundary_frame.variants.push_back(frame_variant);
                has_texture = true;
            }
            if (has_texture) {
                candidate.frames.push_back(boundary_frame);
            }
        }
    }

    if (candidate.frames.empty() && info->preview_texture) {
        BoundaryFrame boundary_frame;
        boundary_frame.duration_ms = kDefaultAnimationFrameMs;
        BoundaryFrameVariant frame_variant;
        frame_variant.texture = info->preview_texture;
        float wf = 0.0f;
        float hf = 0.0f;
        if (SDL_GetTextureSize(frame_variant.texture, &wf, &hf)) {
            frame_variant.width = static_cast<int>(std::lround(wf));
            frame_variant.height = static_cast<int>(std::lround(hf));
        }
        boundary_frame.variants.push_back(frame_variant);
        candidate.frames.push_back(boundary_frame);
    }

    if (candidate.frames.empty()) {
        candidate.is_null = true;
    }
}

void DynamicBoundarySystem::parse_boundary_config(const nlohmann::json& boundary_data) {
    if (!boundary_data.is_object()) {
        boundary_types_.clear();
        boundary_regen_seed_ = 0;
        config_dirty_ = false;
        clear_runtime_caches();
        return;
    }
    try {
        boundary_regen_seed_ = static_cast<std::uint64_t>(
            std::max<long long>(0, boundary_data.value("regen_seed", 0LL)));
    } catch (...) {
        boundary_regen_seed_ = 0;
    }
    const auto selectors_it = boundary_data.find("candidate_selectors");
    if (selectors_it == boundary_data.end() || !selectors_it->is_array()) {
        boundary_types_.clear();
        boundary_regen_seed_ = 0;
        config_dirty_ = false;
        clear_runtime_caches();
        return;
    }

    std::vector<BoundaryType> parsed_types;
    parsed_types.reserve(selectors_it->size());

    for (const auto& selector : *selectors_it) {
        if (!selector.is_object()) {
            continue;
        }
        BoundaryType type;
        type.spawn_id = selector.value("spawn_id", std::string{});
        type.display_name = selector.value("display_name", std::string{});
        type.grid_resolution = selector.value("grid_resolution", 5);
        type.grid_resolution = std::clamp(type.grid_resolution, 0, vibble::grid::kMaxResolution);
        int jitter_px = 0;
        try {
            auto jitter_it = selector.find("jitter");
            if (jitter_it != selector.end()) {
                if (jitter_it->is_number_integer()) {
                    jitter_px = jitter_it->get<int>();
                } else if (jitter_it->is_number_float()) {
                    jitter_px = static_cast<int>(std::lround(jitter_it->get<double>()));
                }
            }
        } catch (...) {
            jitter_px = 0;
        }
        jitter_px = static_cast<int>(render_overlay::clamp_random_jitter(static_cast<float>(jitter_px)));
        type.jitter = jitter_px;

        int total_chance = 0;
        const auto candidates_it = selector.find("candidates");
        if (candidates_it == selector.end() || !candidates_it->is_array()) {
            continue;
        }
        for (const auto& candidate_json : *candidates_it) {
            if (!candidate_json.is_object()) {
                continue;
            }
            BoundaryCandidate candidate;
            candidate.asset_name = candidate_json.value("name",
                                                       candidate_json.value("asset_name", std::string{}));
            candidate.chance = candidate_json.value("chance", 0);
            if (candidate.chance <= 0) {
                continue;
            }
            build_candidate_frames(candidate);
            if (!candidate.is_null && candidate.frames.empty()) {
                continue;
            }
            total_chance += candidate.chance;
            type.candidates.push_back(std::move(candidate));
        }
        type.total_chance = total_chance;
        if (!type.candidates.empty() && type.total_chance > 0) {
            parsed_types.push_back(std::move(type));
        }
    }

    boundary_types_ = std::move(parsed_types);
    config_revision_++;
    config_dirty_ = false;
    clear_runtime_caches();
}

bool DynamicBoundarySystem::refresh_boundary_config_revision(const nlohmann::json& map_info) {
    std::function<std::uint64_t(const nlohmann::json&)> hash_json = [&](const nlohmann::json& value) -> std::uint64_t {
        std::uint64_t hash = static_cast<std::uint64_t>(value.type());
        if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                hash = mix_uint64(hash, std::hash<std::string>{}(it.key()));
                hash = mix_uint64(hash, hash_json(it.value()));
            }
            return hash;
        }
        if (value.is_array()) {
            for (const auto& item : value) {
                hash = mix_uint64(hash, hash_json(item));
            }
            return hash;
        }
        if (value.is_string()) {
            return mix_uint64(hash, std::hash<std::string>{}(value.get_ref<const std::string&>()));
        }
        if (value.is_boolean()) {
            return mix_uint64(hash, value.get<bool>() ? 1ULL : 0ULL);
        }
        if (value.is_number_integer() || value.is_number_unsigned()) {
            return mix_uint64(hash, static_cast<std::uint64_t>(value.get<long long>()));
        }
        if (value.is_number_float()) {
            return mix_uint64(hash, std::hash<double>{}(value.get<double>()));
        }
        return hash;
    };

    std::size_t next_signature = 0;
    auto boundary_it = map_info.find("map_boundary_data");
    if (boundary_it != map_info.end() && boundary_it->is_object()) {
        next_signature = static_cast<std::size_t>(hash_json(*boundary_it));
    }

    if (next_signature == map_boundary_signature_) {
        return false;
    }
    map_boundary_signature_ = next_signature;
    ++map_boundary_revision_;
    return true;
}

void DynamicBoundarySystem::clear_runtime_caches() {
    boundary_assignments_.clear();
    animation_states_.clear();
    active_boundary_sprites_.clear();
    region_cache_.clear();
    region_area_index_.clear();
    static_assignments_.clear();
    static_assignment_fingerprint_ = {};
    region_cache_fingerprint_ = {};
}

void DynamicBoundarySystem::invalidate_config() {
    config_dirty_ = true;
    boundary_regen_seed_ = 0;
    clear_runtime_caches();
}

DynamicBoundarySystem::BoundaryConfig& DynamicBoundarySystem::config() {
    static BoundaryConfig cfg{};
    return cfg;
}

void DynamicBoundarySystem::set_grid_spacing_multiplier(float multiplier) {
    config().grid_spacing_multiplier = render_overlay::clamp_spacing_multiplier(multiplier);
}

float DynamicBoundarySystem::grid_spacing_multiplier() {
    return config().grid_spacing_multiplier;
}

void DynamicBoundarySystem::set_base_size_scale(float scale) {
    config().base_size_scale = render_overlay::clamp_base_size_scale(scale);
}

float DynamicBoundarySystem::base_size_scale() {
    return config().base_size_scale;
}

void DynamicBoundarySystem::set_vertical_offset(float offset) {
    config().vertical_offset = render_overlay::clamp_vertical_offset(offset);
}

float DynamicBoundarySystem::vertical_offset() {
    return config().vertical_offset;
}

void DynamicBoundarySystem::set_max_random_jitter(float jitter) {
    config().max_random_jitter = render_overlay::clamp_random_jitter(jitter);
}

float DynamicBoundarySystem::max_random_jitter() {
    return config().max_random_jitter;
}

void DynamicBoundarySystem::ensure_region_cache_valid(const world::WorldGrid& grid,
                                                      const std::vector<Room*>& rooms,
                                                      std::size_t rooms_hash,
                                                      float spacing_multiplier) {
    RegionCacheFingerprint fingerprint{};
    fingerprint.config_revision = config_revision_;
    fingerprint.map_info_hash = map_boundary_revision_;
    fingerprint.rooms_hash = rooms_hash;
    const world::GridPoint origin = grid.origin();
    fingerprint.origin_x = origin.world_x();
    fingerprint.origin_z = origin.world_z();
    fingerprint.origin_layer = origin.resolution_layer();
    fingerprint.grid_resolution = grid.grid_resolution();
    fingerprint.spacing_multiplier = spacing_multiplier;
    if (fingerprint != region_cache_fingerprint_) {
        region_cache_.clear();
        rebuild_region_area_index(rooms);
        region_cache_fingerprint_ = fingerprint;
    }
}

const DynamicBoundarySystem::RegionCacheEntry& DynamicBoundarySystem::resolve_region_cache(
    const SDL_Point& world_pt,
    const std::vector<Room*>& rooms) {
    if (region_area_index_.empty() && !rooms.empty()) {
        rebuild_region_area_index(rooms);
    }

    RegionCacheKey key{world_pt.x, world_pt.y};
    auto [it, inserted] = region_cache_.try_emplace(key);
    if (inserted) {
        it->second = classify_region_point(world_pt);
    }
    return it->second;
}

void DynamicBoundarySystem::add_indexed_area(const Area* area,
                                             RegionKind kind,
                                             const Room* owner) {
    if (!area || !owner) {
        return;
    }

    int minx = 0;
    int miny = 0;
    int maxx = 0;
    int maxy = 0;
    try {
        std::tie(minx, miny, maxx, maxy) = area->get_bounds();
    } catch (...) {
        return;
    }

    const int start_x = static_cast<int>(std::floor(static_cast<double>(minx) / kRegionIndexCellSize));
    const int end_x = static_cast<int>(std::floor(static_cast<double>(maxx) / kRegionIndexCellSize));
    const int start_y = static_cast<int>(std::floor(static_cast<double>(miny) / kRegionIndexCellSize));
    const int end_y = static_cast<int>(std::floor(static_cast<double>(maxy) / kRegionIndexCellSize));

    IndexedArea indexed_area{area, kind, owner};
    for (int gx = start_x; gx <= end_x; ++gx) {
        for (int gy = start_y; gy <= end_y; ++gy) {
            region_area_index_[RegionCacheKey{gx, gy}].push_back(indexed_area);
        }
    }
}

void DynamicBoundarySystem::rebuild_region_area_index(const std::vector<Room*>& rooms) {
    region_area_index_.clear();
    for (const Room* room : rooms) {
        if (!room) {
            continue;
        }
        add_indexed_area(room->room_area.get(), RegionKind::Room, room);
        for (const auto& named : room->areas) {
            if (!named.area) {
                continue;
            }
            if (!(is_trail_string(named.type) || is_trail_string(named.kind) || is_trail_string(named.name))) {
                continue;
            }
            add_indexed_area(named.area.get(), RegionKind::Trail, room);
        }
    }
}

DynamicBoundarySystem::RegionCacheEntry DynamicBoundarySystem::classify_region_point(
    const SDL_Point& world_pt) const {
    RegionCacheEntry entry;
    entry.region_kind = RegionKind::Boundary;
    entry.owner = nullptr;
    entry.blocked = false;

    const int cell_x = static_cast<int>(std::floor(static_cast<double>(world_pt.x) / kRegionIndexCellSize));
    const int cell_y = static_cast<int>(std::floor(static_cast<double>(world_pt.y) / kRegionIndexCellSize));

    auto it = region_area_index_.find(RegionCacheKey{cell_x, cell_y});
    if (it == region_area_index_.end()) {
        return entry;
    }

    for (const IndexedArea& indexed : it->second) {
        if (!indexed.area || !indexed.owner) {
            continue;
        }
        try {
            if (indexed.area->contains_point(world_pt)) {
                entry.region_kind = indexed.region_kind;
                entry.owner = indexed.owner;
                entry.blocked = true;
                return entry;
            }
        } catch (...) {
        }
    }
    return entry;
}


std::size_t DynamicBoundarySystem::compute_rooms_topology_hash(const Assets* assets) const {
    std::uint64_t hash = 0;
    if (!assets) {
        return static_cast<std::size_t>(hash);
    }
    hash = mix_uint64(hash, static_cast<std::uint64_t>(assets->rooms_generation()));
    const std::hash<std::string> string_hasher{};
    const auto& rooms = assets->rooms();
    for (const Room* room : rooms) {
        if (!room) {
            hash = mix_uint64(hash, 0x9e3779b97f4a7c15ULL);
            continue;
        }
        hash = mix_uint64(hash, static_cast<std::uint64_t>(room->map_origin.first));
        hash = mix_uint64(hash, static_cast<std::uint64_t>(room->map_origin.second));
        hash = mix_uint64(hash, static_cast<std::uint64_t>(room->layer));
        hash = mix_uint64(hash, string_hasher(room->room_name));
        hash = mix_uint64(hash, string_hasher(room->type));
        if (room->room_area) {
            try {
                auto [minx, miny, maxx, maxy] = room->room_area->get_bounds();
                hash = mix_uint64(hash, static_cast<std::uint64_t>(minx));
                hash = mix_uint64(hash, static_cast<std::uint64_t>(miny));
                hash = mix_uint64(hash, static_cast<std::uint64_t>(maxx));
                hash = mix_uint64(hash, static_cast<std::uint64_t>(maxy));
            } catch (...) {
            }
        }
        for (const auto& named : room->areas) {
            hash = mix_uint64(hash, string_hasher(named.name));
            hash = mix_uint64(hash, string_hasher(named.type));
            hash = mix_uint64(hash, string_hasher(named.kind));
            if (named.area) {
                try {
                    auto [minx, miny, maxx, maxy] = named.area->get_bounds();
                    hash = mix_uint64(hash, static_cast<std::uint64_t>(minx));
                    hash = mix_uint64(hash, static_cast<std::uint64_t>(miny));
                    hash = mix_uint64(hash, static_cast<std::uint64_t>(maxx));
                    hash = mix_uint64(hash, static_cast<std::uint64_t>(maxy));
                } catch (...) {
                }
            }
        }
    }
    return static_cast<std::size_t>(hash);
}
