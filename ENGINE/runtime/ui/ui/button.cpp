#include "button.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"
#include "button_settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <vector>
#include <limits>

#include <SDL3_image/SDL_image.h>

#include "core/manifest/manifest_loader.hpp"

namespace {

namespace fs = std::filesystem;

constexpr int   kCaptureBleed  = 16;
constexpr float kEdgeFeatherPx = 2.0f;

struct Float3 {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

struct Float4 {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

inline Float3 make_float3(float r, float g, float b) {
    return Float3{ r, g, b };
}

inline Float3 clamp01(const Float3& c) {
    return Float3{
        std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f), std::clamp(c.b, 0.0f, 1.0f) };
}

inline Float3 add(const Float3& a, const Float3& b) {
    return Float3{ a.r + b.r, a.g + b.g, a.b + b.b };
}

inline Float3 mul(const Float3& a, float s) {
    return Float3{ a.r * s, a.g * s, a.b * s };
}

inline Float3 lerp(const Float3& a, const Float3& b, float t) {
    return Float3{
        a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

struct SurfaceDeleter { void operator()(SDL_Surface* s) const { if (s) SDL_DestroySurface(s); } };
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

inline SurfacePtr make_surface_ptr(SDL_Surface* surface) {
    return SurfacePtr(surface, SurfaceDeleter{});
}

inline Uint8 clamp8(int v) { return static_cast<Uint8>(std::clamp(v, 0, 255)); }

inline SDL_Color unpack(Uint32 px) {
    SDL_Color c;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    c.a =  px        & 0xFF;
    c.b = (px >> 8 ) & 0xFF;
    c.g = (px >> 16) & 0xFF;
    c.r = (px >> 24) & 0xFF;
#else
    c.a = (px >> 24) & 0xFF;
    c.r = (px >> 16) & 0xFF;
    c.g = (px >> 8 ) & 0xFF;
    c.b =  px        & 0xFF;
#endif
    return c;
}
inline Uint32 pack(const SDL_PixelFormatDetails* fmt, const SDL_Palette* palette, const SDL_Color& c) {
    return fmt ? SDL_MapRGBA(fmt, palette, c.r, c.g, c.b, c.a) : 0;
}
inline float luminance(const SDL_Color& c) {
    return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f;
}

inline float luminance(const Float3& c) {
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

struct OverlayImage {
    int w = 0;
    int h = 0;
    std::vector<Float4> pixels;
};

struct OverlayScaled {
    size_t overlay_index = std::numeric_limits<size_t>::max();
    int    w = 0;
    int    h = 0;
    float  opacity = 0.0f;
    float  gamma   = 0.0f;
    uint64_t generation = 0;
    std::vector<Float4> pixels;
};

struct GlassResources {
    bool loaded = false;
    std::vector<OverlayImage> overlays;
    size_t current_index = 0;
    uint64_t generation = 0;
};

GlassResources& glass_resources() {
    static GlassResources res;
    return res;
}

OverlayImage load_overlay_image(const fs::path& path) {
    OverlayImage img;
    if (path.empty()) return img;

    SurfacePtr surface = make_surface_ptr(IMG_Load(path.u8string().c_str()));
    if (!surface) {
        SDL_Log("GlassButton: failed to load overlay '%s': %s", path.u8string().c_str(), SDL_GetError());
        return img;
    }

    SurfacePtr converted = make_surface_ptr(SDL_ConvertSurface(surface.get(), SDL_PIXELFORMAT_RGBA32));
    if (!converted) {
        SDL_Log("GlassButton: failed to convert overlay '%s' to RGBA32: %s", path.u8string().c_str(), SDL_GetError());
        return img;
    }

    if (!SDL_LockSurface(converted.get())) {
        SDL_Log("GlassButton: failed to lock overlay '%s': %s", path.u8string().c_str(), SDL_GetError());
        return img;
    }

    img.w = converted->w;
    img.h = converted->h;
    img.pixels.resize(static_cast<size_t>(img.w) * static_cast<size_t>(img.h));

    Uint32* px = static_cast<Uint32*>(converted->pixels);
    const int pitch = converted->pitch / 4;
    for (int y = 0; y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            SDL_Color c = unpack(px[y * pitch + x]);
            Float4 sample;
            sample.r = static_cast<float>(c.r) / 255.0f;
            sample.g = static_cast<float>(c.g) / 255.0f;
            sample.b = static_cast<float>(c.b) / 255.0f;
            sample.a = static_cast<float>(c.a) / 255.0f;
            img.pixels[static_cast<size_t>(y) * img.w + static_cast<size_t>(x)] = sample;
        }
    }

    SDL_UnlockSurface(converted.get());
    return img;
}

void ensure_overlays_loaded() {
    auto& res = glass_resources();
    if (res.loaded) return;
    res.loaded = true;

    fs::path base;
    try {
        base = fs::absolute(fs::path(manifest::manifest_path())).parent_path();
    } catch (const std::exception&) {
        base = fs::current_path();
    }

    std::vector<fs::path> search_dirs;
    search_dirs.push_back((base / "resources" / "misc_content" / "glass_texture").lexically_normal());
    search_dirs.push_back((fs::current_path() / "resources" / "misc_content" / "glass_texture").lexically_normal());

    std::vector<fs::path> unique_dirs;
    for (const auto& dir : search_dirs) {
        if (dir.empty()) continue;
        if (std::find(unique_dirs.begin(), unique_dirs.end(), dir) == unique_dirs.end()) {
            unique_dirs.push_back(dir);
        }
    }

    std::vector<fs::path> files;
    for (const auto& dir : unique_dirs) {
        if (!fs::exists(dir) || !fs::is_directory(dir)) continue;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch){ return static_cast<char>(std::tolower(ch)); });
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
                files.push_back(entry.path());
            }
        }
    }

    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    for (const auto& file : files) {
        OverlayImage img = load_overlay_image(file);
        if (img.w > 0 && img.h > 0) {
            res.overlays.push_back(std::move(img));
        }
    }

