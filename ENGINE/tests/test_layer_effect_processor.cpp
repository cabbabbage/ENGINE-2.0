#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "rendering/render/layer_effect_processor.hpp"

namespace {

class ScopedSdlVideo {
public:
    ScopedSdlVideo() : initialized_(SDL_InitSubSystem(SDL_INIT_VIDEO)) {}
    ~ScopedSdlVideo() {
        if (initialized_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    bool initialized() const { return initialized_; }

private:
    bool initialized_ = false;
};

class ScopedRenderer {
public:
    ScopedRenderer() {
        if (!video_.initialized()) {
            return;
        }
        window_ = SDL_CreateWindow("layer_effect_processor_tests", 64, 64, SDL_WINDOW_HIDDEN);
        if (!window_) {
            return;
        }
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (!renderer_) {
            renderer_ = SDL_CreateRenderer(window_, SDL_SOFTWARE_RENDERER);
        }
        if (!renderer_) {
            return;
        }

        SDL_Texture* probe = SDL_CreateTexture(renderer_,
                                               SDL_PIXELFORMAT_RGBA8888,
                                               SDL_TEXTUREACCESS_TARGET,
                                               4,
                                               4);
        target_texture_supported_ = (probe != nullptr);
        if (probe) {
            SDL_DestroyTexture(probe);
        }
    }

    ~ScopedRenderer() {
        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    SDL_Renderer* get() const { return renderer_; }
    bool ready() const { return renderer_ != nullptr && target_texture_supported_; }

private:
    ScopedSdlVideo video_{};
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool target_texture_supported_ = false;
};

SDL_Texture* create_target_texture(SDL_Renderer* renderer, int width, int height) {
    if (!renderer || width <= 0 || height <= 0) {
        return nullptr;
    }
    SDL_Texture* texture = SDL_CreateTexture(renderer,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_TARGET,
                                             width,
                                             height);
    if (!texture) {
        return nullptr;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}

bool clear_texture(SDL_Renderer* renderer, SDL_Texture* texture, SDL_Color color) {
    if (!renderer || !texture) {
        return false;
    }
    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        return false;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer);
    SDL_SetRenderTarget(renderer, previous_target);
    return true;
}

bool fill_texture_rect(SDL_Renderer* renderer,
                       SDL_Texture* texture,
                       int x,
                       int y,
                       int w,
                       int h,
                       SDL_Color color) {
    if (!renderer || !texture || w <= 0 || h <= 0) {
        return false;
    }
    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        return false;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const SDL_FRect rect{
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<float>(w),
        static_cast<float>(h)
    };
    const bool ok = SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderTarget(renderer, previous_target);
    return ok;
}

bool read_pixel(SDL_Renderer* renderer, SDL_Texture* texture, int x, int y, SDL_Color& out_color) {
    out_color = SDL_Color{0, 0, 0, 0};
    if (!renderer || !texture || x < 0 || y < 0) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        return false;
    }

    const SDL_Rect pixel_rect{x, y, 1, 1};
    SDL_Surface* captured = SDL_RenderReadPixels(renderer, &pixel_rect);
    SDL_SetRenderTarget(renderer, previous_target);
    if (!captured || !captured->pixels) {
        if (captured) {
            SDL_DestroySurface(captured);
        }
        return false;
    }

    const SDL_PixelFormatDetails* format = SDL_GetPixelFormatDetails(captured->format);
    if (!format) {
        SDL_DestroySurface(captured);
        return false;
    }

    const Uint32 pixel = *static_cast<const Uint32*>(captured->pixels);
    SDL_GetRGBA(pixel,
                format,
                SDL_GetSurfacePalette(captured),
                &out_color.r,
                &out_color.g,
                &out_color.b,
                &out_color.a);
    SDL_DestroySurface(captured);
    return true;
}

bool capture_texture_pixels(SDL_Renderer* renderer,
                            SDL_Texture* texture,
                            int expected_w,
                            int expected_h,
                            std::vector<SDL_Color>& out_pixels) {
    out_pixels.clear();
    if (!renderer || !texture || expected_w <= 0 || expected_h <= 0) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        return false;
    }

    SDL_Surface* captured = SDL_RenderReadPixels(renderer, nullptr);
    SDL_SetRenderTarget(renderer, previous_target);
    if (!captured || !captured->pixels) {
        if (captured) {
            SDL_DestroySurface(captured);
        }
        return false;
    }
    if (captured->w != expected_w || captured->h != expected_h) {
        SDL_DestroySurface(captured);
        return false;
    }

    const SDL_PixelFormatDetails* format = SDL_GetPixelFormatDetails(captured->format);
    if (!format || format->bytes_per_pixel <= 0) {
        SDL_DestroySurface(captured);
        return false;
    }
    const int bpp = format->bytes_per_pixel;

    out_pixels.resize(static_cast<std::size_t>(expected_w * expected_h));
    const SDL_Palette* palette = SDL_GetSurfacePalette(captured);
    for (int y = 0; y < expected_h; ++y) {
        const std::uint8_t* row = static_cast<const std::uint8_t*>(captured->pixels) + (captured->pitch * y);
        for (int x = 0; x < expected_w; ++x) {
            const std::uint8_t* src = row + (x * bpp);
            Uint32 raw = 0;
            switch (bpp) {
            case 1:
                raw = src[0];
                break;
            case 2:
                std::memcpy(&raw, src, 2);
                break;
            case 3:
                raw = static_cast<Uint32>(src[0]) |
                      (static_cast<Uint32>(src[1]) << 8) |
                      (static_cast<Uint32>(src[2]) << 16);
                break;
            default:
                std::memcpy(&raw, src, 4);
                break;
            }

            SDL_Color decoded{};
            SDL_GetRGBA(raw, format, palette, &decoded.r, &decoded.g, &decoded.b, &decoded.a);
            out_pixels[static_cast<std::size_t>(y * expected_w + x)] = decoded;
        }
    }

    SDL_DestroySurface(captured);
    return true;
}

int luminance_u8(const SDL_Color& color) {
    return static_cast<int>(
        std::lround((0.2126 * static_cast<double>(color.r)) +
                    (0.7152 * static_cast<double>(color.g)) +
                    (0.0722 * static_cast<double>(color.b))));
}

bool supports_alpha_preserving_pipeline_blends() {
    const SDL_BlendMode alpha_copy = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDOPERATION_ADD);

