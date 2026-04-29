#include "rendering/render/dynamic_boundary_system.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/grid_overlay.hpp"
#include "gameplay/world/world_grid.hpp"
#include "assets/asset/asset_library.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_types.hpp"
#include "assets/asset/controller_factory.hpp"
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
#include <random>
#include <tuple>
#include <unordered_set>

namespace {
constexpr float kDefaultAnimationFrameMs = 1000.0f / 24.0f;
constexpr long long kMaxBoundaryCells = 250000;
constexpr float kDepthEfficiencyVisibilityHysteresisWidth = 0.05f;

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

inline float clamp_unit_interval(float value, float fallback) {
    if (!std::isfinite(value)) {
        return std::clamp(fallback, 0.0f, 1.0f);
    }
    return std::clamp(value, 0.0f, 1.0f);
}

float sample_uniform_unit_from_hash(std::uint64_t key_hash, std::uint64_t salt) {
    const std::uint64_t mixed = render_overlay::mix_uint64(key_hash, salt);
    const std::uint32_t bucket = static_cast<std::uint32_t>(mixed & 0xFFFFFFFFull);
    return static_cast<float>(bucket) / static_cast<float>(std::numeric_limits<std::uint32_t>::max());
}

float sample_range_from_hash(std::uint64_t key_hash, std::uint64_t salt, float min_value, float max_value) {
    if (!std::isfinite(min_value)) min_value = 0.0f;
    if (!std::isfinite(max_value)) max_value = 0.0f;
    if (max_value < min_value) {
        std::swap(min_value, max_value);
    }
    if (std::fabs(max_value - min_value) <= 1e-6f) {
        return min_value;
    }
    const float t = sample_uniform_unit_from_hash(key_hash, salt);
    return min_value + (max_value - min_value) * t;
}

inline SDL_Point rounded_world_point(const SDL_FPoint& point) {
    return SDL_Point{
        static_cast<int>(std::lround(point.x)),
        static_cast<int>(std::lround(point.y))
    };
}

std::unordered_map<std::string, std::shared_ptr<AssetInfo>> build_non_boundary_asset_catalog(
    const std::unordered_map<std::string, std::shared_ptr<AssetInfo>>& source_catalog) {
    std::unordered_map<std::string, std::shared_ptr<AssetInfo>> filtered;
    filtered.reserve(source_catalog.size());
    for (const auto& [name, info] : source_catalog) {
        if (!info) {
            continue;
        }
        if (info->type == asset_types::boundary) {
            continue;
        }
        filtered.emplace(name, info);
    }
    return filtered;
}

bool room_trail_tag_has_match(
    const std::string& tag,
    const std::unordered_map<std::string, std::shared_ptr<AssetInfo>>& room_trail_catalog) {
    if (tag.empty()) {
        return false;
    }
    for (const auto& [name, info] : room_trail_catalog) {
        (void)name;
        if (!info) {
            continue;
        }
        const auto& tags = info->tag_lookup();
        if (tags.find(tag) != tags.end()) {
            return true;
        }
    }
    return false;
}

std::unordered_set<int> collect_room_trail_excluded_candidate_indices(
    const DynamicBoundarySystem::BoundaryType& boundary_type,
    const vibble::spawn::RuntimeCandidates::AssetCatalogView& full_catalog,
    const std::unordered_map<std::string, std::shared_ptr<AssetInfo>>& room_trail_catalog) {
    std::unordered_set<int> excluded;
    const auto& entries = boundary_type.candidates.entries();
    for (std::size_t idx = 0; idx < entries.size(); ++idx) {
        const auto& entry = entries[idx];
        if (entry.is_null || entry.key.empty()) {
            continue;
        }
        if (entry.kind == vibble::spawn::CandidateKind::Asset) {
            std::string resolved_name;
            std::shared_ptr<AssetInfo> info = full_catalog.find_info(entry.key, &resolved_name);
            (void)resolved_name;
            if (!info || info->type == asset_types::boundary) {
                excluded.insert(static_cast<int>(idx));
            }
            continue;
        }
        if (entry.kind == vibble::spawn::CandidateKind::Tag &&
            !room_trail_tag_has_match(entry.key, room_trail_catalog)) {
            excluded.insert(static_cast<int>(idx));
        }
    }
    return excluded;
}

struct BoundaryScaleResult {
    float remainder_scale = 1.0f;
    int variant_index = 0;
};

BoundaryScaleResult compute_boundary_asset_scale(DynamicBoundarySystem::BoundaryAssetRuntime& candidate,
                                                 const WarpedScreenGrid& cam,
                                                 const Assets* assets,
                                                 const SDL_FPoint& world_pos,
                                                 int world_z,
                                                 float size_variation_sample) {
    (void)assets;
    if (!candidate.info) {
        return BoundaryScaleResult{};
    }

    const SDL_Point world_pt = rounded_world_point(world_pos);
    const WarpedScreenGrid::RenderEffects effects =
        cam.compute_render_effects(world_pt, 0.0f, 0.0f, WarpedScreenGrid::RenderSmoothingKey{}, world_z);

    const float perspective_scale = make_positive_scale(effects.distance_scale);
    const float base_scale = DynamicBoundarySystem::compute_effective_base_scale(
        *candidate.info,
        size_variation_sample);
    const float current_scale = base_scale * perspective_scale;

// התאמה לנכסי זמן ריצה: בחר את הווריאנט הגדול הקרוב ביותר לקנה המידה הזה
// ובצע הגדלה רק אם כל הווריאנטים קטנים מהיעד.
    float desired_variant_scale = current_scale;
    if (!std::isfinite(desired_variant_scale) || desired_variant_scale <= 0.0f) {
        desired_variant_scale = 1.0f;
    }

    const auto& steps = render_pipeline::ScalingLogic::DefaultScaleSteps();

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
        vibble::log::warn("[DynamicBoundarySystem] ה-Renderer או AssetLibrary הם null; אי אפשר לאתחל");
        return false;
    }
    renderer_ = renderer;
    asset_library_ = asset_library;
    initialized_ = true;
    config_dirty_ = true;
    boundary_types_.clear();
    room_trail_catalog_storage_.clear();
    room_trail_catalog_signature_ = std::numeric_limits<std::size_t>::max();
    cached_rooms_generation_ = std::numeric_limits<std::size_t>::max();
    cached_rooms_topology_hash_ = 0;
    clear_runtime_caches();
    return true;
}

