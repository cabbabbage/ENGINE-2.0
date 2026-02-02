#include "render/dynamic_boundary_system.hpp"
#include "render/warped_screen_grid.hpp"
#include "world/world_grid.hpp"
#include "world/grid_point.hpp"
#include "asset/Asset.hpp"
#include "asset/asset_library.hpp"
#include "asset/asset_info.hpp"
#include "utils/log.hpp"

#include <random>
#include <cmath>
#include <algorithm>
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
}

DynamicBoundarySystem::DynamicBoundarySystem() = default;

DynamicBoundarySystem::~DynamicBoundarySystem() {
    boundary_types_.clear();
    boundary_assignments_.clear();
    animation_states_.clear();
    active_boundary_sprites_.clear();
}

bool DynamicBoundarySystem::initialize(SDL_Renderer* renderer, const nlohmann::json& boundary_data, AssetLibrary* asset_library) {
    if (!renderer || !asset_library) {
        vibble::log::warn("[DynamicBoundarySystem] Renderer or AssetLibrary is null; cannot initialize");
        return false;
    }

    renderer_ = renderer;
    boundary_types_.clear();

    if (boundary_data.is_null() || !boundary_data.is_object()) {
        vibble::log::warn("[DynamicBoundarySystem] Boundary data is null or not an object");
        return false;
    }

    const auto selectors_it = boundary_data.find("candidate_selectors");
    if (selectors_it == boundary_data.end() || !selectors_it->is_array() || selectors_it->empty()) {
        vibble::log::warn("[DynamicBoundarySystem] No candidate_selectors found in boundary data");
        return false;
    }

    // Parse each spawn group
    for (const auto& selector : *selectors_it) {
        if (!selector.is_object()) continue;

        BoundaryType boundary_type;
        boundary_type.spawn_id = selector.value("spawn_id", std::string{});
        boundary_type.display_name = selector.value("display_name", std::string{});
        boundary_type.grid_resolution = selector.value("grid_resolution", 5);

        // Clamp grid resolution
        boundary_type.grid_resolution = std::clamp(boundary_type.grid_resolution, 0, 10);

        const auto candidates_it = selector.find("candidates");
        if (candidates_it == selector.end() || !candidates_it->is_array()) {
            continue;
        }

        int total_chance = 0;
        for (const auto& candidate : *candidates_it) {
            if (!candidate.is_object()) continue;

            BoundaryCandidate bc;
            bc.asset_name = candidate.value("name", std::string{});
            bc.chance = candidate.value("chance", 0);

            if (bc.chance <= 0) continue;

            total_chance += bc.chance;
            boundary_type.candidates.push_back(bc);

            // Get texture from asset library
            if (bc.asset_name == "null" || bc.asset_name.empty()) {
                boundary_type.textures.push_back(nullptr);
                boundary_type.texture_widths.push_back(0);
                boundary_type.texture_heights.push_back(0);
            } else {
                auto asset_info = asset_library->get(bc.asset_name);
                if (asset_info && asset_info->preview_texture) {
                    SDL_Texture* tex = asset_info->preview_texture;
                    int w = 0, h = 0;
                    SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
                    boundary_type.textures.push_back(tex);
                    boundary_type.texture_widths.push_back(w);
                    boundary_type.texture_heights.push_back(h);
                } else {
                    vibble::log::warn("[DynamicBoundarySystem] Asset not found or has no texture: " + bc.asset_name);
                    boundary_type.textures.push_back(nullptr);
                    boundary_type.texture_widths.push_back(0);
                    boundary_type.texture_heights.push_back(0);
                }
            }
        }

        boundary_type.total_chance = total_chance;

        if (!boundary_type.candidates.empty() && total_chance > 0) {
            boundary_types_.push_back(std::move(boundary_type));
        }
    }

    if (boundary_types_.empty()) {
        vibble::log::warn("[DynamicBoundarySystem] No valid boundary types loaded");
        return false;
    }

    initialized_ = true;
    vibble::log::info("[DynamicBoundarySystem] Loaded " + std::to_string(boundary_types_.size()) + " boundary types");
    return true;
}

