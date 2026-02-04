#include "render/dynamic_boundary_system.hpp"
#include "render/warped_screen_grid.hpp"
#include "world/world_grid.hpp"
#include "asset/asset_library.hpp"
#include "asset/asset_info.hpp"
#include "render/scaling_logic.hpp"
#include "asset/animation.hpp"
#include "core/AssetsManager.hpp"
#include "map_generation/room.hpp"
#include "utils/area.hpp"
#include "utils/log.hpp"
#include "utils/grid.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstdint>

namespace {
constexpr float kMinGridMultiplier = 0.25f;
constexpr float kMaxGridMultiplier = 8.0f;
constexpr float kMinBaseScale = 0.25f;
constexpr float kMaxBaseScale = 12.0f;
constexpr float kMinVerticalOffset = -300.0f;
constexpr float kMaxVerticalOffset = 300.0f;
constexpr float kMinRandomJitter = 0.0f;
constexpr float kMaxRandomJitter = 500.0f;
constexpr float kDefaultAnimationFrameMs = 1000.0f / 24.0f;
constexpr long long kMaxBoundaryCells = 250000;

inline std::uint64_t mix_uint64(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

inline std::uint64_t xorshift64(std::uint64_t value) {
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    return value;
}

inline bool is_trail_string(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    bool all_space = true;
    for (unsigned char ch : text) {
        if (!std::isspace(ch)) {
            all_space = false;
            break;
        }
    }
    if (all_space) {
        return false;
    }
    std::string lower;
    lower.reserve(text.size());
    for (unsigned char ch : text) {
        lower.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lower == "trail";
}

struct ExclusionArea {
    const Area* area = nullptr;
    int min_x = 0;
    int max_x = 0;
    int min_y = 0;
    int max_y = 0;
};

inline void try_add_area(const Area* area, std::vector<ExclusionArea>& out) {
    if (!area) {
        return;
    }
    try {
        auto [minx, miny, maxx, maxy] = area->get_bounds();
        out.push_back(ExclusionArea{area, minx, maxx, miny, maxy});
    } catch (...) {
        // Ignore malformed areas that cannot produce bounds.
    }
}

inline std::vector<ExclusionArea> collect_exclusion_areas(const Assets* assets) {
    std::vector<ExclusionArea> result;
    if (!assets) {
        return result;
    }
    const auto& rooms = assets->rooms();
    result.reserve(rooms.size() * 2);
    for (const Room* room : rooms) {
        if (!room) {
            continue;
        }
        try_add_area(room->room_area.get(), result);
        for (const auto& named : room->areas) {
            if (!named.area) {
                continue;
            }
            if (is_trail_string(named.type) || is_trail_string(named.kind) || is_trail_string(named.name)) {
                try_add_area(named.area.get(), result);
            }
        }
    }
    return result;
}

inline bool is_blocked(const SDL_FPoint& world_pos, const std::vector<ExclusionArea>& exclusions) {
    if (exclusions.empty()) {
        return false;
    }
    const SDL_Point pt{
        static_cast<int>(std::lround(world_pos.x)),
        static_cast<int>(std::lround(world_pos.y))
    };
    for (const auto& ex : exclusions) {
        if (!ex.area) {
            continue;
        }
        if (pt.x < ex.min_x || pt.x > ex.max_x || pt.y < ex.min_y || pt.y > ex.max_y) {
            continue;
        }
        try {
            if (ex.area->contains_point(pt)) {
                return true;
            }
        } catch (...) {
            // Skip problematic areas
        }
    }
    return false;
}

inline int scaled_spacing(int base_spacing, float multiplier) {
    const double scaled = static_cast<double>(base_spacing) * static_cast<double>(multiplier);
    if (!std::isfinite(scaled) || scaled <= 0.0) {
        return std::max(1, base_spacing);
    }
    const long long rounded = static_cast<long long>(std::llround(scaled));
    if (rounded <= 0) {
        return 1;
    }
    if (rounded > static_cast<long long>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(rounded);
}

inline int spacing_for_resolution(int resolution) {
    const int clamped = vibble::grid::clamp_resolution(resolution);
    long long value = 1;
    for (int i = 0; i < clamped; ++i) {
        if (value > static_cast<long long>(std::numeric_limits<int>::max()) / 3LL) {
            return std::numeric_limits<int>::max();
        }
        value *= 3LL;
    }
    return static_cast<int>(value);
}

inline world::GridPoint::RegionKind classify_region_at(const std::vector<Room*>& rooms,
                                                       SDL_Point pt,
                                                       const Room** owner_out) {
    if (owner_out) *owner_out = nullptr;
    for (Room* room : rooms) {
        if (!room) continue;
        const bool room_is_trail = is_trail_string(room->type);
        if (room->room_area && room->room_area->contains_point(pt)) {
            if (owner_out) *owner_out = room;
            return room_is_trail ? world::GridPoint::RegionKind::Trail
                                 : world::GridPoint::RegionKind::Room;
        }
        for (const auto& named : room->areas) {
            if (!named.area) continue;
            if (!(is_trail_string(named.type) || is_trail_string(named.kind) || is_trail_string(named.name))) {
                continue;
            }
            try {
                if (named.area->contains_point(pt)) {
                    if (owner_out) *owner_out = room;
                    return world::GridPoint::RegionKind::Trail;
                }
            } catch (...) {
            }
        }
    }
    return world::GridPoint::RegionKind::Boundary;
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

    float camera_scale = 1.0f;
    if (assets) {
        camera_scale = static_cast<float>(std::max(0.0001, assets->getView().get_scale()));
        if (!std::isfinite(camera_scale) || camera_scale <= 0.0f) {
            camera_scale = 1.0f;
        }
    }

    float desired_variant_scale = current_scale / camera_scale;
    if (!std::isfinite(desired_variant_scale) || desired_variant_scale <= 0.0f) {
        desired_variant_scale = current_scale;
    }

    const auto& steps = (!candidate.info->scale_variants.empty())
        ? candidate.info->scale_variants
        : render_pipeline::ScalingLogic::DefaultScaleSteps();

    auto selection = render_pipeline::ScalingLogic::Choose(desired_variant_scale,
                                                           steps,
                                                           candidate.hysteresis_state,
                                                           current_scale,
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
    last_boundary_json_ = nlohmann::json{};
    boundary_types_.clear();
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

    const nlohmann::json& map_info = assets->map_info_json();
    auto boundary_it = map_info.find("map_boundary_data");
    if (boundary_it == map_info.end() || !boundary_it->is_object()) {
        boundary_types_.clear();
        last_boundary_json_ = nlohmann::json{};
        return;
    }

    if (config_dirty_ || needs_reparse(map_info)) {
        parse_boundary_config(map_info);
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
    const float spacing_multiplier = std::clamp(config().grid_spacing_multiplier, kMinGridMultiplier, kMaxGridMultiplier);
    const float max_random_jitter = std::clamp(config().max_random_jitter, kMinRandomJitter, kMaxRandomJitter);
    const std::vector<ExclusionArea> exclusion_areas = collect_exclusion_areas(assets);
    const std::vector<Room*>& rooms = assets ? assets->rooms() : std::vector<Room*>{};

    for (size_t type_idx = 0; type_idx < boundary_types_.size(); ++type_idx) {
        BoundaryType& btype = boundary_types_[type_idx];
        const int resolution_layer = vibble::grid::clamp_resolution(btype.grid_resolution);
        const int base_spacing = spacing_for_resolution(resolution_layer);
        if (base_spacing <= 0) {
            continue;
        }
        int grid_spacing = scaled_spacing(base_spacing, spacing_multiplier);
        if (grid_spacing <= 0) {
            continue;
        }

        const float min_x = visible_bounds.x;
        const float max_x = visible_bounds.x + visible_bounds.w;
        const float min_y = visible_bounds.y;
        const float max_y = visible_bounds.y + visible_bounds.h;

        auto compute_span = [&](int spacing, int& start_x, int& end_x, int& start_y, int& end_y) {
            start_x = static_cast<int>(std::floor((min_x - static_cast<float>(grid_origin.world_x())) / spacing));
            end_x = static_cast<int>(std::ceil((max_x - static_cast<float>(grid_origin.world_x())) / spacing));
            start_y = static_cast<int>(std::floor((min_y - static_cast<float>(grid_origin.world_y())) / spacing));
            end_y = static_cast<int>(std::ceil((max_y - static_cast<float>(grid_origin.world_y())) / spacing));
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

        const int world_z = 0;

        for (int ix = start_idx_x; ix <= end_idx_x; ++ix) {
            const int world_x = grid_origin.world_x() + ix * grid_spacing;
            for (int iy = start_idx_y; iy <= end_idx_y; ++iy) {
                const int world_y = grid_origin.world_y() + iy * grid_spacing;
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
                SDL_FPoint world_pos{
                    static_cast<float>(world_x) + jitter.x,
                    static_cast<float>(world_y) + jitter.y
                };
                const SDL_Point world_pt{static_cast<int>(std::lround(world_pos.x)), static_cast<int>(std::lround(world_pos.y))};
                const Room* owning_room = nullptr;
                const auto region_kind = classify_region_at(rooms, world_pt, &owning_room);
                if (region_kind != world::GridPoint::RegionKind::Boundary) {
                    continue;
                }
                const world::GridPoint virtual_point =
                    world::GridPoint::make_virtual(world_x, world_y, world_z, resolution_layer);
                const world::GridKey virtual_key =
                    grid.grid_key_from_world(virtual_point, world_z, resolution_layer);
                if (world::GridPoint* gp = grid.find_grid_point(virtual_key)) {
                    gp->region_kind = region_kind;
                    gp->region_owner = owning_room;
                }

                if (is_blocked(world_pos, exclusion_areas)) {
                    continue;
                }

                SDL_FPoint screen_pos{};
                if (!cam.project_world_point(world_pos, static_cast<float>(world_z), screen_pos)) {
                    continue;
                }
                if (!std::isfinite(screen_pos.x) || !std::isfinite(screen_pos.y)) {
                    continue;
                }

                auto& frame_state = animation_states_[key];
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
                    compute_boundary_asset_scale(candidate, cam, assets, world_pos, world_z);
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
                if (!texture) {
                    continue;
                }

                BoundarySprite sprite;
                sprite.texture = texture;
                sprite.world_pos = world_pos;
                sprite.screen_pos = screen_pos;
                sprite.world_z = world_z;
                sprite.texture_w = frame_variant->width;
                sprite.texture_h = frame_variant->height;
                if (sprite.texture_w <= 0 || sprite.texture_h <= 0) {
                    int texture_w = 0;
                    int texture_h = 0;
                    if (SDL_QueryTexture(texture, nullptr, nullptr, &texture_w, &texture_h) == 0) {
                        sprite.texture_w = texture_w;
                        sprite.texture_h = texture_h;
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
                sprite.boundary_type_index = static_cast<int>(type_idx);
                sprite.candidate_index = candidate_idx;
                sprite.current_frame_index = current_index;
                sprite.total_frames = total_frames;
                sprite.frame_duration_ms = frame_duration;
                sprite.frame_elapsed_ms = frame_state.elapsed_ms;

                active_boundary_sprites_.push_back(sprite);
            }
        }
    }
}

std::size_t DynamicBoundarySystem::BoundaryKeyHash::operator()(const DynamicBoundarySystem::BoundaryKey& key) const noexcept {
    std::uint64_t hash = 0;
    hash = mix_uint64(hash, static_cast<std::uint64_t>(key.group));
    hash = mix_uint64(hash, static_cast<std::uint64_t>(key.resolution_layer));
    hash = mix_uint64(hash, static_cast<std::uint64_t>(key.grid_x));
    hash = mix_uint64(hash, static_cast<std::uint64_t>(key.grid_y));
    hash = mix_uint64(hash, static_cast<std::uint64_t>(key.world_z));
    return static_cast<std::size_t>(hash);
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
    std::uint64_t hash = mix_uint64(boundary_regen_seed_, 0x9e3779b97f4a7c15ULL);
    hash = mix_uint64(hash, static_cast<std::uint64_t>(key.group));
    hash = mix_uint64(hash, static_cast<std::uint64_t>(key.resolution_layer));
    hash = mix_uint64(hash, static_cast<std::uint64_t>(key.grid_x));
    hash = mix_uint64(hash, static_cast<std::uint64_t>(key.grid_y));
    hash = mix_uint64(hash, static_cast<std::uint64_t>(key.world_z));
    return hash;
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
    std::uint64_t state = hash_key(key);
    state ^= 0x9e3779b97f4a7c15ULL;
    state = xorshift64(state);
    const int roll = static_cast<int>(state % static_cast<std::uint64_t>(btype.total_chance));

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
    if (max_jitter <= 0.0f) {
        return SDL_FPoint{0.0f, 0.0f};
    }
    std::uint64_t state = hash_key(key);
    state ^= 0x9e3779b97f4a7c15ULL;
    double uniform_x = static_cast<double>(xorshift64(state) & 0xFFFFFFFFULL) / static_cast<double>(std::numeric_limits<uint32_t>::max());
    state = xorshift64(state);
    double uniform_y = static_cast<double>(xorshift64(state) & 0xFFFFFFFFULL) / static_cast<double>(std::numeric_limits<uint32_t>::max());
    const double jitter_x = (uniform_x * 2.0 - 1.0) * static_cast<double>(max_jitter);
    const double jitter_y = (uniform_y * 2.0 - 1.0) * static_cast<double>(max_jitter);
    return SDL_FPoint{static_cast<float>(jitter_x), static_cast<float>(jitter_y)};
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
                SDL_QueryTexture(frame_variant.texture, nullptr, nullptr, &frame_variant.width, &frame_variant.height);
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
        SDL_QueryTexture(frame_variant.texture, nullptr, nullptr, &frame_variant.width, &frame_variant.height);
        boundary_frame.variants.push_back(frame_variant);
        candidate.frames.push_back(boundary_frame);
    }

    if (candidate.frames.empty()) {
        candidate.is_null = true;
    }
}

void DynamicBoundarySystem::parse_boundary_config(const nlohmann::json& map_info) {
    auto boundary_it = map_info.find("map_boundary_data");
    if (boundary_it == map_info.end() || !boundary_it->is_object()) {
        boundary_types_.clear();
        last_boundary_json_ = nlohmann::json{};
        boundary_regen_seed_ = 0;
        config_dirty_ = false;
        clear_runtime_caches();
        return;
    }

    const nlohmann::json& boundary_data = *boundary_it;
    try {
        boundary_regen_seed_ = static_cast<std::uint64_t>(
            std::max<long long>(0, boundary_data.value("regen_seed", 0LL)));
    } catch (...) {
        boundary_regen_seed_ = 0;
    }
    const auto selectors_it = boundary_data.find("candidate_selectors");
    if (selectors_it == boundary_data.end() || !selectors_it->is_array()) {
        boundary_types_.clear();
        last_boundary_json_ = boundary_data;
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
    last_boundary_json_ = boundary_data;
    config_revision_++;
    config_dirty_ = false;
    clear_runtime_caches();
}

bool DynamicBoundarySystem::needs_reparse(const nlohmann::json& map_info) const {
    auto boundary_it = map_info.find("map_boundary_data");
    if (boundary_it == map_info.end() || !boundary_it->is_object()) {
        return !last_boundary_json_.is_null();
    }
    return (*boundary_it) != last_boundary_json_;
}

void DynamicBoundarySystem::clear_runtime_caches() {
    boundary_assignments_.clear();
    animation_states_.clear();
    active_boundary_sprites_.clear();
}

void DynamicBoundarySystem::invalidate_config() {
    config_dirty_ = true;
    last_boundary_json_ = nlohmann::json{};
    boundary_regen_seed_ = 0;
    clear_runtime_caches();
}

DynamicBoundarySystem::BoundaryConfig& DynamicBoundarySystem::config() {
    static BoundaryConfig cfg{};
    return cfg;
}

void DynamicBoundarySystem::set_grid_spacing_multiplier(float multiplier) {
    if (!std::isfinite(multiplier)) {
        return;
    }
    config().grid_spacing_multiplier = std::clamp(multiplier, kMinGridMultiplier, kMaxGridMultiplier);
}

float DynamicBoundarySystem::grid_spacing_multiplier() {
    return config().grid_spacing_multiplier;
}

void DynamicBoundarySystem::set_base_size_scale(float scale) {
    if (!std::isfinite(scale)) {
        return;
    }
    config().base_size_scale = std::clamp(scale, kMinBaseScale, kMaxBaseScale);
}

float DynamicBoundarySystem::base_size_scale() {
    return config().base_size_scale;
}

void DynamicBoundarySystem::set_vertical_offset(float offset) {
    if (!std::isfinite(offset)) {
        return;
    }
    config().vertical_offset = std::clamp(offset, kMinVerticalOffset, kMaxVerticalOffset);
}

float DynamicBoundarySystem::vertical_offset() {
    return config().vertical_offset;
}

void DynamicBoundarySystem::set_max_random_jitter(float jitter) {
    if (!std::isfinite(jitter)) {
        return;
    }
    config().max_random_jitter = std::clamp(jitter, kMinRandomJitter, kMaxRandomJitter);
}

float DynamicBoundarySystem::max_random_jitter() {
    return config().max_random_jitter;
}