    if (!res.overlays.empty() && res.generation == 0) {
        res.current_index = std::min(res.current_index, res.overlays.size() - 1);
        res.generation = 1;
    }
}

Float4 sample_overlay(const OverlayImage& img, float fx, float fy) {
    if (img.w <= 0 || img.h <= 0 || img.pixels.empty()) return Float4{};
    fx = std::clamp(fx, 0.0f, static_cast<float>(img.w - 1));
    fy = std::clamp(fy, 0.0f, static_cast<float>(img.h - 1));

    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    int x1 = std::min(x0 + 1, img.w - 1);
    int y1 = std::min(y0 + 1, img.h - 1);

    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);

    const Float4& c00 = img.pixels[static_cast<size_t>(y0) * img.w + static_cast<size_t>(x0)];
    const Float4& c10 = img.pixels[static_cast<size_t>(y0) * img.w + static_cast<size_t>(x1)];
    const Float4& c01 = img.pixels[static_cast<size_t>(y1) * img.w + static_cast<size_t>(x0)];
    const Float4& c11 = img.pixels[static_cast<size_t>(y1) * img.w + static_cast<size_t>(x1)];

    auto lerp4 = [](const Float4& a, const Float4& b, float t) {
        return Float4{
            a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
};

    Float4 cx0 = lerp4(c00, c10, tx);
    Float4 cx1 = lerp4(c01, c11, tx);
    return lerp4(cx0, cx1, ty);
}

OverlayScaled& overlay_cache() {
    static OverlayScaled cache;
    return cache;
}

const std::vector<Float4>& scaled_overlay_pixels(const GlassButtonStyle& style, int w, int h) {
    auto& res = glass_resources();
    ensure_overlays_loaded();
    if (!style.overlay_enabled || res.overlays.empty()) {
        OverlayScaled& cache = overlay_cache();
        cache.overlay_index = std::numeric_limits<size_t>::max();
        cache.w = w;
        cache.h = h;
        cache.generation = 0;
        cache.pixels.clear();
        return cache.pixels;
    }

    const size_t index = std::min(res.current_index, res.overlays.size() - 1);
    OverlayScaled& cache = overlay_cache();
    if (cache.overlay_index != index || cache.w != w || cache.h != h ||
        cache.opacity != style.overlay_opacity || cache.gamma != style.overlay_bright_to_alpha_gamma ||
        cache.generation != res.generation) {
        cache.overlay_index = index;
        cache.w = w;
        cache.h = h;
        cache.opacity = style.overlay_opacity;
        cache.gamma = style.overlay_bright_to_alpha_gamma;
        cache.generation = res.generation;
        cache.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h));

        const OverlayImage& src = res.overlays[index];
        const float sx = (src.w > 1) ? (static_cast<float>(src.w - 1)) : 1.0f;
        const float sy = (src.h > 1) ? (static_cast<float>(src.h - 1)) : 1.0f;

        for (int y = 0; y < h; ++y) {
            float v = (h > 1) ? static_cast<float>(y) / static_cast<float>(h - 1) : 0.0f;
            for (int x = 0; x < w; ++x) {
                float u = (w > 1) ? static_cast<float>(x) / static_cast<float>(w - 1) : 0.0f;
                Float4 sample = sample_overlay(src, u * sx, v * sy);
                float L = std::clamp(luminance(Float3{ sample.r, sample.g, sample.b }), 0.0f, 1.0f);
                float alpha = (1.0f - std::pow(L, style.overlay_bright_to_alpha_gamma)) * style.overlay_opacity;
                alpha = std::clamp(alpha * sample.a, 0.0f, 1.0f);
                cache.pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = Float4{
                    sample.r,
                    sample.g,
                    sample.b,
                    alpha
};
            }
        }
    }

    return cache.pixels;
}

