#include "rendering/render/engine_renderer.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

#include "utils/log.hpp"

namespace {
std::string safe_string(const char* value) {
    return value ? std::string(value) : std::string();
}

SDL_Renderer* create_renderer_with_properties(SDL_PropertiesID props, const char* context) {
    SDL_Renderer* renderer = SDL_CreateRendererWithProperties(props);
    SDL_DestroyProperties(props);
    if (!renderer) {
        vibble::log::error(std::string("[EngineRenderer] Failed to create renderer (") +
                           (context ? context : "unknown") + "): " + SDL_GetError());
    }
    return renderer;
}

bool probe_render_target_support(SDL_Renderer* renderer) {
    SDL_Texture* probe = SDL_CreateTexture(renderer,
                                           SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET,
                                           4, 4);
    if (!probe) {
        return false;
    }
    SDL_DestroyTexture(probe);
    return true;
}

bool probe_texture_scale_mode(SDL_Renderer* renderer) {
    SDL_Texture* probe = SDL_CreateTexture(renderer,
                                           SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_STATIC,
                                           2, 2);
    if (!probe) {
        return false;
    }
    const bool ok = SDL_SetTextureScaleMode(probe, SDL_SCALEMODE_LINEAR);
    SDL_DestroyTexture(probe);
    return ok;
}

RenderBackendType classify_backend(SDL_Renderer* renderer, RenderBackendType hinted) {
    if (!renderer) {
        return hinted;
    }
    SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
    if (!props) {
        return hinted;
    }
    if (SDL_GetPointerProperty(props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr)) {
        return RenderBackendType::GPU;
    }
    if (SDL_GetPointerProperty(props, SDL_PROP_RENDERER_SURFACE_POINTER, nullptr)) {
        return RenderBackendType::Software;
    }
    return hinted;
}
} // namespace

EngineRenderer::EngineRenderer(SDL_Renderer* renderer, RenderCaps caps, RenderQualityTier tier, SDL_Window* window)
    : renderer_(renderer), window_(window), caps_(std::move(caps)), quality_tier_(tier) {}

EngineRenderer::~EngineRenderer() {
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
}

std::unique_ptr<EngineRenderer> EngineRenderer::Create(SDL_Window* window, bool prefer_vsync) {
    if (!window) {
        vibble::log::error("[EngineRenderer] Cannot create renderer: window is null.");
        return nullptr;
    }

    AttemptResult gpu_attempt = try_create_gpu(window, prefer_vsync);
    if (gpu_attempt.renderer) {
        auto tier = choose_quality_tier(gpu_attempt.caps);
        log_caps(gpu_attempt.caps);
        return std::unique_ptr<EngineRenderer>(new EngineRenderer(gpu_attempt.renderer, gpu_attempt.caps, tier, window));
    }
    if (!gpu_attempt.failure_reason.empty()) {
        vibble::log::warn("[EngineRenderer] GPU renderer unavailable, falling back to accelerated 2D. Reason: " +
                          gpu_attempt.failure_reason);
    }

    AttemptResult accel_attempt = try_create_accelerated(window, prefer_vsync);
    if (accel_attempt.renderer) {
        auto tier = choose_quality_tier(accel_attempt.caps);
        log_caps(accel_attempt.caps);
        return std::unique_ptr<EngineRenderer>(new EngineRenderer(accel_attempt.renderer, accel_attempt.caps, tier, window));
    }
    if (!accel_attempt.failure_reason.empty()) {
        vibble::log::warn("[EngineRenderer] Accelerated renderer unavailable, falling back to software. Reason: " +
                          accel_attempt.failure_reason);
    }

    AttemptResult software_attempt = try_create_software(window);
    if (software_attempt.renderer) {
        auto tier = choose_quality_tier(software_attempt.caps);
        log_caps(software_attempt.caps);
        return std::unique_ptr<EngineRenderer>(new EngineRenderer(software_attempt.renderer, software_attempt.caps, tier, window));
    }

    vibble::log::error("[EngineRenderer] Failed to create any renderer. GPU failure: " +
                       gpu_attempt.failure_reason +
                       " | Accelerated failure: " + accel_attempt.failure_reason +
                       " | Software failure: " + software_attempt.failure_reason);
    return nullptr;
}

void EngineRenderer::begin_frame(const SDL_Color& clear_color) {
    if (!renderer_) return;
    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, clear_color.r, clear_color.g, clear_color.b, clear_color.a);
    SDL_RenderClear(renderer_);
}

void EngineRenderer::end_frame() {
    // Placeholder for backend-specific flushes if needed later.
}

void EngineRenderer::present() {
    if (renderer_) {
        SDL_RenderPresent(renderer_);
    }
}

void EngineRenderer::set_scale(float scale_x, float scale_y) {
    if (renderer_) {
        SDL_SetRenderScale(renderer_, scale_x, scale_y);
    }
}

void EngineRenderer::set_viewport(const SDL_Rect& rect) {
    if (renderer_) {
        SDL_SetRenderViewport(renderer_, &rect);
    }
}

void EngineRenderer::clear_viewport() {
    if (renderer_) {
        SDL_SetRenderViewport(renderer_, nullptr);
    }
}