    const SDL_BlendMode add_rgb_preserve_alpha = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);

    const SDL_BlendMode alpha_masked_mul = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_DST_COLOR,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);

    return alpha_copy != SDL_BLENDMODE_INVALID &&
           add_rgb_preserve_alpha != SDL_BLENDMODE_INVALID &&
           alpha_masked_mul != SDL_BLENDMODE_INVALID;
}

bool supports_sum_blend() {
    const SDL_BlendMode sum = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);
    return sum != SDL_BLENDMODE_INVALID;
}

} // namespace

TEST_CASE("LayerEffectProcessor preserves dark-mask alpha from base layer") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    if (!supports_alpha_preserving_pipeline_blends()) {
        return;
    }

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 8;
    constexpr int kH = 8;
    SDL_Texture* base = create_target_texture(renderer, kW, kH);
    SDL_Texture* output = create_target_texture(renderer, kW, kH);
    SDL_Texture* dark_mask = create_target_texture(renderer, kW, kH);
    REQUIRE(base != nullptr);
    REQUIRE(output != nullptr);
    REQUIRE(dark_mask != nullptr);

    REQUIRE(clear_texture(renderer, base, SDL_Color{0, 0, 0, 0}));
    REQUIRE(fill_texture_rect(renderer, base, 0, 0, 4, 4, SDL_Color{255, 255, 255, 0}));
    REQUIRE(fill_texture_rect(renderer, base, 4, 0, 4, 4, SDL_Color{255, 255, 255, 64}));
    REQUIRE(fill_texture_rect(renderer, base, 0, 4, 4, 4, SDL_Color{255, 255, 255, 128}));
    REQUIRE(fill_texture_rect(renderer, base, 4, 4, 4, 4, SDL_Color{255, 255, 255, 255}));

    LayerEffectProcessor processor(renderer);
    LayerEffectProcessor::LayerLightingParams lighting{};
    lighting.enabled = true;
    lighting.ambient_color = SDL_Color{18, 20, 24, 255};

    LayerEffectProcessor::RuntimeLight light{};
    light.screen_center = SDL_FPoint{4.0f, 4.0f};
    light.color = SDL_Color{255, 255, 255, 255};
    light.intensity = 1.0f;
    light.radius_px = 12.0f;
    light.falloff = 1.8f;
    light.world_z = 0.0f;

    LayerEffectProcessor::LayerScratchTextures scratch{};
    scratch.dark_mask_texture = dark_mask;

    const LayerEffectProcessor::LayerProcessResult result = processor.process_layer(
        base,
        output,
        0.0,
        10.0,
        lighting,
        std::vector<LayerEffectProcessor::RuntimeLight>{light},
        LayerEffectProcessor::LayerFogParams{},
        LayerEffectProcessor::LayerBlurParams{},
        scratch);
    CHECK(result.lighting_applied);

    SDL_Color c00{};
    SDL_Color c10{};
    SDL_Color c01{};
    SDL_Color c11{};
    REQUIRE(read_pixel(renderer, dark_mask, 1, 1, c00));
    REQUIRE(read_pixel(renderer, dark_mask, 6, 1, c10));
    REQUIRE(read_pixel(renderer, dark_mask, 1, 6, c01));
    REQUIRE(read_pixel(renderer, dark_mask, 6, 6, c11));

    CHECK(c00.a == 0);
    CHECK(c10.a == 64);
    CHECK(c01.a == 128);
    CHECK(c11.a == 255);

    SDL_DestroyTexture(dark_mask);
    SDL_DestroyTexture(output);
    SDL_DestroyTexture(base);
}