inline SDL_Rect adjusted_for_state(SDL_Rect r, bool hovered, bool pressed) {
    if (pressed)      r.y += 1;
    else if (hovered) r.y -= 1;
    return r;
}

inline SDL_Rect clamp_to_view(SDL_Renderer* r, SDL_Rect rect) {
    SDL_Rect vp{0,0,0,0};
    if (r) SDL_GetRenderViewport(r, &vp);
    int x1 = std::max(rect.x, vp.x);
    int y1 = std::max(rect.y, vp.y);
    int x2 = std::min(rect.x + rect.w, vp.x + vp.w);
    int y2 = std::min(rect.y + rect.h, vp.y + vp.h);
    return SDL_Rect{ x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1) };
}

SurfacePtr capture(SDL_Renderer* renderer, const SDL_Rect& rect) {
    if (rect.w <= 0 || rect.h <= 0) return {};
    SDL_Surface* captured = SDL_RenderReadPixels(renderer, &rect);
    if (!captured) {
        SDL_Log("GlassButton: SDL_RenderReadPixels failed: %s", SDL_GetError());
        return {};
    }

    SDL_Surface* working = captured;
    if (captured->format != SDL_PIXELFORMAT_RGBA32) {
        working = SDL_ConvertSurface(captured, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(captured);
        if (!working) {
            SDL_Log("GlassButton: SDL_ConvertSurface failed: %s", SDL_GetError());
            return {};
        }
    }

    return make_surface_ptr(working);
}

static inline uint32_t wang_hash(uint32_t x) {
    x ^= 61u; x ^= x >> 16; x *= 9u; x ^= x >> 4; x *= 0x27d4eb2du; x ^= x >> 15;
    return x;
}
static inline float rand01(int xi, int yi) {
    return (wang_hash(static_cast<uint32_t>(xi) * 73856093u ^ static_cast<uint32_t>(yi) * 19349663u) & 0xFFFFFF) / float(0xFFFFFF);
}
static inline float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

static float value_noise(float x, float y) {
    int xi = static_cast<int>(std::floor(x));
    int yi = static_cast<int>(std::floor(y));
    float xf = x - static_cast<float>(xi);
    float yf = y - static_cast<float>(yi);
    float u = smooth(xf), v = smooth(yf);

    float v00 = rand01(xi,   yi);
    float v10 = rand01(xi+1, yi);
    float v01 = rand01(xi,   yi+1);
    float v11 = rand01(xi+1, yi+1);

    float a = v00 + (v10 - v00) * u;
    float b = v01 + (v11 - v01) * u;
    return a + (b - a) * v;
}

static float fbm(float x, float y, int octaves=4, float lacunarity=2.0f, float gain=0.5f) {
    float amp = 0.5f;
    float freq = 1.0f;
    float sum = 0.0f;
    for (int i=0; i<octaves; ++i) {
        sum += amp * value_noise(x * freq, y * freq);
        freq *= lacunarity;
        amp *= gain;
    }
    return sum;
}

static std::array<float,2> fbm_grad(float x, float y, float eps=0.8f) {
    float nx1 = fbm(x + eps, y);
    float nx0 = fbm(x - eps, y);
    float ny1 = fbm(x, y + eps);
    float ny0 = fbm(x, y - eps);
    float gx = nx1 - nx0;
    float gy = ny1 - ny0;
    float len = std::max(1e-6f, std::sqrt(gx*gx + gy*gy));
    return { gx/len, gy/len };
}

inline float rr_coverage_px(int x, int y, int w, int h, int radius) {
    if (radius <= 0) return 1.0f;
    const float cx[2] = { x + 0.25f, x + 0.75f };
    const float cy[2] = { y + 0.25f, y + 0.75f };
    const float R = static_cast<float>(radius) - 0.5f;
    const float left   = R;
    const float right  = static_cast<float>(w) - R - 1.0f;
    const float top    = R;
    const float bottom = static_cast<float>(h) - R - 1.0f;

    auto inside = [&](float px, float py) -> bool {
        if (px >= left && px <= right && py >= top && py <= bottom) return true;
        float dx = (px < left)  ? (left  - px) : ((px > right)  ? (px - right)  : 0.0f);
        float dy = (py < top)   ? (top   - py) : ((py > bottom) ? (py - bottom) : 0.0f);
        return (dx*dx + dy*dy) <= (R * R);
};

    int count = 0;
    for (float yy : cy) for (float xx : cx) if (inside(xx, yy)) ++count;
    float base = static_cast<float>(count) * 0.25f;
    return std::clamp(base * (1.0f + (kEdgeFeatherPx * 0.02f)), 0.0f, 1.0f);
}

SDL_Texture* to_texture(SDL_Renderer* r, SDL_Surface* s) {
    if (!s) return nullptr;
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

inline void clamp_sample(int& sx, int& sy, int w, int h) {
    if (sx < 0) sx = 0; else if (sx >= w) sx = w - 1;
    if (sy < 0) sy = 0; else if (sy >= h) sy = h - 1;
}

}

Button Button::get_main_button(const std::string& text) {
    return Button(text, &Styles::MainDecoButton(), width(), height());
}
Button Button::get_exit_button(const std::string& text) {
    return Button(text, &Styles::ExitDecoButton(), width(), height());
}

Button::Button() = default;

Button::Button(const std::string& text, const ButtonStyle* style, int w, int h)
: rect_{0,0,w,h}, label_(text), style_(style) {}

void Button::set_position(SDL_Point p) { rect_.x = p.x; rect_.y = p.y; }
void Button::set_rect(const SDL_Rect& r) { rect_ = r; }
const SDL_Rect& Button::rect() const { return rect_; }
void Button::set_text(const std::string& text) { label_ = text; }
const std::string& Button::text() const { return label_; }

bool Button::handle_event(const SDL_Event& e) {
    bool clicked = false;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
        hovered_ = SDL_PointInRect(&p, &rect_);
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        if (SDL_PointInRect(&p, &rect_)) pressed_ = true;
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        const bool inside = SDL_PointInRect(&p, &rect_);
        if (pressed_ && inside) clicked = true;
        pressed_ = false;
    }
    return clicked;
}

