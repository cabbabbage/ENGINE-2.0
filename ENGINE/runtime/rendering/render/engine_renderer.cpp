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

void log_opengl_renderer(SDL_Renderer* renderer) {
    int max_texture_size = 0;
    bool vsync_enabled = false;
    if (SDL_PropertiesID props = SDL_GetRendererProperties(renderer)) {
        max_texture_size = static_cast<int>(SDL_GetNumberProperty(props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0));
        vsync_enabled = SDL_GetNumberProperty(props, SDL_PROP_RENDERER_VSYNC_NUMBER, 0) != 0;
    }

    std::ostringstream oss;
    oss << "[EngineRenderer] Renderer=OpenGL"
        << " name=" << safe_string(SDL_GetRendererName(renderer))
        << " max_texture_size=" << max_texture_size
        << " render_targets=" << (probe_render_target_support(renderer) ? "yes" : "no")
        << " scale_mode=" << (probe_texture_scale_mode(renderer) ? "yes" : "no")
        << " vsync=" << (vsync_enabled ? "on" : "off");
    vibble::log::info(oss.str());
}

} // namespace

EngineRenderer::EngineRenderer(SDL_Renderer* renderer, SDL_Window* window)
    : renderer_(renderer), window_(window) {
    if (renderer_) {
        renderer_name_ = safe_string(SDL_GetRendererName(renderer_));
        int vsync = 0;
        if (SDL_GetRenderVSync(renderer_, &vsync)) {
            present_mode_name_ = vsync != 0 ? "vsync" : "immediate";
        } else {
            present_mode_name_ = "unknown";
        }
    } else {
        renderer_name_ = "unknown";
        present_mode_name_ = "unknown";
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

    log_opengl_renderer(attempt.renderer);
    return std::unique_ptr<EngineRenderer>(new EngineRenderer(attempt.renderer, window));
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
    static std::uint64_t present_log_count = 0;
    const std::uint64_t present_index = present_log_count + 1;
    const bool log_present_guard = present_index <= 8 || (present_index % 120) == 0;
    if (log_present_guard) {
        const std::string message = "[RenderGuard] before frame present call=" + std::to_string(present_index);
        if (present_index <= 8) {
            vibble::log::info(message);
        } else {
            vibble::log::debug(message);
        }
    }
    const std::uint64_t perf_frequency = SDL_GetPerformanceFrequency();
    const std::uint64_t present_begin = SDL_GetPerformanceCounter();
    SDL_RenderPresent(renderer_);
    const std::uint64_t present_end = SDL_GetPerformanceCounter();
    ++present_log_count;

    const double present_block_ms =
        (perf_frequency > 0 && present_end >= present_begin)
            ? (static_cast<double>(present_end - present_begin) * 1000.0 / static_cast<double>(perf_frequency))
            : 0.0;
    if (log_present_guard) {
        const std::string message = "[RenderGuard] after frame present call=" + std::to_string(present_index) +
                                    " block_ms=" + std::to_string(present_block_ms);
        if (present_index <= 8) {
            vibble::log::info(message);
        } else {
            vibble::log::debug(message);
        }
    }

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
    return attempt;
}

void EngineRenderer::log_opengl_renderer(SDL_Renderer* renderer) {
    ::log_opengl_renderer(renderer);
}
