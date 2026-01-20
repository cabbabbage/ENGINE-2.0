#include "fog_asset_manager.hpp"
#include "Asset.hpp"
#include "utils/log.hpp"
#include <random>
#include <algorithm>

FogAssetManager& FogAssetManager::instance() {
    static FogAssetManager instance;
    return instance;
}

bool FogAssetManager::initialize(SDL_Renderer* renderer, const std::string& fog_dir) {
    if (!renderer) {
        vibble::log::error("[FogAssetManager] Cannot initialize: renderer is null");
        return false;
    }

    fog_renderer_ = std::make_unique<FogRenderer>(renderer);

    if (!fog_renderer_->load_fog_textures(fog_dir)) {
        vibble::log::warn("[FogAssetManager] Failed to load fog textures from: " + fog_dir);
        fog_renderer_.reset();
        return false;
    }

    vibble::log::info("[FogAssetManager] Initialized with " +
                     std::to_string(fog_renderer_->num_loaded_textures()) + " fog textures");

    return true;
}

bool FogAssetManager::assign_fog_texture(Asset* asset) {
    if (!asset || !fog_renderer_ || !fog_renderer_->is_loaded()) {
        return false;
    }

    // Don't assign fog to animation timeline children
    if (asset->spawn_method == "animation_timeline_child") {
        return false;
    }

    // Randomly select a fog texture index (0 to num_loaded - 1)
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, fog_renderer_->num_loaded_textures() - 1);

    const int fog_index = dist(gen);

    // Get asset dimensions to determine fog texture size
    int asset_max_dim = std::max(asset->cached_w, asset->cached_h);
    if (asset_max_dim <= 0) {
        asset_max_dim = 100;  // Default size if not cached yet
    }

    // Get the appropriate fog variant
    SDL_Texture* fog_tex = fog_renderer_->get_fog_variant(fog_index, asset_max_dim);

    if (fog_tex) {
        asset->set_fog_texture(fog_tex, fog_index);
        return true;
    }

    return false;
}

void FogAssetManager::cleanup() {
    fog_renderer_.reset();
}