void Button::render(SDL_Renderer* renderer) const {
    if (!style_) return;
    if (glass_enabled_) {
        draw_glass(renderer, rect_);
        draw_glass_text(renderer, rect_);
        return;
    }

    draw_deco(renderer, rect_, hovered_);
    const SDL_Color chosen = hovered_ ? style_->text_hover : style_->text_normal;

    TTF_Font* f = style_->label.open_font();
    if (f) {
        int tw=0, th=0; ttf_util::GetStringSize(f, label_, &tw, &th);
        SDL_Surface* s = ttf_util::RenderTextBlended(f, label_, chosen);
        if (s) {
            SDL_Texture* t = SDL_CreateTextureFromSurface(renderer, s);
            SDL_Rect dst{ rect_.x + (rect_.w - tw)/2, rect_.y + (rect_.h - th)/2, tw, th };
            sdl_render::Texture(renderer, t, nullptr, &dst);
            SDL_DestroyTexture(t);
            SDL_DestroySurface(s);
        }
        TTF_CloseFont(f);
    }
}

bool Button::is_hovered() const { return hovered_; }
bool Button::is_pressed() const { return pressed_; }
int  Button::width()  { return 520; }
int  Button::height() { return 64; }

void Button::draw_deco(SDL_Renderer* r, const SDL_Rect& b, bool hovered) const {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 20,20,20, hovered ? 120 : 96);
    sdl_render::FillRect(r, &b);
    SDL_SetRenderDrawColor(r, 255,255,255, 36);
    sdl_render::Rect(r, &b);
}

const GlassButtonStyle& Button::default_glass_style() {
    return ButtonSettings::instance().style();
}

void Button::refresh_glass_overlay() {
    // Glass rendering is intentionally lightweight now; no dynamic overlay refresh required.
}

