#include "rendering/render/dynamic_fog_system.hpp"
#include "assets/Asset.hpp"
#include "rendering/render/grid_overlay.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "gameplay/world/world_grid.hpp"
#include "gameplay/world/grid_point.hpp"
#include "utils/log.hpp"

#include <SDL3_image/SDL_image.h>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <cstdint>

namespace fs = std::filesystem;

DynamicFogSystem::DynamicFogSystem() = default;

DynamicFogSystem::~DynamicFogSystem() {
    for (auto& entry : fog_textures_) {
        if (entry.texture) {
            SDL_DestroyTexture(entry.texture);
            entry.texture = nullptr;
        }
    }
    fog_textures_.clear();
    fog_assignments_.clear();
    active_fog_sprites_.clear();
}

bool DynamicFogSystem::initialize(SDL_Renderer* renderer) {
    if (!renderer) {
        vibble::log::warn("[DynamicFogSystem] Renderer is null; cannot initialize");
        return false;
    }

    renderer_ = renderer;

    // Load fog textures from resources/fog directory off the project root
#ifdef PROJECT_ROOT
    const fs::path fog_root = fs::path(PROJECT_ROOT);
#else
    const fs::path fog_root = fs::current_path();
#endif
    const fs::path fog_dir = (fog_root / "resources" / "fog").lexically_normal();
    if (!fs::exists(fog_dir)) {
        vibble::log::warn("[DynamicFogSystem] Fog directory not found: " + fog_dir.generic_string());
        return false;
    }

    int loaded_count = 0;
    for (int i = 1; i <= kNumFogTextures; ++i) {
        const fs::path filename = fog_dir / ("fog_" + std::to_string(i) + ".png");

        if (!fs::exists(filename)) {
            vibble::log::warn("[DynamicFogSystem] Fog texture not found: " + filename.generic_string());
            continue;
        }

        SDL_Texture* texture = IMG_LoadTexture(renderer_, filename.generic_string().c_str());
        if (!texture) {
            vibble::log::warn("[DynamicFogSystem] Failed to load fog texture: " + filename.generic_string());
            continue;
        }

        float tex_wf = 0.0f;
        float tex_hf = 0.0f;
        if (!SDL_GetTextureSize(texture, &tex_wf, &tex_hf)) {
            vibble::log::warn("[DynamicFogSystem] Failed to query size for fog texture: " + filename.generic_string());
            SDL_DestroyTexture(texture);
            continue;
        }

        const int tex_w = static_cast<int>(std::lround(tex_wf));
        const int tex_h = static_cast<int>(std::lround(tex_hf));
        if (tex_w <= 0 || tex_h <= 0) {
            vibble::log::warn("[DynamicFogSystem] Invalid dimensions for fog texture: " + filename.generic_string());
            SDL_DestroyTexture(texture);
            continue;
        }

        fog_textures_.push_back(FogTexture{texture, tex_w, tex_h});
        ++loaded_count;
    }

    if (loaded_count == 0) {
        vibble::log::error("[DynamicFogSystem] No fog textures loaded");
        return false;
    }

    vibble::log::info("[DynamicFogSystem] Loaded " + std::to_string(loaded_count) + " fog textures");
    return true;
}