void DynamicBoundarySystem::update(const WarpedScreenGrid& cam,
                                   world::WorldGrid& grid,
                                   Assets* assets,
                                   float delta_ms) {
    active_boundary_sprites_.clear();
    if (!initialized_ || !renderer_ || !asset_library_ || !assets) {
        return;
    }
    if (delta_ms < 0.0f) {
        delta_ms = 0.0f;
    }

    auto promoted_asset_is_valid = [&](const Asset* promoted) -> bool {
        return promoted && !promoted->dead && assets->contains_asset(promoted);
    };
    auto erase_promoted_slot = [&](auto it, bool delete_asset) {
        Asset* promoted = it->second.asset;
        if (delete_asset && promoted_asset_is_valid(promoted)) {
            promoted->Delete();
        }
        return promoted_boundary_assets_.erase(it);
    };
    for (auto it = promoted_boundary_assets_.begin(); it != promoted_boundary_assets_.end();) {
        if (!promoted_asset_is_valid(it->second.asset)) {
            it = erase_promoted_slot(it, false);
            continue;
        }
        ++it;
    }
    std::unordered_set<BoundaryKey, BoundaryKeyHash> active_origin_slots;

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
        for (auto it = promoted_boundary_assets_.begin(); it != promoted_boundary_assets_.end();) {
            it = erase_promoted_slot(it, true);
        }
        depth_visibility_states_.clear();
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
    const auto& asset_catalog = asset_library_->all();
    const vibble::spawn::RuntimeCandidates::AssetCatalogView boundary_catalog{&asset_catalog, true};
    bool room_trail_catalog_rebuilt = false;
    const std::size_t asset_catalog_signature = asset_catalog.size();
    if (room_trail_catalog_storage_.empty() || room_trail_catalog_signature_ != asset_catalog_signature) {
        room_trail_catalog_storage_ = build_non_boundary_asset_catalog(asset_catalog);
        room_trail_catalog_signature_ = asset_catalog_signature;
        room_trail_catalog_rebuilt = true;
    }
    if (room_trail_catalog_rebuilt) {
        static_assignment_fingerprint_ = {};
        for (auto& btype : boundary_types_) {
            btype.room_trail_exclusions_ready = false;
        }
    }
    const vibble::spawn::RuntimeCandidates::AssetCatalogView room_trail_catalog{&room_trail_catalog_storage_, true};
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
            if (!btype.room_trail_exclusions_ready || room_trail_catalog_rebuilt) {
                btype.room_trail_excluded_candidate_indices =
                    collect_room_trail_excluded_candidate_indices(btype, boundary_catalog, room_trail_catalog_storage_);
                btype.room_trail_exclusions_ready = true;
            }
            const std::unordered_set<int>& room_trail_excluded_candidate_indices =
                btype.room_trail_excluded_candidate_indices;
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
                    vibble::log::warn(std::string{"[DynamicBoundarySystem] מדלג על סוג גבול צפוף '"} +
                                      btype.display_name + "' (grid_resolution=" +
                                      std::to_string(btype.grid_resolution) +
                                      ") כדי להימנע ממספר תאים מוגזם.");
                }
                continue;
            }

            for (int ix = start_idx_x; ix <= end_idx_x; ++ix) {
                const int world_x = grid_origin.world_x() + ix * grid_spacing;
                for (int iy = start_idx_y; iy <= end_idx_y; ++iy) {
                    const int world_z = grid_origin.world_z() + iy * grid_spacing;
                    const SDL_Point world_pt{world_x, world_z};
                    const auto& region_entry = resolve_region_cache(world_pt, rooms);
                    if (region_entry.blocked && region_entry.region_kind == RegionKind::Boundary) {
                        continue;
                    }

                    const bool boundary_region = (region_entry.region_kind == RegionKind::Boundary);
                    const bool room_or_trail_region =
                        (region_entry.region_kind == RegionKind::Room || region_entry.region_kind == RegionKind::Trail);
                    if (!boundary_region && !room_or_trail_region) {
                        continue;
                    }
                    if (room_or_trail_region && region_entry.owner &&
                        !region_entry.owner->inherits_map_assets()) {
                        continue;
                    }

                    const int region_domain = boundary_region ? 0 : 1;
                    const BoundaryKey key =
                        make_key(static_cast<int>(type_idx), region_domain, resolution_layer, ix, iy, world_z);
                    const SDL_FPoint jitter = sample_jitter_offset(key, max_random_jitter);
                    const float jittered_z = static_cast<float>(world_z) + jitter.y;
                    SDL_FPoint world_pos{
                        static_cast<float>(world_x) + jitter.x,
                        0.0f
                    };

                    const auto& active_catalog = boundary_region ? boundary_catalog : room_trail_catalog;
                    const std::unordered_set<int>* exclusions_ptr =
                        boundary_region ? nullptr : &room_trail_excluded_candidate_indices;

                    const BoundaryAssignment& assignment =
                        select_candidate_for_key(key, btype, active_catalog, exclusions_ptr);
                    if (assignment.is_null || assignment.resolved_asset_name.empty()) {
                        continue;
                    }
                    BoundaryAssetRuntime* candidate_runtime =
                        ensure_candidate_runtime(btype, assignment.resolved_asset_name);
                    if (!candidate_runtime || candidate_runtime->is_null || candidate_runtime->frames.empty()) {
                        continue;
                    }

                    static_assignments_.push_back(StaticCellAssignment{
                        key,
                        assignment,
                        static_cast<int>(type_idx),
                        world_pos,
                        static_cast<int>(std::lround(jittered_z))
                    });
                }
            }
        }
        static_assignment_fingerprint_ = next_static;
    }

    const WarpedScreenGrid::RealismSettings realism_settings = cam.get_settings();
    const world::CameraProjectionParams projection = cam.projection_params();
    const float depth_axis_sign =
        render_depth::normalize_depth_axis_sign(static_cast<float>(projection.forward_z));
    const double max_cull_depth = std::max(1.0, static_cast<double>(realism_settings.max_cull_depth));
    const double depth_efficiency_depth = std::clamp(
        static_cast<double>(std::isfinite(realism_settings.dynamic_renderer_depth_efficiency_depth)
                                ? realism_settings.dynamic_renderer_depth_efficiency_depth
                                : 2000.0f),
        0.0,
        max_cull_depth);
    const float depth_efficiency_min_density_ratio =
        clamp_unit_interval(realism_settings.dynamic_renderer_depth_efficiency_min_density_ratio, 0.10f);
    ++depth_visibility_epoch_;
    if (depth_visibility_epoch_ == 0) {
        depth_visibility_epoch_ = 1;
        for (auto& [key, state] : depth_visibility_states_) {
            (void)key;
            state.last_seen_epoch = 0;
        }
    }

    for (const StaticCellAssignment& assignment : static_assignments_) {
        if (assignment.boundary_type_index < 0 || assignment.boundary_type_index >= static_cast<int>(boundary_types_.size())) {
            continue;
        }
        BoundaryType& btype = boundary_types_[assignment.boundary_type_index];
        if (assignment.assignment.is_null || assignment.assignment.resolved_asset_name.empty()) {
            continue;
        }

        const double depth_from_anchor =
            render_depth::depth_from_anchor(cam.anchor_world_z(), static_cast<double>(assignment.world_z));
        const double forward_depth_offset = compute_forward_depth_offset(depth_from_anchor, depth_axis_sign);
        if (!std::isfinite(forward_depth_offset) || forward_depth_offset > max_cull_depth) {
            continue;
        }
        const float depth_keep_ratio = compute_depth_efficiency_keep_ratio(forward_depth_offset,
                                                                            max_cull_depth,
                                                                            depth_efficiency_depth,
                                                                            depth_efficiency_min_density_ratio);
        const std::uint64_t key_hash = hash_key(assignment.key);
        const float deterministic_sample = depth_efficiency_sample_from_hash(key_hash);
        auto& visibility_state = depth_visibility_states_[assignment.key];
        visibility_state.last_seen_epoch = depth_visibility_epoch_;
        const bool visible_after_efficiency = evaluate_depth_efficiency_visibility(
            deterministic_sample,
            depth_keep_ratio,
            visibility_state.visible,
            kDepthEfficiencyVisibilityHysteresisWidth);
        visibility_state.visible = visible_after_efficiency;
        if (!visible_after_efficiency) {
            continue;
        }

        BoundaryAssetRuntime* candidate =
            ensure_candidate_runtime(btype, assignment.assignment.resolved_asset_name);
        if (!candidate || candidate->is_null || candidate->frames.empty()) {
            continue;
        }
        const SDL_Point assignment_world_point{
            static_cast<int>(std::lround(assignment.world_pos.x)),
            assignment.world_z
        };
        const auto& assignment_region = resolve_region_cache(assignment_world_point, rooms);
        const std::string assignment_owner_room_name =
            (assignment_region.owner ? assignment_region.owner->room_name : std::string{});
        const bool has_registered_controller =
            candidate->info &&
            ControllerFactory::has_registered_controller_for_asset_name(candidate->info->name);
        if (should_attempt_room_trail_promotion(assignment.key.region_domain, has_registered_controller)) {
            active_origin_slots.insert(assignment.key);
            auto promoted_it = promoted_boundary_assets_.find(assignment.key);
            if (promoted_it != promoted_boundary_assets_.end()) {
                Asset* promoted = promoted_it->second.asset;
                if (promoted_asset_is_valid(promoted)) {
                    continue;
                }
                promoted_it = erase_promoted_slot(promoted_it, false);
                (void)promoted_it;
            }
            const bool allow_promotion = should_promote_controller_candidate(
                true,
                visible_after_efficiency,
                forward_depth_offset,
                depth_efficiency_depth);
            if (allow_promotion) {
                const std::string promoted_spawn_id = btype.spawn_id.empty()
                    ? std::string{}
                    : (btype.spawn_id + "::promoted::" +
                       std::to_string(assignment.key.grid_x) + ":" +
                       std::to_string(assignment.key.grid_y) + ":" +
                       std::to_string(assignment.key.region_domain));
                if (!promoted_spawn_id.empty()) {
                    if (Asset* existing = assets->find_asset_by_stable_id(promoted_spawn_id)) {
                        if (promoted_asset_is_valid(existing)) {
                            if (!assignment_owner_room_name.empty()) {
                                existing->set_owning_room_name(assignment_owner_room_name);
                            }
                            promoted_boundary_assets_[assignment.key] = PromotedBoundaryEntry{
                                assignment.key,
                                existing,
                                assignment_owner_room_name,
                                assignment.key.region_domain,
                                true};
                            continue;
                        }
                    }
                }
                const SDL_Point spawn_pos = assignment_world_point;
                Asset* promoted = assets->spawn_asset(assignment.assignment.resolved_asset_name, spawn_pos);
                if (promoted) {
                    promoted->spawn_method = "DynamicBoundaryPromotedPersistent";
                    if (!promoted_spawn_id.empty()) {
                        promoted->spawn_id = promoted_spawn_id;
                    }
                    if (!assignment_owner_room_name.empty()) {
                        promoted->set_owning_room_name(assignment_owner_room_name);
                    }
                    promoted_boundary_assets_[assignment.key] = PromotedBoundaryEntry{
                        assignment.key,
                        promoted,
                        assignment_owner_room_name,
                        assignment.key.region_domain,
                        true};
                    continue;
                }
            }
            continue;
        }

        SDL_FPoint screen_pos{};
        if (!cam.project_world_point(assignment.world_pos, static_cast<float>(assignment.world_z), screen_pos)) {
            continue;
        }
        if (!std::isfinite(screen_pos.x) || !std::isfinite(screen_pos.y)) {
            continue;
        }

        auto& frame_state = animation_states_[assignment.key];
        const int total_frames = static_cast<int>(candidate->frames.size());
        if (total_frames <= 0) {
            continue;
        }
        const bool freeze_animation = forward_depth_offset > depth_efficiency_depth;
        advance_frame_state(frame_state, candidate->frames, delta_ms, freeze_animation);

        int current_index = frame_state.frame_index % total_frames;
        if (current_index < 0) {
            current_index += total_frames;
        }
        float frame_duration = candidate->frames[current_index].duration_ms;
        if (!(frame_duration > 0.0f)) {
            frame_duration = kDefaultAnimationFrameMs;
        }
        const BoundaryFrame& active_frame = candidate->frames[current_index];
        const BoundaryScaleResult scale_result =
            compute_boundary_asset_scale(*candidate,
                                         cam,
                                         assets,
                                         assignment.world_pos,
                                         assignment.world_z,
                                         assignment.assignment.size_variation_sample);
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
        sprite.asset_name = assignment.assignment.resolved_asset_name;
        sprite.world_pos = assignment.world_pos;
        sprite.screen_pos = screen_pos;
        sprite.world_z = assignment.world_z;
        sprite.spawn_tilt_degrees = assignment.assignment.spawn_tilt_degrees;
        sprite.spawn_y_offset_px = assignment.assignment.spawn_y_offset_px;
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
        sprite.candidate_index = assignment.assignment.candidate_entry_index;
        sprite.current_frame_index = current_index;
        sprite.total_frames = total_frames;
        sprite.frame_duration_ms = frame_duration;
        sprite.frame_elapsed_ms = frame_state.elapsed_ms;

        active_boundary_sprites_.push_back(sprite);
    }

    if (active_boundary_sprites_.size() > 1) {
        const double anchor_depth = cam.anchor_world_z();
        std::sort(active_boundary_sprites_.begin(), active_boundary_sprites_.end(),
            [anchor_depth](const BoundarySprite& a, const BoundarySprite& b) {
                const double da = render_depth::depth_from_anchor(anchor_depth, static_cast<double>(a.world_z));
                const double db = render_depth::depth_from_anchor(anchor_depth, static_cast<double>(b.world_z));
                if (da != db) return da > db;
                return a.world_pos.x < b.world_pos.x;
            });
    }

    const std::unordered_set<BoundaryKey, BoundaryKeyHash> scoped_origin_slots = active_origin_slots;
    for (auto it = promoted_boundary_assets_.begin(); it != promoted_boundary_assets_.end();) {
        const bool origin_slot_in_scope = (scoped_origin_slots.find(it->first) != scoped_origin_slots.end());
        const bool promoted_valid = promoted_asset_is_valid(it->second.asset);
        if (should_release_origin_slot_reservation(origin_slot_in_scope, promoted_valid)) {
            it = erase_promoted_slot(it, false);
            continue;
        }
        ++it;
    }
    for (auto it = depth_visibility_states_.begin(); it != depth_visibility_states_.end();) {
        if (it->second.last_seen_epoch != depth_visibility_epoch_) {
            it = depth_visibility_states_.erase(it);
            continue;
        }
        ++it;
    }

}