void Button::enable_glass_style(bool enabled) { glass_enabled_ = enabled; }
void Button::set_glass_style(const GlassButtonStyle& style) {
    glass_style_override_ = style;
    has_glass_override_ = true;
}
void Button::reset_glass_style_override() {
    has_glass_override_ = false;
}
const GlassButtonStyle& Button::current_glass_style() const {
    return has_glass_override_ ? glass_style_override_ : ButtonSettings::instance().style();
}

void Button::draw_glass(SDL_Renderer* renderer, const SDL_Rect& rect) const {
    (void)current_glass_style();
    SDL_Rect r = adjusted_for_state(rect, hovered_, pressed_);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_Color fill{42, 52, 64, static_cast<Uint8>(pressed_ ? 210 : (hovered_ ? 195 : 178))};
    SDL_Color border_outer{255, 255, 255, static_cast<Uint8>(hovered_ ? 96 : 72)};
    SDL_Color border_inner{255, 255, 255, static_cast<Uint8>(hovered_ ? 54 : 36)};

    SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, fill.a);
    sdl_render::FillRect(renderer, &r);
    SDL_SetRenderDrawColor(renderer, border_outer.r, border_outer.g, border_outer.b, border_outer.a);
    sdl_render::Rect(renderer, &r);

    SDL_Rect inner{r.x + 1, r.y + 1, std::max(0, r.w - 2), std::max(0, r.h - 2)};
    SDL_SetRenderDrawColor(renderer, border_inner.r, border_inner.g, border_inner.b, border_inner.a);
    sdl_render::Rect(renderer, &inner);

    SDL_Rect sheen{r.x + 2, r.y + 2, std::max(0, r.w - 4), std::max(0, (r.h / 2) - 2)};
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, hovered_ ? 42 : 30);
    sdl_render::FillRect(renderer, &sheen);

    glass_luminance_ = 0.35f;
    glass_has_luminance_ = true;
}

void Button::draw_glass_text(SDL_Renderer* renderer, const SDL_Rect& rect) const {
    if (label_.empty()) return;
    TTF_Font* font = Styles::LabelSmallMain().open_font();;
    if (!font) return;

    SDL_Rect rr = adjusted_for_state(rect, hovered_, pressed_);
    int tw=0, th=0;
    ttf_util::GetStringSize(font, label_, &tw, &th);
    const int x = rr.x + (rr.w - tw)/2;
    const int y = rr.y + (rr.h - th)/2;

    const GlassButtonStyle& text_style = current_glass_style();
    SDL_Color text = text_style.text_color;
    SDL_Color stroke = text_style.text_stroke;

    if (hovered_ && !pressed_) {
        text.r = clamp8(text.r + 8); text.g = clamp8(text.g + 8); text.b = clamp8(text.b + 8);
    } else if (pressed_) {
        text.r = clamp8(static_cast<int>(text.r * 0.95f));
        text.g = clamp8(static_cast<int>(text.g * 0.95f));
        text.b = clamp8(static_cast<int>(text.b * 0.95f));
    }

    SDL_Surface* s_text   = ttf_util::RenderTextBlended(font, label_, text);
    SDL_Surface* s_stroke = stroke.a ? ttf_util::RenderTextBlended(font, label_, stroke) : nullptr;

    SDL_Texture* t_text   = s_text   ? SDL_CreateTextureFromSurface(renderer, s_text)   : nullptr;
    SDL_Texture* t_stroke = s_stroke ? SDL_CreateTextureFromSurface(renderer, s_stroke) : nullptr;

    if (t_stroke) {
        SDL_SetTextureBlendMode(t_stroke, SDL_BLENDMODE_BLEND);
        static const std::array<SDL_Point, 8> offs{{
            {-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}
        }};
        for (auto o : offs) {
            SDL_Rect d{ x + o.x, y + o.y, s_stroke->w, s_stroke->h };
            sdl_render::Texture(renderer, t_stroke, nullptr, &d);
        }
    }
    if (t_text) {
        SDL_SetTextureBlendMode(t_text, SDL_BLENDMODE_BLEND);
        SDL_Rect d{ x, y, s_text->w, s_text->h };
        sdl_render::Texture(renderer, t_text, nullptr, &d);
    }

    if (t_text) SDL_DestroyTexture(t_text);
    if (t_stroke) SDL_DestroyTexture(t_stroke);
    if (s_text) SDL_DestroySurface(s_text);
    if (s_stroke) SDL_DestroySurface(s_stroke);
    TTF_CloseFont(font);
}



