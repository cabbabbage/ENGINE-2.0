#pragma once

#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

namespace dof_blur_chain {

namespace damage_pulse_tuning {
inline constexpr float kDamageReference = 30.0f;
inline constexpr float kMaxTintStrength = 0.56f;
inline constexpr float kMaxWarpPx = 8.5f;
inline constexpr float kBasePropagationLayersPerSecond = 8.0f;
inline constexpr float kLowHealthSpeedScaleMin = 0.62f;
inline constexpr float kWaveFrontSoftnessLayers = 1.15f;
inline constexpr float kEnvelopeRiseSeconds = 0.055f;
inline constexpr float kEnvelopeDecaySeconds = 0.48f;
inline constexpr float kPulseLifetimeSeconds = 0.90f;
inline constexpr std::uint32_t kMaxConcurrentPulses = 5;
inline constexpr float kPhaseFrequencyHz = 8.5f;
} // namespace damage_pulse_tuning

namespace radial_blur_tuning {
inline constexpr float kMinProcessQualityScale = 0.20f;
inline constexpr float kFarLayerQualityMultiplier = 0.48f;
inline constexpr float kBackgroundSeedQualityMultiplier = 0.42f;

inline constexpr int kMinSamples = 4;
inline constexpr int kMaxSamples = 18;
inline constexpr float kSamplesPerSqrtRadius = 1.75f;

inline constexpr float kMaxScaleDelta = 0.95f;
inline constexpr float kScaleDeltaMultiplier = 3.35f;
} // namespace radial_blur_tuning

namespace edge_lens_warp_tuning {
// Pre-warp applied per already-composited layer before radial zoom blur.
// It approximates lens curvature by pushing only screen-edge strips outward from the layer center.
inline constexpr bool kEnabled = true;
inline constexpr int kMinSamples = 2;
inline constexpr int kMaxSamples = 9;
inline constexpr float kMaxEdgePushRatio = 0.075f;
inline constexpr float kMaxScaleDelta = 0.16f;
inline constexpr float kMinEdgeBandRatio = 0.12f;
inline constexpr float kMaxEdgeBandRatio = 0.32f;
inline constexpr float kCornerWeight = 0.58f;
inline constexpr float kSideWeight = 0.78f;
inline constexpr float kBaseWeight = 1.0f;
} // namespace edge_lens_warp_tuning

namespace atmospheric_dust_tuning {
inline constexpr bool kEnabled = true;
inline constexpr float kAnimationFps = 18.0f;

// Dust PNG alpha owns opacity. The renderer always draws the tile at full alpha mod.
inline constexpr float kDrawAlpha = 1.0f;

// Depth changes scale only. Actual pixel size is based on incoming dust frame size.
inline constexpr float kFocusTileScale = 0.50f;
inline constexpr float kBackgroundNearTileScale = 0.46f;
inline constexpr float kBackgroundFarTileScale = 0.12f;
inline constexpr float kForegroundNearTileScale = 0.72f;
inline constexpr float kForegroundFarTileScale = 1.15f;

// Converts camera layer distance into tile scale.
// Keep world-distance behavior data-driven through DustAnchor.
inline constexpr float kDepthRampPower = 1.35f;
} // namespace atmospheric_dust_tuning

struct DustAnchor {
    // Used only for computing distance/visibility behavior.
    // The dust tile field itself is anchored to the bottom-center of the layer target.
    float world_x = 0.0f;
    float world_z = 0.0f;

    // Set this from camera_settings.layer_depth_interval.
    float world_units_per_depth_layer = 1.0f;

    // Set this from dynamic_renderer_depth_efficiency_depth or max_cull_depth.
    // Dust stops past this world distance. <= 0 means no explicit world cutoff.
    float max_dust_world_distance = 0.0f;
};

struct LayerTexture {
    int depth_layer = 0;
    float blur_strength = 0.0f;
    SDL_Texture* texture = nullptr;
    float warp_px = 0.0f;
    float tint_strength = 0.0f;
    float phase = 0.0f;

    // Optional. If left at 0 for non-focus layers, compose() falls back to:
    // abs(depth_layer - focus_depth_layer) * DustAnchor::world_units_per_depth_layer.
    float world_distance_from_focus = 0.0f;
};

struct CompositeResult {
    bool valid = false;
    SDL_Texture* background_mid = nullptr;
    SDL_Texture* foreground_mid = nullptr;
    std::uint32_t blur_pass_count = 0;
};

bool enabled(bool depth_of_field_enabled, float blur_px, float radial_blur_px);

class Renderer {
public:
    explicit Renderer(SDL_Renderer* renderer = nullptr);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void set_renderer(SDL_Renderer* renderer);
    void set_output_dimensions(int width, int height);
    void destroy_targets();

    // Borrowed textures. The OpenGL runtime renderer owns and destroys these.
    void set_dust_frames(const std::vector<SDL_Texture*>& dust_frames);

    CompositeResult compose(const std::vector<LayerTexture>& layers,
                            SDL_Texture* background_seed,
                            bool depth_of_field_enabled,
                            float blur_px,
                            float radial_blur_px,
                            SDL_FPoint optical_center,
                            int focus_depth_layer = 0,
                            float camera_zoom_percent = 0.0f,
                            float time_seconds = 0.0f,
                            DustAnchor dust_anchor = {});

private:
    bool ensure_targets();
    bool ensure_target(SDL_Texture*& texture, const char* label);

    void clear_target(SDL_Texture* texture) const;
    bool copy_texture(SDL_Texture* src, SDL_Texture* dst) const;
    bool composite_texture_over(SDL_Texture* src, SDL_Texture* dst) const;

    bool blur_step(SDL_Texture* src,
                   SDL_Texture* dst,
                   SDL_Texture* blur_work,
                   float blur_px,
                   SDL_FPoint optical_center,
                   float radial_blur_px,
                   float quality_scale) const;

    SDL_Renderer* renderer_ = nullptr;
    int width_ = 1;
    int height_ = 1;

    SDL_Texture* background_mid_ = nullptr;
    SDL_Texture* foreground_mid_ = nullptr;
    SDL_Texture* foreground_layer_ = nullptr;
    SDL_Texture* chain_temp_ = nullptr;
    SDL_Texture* blur_work_ = nullptr;
    SDL_Texture* dust_work_ = nullptr;

    std::vector<SDL_Texture*> dust_frames_{};
    std::vector<LayerTexture> scratch_background_layers_{};
    std::vector<LayerTexture> scratch_foreground_layers_{};
};

} // namespace dof_blur_chain