std::size_t DynamicBoundarySystem::BoundaryKeyHash::operator()(const DynamicBoundarySystem::BoundaryKey& key) const noexcept {
    return static_cast<std::size_t>(
        render_overlay::hash_grid_cell(key.grid_x,
                                       key.grid_y,
                                       key.world_z,
                                       key.resolution_layer,
                                       key.group,
                                       static_cast<std::uint64_t>(key.region_domain)));
}

DynamicBoundarySystem::BoundaryKey DynamicBoundarySystem::make_key(int group_idx,
                                                                   int region_domain,
                                                                   int resolution_layer,
                                                                   int grid_x,
                                                                   int grid_y,
                                                                   int world_z) const {
    return DynamicBoundarySystem::BoundaryKey{
        group_idx,
        region_domain,
        resolution_layer,
        grid_x,
        grid_y,
        world_z
    };
}

std::uint64_t DynamicBoundarySystem::hash_key(const BoundaryKey& key) const {
    const std::uint64_t base_hash = render_overlay::hash_grid_cell(key.grid_x,
                                                                    key.grid_y,
                                                                    key.world_z,
                                                                    key.resolution_layer,
                                                                    key.group,
                                                                    boundary_regen_seed_);
    return render_overlay::mix_uint64(base_hash, static_cast<std::uint64_t>(key.region_domain));
}

