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

namespace layer_effect_tuning {
// This is intentionally radial-only. Normal Gaussian blur was removed for speed.
inline constexpr float kMinProcessQualityScale = 0.18f;

// Foreground layers are never downscaled. Background layers can be downscaled as they get farther.
inline constexpr float kBackgroundFarQualityMultiplier = 0.38f;
inline constexpr float kBackgroundSeedQualityMultiplier = 0.34f;

// Radial blur sampling.
inline constexpr int kMinRadialSamples = 3;
inline constexpr int kMaxRadialSamples = 14;
inline constexpr float kSamplesPerSqrtRadius = 1.25f;
inline constexpr float kMaxRadialScaleDelta = 0.88f;
inline constexpr float kRadialScaleMultiplier = 3.05f;

// Very soft ramp near focus. This fixes the aggressive jump from focus to adjacent layers.
inline constexpr float kDepthRampPower = 2.45f;
inline constexpr float kDepthRampSoftFloor = 0.015f;

// Lens warp. This is a fast GPU scaling approximation, not a mesh warp.
inline constexpr bool kLensWarpEnabled = true;
inline constexpr float kLensWarpMaxScaleDelta = 0.155f;
inline constexpr float kLensWarpForegroundMultiplier = 1.25f;
inline constexpr float kLensWarpBackgroundMultiplier = 0.95f;
inline constexpr float kLensWarpBackgroundSeedMultiplier = 0.55f;
inline constexpr float kLensWarpWideZoomPower = 1.15f;
inline constexpr float kLensWarpMaxSafeScale = 1.22f;

// Chromatic aberration. This is deliberately visible/aggressive to start.
inline constexpr bool kChromaticAberrationEnabled = true;
inline constexpr float kChromaticBasePx = 1.35f;
inline constexpr float kChromaticMaxPx = 8.5f;
inline constexpr float kChromaticForegroundMultiplier = 1.28f;
inline constexpr float kChromaticBackgroundMultiplier = 1.05f;
inline constexpr float kChromaticBackgroundSeedMultiplier = 0.80f;
inline constexpr float kChromaticFringeOpacity = 0.52f;
inline constexpr float kChromaticCenterOpacity = 0.92f;
} // namespace layer_effect_tuning

struct LayerTexture {
    int depth_layer = 0;
    float blur_strength = 0.0f;
    SDL_Texture* texture = nullptr;
    float warp_px = 0.0f;
    float tint_strength = 0.0f;
    float phase = 0.0f;
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

    CompositeResult compose(const std::vector<LayerTexture>& layers,
                            SDL_Texture* background_seed,
                            bool depth_of_field_enabled,
                            float blur_px,
                            float radial_blur_px,
                            SDL_FPoint optical_center,
                            int focus_depth_layer = 0,
                            float camera_zoom_percent = 0.0f);

private:
    bool ensure_targets();
    bool ensure_target(SDL_Texture*& texture, const char* label);

    void clear_target(SDL_Texture* texture) const;
    bool copy_texture(SDL_Texture* src, SDL_Texture* dst) const;
    bool composite_texture_over(SDL_Texture* src, SDL_Texture* dst) const;

    bool process_layer_effects(SDL_Texture* src,
                               SDL_Texture* dst,
                               SDL_Texture* work,
                               SDL_Texture* chromatic_work,
                               float radial_blur_px,
                               float quality_scale,
                               float lens_scale_delta,
                               float chromatic_px,
                               bool foreground_layer) const;

    SDL_Renderer* renderer_ = nullptr;
    int width_ = 1;
    int height_ = 1;

    SDL_Texture* background_mid_ = nullptr;
    SDL_Texture* foreground_mid_ = nullptr;
    SDL_Texture* layer_effect_target_ = nullptr;
    SDL_Texture* chain_temp_ = nullptr;
    SDL_Texture* blur_work_ = nullptr;
    SDL_Texture* chromatic_work_ = nullptr;

    std::vector<LayerTexture> scratch_background_layers_{};
    std::vector<LayerTexture> scratch_foreground_layers_{};
};

} // namespace dof_blur_chain