void DynamicFogSystem::update(const WarpedScreenGrid& cam, const world::WorldGrid& grid) {
    active_fog_sprites_.clear();

    if (fog_textures_.empty()) {
        return;
    }

    // Get visible world bounds from camera
    SDL_FPoint view_center = cam.get_view_center_f();
    double view_height = cam.view_height_world();

    // Use grid-resolution aware spacing, scaled by dev-configured multiplier
    const int resolution_layer = std::clamp(grid.grid_resolution(), 0, grid.max_resolution_layers());
    int base_spacing = grid.grid_spacing_for_layer(resolution_layer);
    if (base_spacing <= 0 || base_spacing > 20000) {
        base_spacing = grid.grid_spacing_for_layer(kFogResolutionLayer);
    }
    const float spacing_multiplier = render_overlay::clamp_spacing_multiplier(config().grid_spacing_multiplier);
    const int grid_spacing = render_overlay::scaled_spacing(base_spacing, spacing_multiplier);
    const float max_random_jitter = render_overlay::clamp_random_jitter(config().max_random_jitter);

    // Expand visible bounds to ensure coverage
    const float margin = view_height * 0.5f;
    SDL_FRect visible_bounds{
        static_cast<float>(view_center.x - view_height - margin),
        static_cast<float>(view_center.y - view_height - margin),
        static_cast<float>(view_height * 2.0 + margin * 2.0),
        static_cast<float>(view_height * 2.0 + margin * 2.0)
    };

    // Render fog on the map floor
    int min_world_z = 0;
    int max_world_z = 0;

    // Snap to grid alignment
    int start_x = static_cast<int>(std::floor(visible_bounds.x / grid_spacing)) * grid_spacing;
    int start_y = static_cast<int>(std::floor(visible_bounds.y / grid_spacing)) * grid_spacing;
    int end_x = static_cast<int>(std::ceil((visible_bounds.x + visible_bounds.w) / grid_spacing)) * grid_spacing;
    int end_y = static_cast<int>(std::ceil((visible_bounds.y + visible_bounds.h) / grid_spacing)) * grid_spacing;

    // Iterate over z layers (sample only 2 layers for sparse fog)
    const int z_step = std::max(1, (max_world_z - min_world_z) / 2);
    for (int world_z = min_world_z; world_z <= max_world_z; world_z += z_step) {
        // Iterate over grid positions at layer 4 spacing (729 pixels)
        for (int world_x = start_x; world_x <= end_x; world_x += grid_spacing) {
            for (int world_y = start_y; world_y <= end_y; world_y += grid_spacing) {
                // Assign fog texture for this grid point (memoized random assignment)
                int fog_index = assign_fog_texture_for_point(world_x, world_y, world_z, kFogResolutionLayer);
                if (fog_index < 0 || fog_index >= static_cast<int>(fog_textures_.size())) {
                    continue;
                }

                const auto& fog_tex_entry = fog_textures_[fog_index];
                if (!fog_tex_entry.texture || fog_tex_entry.width <= 0 || fog_tex_entry.height <= 0) {
                    continue;
                }

                SDL_FPoint base_world_pos{static_cast<float>(world_x), static_cast<float>(world_y)};
                const SDL_FPoint jitter_offset = sample_jitter_offset(world_x, world_y, world_z, kFogResolutionLayer, max_random_jitter);
                SDL_FPoint world_pos{base_world_pos.x + jitter_offset.x, base_world_pos.y + jitter_offset.y};
                SDL_FPoint screen_pos{};
                if (!cam.project_world_point(world_pos, static_cast<float>(world_z), screen_pos)) {
                    continue;
                }
                if (!std::isfinite(screen_pos.x) || !std::isfinite(screen_pos.y)) {
                    continue;
                }

                active_fog_sprites_.push_back(FogSprite{
                    fog_tex_entry.texture, world_pos, screen_pos, 1.0f, world_z,
                    fog_tex_entry.width, fog_tex_entry.height
                });
            }
        }
    }

    // Depth-sort so render.cpp can merge fog into the interleaved draw order without copying
    const double anchor_y = cam.anchor_world_y();
    std::sort(active_fog_sprites_.begin(), active_fog_sprites_.end(),
        [anchor_y](const FogSprite& a, const FogSprite& b) {
            const double da = anchor_y - static_cast<double>(a.world_pos.y);
            const double db = anchor_y - static_cast<double>(b.world_pos.y);
            if (da != db) return da > db;
            return a.world_pos.x < b.world_pos.x;
        });
}

void DynamicFogSystem::render(SDL_Renderer* renderer, const WarpedScreenGrid& cam) {
    // Note: Fog rendering is now handled in SceneRenderer::render() with depth-sorted warped quads.
    // This method is kept for API compatibility but is no longer used.
}

std::uint64_t DynamicFogSystem::make_grid_point_hash(int world_x, int world_y, int world_z, int layer) const {
    return render_overlay::hash_grid_cell(world_x, world_y, world_z, layer);
}

int DynamicFogSystem::assign_fog_texture_for_point(int world_x, int world_y, int world_z, int layer) {
    std::uint64_t hash = make_grid_point_hash(world_x, world_y, world_z, layer);

    auto it = fog_assignments_.find(hash);
    if (it != fog_assignments_.end()) {
        return it->second;  // Already assigned
    }

    const int fog_index = render_overlay::hashed_roll(hash, static_cast<int>(fog_textures_.size()));

    fog_assignments_[hash] = fog_index;
    return fog_index;
}

void DynamicFogSystem::set_grid_spacing_multiplier(float multiplier) {
    config().grid_spacing_multiplier = render_overlay::clamp_spacing_multiplier(multiplier);
}

float DynamicFogSystem::grid_spacing_multiplier() {
    return config().grid_spacing_multiplier;
}

void DynamicFogSystem::set_base_size_scale(float scale) {
    config().base_size_scale = render_overlay::clamp_base_size_scale(scale);
}

float DynamicFogSystem::base_size_scale() {
    return config().base_size_scale;
}

void DynamicFogSystem::set_vertical_offset(float offset) {
    config().vertical_offset = render_overlay::clamp_vertical_offset(offset);
}

float DynamicFogSystem::vertical_offset() {
    return config().vertical_offset;
}

void DynamicFogSystem::set_max_random_jitter(float jitter) {
    config().max_random_jitter = render_overlay::clamp_random_jitter(jitter);
}

float DynamicFogSystem::max_random_jitter() {
    return config().max_random_jitter;
}

SDL_FPoint DynamicFogSystem::sample_jitter_offset(int world_x,
                                                  int world_y,
                                                  int world_z,
                                                  int layer,
                                                  float max_jitter) const {
    return render_overlay::jitter_from_hash(make_grid_point_hash(world_x, world_y, world_z, layer), max_jitter);
}

DynamicFogSystem::FogConfig& DynamicFogSystem::config() {
    static FogConfig cfg{};
    return cfg;
}