const DynamicBoundarySystem::BoundaryAssignment& DynamicBoundarySystem::select_candidate_for_key(
    const BoundaryKey& key,
    BoundaryType& btype,
    const vibble::spawn::RuntimeCandidates::AssetCatalogView& catalog,
    const std::unordered_set<int>* excluded_entry_indices) {
    static const BoundaryAssignment kNullAssignment{};
    auto it = boundary_assignments_.find(key);
    if (it != boundary_assignments_.end()) {
        return it->second;
    }

    BoundaryAssignment assignment{};
    const std::uint64_t key_hash = hash_key(key);
    const std::uint32_t seed_lo = static_cast<std::uint32_t>(key_hash & 0xFFFFFFFFULL);
    const std::uint32_t seed_hi = static_cast<std::uint32_t>((key_hash >> 32) & 0xFFFFFFFFULL);
    std::seed_seq seq{
        seed_lo,
        seed_hi,
        static_cast<std::uint32_t>(key.group),
        static_cast<std::uint32_t>(key.region_domain),
        static_cast<std::uint32_t>(key.resolution_layer),
        static_cast<std::uint32_t>(key.grid_x),
        static_cast<std::uint32_t>(key.grid_y),
        static_cast<std::uint32_t>(key.world_z)
    };
    std::mt19937 key_rng(seq);
    const auto resolved = (excluded_entry_indices && !excluded_entry_indices->empty())
        ? btype.candidates.pick_random_excluding(
              key_rng, catalog, *excluded_entry_indices, vibble::spawn::ZeroWeightPolicy::NoSelection)
        : btype.candidates.pick_random(
              key_rng, catalog, vibble::spawn::ZeroWeightPolicy::NoSelection);
    if (resolved && !resolved->is_null && !resolved->resolved_asset_name.empty() && resolved->info) {
        assignment.candidate_entry_index = resolved->entry_index;
        assignment.resolved_asset_name = resolved->resolved_asset_name;
        assignment.is_null = false;
        assignment.size_variation_sample = sample_size_variation_from_hash(key_hash);
        int tilt_min = std::clamp(resolved->info->tilt_range_min_deg, -180, 180);
        int tilt_max = std::clamp(resolved->info->tilt_range_max_deg, -180, 180);
        if (tilt_max < tilt_min) {
            std::swap(tilt_min, tilt_max);
        }
        assignment.spawn_tilt_degrees = sample_range_from_hash(
            key_hash,
            0x9D8E4F6A21C3B5D7ull,
            static_cast<float>(tilt_min),
            static_cast<float>(tilt_max));

        int y_min = std::clamp(resolved->info->y_pos_min, -50, 200);
        int y_max = std::clamp(resolved->info->y_pos_max, -50, 200);
        if (y_max < y_min) {
            std::swap(y_min, y_max);
        }
        assignment.spawn_y_offset_px = sample_range_from_hash(
            key_hash,
            0x6A31BF4CE9D20587ull,
            static_cast<float>(y_min),
            static_cast<float>(y_max));
    }

    auto [inserted_it, inserted] = boundary_assignments_.emplace(key, std::move(assignment));
    if (!inserted) {
        return inserted_it->second;
    }
    if (inserted_it == boundary_assignments_.end()) {
        return kNullAssignment;
    }
    return inserted_it->second;
}

