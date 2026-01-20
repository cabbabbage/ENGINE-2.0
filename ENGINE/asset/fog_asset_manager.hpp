#pragma once

#include "render/fog_renderer.hpp"
#include <memory>
#include <string>

class Asset;

/**
 * FogAssetManager is a singleton that manages the global FogRenderer instance
 * and provides fog texture assignment functionality for assets.
 */
class FogAssetManager {
public:
    static FogAssetManager& instance();

    /**
     * Initialize the fog renderer with textures from the specified directory.
     */
    bool initialize(SDL_Renderer* renderer, const std::string& fog_dir);

    /**
     * Assign a random fog texture to an asset.
     * Should be called during asset finalization.
     *
     * @param asset Asset to assign fog texture to
     * @return true if fog texture was assigned, false otherwise
     */
    bool assign_fog_texture(Asset* asset);

    /**
     * Get the fog renderer instance.
     */
    FogRenderer* fog_renderer() { return fog_renderer_.get(); }

    /**
     * Check if fog textures are loaded and ready.
     */
    bool is_initialized() const { return fog_renderer_ && fog_renderer_->is_loaded(); }

    /**
     * Clean up fog resources.
     */
    void cleanup();

private:
    FogAssetManager() = default;
    ~FogAssetManager() = default;

    // Non-copyable
    FogAssetManager(const FogAssetManager&) = delete;
    FogAssetManager& operator=(const FogAssetManager&) = delete;

    std::unique_ptr<FogRenderer> fog_renderer_;
};
