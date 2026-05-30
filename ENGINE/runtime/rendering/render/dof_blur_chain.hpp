#pragma once

#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

#include "gameplay/world/grid_point.hpp"

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

// The old radial zoom and pre-edge-warp tuning namespaces are retired.
// Lens blur now uses a per-layer depth-bin multi-sample kernel driven by CinematicLensSettings.
namespace lens_blur_tuning {
inline constexpr float kMinProcessQualityScale = 0.20f;
inline constexpr float kFarLayerQualityMultiplier = 0.48f;
inline constexpr float kBackgroundSeedQualityMultiplier = 0.42f;

inline constexpr int kMinSamples = 5;
inline constexpr int kMaxSamples = 17;
inline constexpr float kSamplesPerSqrtRadius = 1.35f;
} // namespace lens_blur_tuning

enum AlphaDebugMode : int {
    kAlphaDebugOff = 0,
    kAlphaDebugShowSourceAlpha = 1,
    kAlphaDebugShowAccumulatedBlurAlpha = 2,
    kAlphaDebugShowFinalBlurredOutput = 3,
    kAlphaDebugCompareAlphaClampProtection = 4,
    kAlphaDebugBlurPaddingPreview = 5,
};

namespace atmospheric_dust_tuning {
inline constexpr bool kEnabled = true;
inline constexpr float kAnimationFps = 18.0f;

// Dust PNG alpha owns opacity. The renderer always draws the tile at full alpha mod.
inline constexpr float kDrawAlpha = 1.0f;

// Depth and camera zoom change scale. Actual pixel size is based on incoming dust frame size.
inline constexpr float kFocusTileScale = 0.50f;
inline constexpr float kBackgroundNearTileScale = 0.46f;
inline constexpr float kBackgroundFarTileScale = 0.12f;
inline constexpr float kForegroundNearTileScale = 0.72f;
inline constexpr float kForegroundFarTileScale = 1.15f;

// Converts camera layer distance into tile scale.
// Keep world-distance behavior data-driven through DustAnchor.
inline constexpr float kDepthRampPower = 1.35f;
} // namespace atmospheric_dust_tuning


struct CinematicLensSettings {
    bool enabled = false;
    float focus_depth_offset = 0.0f;
    float aperture = 1.0f;
    float focus_falloff_acceleration = 1.65f;
    float max_near_blur_px = 12.0f;
    float max_far_blur_px = 48.0f;
    float near_far_blur_bias = 0.0f;
    float swirl_strength = 0.28f;
    float swirl_radius_start = 0.18f;
    float tangential_blur_stretch = 1.0f;
    float anamorphic_strength = 0.0f;
    float bokeh_oval_ratio = 1.0f;
    float bokeh_rotation = 0.0f;
    float field_curvature = 0.0f;
    float edge_softness = 1.0f;
    bool alpha_clamp_protection = false;
    int alpha_debug_mode = 0;
    int blur_padding_px = 0;
    int sample_count = 9;
    float downsample_scale = 1.0f;
    int quality_preset = 1;
};

struct DustAnchor {
    // Used only for computing distance/visibility behavior.
    // The dust tile field itself is projected as world-depth planes.
    float world_x = 0.0f;
    float world_z = 0.0f;

    // Set this from camera_settings.layer_depth_interval.
    float world_units_per_depth_layer = 1.0f;

    // Set this from dynamic_renderer_depth_efficiency_depth or max_cull_depth.
    // Dust stops past this world distance. <= 0 means no explicit world cutoff.
    float max_dust_world_distance = 0.0f;

    float focus_world_z = 0.0f;
    float depth_axis_sign = 1.0f;
    world::CameraProjectionParams projection{};
    bool has_projection = false;
};

struct LayerTexture {
    int depth_layer = 0;
    // Deprecated input multiplier retained for existing callers. Prefer blur_amount.
    float blur_strength = 1.0f;
    float layer_depth = 0.0f;
    float world_distance_from_focus = 0.0f;
    SDL_Texture* texture = nullptr;
    float blur_amount = 0.0f;
    bool is_foreground = false;
    bool is_focus_protected = false;
    float warp_px = 0.0f;
    float tint_strength = 0.0f;
    float phase = 0.0f;

    // Optional. If left at 0 for non-focus layers, compose() falls back to:
    // abs(depth_layer - focus_depth_layer) * DustAnchor::world_units_per_depth_layer.

    float dust_world_z = 0.0f;
    bool has_dust_world_z = false;
};

struct CompositeResult {
    bool valid = false;
    SDL_Texture* background_mid = nullptr;
    SDL_Texture* foreground_mid = nullptr;
    std::uint32_t blur_pass_count = 0;
};

bool enabled(const CinematicLensSettings& settings);

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
                            const CinematicLensSettings& lens_settings,
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
                   const CinematicLensSettings& lens_settings,
                   SDL_FPoint optical_center,
                   float blur_radius_px,
                   bool foreground_layer,
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