SDL_FPoint DynamicBoundarySystem::sample_jitter_offset(const BoundaryKey& key, float max_jitter) const {
    return render_overlay::jitter_from_hash(hash_key(key), max_jitter);
}

void DynamicBoundarySystem::build_candidate_frames(BoundaryAssetRuntime& candidate_runtime,
                                                   const std::string& asset_name) {
    candidate_runtime.frames.clear();
    candidate_runtime.info.reset();
    candidate_runtime.hysteresis_state = {};
    candidate_runtime.is_null = false;
    if (asset_name.empty() || asset_name == "null") {
        candidate_runtime.is_null = true;
        return;
    }
    if (!asset_library_) {
        candidate_runtime.is_null = true;
        return;
    }

    auto info = asset_library_->get(asset_name);
    if (!info) {
        auto normalize = [](std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        };
        const std::string wanted = normalize(asset_name);
        for (const auto& [asset_key, asset_info] : asset_library_->all()) {
            if (!asset_info) {
                continue;
            }
            const std::string key_name = normalize(asset_key);
            const std::string display_name = normalize(asset_info->name);
            if (wanted == key_name || (!display_name.empty() && wanted == display_name)) {
                info = asset_info;
                break;
            }
        }
    }
    if (!info) {
        candidate_runtime.is_null = true;
        return;
    }
    candidate_runtime.info = info;

    std::string animation_id = !info->start_animation.empty() ? info->start_animation : "default";
    auto anim_it = info->animations.find(animation_id);
    if (anim_it == info->animations.end() && !info->animations.empty()) {
        anim_it = info->animations.begin();
    }

    if (anim_it != info->animations.end()) {
        const Animation& animation = anim_it->second;
        for (const AnimationFrame& frame : animation.primary_frames()) {
            if (frame.variants.empty()) {
                continue;
            }
            BoundaryFrame boundary_frame;
            boundary_frame.duration_ms = kDefaultAnimationFrameMs;
            boundary_frame.variants.reserve(frame.variants.size());
            bool has_texture = false;
            for (const FrameVariant& variant : frame.variants) {
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
                candidate_runtime.frames.push_back(boundary_frame);
            }
        }
    }

    if (candidate_runtime.frames.empty() && info->preview_texture) {
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
        candidate_runtime.frames.push_back(boundary_frame);
    }

    if (candidate_runtime.frames.empty()) {
        candidate_runtime.is_null = true;
    }
}