TEST_CASE("LayerEffectProcessor attenuates lights behind a layer") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    if (!supports_alpha_preserving_pipeline_blends()) {
        return;
    }

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 32;
    constexpr int kH = 32;
    SDL_Texture* base = create_target_texture(renderer, kW, kH);
    SDL_Texture* output = create_target_texture(renderer, kW, kH);
    SDL_Texture* dark_mask = create_target_texture(renderer, kW, kH);
    REQUIRE(base != nullptr);
    REQUIRE(output != nullptr);
    REQUIRE(dark_mask != nullptr);
    REQUIRE(clear_texture(renderer, base, SDL_Color{255, 255, 255, 255}));

    LayerEffectProcessor processor(renderer);
    LayerEffectProcessor::LayerLightingParams lighting{};
    lighting.enabled = true;
    lighting.ambient_color = SDL_Color{0, 0, 0, 255};

    auto render_light_sample = [&](float world_z) -> int {
        LayerEffectProcessor::RuntimeLight light{};
        light.screen_center = SDL_FPoint{16.0f, 16.0f};
        light.color = SDL_Color{255, 255, 255, 255};
        light.intensity = 0.9f;
        light.radius_px = 12.0f;
        light.falloff = 1.6f;
        light.world_z = world_z;

        LayerEffectProcessor::LayerScratchTextures scratch{};
        scratch.dark_mask_texture = dark_mask;

        processor.process_layer(base,
                                output,
                                100.0,
                                120.0,
                                lighting,
                                std::vector<LayerEffectProcessor::RuntimeLight>{light},
                                LayerEffectProcessor::LayerFogParams{},
                                LayerEffectProcessor::LayerBlurParams{},
                                scratch);

        SDL_Color center{};
        CHECK(read_pixel(renderer, output, 16, 16, center));
        return luminance_u8(center);
    };

    const int front_value = render_light_sample(80.0f);
    const int behind_value = render_light_sample(220.0f);

    CHECK(front_value > behind_value);
    CHECK(front_value - behind_value >= 8);

    SDL_DestroyTexture(dark_mask);
    SDL_DestroyTexture(output);
    SDL_DestroyTexture(base);
}