SDL_Texture* EngineRenderer::create_texture(SDL_PixelFormat format, SDL_TextureAccess access, int w, int h) const {
    if (!renderer_) return nullptr;
    return SDL_CreateTexture(renderer_, format, access, w, h);
}

SDL_Texture* EngineRenderer::create_texture_from_surface(SDL_Surface* surface) const {
    if (!renderer_ || !surface) return nullptr;
    return SDL_CreateTextureFromSurface(renderer_, surface);
}

EngineRenderer::AttemptResult EngineRenderer::try_create_gpu(SDL_Window* window, bool prefer_vsync) {
    AttemptResult attempt{};

    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props ||
        !SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window) ||
        !SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER) ||
        !SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, prefer_vsync ? 1 : 0)) {
        attempt.failure_reason = "Failed to configure GPU renderer properties: " + safe_string(SDL_GetError());
        if (props) SDL_DestroyProperties(props);
        return attempt;
    }

    attempt.renderer = create_renderer_with_properties(props, "gpu");
    if (!attempt.renderer) {
        attempt.failure_reason = safe_string(SDL_GetError());
        return attempt;
    }

    SDL_SetDefaultTextureScaleMode(attempt.renderer, SDL_SCALEMODE_LINEAR);
    attempt.caps = build_caps(attempt.renderer, RenderBackendType::GPU);
    return attempt;
}

EngineRenderer::AttemptResult EngineRenderer::try_create_accelerated(SDL_Window* window, bool prefer_vsync) {
    AttemptResult attempt{};

    // Prefer explicit vsync selection via properties; allow SDL to pick the driver.
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props ||
        !SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window) ||
        !SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, prefer_vsync ? 1 : 0)) {
        attempt.failure_reason = "Failed to configure accelerated renderer properties: " + safe_string(SDL_GetError());
        if (props) SDL_DestroyProperties(props);
        return attempt;
    }

    attempt.renderer = create_renderer_with_properties(props, "accelerated");
    if (!attempt.renderer) {
        attempt.failure_reason = safe_string(SDL_GetError());
        return attempt;
    }

    SDL_SetDefaultTextureScaleMode(attempt.renderer, SDL_SCALEMODE_LINEAR);
    attempt.caps = build_caps(attempt.renderer, RenderBackendType::Render2D);
    return attempt;
}

EngineRenderer::AttemptResult EngineRenderer::try_create_software(SDL_Window* window) {
    AttemptResult attempt{};

    SDL_Surface* window_surface = SDL_GetWindowSurface(window);
    if (!window_surface) {
        attempt.failure_reason = "[software] SDL_GetWindowSurface failed: " + safe_string(SDL_GetError());
        return attempt;
    }

    attempt.renderer = SDL_CreateSoftwareRenderer(window_surface);
    if (!attempt.renderer) {
        attempt.failure_reason = safe_string(SDL_GetError());
        return attempt;
    }

    attempt.caps = build_caps(attempt.renderer, RenderBackendType::Software);
    return attempt;
}

RenderCaps EngineRenderer::build_caps(SDL_Renderer* renderer, RenderBackendType backend_type) {
    RenderCaps caps{};
    caps.backend_type = classify_backend(renderer, backend_type);
    caps.is_software = caps.backend_type == RenderBackendType::Software;

    SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
    if (props) {
        caps.max_texture_size = static_cast<int>(SDL_GetNumberProperty(props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0));
        caps.vsync_enabled = SDL_GetNumberProperty(props, SDL_PROP_RENDERER_VSYNC_NUMBER, 0) != 0;
        caps.renderer_name = safe_string(SDL_GetRendererName(renderer));
    } else {
        caps.renderer_name = safe_string(SDL_GetRendererName(renderer));
    }

    caps.supports_render_targets = probe_render_target_support(renderer);
    caps.supports_texture_scale_modes = probe_texture_scale_mode(renderer);
    caps.supports_blend_modes = true; // SDL renderers support blend modes; keep flag for completeness.

    return caps;
}

RenderQualityTier EngineRenderer::choose_quality_tier(const RenderCaps& caps) {
    if (caps.backend_type == RenderBackendType::GPU) {
        return RenderQualityTier::GPU;
    }
    if (caps.backend_type == RenderBackendType::Software || caps.is_software) {
        return RenderQualityTier::Software;
    }
    return RenderQualityTier::Accelerated;
}

void EngineRenderer::log_caps(const RenderCaps& caps) {
    std::ostringstream oss;
    oss << "[EngineRenderer] Backend=";
    switch (caps.backend_type) {
    case RenderBackendType::GPU:      oss << "GPU"; break;
    case RenderBackendType::Render2D: oss << "Render2D"; break;
    case RenderBackendType::Software: oss << "Software"; break;
    }
    oss << " Name=" << caps.renderer_name
        << " MaxTex=" << caps.max_texture_size
        << " RT=" << (caps.supports_render_targets ? "yes" : "no")
        << " ScaleMode=" << (caps.supports_texture_scale_modes ? "yes" : "no")
        << " VSync=" << (caps.vsync_enabled ? "on" : "off")
        << " Software=" << (caps.is_software ? "yes" : "no");
    vibble::log::info(oss.str());
}