void DynamicBoundarySystem::update(const WarpedScreenGrid& cam, const world::WorldGrid& grid, float delta_ms) {
    active_boundary_sprites_.clear();

    if (!initialized_ || boundary_types_.empty()) {
        return;
    }

    // Get visible world bounds from camera
    SDL_FPoint view_center = cam.get_view_center_f();
    double view_height = cam.view_height_world();

    // Expand visible bounds for margin
    const float margin = static_cast<float>(view_height) * 0.5f;
    SDL_FRect visible_bounds{
        static_cast<float>(view_center.x - view_height - margin),
        static_cast<float>(view_center.y - view_height - margin),
        static_cast<float>(view_height * 2.0 + margin * 2.0),
        static_cast<float>(view_height * 2.0 + margin * 2.0)
    };

    const float spacing_multiplier = std::clamp(config().grid_spacing_multiplier, kMinGridMultiplier, kMaxGridMultiplier);
    const float max_random_jitter = std::clamp(config().max_random_jitter, kMinRandomJitter, kMaxRandomJitter);

    // Iterate over each boundary type (each may have different grid resolution)
    for (size_t type_idx = 0; type_idx < boundary_types_.size(); ++type_idx) {
        const BoundaryType& btype = boundary_types_[type_idx];

        // Use grid resolution from this boundary type
        int resolution_layer = std::clamp(btype.grid_resolution, 0, grid.max_resolution_layers());
        int base_spacing = grid.grid_spacing_for_layer(resolution_layer);
        if (base_spacing <= 0 || base_spacing > 20000) {
            base_spacing = grid.grid_spacing_for_layer(kBoundaryResolutionLayer);
        }
        const int grid_spacing = std::max(1, static_cast<int>(std::lround(static_cast<float>(base_spacing) * spacing_multiplier)));

        // Snap to grid alignment
        int start_x = static_cast<int>(std::floor(visible_bounds.x / grid_spacing)) * grid_spacing;
        int start_y = static_cast<int>(std::floor(visible_bounds.y / grid_spacing)) * grid_spacing;
        int end_x = static_cast<int>(std::ceil((visible_bounds.x + visible_bounds.w) / grid_spacing)) * grid_spacing;
        int end_y = static_cast<int>(std::ceil((visible_bounds.y + visible_bounds.h) / grid_spacing)) * grid_spacing;

        const int world_z = 0;

        for (int world_x = start_x; world_x <= end_x; world_x += grid_spacing) {
            for (int world_y = start_y; world_y <= end_y; world_y += grid_spacing) {
                // Assign boundary candidate for this grid point
                auto [assigned_type, candidate_idx] = assign_boundary_for_point(world_x, world_y, world_z, resolution_layer);

                // Only process if this grid point belongs to this boundary type
                if (assigned_type != static_cast<int>(type_idx)) {
                    continue;
                }

                if (candidate_idx < 0 || candidate_idx >= static_cast<int>(btype.textures.size())) {
                    continue;
                }

                SDL_Texture* tex = btype.textures[candidate_idx];
                if (!tex) {
                    continue;  // null candidate (empty space)
                }

                int tex_w = btype.texture_widths[candidate_idx];
                int tex_h = btype.texture_heights[candidate_idx];
                if (tex_w <= 0 || tex_h <= 0) {
                    continue;
                }

                // Calculate world position with optional jitter
                SDL_FPoint base_world_pos{static_cast<float>(world_x), static_cast<float>(world_y)};
                const SDL_FPoint jitter_offset = sample_jitter_offset(world_x, world_y, world_z, resolution_layer, max_random_jitter);
                SDL_FPoint world_pos{base_world_pos.x + jitter_offset.x, base_world_pos.y + jitter_offset.y};

                // Project to screen
                SDL_FPoint screen_pos{};
                if (!cam.project_world_point(world_pos, static_cast<float>(world_z), screen_pos)) {
                    continue;
                }
                if (!std::isfinite(screen_pos.x) || !std::isfinite(screen_pos.y)) {
                    continue;
                }

                // Create boundary sprite
                BoundarySprite sprite;
                sprite.texture = tex;
                sprite.world_pos = world_pos;
                sprite.screen_pos = screen_pos;
                sprite.scale = 1.0f;
                sprite.world_z = world_z;
                sprite.texture_w = tex_w;
                sprite.texture_h = tex_h;
                sprite.boundary_type_index = static_cast<int>(type_idx);

                // Get/update animation state for this grid point
                std::uint64_t hash = make_grid_point_hash(world_x, world_y, world_z, resolution_layer);
                auto& anim_state = animation_states_[hash];
                anim_state.second += delta_ms;

                // For now, single frame (no animation advancement)
                // TODO: Add multi-frame animation support when needed
                sprite.current_frame_index = 0;
                sprite.frame_elapsed_ms = anim_state.second;
                sprite.total_frames = 1;

                active_boundary_sprites_.push_back(sprite);
            }
        }
    }
}

