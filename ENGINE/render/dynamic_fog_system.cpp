#include "render/dynamic_fog_system.hpp"
#include "asset/Asset.hpp"
#include "render/warped_screen_grid.hpp"
#include "world/world_grid.hpp"
#include "world/grid_point.hpp"
#include "utils/log.hpp"

#include <SDL_image.h>
#include <random>
#include <cmath>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

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

    // Load fog textures from fog/ directory
    std::string fog_dir = "fog";
    if (!fs::exists(fog_dir)) {
        vibble::log::warn("[DynamicFogSystem] Fog directory not found: " + fog_dir);
        return false;
    }

    int loaded_count = 0;
    for (int i = 1; i <= kNumFogTextures; ++i) {
        std::string filename = fog_dir + "/" + "fog_" + std::to_string(i) + ".png";

        if (!fs::exists(filename)) {
            vibble::log::warn("[DynamicFogSystem] Fog texture not found: " + filename);
            continue;
        }

        SDL_Surface* surface = IMG_Load(filename.c_str());
        if (!surface) {
            vibble::log::warn("[DynamicFogSystem] Failed to load fog texture: " + filename);
            continue;
        }

        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_FreeSurface(surface);

        if (!texture) {
            vibble::log::warn("[DynamicFogSystem] Failed to create SDL_Texture for: " + filename);
            continue;
        }

        fog_textures_.push_back(texture);
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

    // Calculate grid spacing for layer 4
    // Grid spacing formula: 3^(max_layers - layer)
    // For layer 4 with max_layers=10: 3^(10-4) = 3^6 = 729 pixels
    constexpr int kMaxLayers = 10;
    constexpr int kExponent = kMaxLayers - kFogResolutionLayer;
    int grid_spacing = 1;
    for (int i = 0; i < kExponent; ++i) {
        grid_spacing *= 3;
    }
    if (grid_spacing <= 0) {
        return;
    }

    // Expand visible bounds to ensure coverage
    const float margin = view_height * 0.5f;
    SDL_FRect visible_bounds{
        static_cast<float>(view_center.x - view_height - margin),
        static_cast<float>(view_center.y - view_height - margin),
        static_cast<float>(view_height * 2.0 + margin * 2.0),
        static_cast<float>(view_height * 2.0 + margin * 2.0)
    };

    int min_world_z = static_cast<int>(settings.depth_near_world);
    int max_world_z = static_cast<int>(settings.depth_far_world);

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
                // Calculate 3D distance from camera
                float dx = world_x - view_center.x;
                float dy = world_y - view_center.y;
                float dz = static_cast<float>(world_z);
                float distance_to_camera = std::sqrt(dx * dx + dy * dy + dz * dz);

                // Calculate opacity - skip if too close (opacity would be 0)
                float opacity = calculate_fog_opacity(distance_to_camera);
                if (opacity <= 0.01f) {
                    continue;
                }

                // Calculate perspective scale (objects further away appear smaller)
                // Use exponential decay for more realistic perspective
                const float scale_reference_distance = 1500.0f;
                float perspective_scale = std::exp(-distance_to_camera / scale_reference_distance);
                perspective_scale = std::clamp(perspective_scale, 0.1f, 2.0f);

                // Project world position to screen
                SDL_FPoint world_pos{static_cast<float>(world_x), static_cast<float>(world_y)};
                SDL_FPoint screen_pos = cam.map_to_screen_f(world_pos);

                // Assign fog texture for this grid point (memoized random assignment)
                int fog_index = assign_fog_texture_for_point(world_x, world_y, world_z, kFogResolutionLayer);
                if (fog_index < 0 || fog_index >= static_cast<int>(fog_textures_.size())) {
                    continue;
                }

                SDL_Texture* fog_tex = fog_textures_[fog_index];
                if (!fog_tex) {
                    continue;
                }

                // Create fog sprite
                FogSprite sprite;
                sprite.texture = fog_tex;
                sprite.world_pos = world_pos;
                sprite.screen_pos = screen_pos;
                sprite.scale = perspective_scale;
                sprite.opacity = opacity;
                sprite.world_z = world_z;

                active_fog_sprites_.push_back(sprite);
            }
        }
    }
}

void DynamicFogSystem::render(SDL_Renderer* renderer, const WarpedScreenGrid& cam) {
    if (!renderer || active_fog_sprites_.empty()) {
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    for (const FogSprite& sprite : active_fog_sprites_) {
        if (!sprite.texture) {
            continue;
        }

        // Get texture dimensions
        int tex_w = 0, tex_h = 0;
        SDL_QueryTexture(sprite.texture, nullptr, nullptr, &tex_w, &tex_h);
        if (tex_w <= 0 || tex_h <= 0) {
            continue;
        }

        // Calculate fog size on screen
        float fog_size = kBaseFogSize * sprite.scale;

        // Center fog on grid point screen position
        SDL_FRect fog_rect{
            sprite.screen_pos.x - fog_size / 2.0f,
            sprite.screen_pos.y - fog_size / 2.0f,
            fog_size,
            fog_size
        };

        // Calculate alpha from opacity
        Uint8 alpha = static_cast<Uint8>(std::lround(sprite.opacity * 255.0f));
        if (alpha == 0) {
            continue;
        }

        // Set texture color modulation
        SDL_SetTextureColorMod(sprite.texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(sprite.texture, alpha);
        SDL_SetTextureBlendMode(sprite.texture, SDL_BLENDMODE_BLEND);

        // Render fog texture
        SDL_RenderCopyF(renderer, sprite.texture, nullptr, &fog_rect);
    }
}

float DynamicFogSystem::calculate_fog_opacity(float distance_to_camera) const {
    if (distance_to_camera <= kNearFogDistance) {
        return 0.0f;  // No fog near camera
    }
    if (distance_to_camera >= kFarFogDistance) {
        return 1.0f;  // Full fog at horizon
    }

    // Linear interpolation between near and far
    const float range = kFarFogDistance - kNearFogDistance;
    const float normalized = (distance_to_camera - kNearFogDistance) / range;
    return std::clamp(normalized, 0.0f, 1.0f);
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