DynamicBoundarySystem::BoundaryAssetRuntime* DynamicBoundarySystem::ensure_candidate_runtime(
    BoundaryType& type,
    const std::string& asset_name) {
    if (asset_name.empty()) {
        return nullptr;
    }
    auto it = type.candidate_runtime_by_asset.find(asset_name);
    if (it != type.candidate_runtime_by_asset.end()) {
        return &it->second;
    }

    BoundaryAssetRuntime runtime{};
    build_candidate_frames(runtime, asset_name);
    auto [inserted_it, inserted] =
        type.candidate_runtime_by_asset.emplace(asset_name, std::move(runtime));
    if (!inserted) {
        return &inserted_it->second;
    }
    return &inserted_it->second;
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
        const auto candidates_it = selector.find("candidates");
        if (candidates_it == selector.end() || !candidates_it->is_array()) {
            continue;
        }
        type.candidates = vibble::spawn::RuntimeCandidates::from_json(*candidates_it);
        type.candidate_runtime_by_asset.clear();
        type.room_trail_excluded_candidate_indices.clear();
        type.room_trail_exclusions_ready = false;
        if (!type.candidates.empty()) {
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
    promoted_boundary_assets_.clear();
    boundary_assignments_.clear();
    animation_states_.clear();
    depth_visibility_states_.clear();
    depth_visibility_epoch_ = 0;
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

float DynamicBoundarySystem::sample_size_variation_from_hash(std::uint64_t key_hash) {
    const std::uint32_t seed_lo = static_cast<std::uint32_t>(key_hash & 0xFFFFFFFFULL);
    const std::uint32_t seed_hi = static_cast<std::uint32_t>((key_hash >> 32) & 0xFFFFFFFFULL);
    std::seed_seq seq{
        seed_lo,
        seed_hi,
        0x53495A45u, // "גודל"
        0x56415259u  // "שונות"
    };
    std::mt19937 rng(seq);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    return dist(rng);
}

float DynamicBoundarySystem::compute_effective_base_scale(const AssetInfo& info, float size_variation_sample) {
    const float authored_base =
        (std::isfinite(info.scale_factor) && info.scale_factor > 0.0f) ? info.scale_factor : 1.0f;
    if (info.tillable) {
        return authored_base;
    }

    float variation = info.size_variation_percent;
    if (!std::isfinite(variation)) {
        variation = 0.0f;
    }
    variation = std::clamp(variation, 0.0f, 20.0f);
    if (variation <= 0.0f) {
        return authored_base;
    }

    float sample = size_variation_sample;
    if (!std::isfinite(sample)) {
        sample = 0.0f;
    }
    sample = std::clamp(sample, -1.0f, 1.0f);

    const float multiplier = 1.0f + (sample * (variation / 100.0f));
    if (!std::isfinite(multiplier) || multiplier <= 0.0f) {
        return authored_base;
    }
    return authored_base * multiplier;
}

float DynamicBoundarySystem::compute_depth_efficiency_keep_ratio(double depth_distance,
                                                                 double max_cull_depth,
                                                                 double efficiency_depth,
                                                                 float min_density_ratio) {
    if (!std::isfinite(depth_distance) || !std::isfinite(max_cull_depth) || max_cull_depth <= 0.0) {
        return 0.0f;
    }

    const double clamped_efficiency_depth = std::clamp(efficiency_depth, 0.0, max_cull_depth);
    const float clamped_min_density_ratio = clamp_unit_interval(min_density_ratio, 0.10f);
    if (depth_distance <= 0.0) {
        return 1.0f;
    }

    if (depth_distance <= clamped_efficiency_depth) {
        return 1.0f;
    }
    if (depth_distance >= max_cull_depth) {
        return clamped_min_density_ratio;
    }

    const double span = max_cull_depth - clamped_efficiency_depth;
    if (span <= 1e-6) {
        return clamped_min_density_ratio;
    }
    const double t = std::clamp((depth_distance - clamped_efficiency_depth) / span, 0.0, 1.0);
    const double keep_ratio =
        1.0 + (static_cast<double>(clamped_min_density_ratio) - 1.0) * t;
    if (!std::isfinite(keep_ratio)) {
        return clamped_min_density_ratio;
    }
    return std::clamp(static_cast<float>(keep_ratio), clamped_min_density_ratio, 1.0f);
}

double DynamicBoundarySystem::compute_forward_depth_offset(double depth_from_anchor, float depth_axis_sign) {
    if (!std::isfinite(depth_from_anchor) || !std::isfinite(depth_axis_sign)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double sign = static_cast<double>(render_depth::normalize_depth_axis_sign(depth_axis_sign));
    return -sign * depth_from_anchor;
}

float DynamicBoundarySystem::depth_efficiency_sample_from_hash(std::uint64_t key_hash) {
    const std::uint64_t mixed_hash = mix_uint64(key_hash, 0x4450524546464943ULL);
    const double sample =
        static_cast<double>(mixed_hash) /
        static_cast<double>(std::numeric_limits<std::uint64_t>::max());
    if (!std::isfinite(sample)) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(sample), 0.0f, 1.0f);
}

bool DynamicBoundarySystem::evaluate_depth_efficiency_visibility(float deterministic_sample,
                                                                 float keep_ratio,
                                                                 bool was_visible,
                                                                 float hysteresis_width) {
    const float clamped_keep_ratio = clamp_unit_interval(keep_ratio, 0.0f);
    if (clamped_keep_ratio <= 0.0f) {
        return false;
    }
    if (clamped_keep_ratio >= 1.0f) {
        return true;
    }

    const float clamped_sample = clamp_unit_interval(deterministic_sample, 0.0f);
    const float clamped_hysteresis = std::clamp(std::fabs(hysteresis_width), 0.0f, 0.49f);
    const float entry_threshold = std::max(0.0f, clamped_keep_ratio - clamped_hysteresis);
    const float exit_threshold = std::min(1.0f, clamped_keep_ratio + clamped_hysteresis);
    return was_visible ? (clamped_sample <= exit_threshold) : (clamped_sample <= entry_threshold);
}

bool DynamicBoundarySystem::should_promote_controller_candidate(bool has_registered_controller,
                                                                bool visible_after_efficiency,
                                                                double forward_depth_offset,
                                                                double efficiency_depth) {
    if (!has_registered_controller || !visible_after_efficiency) {
        return false;
    }
    if (!std::isfinite(forward_depth_offset) || !std::isfinite(efficiency_depth)) {
        return false;
    }
    const double clamped_efficiency_depth = std::max(0.0, efficiency_depth);
    return forward_depth_offset >= 0.0 && forward_depth_offset <= clamped_efficiency_depth;
}

bool DynamicBoundarySystem::should_attempt_room_trail_promotion(int region_domain,
                                                                bool has_registered_controller) {
    return has_registered_controller && region_domain == 1;
}

bool DynamicBoundarySystem::should_release_origin_slot_reservation(bool origin_slot_in_scope,
                                                                   bool promoted_asset_valid) {
    return !origin_slot_in_scope || !promoted_asset_valid;
}

bool DynamicBoundarySystem::should_keep_depth_efficiency_sample(std::uint64_t key_hash, float keep_ratio) {
    const float clamped_keep_ratio = clamp_unit_interval(keep_ratio, 0.0f);
    if (clamped_keep_ratio <= 0.0f) {
        return false;
    }
    if (clamped_keep_ratio >= 1.0f) {
        return true;
    }
    return depth_efficiency_sample_from_hash(key_hash) <= clamped_keep_ratio;
}

void DynamicBoundarySystem::advance_frame_state(FrameState& frame_state,
                                                const std::vector<BoundaryFrame>& frames,
                                                float delta_ms,
                                                bool freeze_animation) {
    if (frames.empty()) {
        return;
    }
    if (!std::isfinite(delta_ms) || delta_ms < 0.0f) {
        delta_ms = 0.0f;
    }

    const int total_frames = static_cast<int>(frames.size());
    if (total_frames <= 0) {
        return;
    }
    int current_index = frame_state.frame_index % total_frames;
    if (current_index < 0) {
        current_index += total_frames;
    }

    float elapsed_ms = frame_state.elapsed_ms;
    if (!std::isfinite(elapsed_ms) || elapsed_ms < 0.0f) {
        elapsed_ms = 0.0f;
    }
    if (freeze_animation || delta_ms <= 0.0f) {
        frame_state.frame_index = current_index;
        frame_state.elapsed_ms = elapsed_ms;
        return;
    }

    elapsed_ms += delta_ms;
    float frame_duration = frames[current_index].duration_ms;
    if (!(frame_duration > 0.0f)) {
        frame_duration = kDefaultAnimationFrameMs;
    }
    while (elapsed_ms >= frame_duration && total_frames > 0) {
        elapsed_ms -= frame_duration;
        current_index = (current_index + 1) % total_frames;
        frame_duration = frames[current_index].duration_ms;
        if (!(frame_duration > 0.0f)) {
            frame_duration = kDefaultAnimationFrameMs;
        }
    }

    frame_state.frame_index = current_index;
    frame_state.elapsed_ms = elapsed_ms;
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
    const std::size_t generation = assets->rooms_generation();
    if (cached_rooms_generation_ == generation) {
        return cached_rooms_topology_hash_;
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
        hash = mix_uint64(hash, room->inherits_map_assets() ? 1ULL : 0ULL);
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
    cached_rooms_generation_ = generation;
    cached_rooms_topology_hash_ = static_cast<std::size_t>(hash);
    return cached_rooms_topology_hash_;
}
