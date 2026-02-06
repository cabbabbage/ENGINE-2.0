#include "rendering/render/dynamic_fog_system.hpp"
#include "assets/Asset.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "gameplay/world/world_grid.hpp"
#include "gameplay/world/grid_point.hpp"
#include "utils/log.hpp"

#include <SDL3_image/SDL_image.h>
#include <random>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <limits>
#include <cstdint>

namespace fs = std::filesystem;

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

DynamicFogSystem::DynamicFogSystem() = default;

DynamicFogSystem::~DynamicFogSystem() {
    // Textures are owned by SDL/renderer and will be cleaned up separately
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

        SDL_Surface* surface = IMG_Load(filename.generic_string().c_str());
        if (!surface) {
            vibble::log::warn("[DynamicFogSystem] Failed to load fog texture: " + filename.generic_string());
            continue;
        }

        const int surf_w = surface->w;
        const int surf_h = surface->h;
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_FreeSurface(surface);

        if (!texture) {
            vibble::log::warn("[DynamicFogSystem] Failed to create SDL_Texture for: " + filename.generic_string());
            continue;
        }

        fog_textures_.push_back(FogTexture{texture, surf_w, surf_h});
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
    const auto& settings = cam.realism_settings();

    // Use grid-resolution aware spacing, scaled by dev-configured multiplier
    const int resolution_layer = std::clamp(grid.grid_resolution(), 0, grid.max_resolution_layers());
    int base_spacing = grid.grid_spacing_for_layer(resolution_layer);
    if (base_spacing <= 0 || base_spacing > 20000) {
        base_spacing = grid.grid_spacing_for_layer(kFogResolutionLayer);
    }
    const float spacing_multiplier = std::clamp(config().grid_spacing_multiplier, kMinGridMultiplier, kMaxGridMultiplier);
    const int grid_spacing = std::max(1, static_cast<int>(std::lround(static_cast<float>(base_spacing) * spacing_multiplier)));
    const float max_random_jitter = std::clamp(config().max_random_jitter, kMinRandomJitter, kMaxRandomJitter);

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
    // Create a simple hash from grid coordinates
    // Combine x, y, z, layer into a single 64-bit hash
    std::uint64_t h = 0;
    h ^= std::hash<int>{}(world_x) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(world_y) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(world_z) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(layer) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

int DynamicFogSystem::assign_fog_texture_for_point(int world_x, int world_y, int world_z, int layer) {
    std::uint64_t hash = make_grid_point_hash(world_x, world_y, world_z, layer);

    auto it = fog_assignments_.find(hash);
    if (it != fog_assignments_.end()) {
        return it->second;  // Already assigned
    }

    // Randomly assign a fog texture
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, static_cast<int>(fog_textures_.size()) - 1);
    int fog_index = dist(gen);

    fog_assignments_[hash] = fog_index;
    return fog_index;
}

void DynamicFogSystem::set_grid_spacing_multiplier(float multiplier) {
    if (!std::isfinite(multiplier)) {
        return;
    }
    config().grid_spacing_multiplier = std::clamp(multiplier, kMinGridMultiplier, kMaxGridMultiplier);
}

float DynamicFogSystem::grid_spacing_multiplier() {
    return config().grid_spacing_multiplier;
}

void DynamicFogSystem::set_base_size_scale(float scale) {
    if (!std::isfinite(scale)) {
        return;
    }
    config().base_size_scale = std::clamp(scale, kMinBaseScale, kMaxBaseScale);
}

float DynamicFogSystem::base_size_scale() {
    return config().base_size_scale;
}

void DynamicFogSystem::set_vertical_offset(float offset) {
    if (!std::isfinite(offset)) {
        return;
    }
    config().vertical_offset = std::clamp(offset, kMinVerticalOffset, kMaxVerticalOffset);
}

float DynamicFogSystem::vertical_offset() {
    return config().vertical_offset;
}

void DynamicFogSystem::set_max_random_jitter(float jitter) {
    if (!std::isfinite(jitter)) {
        return;
    }
    config().max_random_jitter = std::clamp(jitter, kMinRandomJitter, kMaxRandomJitter);
}

float DynamicFogSystem::max_random_jitter() {
    return config().max_random_jitter;
}

SDL_FPoint DynamicFogSystem::sample_jitter_offset(int world_x,
                                                  int world_y,
                                                  int world_z,
                                                  int layer,
                                                  float max_jitter) const {
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

DynamicFogSystem::FogConfig& DynamicFogSystem::config() {
    static FogConfig cfg{};
    return cfg;
}