std::uint64_t DynamicBoundarySystem::make_grid_point_hash(int world_x, int world_y, int world_z, int layer) const {
    std::uint64_t h = 0;
    h ^= std::hash<int>{}(world_x) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(world_y) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(world_z) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(layer) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

std::pair<int, int> DynamicBoundarySystem::assign_boundary_for_point(int world_x, int world_y, int world_z, int layer) {
    std::uint64_t hash = make_grid_point_hash(world_x, world_y, world_z, layer);

    auto it = boundary_assignments_.find(hash);
    if (it != boundary_assignments_.end()) {
        return it->second;
    }

    // Randomly select boundary type and candidate
    static std::random_device rd;
    static std::mt19937 gen(rd());

    // Pick a boundary type (for now just use type 0, could randomize later)
    int type_idx = 0;
    if (boundary_types_.empty()) {
        boundary_assignments_[hash] = {-1, -1};
        return {-1, -1};
    }

    const BoundaryType& btype = boundary_types_[type_idx];
    if (btype.candidates.empty() || btype.total_chance <= 0) {
        boundary_assignments_[hash] = {-1, -1};
        return {-1, -1};
    }

    // Weighted random selection of candidate
    std::uniform_int_distribution<int> dist(1, btype.total_chance);
    int roll = dist(gen);

    int candidate_idx = 0;
    int cumulative = 0;
    for (size_t i = 0; i < btype.candidates.size(); ++i) {
        cumulative += btype.candidates[i].chance;
        if (roll <= cumulative) {
            candidate_idx = static_cast<int>(i);
            break;
        }
    }

    boundary_assignments_[hash] = {type_idx, candidate_idx};
    return {type_idx, candidate_idx};
}

SDL_FPoint DynamicBoundarySystem::sample_jitter_offset(int world_x, int world_y, int world_z, int layer, float max_jitter) const {
    if (max_jitter <= 0.0f) {
        return SDL_FPoint{0.0f, 0.0f};
    }
    std::uint64_t state = make_grid_point_hash(world_x, world_y, world_z, layer);
    state ^= 0x9e3779b97f4a7c15ULL;
    auto uniform01 = [&state]() -> double {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const uint32_t value = static_cast<uint32_t>(state >> 32);
        return static_cast<double>(value) / static_cast<double>(std::numeric_limits<uint32_t>::max());
    };
    const double jitter_x = (uniform01() * 2.0 - 1.0) * static_cast<double>(max_jitter);
    const double jitter_y = (uniform01() * 2.0 - 1.0) * static_cast<double>(max_jitter);
    return SDL_FPoint{static_cast<float>(jitter_x), static_cast<float>(jitter_y)};
}

void DynamicBoundarySystem::set_grid_spacing_multiplier(float multiplier) {
    if (!std::isfinite(multiplier)) return;
    config().grid_spacing_multiplier = std::clamp(multiplier, kMinGridMultiplier, kMaxGridMultiplier);
}

float DynamicBoundarySystem::grid_spacing_multiplier() {
    return config().grid_spacing_multiplier;
}

void DynamicBoundarySystem::set_base_size_scale(float scale) {
    if (!std::isfinite(scale)) return;
    config().base_size_scale = std::clamp(scale, kMinBaseScale, kMaxBaseScale);
}

float DynamicBoundarySystem::base_size_scale() {
    return config().base_size_scale;
}

void DynamicBoundarySystem::set_vertical_offset(float offset) {
    if (!std::isfinite(offset)) return;
    config().vertical_offset = std::clamp(offset, kMinVerticalOffset, kMaxVerticalOffset);
}

float DynamicBoundarySystem::vertical_offset() {
    return config().vertical_offset;
}

DynamicBoundarySystem::BoundaryConfig& DynamicBoundarySystem::config() {
    static BoundaryConfig cfg{};
    return cfg;
}
