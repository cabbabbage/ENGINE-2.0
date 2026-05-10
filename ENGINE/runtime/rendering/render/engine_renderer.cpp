#include "rendering/render/engine_renderer.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>

#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

namespace {

std::string safe_string(const char* value) {
    return value ? std::string(value) : std::string();
}

bool probe_render_target_support(SDL_Renderer* renderer) {
    SDL_Texture* probe = SDL_CreateTexture(renderer,
                                           SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_TARGET,
                                           4,
                                           4);
    if (!probe) {
        return false;
    }
    SDL_DestroyTexture(probe);
    return true;
}

bool probe_texture_scale_mode(SDL_Renderer* renderer) {
    SDL_Texture* probe = SDL_CreateTexture(renderer,
                                           SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC,
                                           2,
                                           2);
    if (!probe) {
        return false;
    }
    const bool ok = SDL_SetTextureScaleMode(probe, SDL_SCALEMODE_LINEAR);
    SDL_DestroyTexture(probe);
    return ok;
}

RenderCaps build_caps(SDL_Renderer* renderer, RenderBackendType backend_type) {
    RenderCaps caps{};
    caps.backend_type = backend_type;
    caps.is_software = false;

    if (SDL_PropertiesID props = SDL_GetRendererProperties(renderer)) {
        caps.max_texture_size = static_cast<int>(
            SDL_GetNumberProperty(props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0));
        caps.vsync_enabled = SDL_GetNumberProperty(props, SDL_PROP_RENDERER_VSYNC_NUMBER, 0) != 0;
    }

    caps.renderer_name = safe_string(SDL_GetRendererName(renderer));
    caps.supports_render_targets = probe_render_target_support(renderer);
    caps.supports_texture_scale_modes = probe_texture_scale_mode(renderer);
    caps.supports_blend_modes = true;
    return caps;
}

void log_caps(const RenderCaps& caps) {
    std::ostringstream oss;
    oss << "[EngineRenderer] Backend="
        << (caps.backend_type == RenderBackendType::OpenGL ? "OpenGL" : "GPU")
        << " name=" << (caps.renderer_name.empty() ? std::string("<unknown>") : caps.renderer_name)
        << " max_texture_size=" << caps.max_texture_size
        << " render_targets=" << (caps.supports_render_targets ? "yes" : "no")
        << " scale_mode=" << (caps.supports_texture_scale_modes ? "yes" : "no")
        << " vsync=" << (caps.vsync_enabled ? "on" : "off")
        << " software=" << (caps.is_software ? "yes" : "no");
    vibble::log::info(oss.str());
}

} // namespace

EngineRenderer::EngineRenderer(SDL_Renderer* renderer, RenderCaps caps, RenderQualityTier tier, SDL_Window* window)
    : renderer_(renderer), window_(window), caps_(std::move(caps)), quality_tier_(tier) {
    if (renderer_) {
        int vsync = 0;
        if (SDL_GetRenderVSync(renderer_, &vsync)) {
            caps_.vsync_enabled = vsync != 0;
        }
        present_mode_name_ = caps_.vsync_enabled ? "vsync" : "immediate";
    } else {
        present_mode_name_ = "unknown";
    }

    std::string format_error;
    has_gpu_format_policy_ = GpuFormatPolicyResolver::Resolve(nullptr, false, gpu_format_policy_, format_error);
    if (!has_gpu_format_policy_) {
        vibble::log::warn("[EngineRenderer] Default format policy init failed: " + format_error);
    }
}

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

    AttemptResult attempt = try_create_opengl(window, prefer_vsync);
    if (!attempt.renderer) {
        vibble::log::error("[EngineRenderer] OpenGL renderer initialization failed: " +
                           (attempt.failure_reason.empty() ? std::string("unknown error") : attempt.failure_reason));
        return nullptr;
    }

    log_caps(attempt.caps);
    return std::unique_ptr<EngineRenderer>(
        new EngineRenderer(attempt.renderer, attempt.caps, RenderQualityTier::OpenGL, window));
}

void EngineRenderer::begin_frame(const SDL_Color& clear_color) {
    if (!renderer_) {
        return;
    }
    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, clear_color.r, clear_color.g, clear_color.b, clear_color.a);
    SDL_RenderClear(renderer_);
}

void EngineRenderer::end_frame() {
    // Intentionally empty.
}

void EngineRenderer::present() {
    if (!renderer_) {
        return;
    }

    render_diagnostics::note_present_call();
    const std::uint64_t perf_frequency = SDL_GetPerformanceFrequency();
    const std::uint64_t present_begin = SDL_GetPerformanceCounter();
    SDL_RenderPresent(renderer_);
    const std::uint64_t present_end = SDL_GetPerformanceCounter();

    const double present_block_ms =
        (perf_frequency > 0 && present_end >= present_begin)
            ? (static_cast<double>(present_end - present_begin) * 1000.0 / static_cast<double>(perf_frequency))
            : 0.0;

    bool interval_known = false;
    double present_interval_ms = 0.0;
    if (perf_frequency > 0 && last_present_counter_ != 0 && present_begin >= last_present_counter_) {
        present_interval_ms =
            (static_cast<double>(present_begin - last_present_counter_) * 1000.0) / static_cast<double>(perf_frequency);
        interval_known = true;
    }
    last_present_counter_ = present_end;
    render_diagnostics::set_present_pacing(present_block_ms, present_interval_ms, interval_known);
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
    if (!renderer_) {
        return nullptr;
    }
    return SDL_CreateTexture(renderer_, format, access, w, h);
}

SDL_Texture* EngineRenderer::create_texture_from_surface(SDL_Surface* surface) const {
    if (!renderer_ || !surface) {
        return nullptr;
    }
    return SDL_CreateTextureFromSurface(renderer_, surface);
}

EngineRenderer::AttemptResult EngineRenderer::try_create_opengl(SDL_Window* window, bool prefer_vsync) {
    AttemptResult attempt{};

    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props ||
        !SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window) ||
        !SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, "opengl") ||
        !SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, prefer_vsync ? 1 : 0)) {
        attempt.failure_reason = "Failed to configure OpenGL renderer properties: " + safe_string(SDL_GetError());
        if (props) {
            SDL_DestroyProperties(props);
        }
        return attempt;
    }

    attempt.renderer = SDL_CreateRendererWithProperties(props);
    SDL_DestroyProperties(props);
    if (!attempt.renderer) {
        attempt.failure_reason = safe_string(SDL_GetError());
        return attempt;
    }

    SDL_SetDefaultTextureScaleMode(attempt.renderer, SDL_SCALEMODE_LINEAR);
    SDL_SetRenderVSync(attempt.renderer, prefer_vsync ? 1 : 0);
    attempt.caps = EngineRenderer::build_caps(attempt.renderer, RenderBackendType::OpenGL);
    int vsync = 0;
    if (SDL_GetRenderVSync(attempt.renderer, &vsync)) {
        attempt.caps.vsync_enabled = vsync != 0;
    }
    return attempt;
}

RenderCaps EngineRenderer::build_caps(SDL_Renderer* renderer, RenderBackendType backend_type) {
    return ::build_caps(renderer, backend_type);
}

void EngineRenderer::log_caps(const RenderCaps& caps) {
    ::log_caps(caps);
}