TEST_CASE("LayerEffectProcessor behind attenuation is monotonic as depth increases") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    if (!supports_alpha_preserving_pipeline_blends()) {
        return;
    }

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 32;
    constexpr int kH = 32;
    SDL_Texture* base = create_target_texture(renderer, kW, kH);
    SDL_Texture* output = create_target_texture(renderer, kW, kH);
    SDL_Texture* dark_mask = create_target_texture(renderer, kW, kH);
    REQUIRE(base != nullptr);
    REQUIRE(output != nullptr);
    REQUIRE(dark_mask != nullptr);
    REQUIRE(clear_texture(renderer, base, SDL_Color{255, 255, 255, 255}));

    LayerEffectProcessor processor(renderer);
    LayerEffectProcessor::LayerLightingParams lighting{};
    lighting.enabled = true;
    lighting.ambient_color = SDL_Color{0, 0, 0, 255};

    auto sample = [&](float light_world_z) -> int {
        LayerEffectProcessor::RuntimeLight light{};
        light.screen_center = SDL_FPoint{16.0f, 16.0f};
        light.color = SDL_Color{255, 255, 255, 255};
        light.intensity = 0.6f;
        light.radius_px = 14.0f;
        light.falloff = 1.8f;
        light.world_z = light_world_z;

        LayerEffectProcessor::LayerScratchTextures scratch{};
        scratch.dark_mask_texture = dark_mask;

        processor.process_layer(base,
                                output,
                                100.0,
                                120.0,
                                lighting,
                                std::vector<LayerEffectProcessor::RuntimeLight>{light},
                                LayerEffectProcessor::LayerFogParams{},
                                LayerEffectProcessor::LayerBlurParams{},
                                scratch);

        SDL_Color center{};
        CHECK(read_pixel(renderer, output, 16, 16, center));
        return luminance_u8(center);
    };

    const int value_not_behind = sample(120.0f);
    const int value_mildly_behind = sample(150.0f);
    const int value_far_behind = sample(250.0f);

    CHECK(value_not_behind >= value_mildly_behind);
    CHECK(value_mildly_behind >= value_far_behind);

    SDL_DestroyTexture(dark_mask);
    SDL_DestroyTexture(output);
    SDL_DestroyTexture(base);
}

TEST_CASE("LayerEffectProcessor fog pass does not mutate dark-mask texture") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    if (!supports_alpha_preserving_pipeline_blends()) {
        return;
    }

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 24;
    constexpr int kH = 24;
    SDL_Texture* base = create_target_texture(renderer, kW, kH);
    SDL_Texture* output = create_target_texture(renderer, kW, kH);
    SDL_Texture* dark_mask = create_target_texture(renderer, kW, kH);
    REQUIRE(base != nullptr);
    REQUIRE(output != nullptr);
    REQUIRE(dark_mask != nullptr);

    REQUIRE(clear_texture(renderer, base, SDL_Color{0, 0, 0, 0}));
    REQUIRE(fill_texture_rect(renderer, base, 0, 0, 12, 24, SDL_Color{255, 255, 255, 180}));
    REQUIRE(fill_texture_rect(renderer, base, 12, 0, 12, 24, SDL_Color{255, 255, 255, 255}));

    LayerEffectProcessor processor(renderer);
    LayerEffectProcessor::LayerLightingParams lighting{};
    lighting.enabled = true;
    lighting.ambient_color = SDL_Color{8, 8, 10, 255};

    LayerEffectProcessor::RuntimeLight light{};
    light.screen_center = SDL_FPoint{12.0f, 12.0f};
    light.color = SDL_Color{255, 245, 220, 255};
    light.intensity = 0.75f;
    light.radius_px = 10.0f;
    light.falloff = 1.6f;
    light.world_z = 40.0f;

    LayerEffectProcessor::LayerScratchTextures scratch{};
    scratch.dark_mask_texture = dark_mask;

    processor.process_layer(base,
                            output,
                            20.0,
                            60.0,
                            lighting,
                            std::vector<LayerEffectProcessor::RuntimeLight>{light},
                            LayerEffectProcessor::LayerFogParams{},
                            LayerEffectProcessor::LayerBlurParams{},
                            scratch);

    std::vector<SDL_Color> without_fog_mask{};
    REQUIRE(capture_texture_pixels(renderer, dark_mask, kW, kH, without_fog_mask));

    LayerEffectProcessor::LayerFogParams fog{};
    fog.enabled = true;
    fog.normalized_depth = 0.85f;
    fog.bottom_y_px = static_cast<float>(kH);
    fog.tint = SDL_Color{225, 234, 242, 255};

    const LayerEffectProcessor::LayerProcessResult with_fog = processor.process_layer(
        base,
        output,
        20.0,
        60.0,
        lighting,
        std::vector<LayerEffectProcessor::RuntimeLight>{light},
        fog,
        LayerEffectProcessor::LayerBlurParams{},
        scratch);
    CHECK(with_fog.fog_applied);

    std::vector<SDL_Color> with_fog_mask{};
    REQUIRE(capture_texture_pixels(renderer, dark_mask, kW, kH, with_fog_mask));
    REQUIRE(without_fog_mask.size() == with_fog_mask.size());
    for (std::size_t i = 0; i < without_fog_mask.size(); ++i) {
        CHECK(without_fog_mask[i].r == with_fog_mask[i].r);
        CHECK(without_fog_mask[i].g == with_fog_mask[i].g);
        CHECK(without_fog_mask[i].b == with_fog_mask[i].b);
        CHECK(without_fog_mask[i].a == with_fog_mask[i].a);
    }

    SDL_DestroyTexture(dark_mask);
    SDL_DestroyTexture(output);
    SDL_DestroyTexture(base);
}

