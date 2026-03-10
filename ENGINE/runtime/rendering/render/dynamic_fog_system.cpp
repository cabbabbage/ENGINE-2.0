#include "rendering/render/dynamic_fog_system.hpp"
#include "rendering/render/render_depth_policy.hpp"
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
namespace {
constexpr std::int64_t kMaxFogCellsPerFrame = 50000;
}

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
    int grid_spacing = render_overlay::scaled_spacing(base_spacing, spacing_multiplier);
    if (grid_spacing <= 0) {
        grid_spacing = 1;
    }
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
    int min_world_z = cam.last_min_world_z();
    int max_world_z = cam.last_max_world_z();
    if (min_world_z > max_world_z) {
        min_world_z = 0;
        max_world_z = 0;
    }

    // Snap to grid alignment
    auto compute_grid_span = [&](int spacing, int& out_start_x, int& out_end_x, int& out_start_z, int& out_end_z) {
        out_start_x = static_cast<int>(std::floor(visible_bounds.x / spacing)) * spacing;
        out_start_z = static_cast<int>(std::floor(visible_bounds.y / spacing)) * spacing;
        out_end_x = static_cast<int>(std::ceil((visible_bounds.x + visible_bounds.w) / spacing)) * spacing;
        out_end_z = static_cast<int>(std::ceil((visible_bounds.y + visible_bounds.h) / spacing)) * spacing;
        out_start_z = std::max(out_start_z, min_world_z);
        out_end_z = std::max(out_start_z, std::min(out_end_z, max_world_z));
    };
    auto estimate_cell_count = [](int spacing, int start_x, int end_x, int start_z, int end_z) -> std::int64_t {
        if (spacing <= 0 || end_x < start_x || end_z < start_z) {
            return 0;
        }
        const std::int64_t count_x = (static_cast<std::int64_t>(end_x) - static_cast<std::int64_t>(start_x)) /
                                     static_cast<std::int64_t>(spacing) + 1;
        const std::int64_t count_z = (static_cast<std::int64_t>(end_z) - static_cast<std::int64_t>(start_z)) /
                                     static_cast<std::int64_t>(spacing) + 1;
        if (count_x <= 0 || count_z <= 0) {
            return 0;
        }
        return count_x * count_z;
    };

    int effective_spacing = grid_spacing;
    int start_x = 0;
    int end_x = 0;
    int start_z = 0;
    int end_z = 0;
    compute_grid_span(effective_spacing, start_x, end_x, start_z, end_z);

    std::int64_t estimated_cells = estimate_cell_count(effective_spacing, start_x, end_x, start_z, end_z);
    if (estimated_cells > kMaxFogCellsPerFrame) {
        const double scale = std::sqrt(static_cast<double>(estimated_cells) / static_cast<double>(kMaxFogCellsPerFrame));
        const int spacing_scale = std::max(1, static_cast<int>(std::ceil(scale)));
        const int adjusted_spacing = effective_spacing * spacing_scale;
        if (adjusted_spacing > effective_spacing) {
            effective_spacing = adjusted_spacing;
            compute_grid_span(effective_spacing, start_x, end_x, start_z, end_z);
            estimated_cells = estimate_cell_count(effective_spacing, start_x, end_x, start_z, end_z);
        }
    }

    if (estimated_cells <= 0) {
        return;
    }

    const std::size_t reserve_count = static_cast<std::size_t>(
        std::min<std::int64_t>(estimated_cells, kMaxFogCellsPerFrame));
    active_fog_sprites_.reserve(reserve_count);

    std::int64_t generated_cells = 0;
    bool hit_cell_cap = false;
    for (int world_depth = start_z; world_depth <= end_z; world_depth += effective_spacing) {
        for (int world_x = start_x; world_x <= end_x; world_x += effective_spacing) {
            if (generated_cells >= kMaxFogCellsPerFrame) {
                hit_cell_cap = true;
                break;
            }
            ++generated_cells;
            int fog_index = assign_fog_texture_for_point(world_x, world_depth, world_depth, kFogResolutionLayer);
            if (fog_index < 0 || fog_index >= static_cast<int>(fog_textures_.size())) {
                continue;
            }

            const auto& fog_tex_entry = fog_textures_[fog_index];
            if (!fog_tex_entry.texture || fog_tex_entry.width <= 0 || fog_tex_entry.height <= 0) {
                continue;
            }

            SDL_FPoint ground_pos{static_cast<float>(world_x), 0.0f};
            const SDL_FPoint jitter_offset = sample_jitter_offset(world_x, world_depth, world_depth, kFogResolutionLayer, max_random_jitter);
            const float jittered_depth = static_cast<float>(world_depth) + jitter_offset.y;
            const float jittered_x = ground_pos.x + jitter_offset.x;
            SDL_FPoint screen_pos{};
            if (!cam.project_world_point(SDL_FPoint{jittered_x, 0.0f}, jittered_depth, screen_pos)) {
                continue;
            }
            if (!std::isfinite(screen_pos.x) || !std::isfinite(screen_pos.y)) {
                continue;
            }

            SDL_FPoint world_pos{jittered_x, jittered_depth};
            active_fog_sprites_.push_back(FogSprite{
                fog_tex_entry.texture, world_pos, screen_pos, 1.0f, static_cast<int>(std::lround(jittered_depth)),
                fog_tex_entry.width, fog_tex_entry.height
            });
        }
        if (hit_cell_cap) {
            break;
        }
    }
    // Depth-sort so render.cpp can merge fog into the interleaved draw order without copying
    const double anchor_depth = cam.anchor_world_z();
    std::sort(active_fog_sprites_.begin(), active_fog_sprites_.end(),
        [anchor_depth](const FogSprite& a, const FogSprite& b) {
            const double da = render_depth::depth_from_anchor(anchor_depth, static_cast<double>(a.world_pos.y));
            const double db = render_depth::depth_from_anchor(anchor_depth, static_cast<double>(b.world_pos.y));
            if (da != db) return da > db;
            return a.world_pos.x < b.world_pos.x;
        });
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