TEST_CASE("LayerEffectProcessor blur pass does not mutate dark-mask texture") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    if (!supports_alpha_preserving_pipeline_blends() || !supports_sum_blend()) {
        return;
    }

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 32;
    constexpr int kH = 32;
    SDL_Texture* base = create_target_texture(renderer, kW, kH);
    SDL_Texture* output = create_target_texture(renderer, kW, kH);
    SDL_Texture* dark_mask = create_target_texture(renderer, kW, kH);
    SDL_Texture* blur = create_target_texture(renderer, kW, kH);
    SDL_Texture* blur_scratch = create_target_texture(renderer, kW, kH);
    REQUIRE(base != nullptr);
    REQUIRE(output != nullptr);
    REQUIRE(dark_mask != nullptr);
    REQUIRE(blur != nullptr);
    REQUIRE(blur_scratch != nullptr);

    REQUIRE(clear_texture(renderer, base, SDL_Color{255, 255, 255, 255}));

    LayerEffectProcessor processor(renderer);
    LayerEffectProcessor::LayerLightingParams lighting{};
    lighting.enabled = true;
    lighting.ambient_color = SDL_Color{4, 4, 4, 255};

    LayerEffectProcessor::RuntimeLight light{};
    light.screen_center = SDL_FPoint{16.0f, 16.0f};
    light.color = SDL_Color{255, 255, 255, 255};
    light.intensity = 1.0f;
    light.radius_px = 11.0f;
    light.falloff = 1.5f;
    light.world_z = 15.0f;

    LayerEffectProcessor::LayerScratchTextures scratch_no_blur{};
    scratch_no_blur.dark_mask_texture = dark_mask;

    processor.process_layer(base,
                            output,
                            5.0,
                            25.0,
                            lighting,
                            std::vector<LayerEffectProcessor::RuntimeLight>{light},
                            LayerEffectProcessor::LayerFogParams{},
                            LayerEffectProcessor::LayerBlurParams{},
                            scratch_no_blur);

    std::vector<SDL_Color> without_blur_mask{};
    REQUIRE(capture_texture_pixels(renderer, dark_mask, kW, kH, without_blur_mask));

    LayerEffectProcessor::LayerBlurParams blur_params{};
    blur_params.enabled = true;
    blur_params.radius_px = 2.6f;
    blur_params.optical_center = SDL_FPoint{16.0f, 16.0f};
    blur_params.radial_radius_px = 1.0f;
    blur_params.quality_scale = 1.0f;

    LayerEffectProcessor::LayerScratchTextures scratch_blur{};
    scratch_blur.dark_mask_texture = dark_mask;
    scratch_blur.blur_texture = blur;
    scratch_blur.blur_scratch_texture = blur_scratch;

    const LayerEffectProcessor::LayerProcessResult with_blur = processor.process_layer(
        base,
        output,
        5.0,
        25.0,
        lighting,
        std::vector<LayerEffectProcessor::RuntimeLight>{light},
        LayerEffectProcessor::LayerFogParams{},
        blur_params,
        scratch_blur);
    CHECK(with_blur.blur_applied);

    std::vector<SDL_Color> with_blur_mask{};
    REQUIRE(capture_texture_pixels(renderer, dark_mask, kW, kH, with_blur_mask));
    REQUIRE(without_blur_mask.size() == with_blur_mask.size());
    for (std::size_t i = 0; i < without_blur_mask.size(); ++i) {
        CHECK(without_blur_mask[i].r == with_blur_mask[i].r);
        CHECK(without_blur_mask[i].g == with_blur_mask[i].g);
        CHECK(without_blur_mask[i].b == with_blur_mask[i].b);
        CHECK(without_blur_mask[i].a == with_blur_mask[i].a);
    }

    SDL_DestroyTexture(blur_scratch);
    SDL_DestroyTexture(blur);
    SDL_DestroyTexture(dark_mask);
    SDL_DestroyTexture(output);
    SDL_DestroyTexture(base);
}

TEST_CASE("LayerEffectProcessor applies lighting and fog when blur is disabled") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    if (!supports_alpha_preserving_pipeline_blends()) {
        return;
    }

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 24;
    constexpr int kH = 24;
    SDL_Texture* base = create_target_texture(renderer, kW, kH);
    SDL_Texture* output = create_target_texture(renderer, kW, kH);
    SDL_Texture* dark_mask = create_target_texture(renderer, kW, kH);
    REQUIRE(base != nullptr);
    REQUIRE(output != nullptr);
    REQUIRE(dark_mask != nullptr);
    REQUIRE(clear_texture(renderer, base, SDL_Color{255, 255, 255, 255}));

    LayerEffectProcessor processor(renderer);
    LayerEffectProcessor::LayerLightingParams lighting{};
    lighting.enabled = true;
    lighting.ambient_color = SDL_Color{8, 8, 10, 255};

    LayerEffectProcessor::RuntimeLight light{};
    light.screen_center = SDL_FPoint{12.0f, 12.0f};
    light.color = SDL_Color{255, 245, 220, 255};
    light.intensity = 0.8f;
    light.radius_px = 10.0f;
    light.falloff = 1.7f;
    light.world_z = 20.0f;

    LayerEffectProcessor::LayerFogParams fog{};
    fog.enabled = true;
    fog.normalized_depth = 1.0f;
    fog.bottom_y_px = static_cast<float>(kH);
    fog.tint = SDL_Color{225, 234, 242, 255};

    LayerEffectProcessor::LayerBlurParams blur{};
    blur.enabled = false;
    blur.radius_px = 0.0f;
    blur.radial_radius_px = 0.0f;

    LayerEffectProcessor::LayerScratchTextures scratch{};
    scratch.dark_mask_texture = dark_mask;

    const LayerEffectProcessor::LayerProcessResult result = processor.process_layer(
        base,
        output,
        -40.0,
        40.0,
        lighting,
        std::vector<LayerEffectProcessor::RuntimeLight>{light},
        fog,
        blur,
        scratch);

    CHECK(result.lighting_applied);
    CHECK(result.fog_applied);
    CHECK_FALSE(result.blur_applied);

    SDL_Color center{};
    REQUIRE(read_pixel(renderer, output, 12, 12, center));
    CHECK(center.a > 0);

    SDL_DestroyTexture(dark_mask);
    SDL_DestroyTexture(output);
    SDL_DestroyTexture(base);
}
