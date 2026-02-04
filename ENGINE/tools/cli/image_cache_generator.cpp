// image_cache_generator.cpp
//
// Full implementation for ImageCacheGenerator (offline cache build tool).
// Mirrors the current Python pipeline behavior found in asset_tool.py and apply_color_effects.py:
//
// - Repo root discovery: search upward for manifest.json
// - Asset src resolution: default <manifest_dir>/resources/assets/<asset>, or asset_directory override
// - Animation discovery: subdirectories, else single "default"
// - Frame enumeration: 0.png, 1.png, ... stop at first missing
// - Speed multiplier expansion: closest of {0.25, 0.5, 1.0, 2.0, 4.0}
// - Crop bounds: union alpha bbox across all frames, skip if inconsistent sizes
// - Crop scaling: int(round(bounds * scale_factor))
// - Rebuild selection: flagged needs_rebuild frames OR missing output file(s) OR force option
// - Output layout:
//   <cache_root>/<asset>/animations/<anim>/scale_<pct>/{normal,foreground,background}/{out_idx}.png
// - Effects math matches apply_color_effects.py CPU path (brightness, contrast, hue, per-channel saturation)
// - Blur/sharpen matches apply_color_effects.py logic closely (foreground ringy blur vs background bloom blur)
//
// Requirements:
// - nlohmann::json (header-only)
// - stb_image.h, stb_image_write.h (and optionally stb_image_resize.h if you want to swap the resizer)
//
// This file does not define a main(). Wire it into your CLI separately.

#include "image_cache_generator.hpp"

#include "asset_metadata.hpp"
#include "rebuild_queue.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>

// ---- stb ----
// You can move these defines into a single translation unit in your tools project if you prefer.
#if defined(IMGCACHE_STB_IMAGE_IMPL) && !defined(STB_IMAGE_IMPLEMENTATION)
#define STB_IMAGE_IMPLEMENTATION
#endif
#ifndef IMGCACHE_STB_IMAGE_IMPL
#define IMGCACHE_STB_IMAGE_IMPL
#define STB_IMAGE_IMPLEMENTATION
#endif
#include "utils/stb_image.h"

#if defined(IMGCACHE_STB_IMAGE_WRITE_IMPL) && !defined(STB_IMAGE_WRITE_IMPLEMENTATION)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#ifndef IMGCACHE_STB_IMAGE_WRITE_IMPL
#define IMGCACHE_STB_IMAGE_WRITE_IMPL
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "utils/stb_image_write.h"

namespace imgcache {

namespace {

using ordered_json = nlohmann::ordered_json;

static constexpr float kSpeedMultipliers[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
static constexpr float kPi = 3.14159265358979323846f;

static inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

static inline int clampi(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}

static inline bool is_finite(float v) {
    return std::isfinite(static_cast<double>(v)) != 0;
}

static inline float closest_speed_multiplier(float value) {
    if (!is_finite(value) || value <= 0.0f) return 1.0f;
    float best = kSpeedMultipliers[0];
    float best_diff = std::fabs(best - value);
    for (size_t i = 1; i < sizeof(kSpeedMultipliers) / sizeof(kSpeedMultipliers[0]); ++i) {
        float c = kSpeedMultipliers[i];
        float d = std::fabs(c - value);
        if (d < best_diff) {
            best_diff = d;
            best = c;
        }
    }
    return best;
}

static inline bool parse_bool_like(const ordered_json& v, bool fallback) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<int>() != 0;
    if (v.is_number_float()) return std::fabs(v.get<double>()) > 0.0;
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
        if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    }
    return fallback;
}

static inline float parse_float_like(const ordered_json& v, float fallback) {
    try {
        if (v.is_number_float()) return static_cast<float>(v.get<double>());
        if (v.is_number_integer()) return static_cast<float>(v.get<long long>());
        if (v.is_string()) return std::stof(v.get<std::string>());
    } catch (...) {
    }
    return fallback;
}

static inline std::string read_text_file(const fs::path& p, std::string& err) {
    err.clear();
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        err = "Failed to open for read: " + p.string();
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in.good() && !in.eof()) {
        err = "Failed while reading: " + p.string();
        return {};
    }
    return ss.str();
}

static inline bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && !ec;
}

static inline fs::file_time_type safe_last_write_time(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return fs::file_time_type::min();
    return t;
}

static inline bool file_missing_or_stale(const fs::path& p, fs::file_time_type baseline) {
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) return true;
    auto t = fs::last_write_time(p, ec);
    if (ec) return true;
    if (baseline != fs::file_time_type::min() && t < baseline) return true;
    uintmax_t sz = fs::file_size(p, ec);
    if (ec) return true;
    return sz < 32;
}

// -----------------------------
// Minimal thread pool
// -----------------------------
class ThreadPool final {
public:
    explicit ThreadPool(std::uint32_t workers) : stop_(false) {
        if (workers < 1) workers = 1;
        threads_.reserve(workers);
        for (std::uint32_t i = 0; i < workers; ++i) {
            threads_.emplace_back([this]() { worker_loop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
    }

    void enqueue(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

    void wait_idle() {
        std::unique_lock<std::mutex> lk(mu_);
        idle_cv_.wait(lk, [this]() { return q_.empty() && active_ == 0; });
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this]() { return stop_ || !q_.empty(); });
                if (stop_ && q_.empty()) return;
                fn = std::move(q_.front());
                q_.pop_front();
                ++active_;
            }

            fn();

            {
                std::lock_guard<std::mutex> lk(mu_);
                --active_;
                if (q_.empty() && active_ == 0) {
                    idle_cv_.notify_all();
                }
            }
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;
    std::deque<std::function<void()>> q_;
    std::vector<std::thread> threads_;
    bool stop_;
    std::uint32_t active_ = 0;
};

// -----------------------------
// Image helpers
// -----------------------------
static inline std::uint8_t f2u8(float v) {
    v = clampf(v, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(v * 255.0f));
}

static inline float u82f(std::uint8_t v) {
    return static_cast<float>(v) / 255.0f;
}

static inline float sinc(float x) {
    if (std::fabs(x) < 1e-6f) return 1.0f;
    const float pix = kPi * x;
    return std::sin(pix) / pix;
}

static inline float lanczos(float x, float a) {
    x = std::fabs(x);
    if (x >= a) return 0.0f;
    return sinc(x) * sinc(x / a);
}

struct ResampleContrib {
    int src0 = 0;
    std::array<int, 8> idx{};
    std::array<float, 8> w{};
    int taps = 0;
};

static inline void build_lanczos_contribs(int src_len, int dst_len, float a,
                                         std::vector<ResampleContrib>& out) {
    out.clear();
    out.resize(dst_len);
    const float scale = static_cast<float>(dst_len) / static_cast<float>(src_len);
    const float inv_scale = static_cast<float>(src_len) / static_cast<float>(dst_len);

    // Use fixed taps for a=3 -> 6 taps. We allocate up to 8 for safety.
    const int taps = static_cast<int>(std::ceil(a * 2.0f));

    for (int dx = 0; dx < dst_len; ++dx) {
        const float center = (static_cast<float>(dx) + 0.5f) * inv_scale - 0.5f;
        int left = static_cast<int>(std::floor(center - a + 1.0f));
        ResampleContrib c;
        c.taps = taps;
        float sum = 0.0f;

        for (int k = 0; k < taps; ++k) {
            int sx = left + k;
            float w = lanczos((static_cast<float>(sx) - center), a);
            // Clamp source index
            int sxc = clampi(sx, 0, src_len - 1);
            c.idx[k] = sxc;
            c.w[k] = w;
            sum += w;
        }

        if (std::fabs(sum) < 1e-8f) sum = 1.0f;
        for (int k = 0; k < taps; ++k) c.w[k] /= sum;

        out[dx] = c;
    }
}

static std::optional<ImageRGBA> resize_lanczos_rgba(const ImageRGBA& src, int dst_w, int dst_h, std::string& err) {
    err.clear();
    if (!src.valid()) {
        err = "Resize: invalid source image";
        return std::nullopt;
    }
    if (dst_w < 1 || dst_h < 1) {
        err = "Resize: invalid dst size";
        return std::nullopt;
    }
    if (src.w == dst_w && src.h == dst_h) return src;

    const float a = 3.0f;

    // Horizontal pass: src.w -> dst_w, keep src.h
    std::vector<ResampleContrib> cx;
    build_lanczos_contribs(src.w, dst_w, a, cx);

    std::vector<float> tmp(static_cast<size_t>(dst_w) * static_cast<size_t>(src.h) * 4u, 0.0f);

    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < dst_w; ++x) {
            const ResampleContrib& c = cx[x];
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < c.taps; ++k) {
                const int sx = c.idx[k];
                const float w = c.w[k];
                const std::uint8_t* sp = &src.pixels[(static_cast<size_t>(y) * src.w + sx) * 4u];
                acc[0] += u82f(sp[0]) * w;
                acc[1] += u82f(sp[1]) * w;
                acc[2] += u82f(sp[2]) * w;
                acc[3] += u82f(sp[3]) * w;
            }
            float* dp = &tmp[(static_cast<size_t>(y) * dst_w + x) * 4u];
            dp[0] = acc[0];
            dp[1] = acc[1];
            dp[2] = acc[2];
            dp[3] = acc[3];
        }
    }

    // Vertical pass: src.h -> dst_h, width dst_w
    std::vector<ResampleContrib> cy;
    build_lanczos_contribs(src.h, dst_h, a, cy);

    ImageRGBA out;
    out.w = dst_w;
    out.h = dst_h;
    out.pixels.resize(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 4u);

    for (int y = 0; y < dst_h; ++y) {
        const ResampleContrib& c = cy[y];
        for (int x = 0; x < dst_w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < c.taps; ++k) {
                const int sy = c.idx[k];
                const float w = c.w[k];
                const float* sp = &tmp[(static_cast<size_t>(sy) * dst_w + x) * 4u];
                acc[0] += sp[0] * w;
                acc[1] += sp[1] * w;
                acc[2] += sp[2] * w;
                acc[3] += sp[3] * w;
            }
            std::uint8_t* dp = &out.pixels[(static_cast<size_t>(y) * dst_w + x) * 4u];
            dp[0] = f2u8(acc[0]);
            dp[1] = f2u8(acc[1]);
            dp[2] = f2u8(acc[2]);
            dp[3] = f2u8(acc[3]);
        }
    }

    return out;
}

static ImageRGBA crop_rgba_margins(const ImageRGBA& src, int left, int top, int right, int bottom) {
    int crop_left = std::max(0, left);
    int crop_top = std::max(0, top);
    int crop_right = std::max(0, right);
    int crop_bottom = std::max(0, bottom);

    int crop_w = std::max(1, src.w - crop_left - crop_right);
    int crop_h = std::max(1, src.h - crop_top - crop_bottom);

    const int x0 = clampi(crop_left, 0, src.w - 1);
    const int y0 = clampi(crop_top, 0, src.h - 1);

    ImageRGBA out;
    out.w = crop_w;
    out.h = crop_h;
    out.pixels.resize(static_cast<size_t>(crop_w) * static_cast<size_t>(crop_h) * 4u);

    for (int y = 0; y < crop_h; ++y) {
        int sy = clampi(y0 + y, 0, src.h - 1);
        const std::uint8_t* sp = &src.pixels[(static_cast<size_t>(sy) * src.w + x0) * 4u];
        std::uint8_t* dp = &out.pixels[(static_cast<size_t>(y) * crop_w) * 4u];
        std::memcpy(dp, sp, static_cast<size_t>(crop_w) * 4u);
    }
    return out;
}

// -----------------------------
// HSV helpers (match apply_color_effects.py math)
// -----------------------------
static inline void rgb_to_hsv(float r, float g, float b, float& h, float& s, float& v) {
    float maxc = std::max(r, std::max(g, b));
    float minc = std::min(r, std::min(g, b));
    v = maxc;
    float delta = maxc - minc;

    if (maxc > 0.0f) s = delta / (maxc == 0.0f ? 1.0f : maxc);
    else s = 0.0f;

    h = 0.0f;
    if (delta > 1e-6f) {
        if (maxc == r) {
            float hr = (g - b) / (delta == 0.0f ? 1.0f : delta);
            hr = std::fmod(hr, 6.0f);
            if (hr < 0.0f) hr += 6.0f;
            h = hr;
        } else if (maxc == g) {
            h = ((b - r) / (delta == 0.0f ? 1.0f : delta)) + 2.0f;
        } else {
            h = ((r - g) / (delta == 0.0f ? 1.0f : delta)) + 4.0f;
        }
        h = std::fmod((h / 6.0f), 1.0f);
        if (h < 0.0f) h += 1.0f;
    }
}

static inline void hsv_to_rgb(float h, float s, float v, float& r, float& g, float& b) {
    float h6 = h * 6.0f;
    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h6, 2.0f) - 1.0f));
    float m = v - c;

    float rr = 0, gg = 0, bb = 0;
    if (0.0f <= h6 && h6 < 1.0f) { rr = c; gg = x; bb = 0; }
    else if (1.0f <= h6 && h6 < 2.0f) { rr = x; gg = c; bb = 0; }
    else if (2.0f <= h6 && h6 < 3.0f) { rr = 0; gg = c; bb = x; }
    else if (3.0f <= h6 && h6 < 4.0f) { rr = 0; gg = x; bb = c; }
    else if (4.0f <= h6 && h6 < 5.0f) { rr = x; gg = 0; bb = c; }
    else { rr = c; gg = 0; bb = x; }

    r = rr + m;
    g = gg + m;
    b = bb + m;
}

static inline float sat_factor(float sat_val) {
    // Python: clamp 0..3 of 1 + 2*sat_val
    return clampf(1.0f + 2.0f * sat_val, 0.0f, 3.0f);
}

// -----------------------------
// Gaussian blur (separable), Unsharp mask (RGB only), alpha preserved
// -----------------------------
static std::vector<float> build_gaussian_kernel(float sigma) {
    sigma = std::max(0.01f, sigma);
    int radius = static_cast<int>(std::ceil(3.0f * sigma));
    int size = radius * 2 + 1;
    std::vector<float> k(size);
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        float x = static_cast<float>(i);
        float w = std::exp(-(x * x) / (2.0f * sigma * sigma));
        k[i + radius] = w;
        sum += w;
    }
    if (sum < 1e-8f) sum = 1.0f;
    for (float& v : k) v /= sum;
    return k;
}

static ImageRGBA gaussian_blur_rgb_preserve_alpha(const ImageRGBA& src, float sigma) {
    ImageRGBA out = src;
    if (!src.valid()) return out;

    std::vector<float> k = build_gaussian_kernel(sigma);
    int radius = static_cast<int>(k.size() / 2);

    // Horizontal
    std::vector<float> tmp(static_cast<size_t>(src.w) * static_cast<size_t>(src.h) * 3u, 0.0f);
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            float acc[3] = {0, 0, 0};
            for (int t = -radius; t <= radius; ++t) {
                int sx = clampi(x + t, 0, src.w - 1);
                const std::uint8_t* sp = &src.pixels[(static_cast<size_t>(y) * src.w + sx) * 4u];
                float w = k[t + radius];
                acc[0] += u82f(sp[0]) * w;
                acc[1] += u82f(sp[1]) * w;
                acc[2] += u82f(sp[2]) * w;
            }
            float* dp = &tmp[(static_cast<size_t>(y) * src.w + x) * 3u];
            dp[0] = acc[0];
            dp[1] = acc[1];
            dp[2] = acc[2];
        }
    }

    // Vertical
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            float acc[3] = {0, 0, 0};
            for (int t = -radius; t <= radius; ++t) {
                int sy = clampi(y + t, 0, src.h - 1);
                const float* sp = &tmp[(static_cast<size_t>(sy) * src.w + x) * 3u];
                float w = k[t + radius];
                acc[0] += sp[0] * w;
                acc[1] += sp[1] * w;
                acc[2] += sp[2] * w;
            }
            std::uint8_t* dp = &out.pixels[(static_cast<size_t>(y) * src.w + x) * 4u];
            dp[0] = f2u8(acc[0]);
            dp[1] = f2u8(acc[1]);
            dp[2] = f2u8(acc[2]);
            // alpha preserved already in out
        }
    }

    return out;
}

static ImageRGBA unsharp_mask_rgb_preserve_alpha(const ImageRGBA& src, float radius, float percent, int threshold_u8) {
    ImageRGBA out = src;
    if (!src.valid()) return out;

    ImageRGBA blurred = gaussian_blur_rgb_preserve_alpha(src, std::max(0.01f, radius));
    float scale = std::max(0.0f, percent) / 100.0f;
    float thresh = static_cast<float>(threshold_u8) / 255.0f;

    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            const std::uint8_t* sp = &src.pixels[(static_cast<size_t>(y) * src.w + x) * 4u];
            const std::uint8_t* bp = &blurred.pixels[(static_cast<size_t>(y) * src.w + x) * 4u];
            std::uint8_t* dp = &out.pixels[(static_cast<size_t>(y) * src.w + x) * 4u];

            for (int c = 0; c < 3; ++c) {
                float s = u82f(sp[c]);
                float b = u82f(bp[c]);
                float d = s - b;
                if (std::fabs(d) < thresh) {
                    dp[c] = sp[c];
                } else {
                    dp[c] = f2u8(s + d * scale);
                }
            }
            dp[3] = sp[3];
        }
    }

    return out;
}

static ImageRGBA apply_blur_or_sharpen_like_python(const ImageRGBA& src, float blur_val, bool is_foreground) {
    float v = clampf(blur_val, -1.0f, 1.0f);
    if (std::fabs(v) < 1e-3f) return src;

    if (v > 0.0f) {
        float max_radius = 20.0f;
        float base_radius = v * max_radius;
        if (base_radius < 1.0f) base_radius = 1.0f;

        if (is_foreground) {
            float radius = base_radius * 2.0f;
            ImageRGBA blurred = gaussian_blur_rgb_preserve_alpha(src, radius);
            float ring_radius = std::max(1.0f, radius * 0.5f);
            int ring_percent = 80;
            int ring_threshold = 3;
            ImageRGBA ring = unsharp_mask_rgb_preserve_alpha(blurred, ring_radius, static_cast<float>(ring_percent), ring_threshold);
            return ring;
        } else {
            float radius = base_radius * 1.3f;
            ImageRGBA blurred = gaussian_blur_rgb_preserve_alpha(src, radius);

            // luminance from src (already color-adjusted), build mask
            ImageRGBA mask_img;
            mask_img.w = src.w;
            mask_img.h = src.h;
            mask_img.pixels.resize(static_cast<size_t>(src.w) * static_cast<size_t>(src.h) * 4u, 0);

            for (int y = 0; y < src.h; ++y) {
                for (int x = 0; x < src.w; ++x) {
                    const std::uint8_t* sp = &src.pixels[(static_cast<size_t>(y) * src.w + x) * 4u];
                    float r = u82f(sp[0]);
                    float g = u82f(sp[1]);
                    float b = u82f(sp[2]);
                    // approximate PIL L conversion
                    float lum = 0.299f * r + 0.587f * g + 0.114f * b;
                    int L = static_cast<int>(std::lround(lum * 255.0f));
                    int m = 0;
                    if (L < 170) m = 0;
                    else m = std::min(255, (L - 170) * 3);
                    std::uint8_t* mp = &mask_img.pixels[(static_cast<size_t>(y) * src.w + x) * 4u];
                    mp[0] = mp[1] = mp[2] = static_cast<std::uint8_t>(m);
                    mp[3] = 255;
                }
            }

            float mask_sigma = std::max(1.0f, radius * 0.8f);
            ImageRGBA mask_blur = gaussian_blur_rgb_preserve_alpha(mask_img, mask_sigma);

            // bright_blurred_rgb = blurred * 1.4
            ImageRGBA bright = blurred;
            for (size_t i = 0; i < bright.pixels.size(); i += 4) {
                bright.pixels[i + 0] = f2u8(u82f(blurred.pixels[i + 0]) * 1.4f);
                bright.pixels[i + 1] = f2u8(u82f(blurred.pixels[i + 1]) * 1.4f);
                bright.pixels[i + 2] = f2u8(u82f(blurred.pixels[i + 2]) * 1.4f);
                bright.pixels[i + 3] = blurred.pixels[i + 3];
            }

            // composite(bright, blurred, mask)
            ImageRGBA out = blurred;
            for (int y = 0; y < src.h; ++y) {
                for (int x = 0; x < src.w; ++x) {
                    size_t idx = (static_cast<size_t>(y) * src.w + x) * 4u;
                    float m = u82f(mask_blur.pixels[idx]); // 0..1
                    for (int c = 0; c < 3; ++c) {
                        float a = u82f(bright.pixels[idx + c]);
                        float b = u82f(blurred.pixels[idx + c]);
                        out.pixels[idx + c] = f2u8(a * m + b * (1.0f - m));
                    }
                    out.pixels[idx + 3] = src.pixels[idx + 3]; // preserve alpha
                }
            }

            return out;
        }
    }

    // Sharpen
    float strength = -v;
    float radius = 0.7f + strength * 3.3f;
    float percent = 80.0f + strength * 220.0f;
    int threshold = 0;
    return unsharp_mask_rgb_preserve_alpha(src, radius, percent, threshold);
}

// -----------------------------
// Effects apply (matches apply_color_effects.py CPU path)
// -----------------------------
static ImageRGBA apply_color_effects_like_python(const ImageRGBA& src, const EffectsParams& fx, bool is_foreground) {
    ImageRGBA out = src;
    if (!src.valid()) return out;

    float brightness = clampf(fx.brightness, -1.0f, 1.0f);
    float contrast = clampf(fx.contrast, -1.0f, 1.0f);
    float blur = clampf(fx.blur_radius, -1.0f, 1.0f);

    float sat_r = clampf(fx.saturation_r, -1.0f, 1.0f);
    float sat_g = clampf(fx.saturation_g, -1.0f, 1.0f);
    float sat_b = clampf(fx.saturation_b, -1.0f, 1.0f);

    float hue_deg = clampf(fx.hue_shift, -180.0f, 180.0f);
    float hue_offset = hue_deg / 360.0f;

    // Color ops only where alpha > 0
    bool any_alpha = false;
    for (size_t i = 0; i < out.pixels.size(); i += 4) {
        if (out.pixels[i + 3] > 0) { any_alpha = true; break; }
    }

    const bool no_color_changes =
        (std::fabs(brightness) < 1e-6f) &&
        (std::fabs(contrast) < 1e-6f) &&
        (std::fabs(sat_r) < 1e-6f) &&
        (std::fabs(sat_g) < 1e-6f) &&
        (std::fabs(sat_b) < 1e-6f) &&
        (std::fabs(hue_deg) < 1e-6f);

    if (any_alpha && !no_color_changes) {
        for (int y = 0; y < out.h; ++y) {
            for (int x = 0; x < out.w; ++x) {
                size_t idx = (static_cast<size_t>(y) * out.w + x) * 4u;
                std::uint8_t a8 = out.pixels[idx + 3];
                if (a8 == 0) continue;

                float r = u82f(out.pixels[idx + 0]);
                float g = u82f(out.pixels[idx + 1]);
                float b = u82f(out.pixels[idx + 2]);

                if (std::fabs(brightness) > 1e-6f) {
                    r = clampf(r + brightness, 0.0f, 1.0f);
                    g = clampf(g + brightness, 0.0f, 1.0f);
                    b = clampf(b + brightness, 0.0f, 1.0f);
                }

                if (std::fabs(contrast) > 1e-6f) {
                    float c = 1.0f + contrast;
                    r = clampf((r - 0.5f) * c + 0.5f, 0.0f, 1.0f);
                    g = clampf((g - 0.5f) * c + 0.5f, 0.0f, 1.0f);
                    b = clampf((b - 0.5f) * c + 0.5f, 0.0f, 1.0f);
                }

                if (std::fabs(hue_deg) > 1e-6f) {
                    float h, s, v;
                    rgb_to_hsv(r, g, b, h, s, v);
                    h = std::fmod(h + hue_offset, 1.0f);
                    if (h < 0.0f) h += 1.0f;
                    hsv_to_rgb(h, s, v, r, g, b);
                }

                if (std::fabs(sat_r) > 1e-6f || std::fabs(sat_g) > 1e-6f || std::fabs(sat_b) > 1e-6f) {
                    float gray = (r + g + b) / 3.0f;
                    if (std::fabs(sat_r) > 1e-6f) {
                        float fr = sat_factor(sat_r);
                        r = clampf(gray + (r - gray) * fr, 0.0f, 1.0f);
                    }
                    if (std::fabs(sat_g) > 1e-6f) {
                        float fg = sat_factor(sat_g);
                        g = clampf(gray + (g - gray) * fg, 0.0f, 1.0f);
                    }
                    if (std::fabs(sat_b) > 1e-6f) {
                        float fb = sat_factor(sat_b);
                        b = clampf(gray + (b - gray) * fb, 0.0f, 1.0f);
                    }
                }

                out.pixels[idx + 0] = f2u8(r);
                out.pixels[idx + 1] = f2u8(g);
                out.pixels[idx + 2] = f2u8(b);
                // alpha preserved
            }
        }
    }

    // blur/sharpen always runs in Python (even if no color changes), but early exits internally
    out = apply_blur_or_sharpen_like_python(out, blur, is_foreground);
    return out;
}

// -----------------------------
// Animation metadata helpers
// -----------------------------

static inline float read_speed_multiplier(const ordered_json& anim_meta) {
    if (!anim_meta.is_object()) return 1.0f;
    float raw = 1.0f;
    if (anim_meta.contains("speed_multiplier")) raw = parse_float_like(anim_meta["speed_multiplier"], 1.0f);
    else if (anim_meta.contains("speed_factor")) raw = parse_float_like(anim_meta["speed_factor"], 1.0f);
    return closest_speed_multiplier(raw);
}

static inline bool read_crop_frames(const ordered_json& anim_meta) {
    if (!anim_meta.is_object()) return false;
    if (!anim_meta.contains("crop_frames")) return false;
    return parse_bool_like(anim_meta["crop_frames"], false);
}

static EffectsParams parse_effects_block(const ordered_json& manifest, const char* block_name) {
    EffectsParams fx;
    // Defaults 0.0 already

    if (!manifest.is_object()) return fx;
    if (!manifest.contains("image_effects") || !manifest["image_effects"].is_object()) return fx;

    const ordered_json& imgfx = manifest["image_effects"];
    if (!imgfx.contains(block_name) || !imgfx[block_name].is_object()) return fx;
    const ordered_json& b = imgfx[block_name];

    // Python keys:
    // brightness, contrast, blur, saturation_red, saturation_green, saturation_blue, hue
    fx.brightness = parse_float_like(b.value("brightness", 0.0), 0.0f);
    fx.contrast = parse_float_like(b.value("contrast", 0.0), 0.0f);
    fx.blur_radius = parse_float_like(b.value("blur", 0.0), 0.0f);

    fx.saturation_r = parse_float_like(b.value("saturation_red", 0.0), 0.0f);
    fx.saturation_g = parse_float_like(b.value("saturation_green", 0.0), 0.0f);
    fx.saturation_b = parse_float_like(b.value("saturation_blue", 0.0), 0.0f);

    fx.hue_shift = parse_float_like(b.value("hue", 0.0), 0.0f); // degrees [-180,180] in Python

    return fx;
}

static void update_effects_cache_like_python(const fs::path& cache_root,
                                             const ordered_json& manifest,
                                             ILogger& log) {
    // AssetTool overrides EffectsParser cache path to: <cache_root>/effects_cache.json
    // Contents shape:
    // { "foreground": { ... }, "background": { ... } }
    ordered_json snippet = ordered_json::object();
    snippet["foreground"] = ordered_json::object();
    snippet["background"] = ordered_json::object();

    auto block_to_dict = [](const ordered_json& manifest, const char* name) -> ordered_json {
        ordered_json out = ordered_json::object();
        if (!manifest.is_object() || !manifest.contains("image_effects") || !manifest["image_effects"].is_object()) {
            // default all keys to 0.0 to match EffectsParser warnings behavior
        } else {
            const ordered_json& ie = manifest["image_effects"];
            if (ie.contains(name) && ie[name].is_object()) out = ie[name];
        }
        // Ensure full key set exists (EffectsParser always writes all keys)
        const char* keys[] = {
            "brightness", "contrast", "blur",
            "saturation_red", "saturation_green", "saturation_blue",
            "hue"
        };
        for (const char* k : keys) {
            if (!out.contains(k)) out[k] = 0.0;
            else out[k] = parse_float_like(out[k], 0.0f);
        }
        return out;
    };

    snippet["foreground"] = block_to_dict(manifest, "foreground");
    snippet["background"] = block_to_dict(manifest, "background");

    fs::path cache_path = cache_root / "effects_cache.json";

    // CacheHelper sorts keys, which is fine for this cache file. Python compare_and_update_json normalizes too.
    CompareUpdateResult r = CacheHelper::CompareAndUpdateJson(nlohmann::json(snippet), cache_path.string(), true);
    if (r.wrote) log.info(std::string("Updated effects cache: ") + cache_path.string());
}

// -----------------------------
// Work execution
// -----------------------------
struct TaskOutcome {
    bool ok = true;
    std::string error;
    std::uint64_t pngs_written = 0;
    std::uint64_t pngs_skipped = 0;
};

static inline std::string to_string_variant(Variant v) {
    switch (v) {
        case Variant::Normal: return "normal";
        case Variant::Foreground: return "foreground";
        case Variant::Background: return "background";
    }
    return "normal";
}

} // namespace

// -----------------------------
// ImageCacheGenerator public API
// -----------------------------
GenResult ImageCacheGenerator::Run(const GeneratorOptions& opt, ILogger& log) {
    GenResult result;

    // Resolve manifest path
    auto manifest_path_opt = ResolveManifestPath(opt);
    if (!manifest_path_opt) {
        result.ok = false;
        result.error = "Could not locate manifest.json";
        return result;
    }
    const fs::path manifest_path = *manifest_path_opt;
    const fs::path manifest_dir = manifest_path.parent_path();

    // Read manifest as ordered_json to preserve ordering on write
    std::string err;
    std::string text = read_text_file(manifest_path, err);
    if (!err.empty()) {
        result.ok = false;
        result.error = err;
        return result;
    }

    ordered_json manifest;
    try {
        manifest = ordered_json::parse(text);
    } catch (const std::exception& e) {
        result.ok = false;
        result.error = std::string("Manifest parse failed: ") + e.what();
        return result;
    }

    const fs::file_time_type manifest_mtime = safe_last_write_time(manifest_path);
    if (!manifest.is_object()) {
        result.ok = false;
        result.error = "Manifest root is not a JSON object.";
        return result;
    }

    // Repo root is defined by python as the directory containing manifest.json (search upward).
    // The python tool defaults asset src to: <manifest_dir>/resources/assets/<asset>
    fs::path repo_root = manifest_dir;

    // Cache root
    fs::path cache_root = ResolveCacheRoot(repo_root, opt);

    // Update effects cache like python (does not influence rebuild selection in current python)
    if (!opt.dry_run) {
        update_effects_cache_like_python(cache_root, manifest, log);
    }

    // Scales: python normalize_variant_steps returns [0.75, 0.5, 0.25, 0.1]
    std::vector<int> scale_pcts = opt.scale_percents;
    if (scale_pcts.empty()) scale_pcts = {75, 50, 25, 10};
    std::sort(scale_pcts.begin(), scale_pcts.end());
    scale_pcts.erase(std::unique(scale_pcts.begin(), scale_pcts.end()), scale_pcts.end());
    std::sort(scale_pcts.begin(), scale_pcts.end(), std::greater<int>());

    // Worker count
    std::uint32_t workers = opt.worker_count_override;
    if (workers == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) hc = 4;
        workers = (hc > 1) ? (hc - 1) : 1;
    }

    // Load rebuild queue
    const fs::path rebuild_queue_path = ResolveRebuildQueuePath(cache_root);
    ordered_json rebuild_queue = LoadRebuildQueue(rebuild_queue_path);
    bool rebuild_queue_dirty = false;

    // Collect tasks
    std::vector<WorkItem> tasks;
    tasks.reserve(2048);

    // Track per-animation flagged indices to clear if everything succeeds
    struct FlagClear {
        std::string asset_name;
        std::string anim_name;
        std::vector<int> flagged;
    };
    std::vector<FlagClear> to_clear;

    std::unordered_set<std::string> assets_touched;
    std::unordered_set<std::string> anims_touched;

    const fs::path assets_root = manifest_dir / "resources" / "assets";
    std::vector<std::string> asset_names;
    if (!opt.filters.assets.empty()) {
        asset_names.assign(opt.filters.assets.begin(), opt.filters.assets.end());
    } else {
        asset_names = DiscoverAssetNames(assets_root);
    }
    if (asset_names.empty() && rebuild_queue.contains("assets") && rebuild_queue["assets"].is_object()) {
        for (auto it = rebuild_queue["assets"].begin(); it != rebuild_queue["assets"].end(); ++it) {
            asset_names.push_back(it.key());
        }
    }

    EffectsParams fx_fg = parse_effects_block(manifest, "foreground");
    EffectsParams fx_bg = parse_effects_block(manifest, "background");

    // Iterate assets
    for (const auto& asset_name : asset_names) {
        if (!opt.filters.matches_asset(asset_name)) continue;

        AssetRecord asset = BuildAssetRecord(manifest_dir, repo_root, cache_root, asset_name);
        const fs::path bundle_path = cache_root / asset_name / "bundle.bin";
        const fs::file_time_type bundle_mtime = safe_last_write_time(bundle_path);

        const ordered_json* anim_payloads = AnimationsObject(asset.meta);

        for (const auto& anim_name : asset.anim_names) {
            if (!opt.filters.matches_anim(anim_name)) continue;

            ordered_json anim_meta = ordered_json::object();
            if (anim_payloads && anim_payloads->is_object() &&
                anim_payloads->contains(anim_name) && (*anim_payloads)[anim_name].is_object()) {
                anim_meta = (*anim_payloads)[anim_name];
            }

            const fs::path anim_dir = ResolveAnimDir(asset.source_dir, anim_name, anim_meta, asset.discovered_anims);

            // Enumerate numeric frames
            std::vector<fs::path> frame_paths = EnumerateSourceFrames(anim_dir);
            if (frame_paths.empty()) {
                log.warn("No frames found for asset '" + asset_name + "' animation '" + anim_name + "' in " + anim_dir.string());
                continue;
            }
            std::vector<fs::file_time_type> frame_mtimes(frame_paths.size(), fs::file_time_type::min());
            for (size_t i = 0; i < frame_paths.size(); ++i) frame_mtimes[i] = safe_last_write_time(frame_paths[i]);

            const ordered_json* queue_anim_entry = FindAnimEntry(rebuild_queue, asset_name, anim_name, false);
            std::vector<int> flagged = queue_anim_entry ? FlaggedFrames(*queue_anim_entry) : std::vector<int>{};
            std::unordered_set<int> flagged_set(flagged.begin(), flagged.end());

            bool crop_frames = read_crop_frames(anim_meta);
            float speed_mult = read_speed_multiplier(anim_meta);
            std::vector<int> frame_sequence = BuildSpeedFrameSequence(static_cast<int>(frame_paths.size()), speed_mult);

            // Crop bounds computed once per animation (unscaled)
            std::optional<CropBounds> crop_bounds;
            if (crop_frames) {
                crop_bounds = ComputeCropBoundsForAnimation(frame_paths, log);
            }

            // For each scale, build output groups like python
            for (int pct : scale_pcts) {
                float scale_factor = static_cast<float>(pct) / 100.0f;

                int src_w0 = 0, src_h0 = 0;
                // Use first frame size to compute target size like python (PIL uses img.size after open)
                {
                    std::string e2;
                    auto img0 = LoadPngRGBA(frame_paths[0], e2);
                    if (!img0) {
                        log.error("Failed to load first frame for sizing: " + frame_paths[0].string() + " : " + e2);
                        // Schedule nothing. This is a hard failure for this animation.
                        // We will treat any load failure as global failure later.
                        result.ok = false;
                        result.error = "Failed loading required frame: " + frame_paths[0].string();
                        return result;
                    }
                    src_w0 = img0->w;
                    src_h0 = img0->h;
                }

                int target_w = std::max(1, static_cast<int>(std::lround(static_cast<float>(src_w0) * scale_factor)));
                int target_h = std::max(1, static_cast<int>(std::lround(static_cast<float>(src_h0) * scale_factor)));

                // Group output indices by source idx where rebuild needed
                std::unordered_map<int, std::vector<int>> output_groups;
                output_groups.reserve(frame_paths.size());

                for (int out_idx = 0; out_idx < static_cast<int>(frame_sequence.size()); ++out_idx) {
                    int src_idx = frame_sequence[out_idx];
                    if (src_idx < 0 || src_idx >= static_cast<int>(frame_paths.size())) continue;
                    if (!opt.filters.matches_source_frame(src_idx)) continue;

                    bool needs = opt.force_rebuild || flagged_set.count(src_idx) > 0;
                    fs::file_time_type baseline = manifest_mtime;
                    if (bundle_mtime > baseline) {
                        baseline = bundle_mtime;
                    }
                    if (src_idx >= 0 && src_idx < static_cast<int>(frame_mtimes.size())) {
                        baseline = std::max(baseline, frame_mtimes[static_cast<size_t>(src_idx)]);
                    }
                    if (!needs) {
                        // Missing output triggers rebuild
                        fs::path npath = CachePaths::frame_png_path(cache_root, asset_name, anim_name, pct, Variant::Normal, out_idx);
                        fs::path fpath = CachePaths::frame_png_path(cache_root, asset_name, anim_name, pct, Variant::Foreground, out_idx);
                        fs::path bpath = CachePaths::frame_png_path(cache_root, asset_name, anim_name, pct, Variant::Background, out_idx);
                        if (file_missing_or_stale(npath, baseline) ||
                            file_missing_or_stale(fpath, baseline) ||
                            file_missing_or_stale(bpath, baseline)) {
                            needs = true;
                        }
                    }
                    if (needs) {
                        output_groups[src_idx].push_back(out_idx);
                    }
                }

                if (output_groups.empty()) continue;

                // Ensure dirs like python
                fs::path normal_dir = CachePaths::variant_dir(cache_root, asset_name, anim_name, pct, Variant::Normal);
                fs::path fg_dir = CachePaths::variant_dir(cache_root, asset_name, anim_name, pct, Variant::Foreground);
                fs::path bg_dir = CachePaths::variant_dir(cache_root, asset_name, anim_name, pct, Variant::Background);

                if (!opt.dry_run) {
                    std::error_code ec;
                    fs::create_directories(normal_dir, ec);
                    fs::create_directories(fg_dir, ec);
                    fs::create_directories(bg_dir, ec);
                }

                std::optional<CropBounds> scaled_crop;
                if (crop_bounds) {
                    CropBounds sc = ScaleCropBounds(*crop_bounds, scale_factor);
                    scaled_crop = sc;
                }

                for (auto& kv : output_groups) {
                    int src_idx = kv.first;
                    WorkItem w;
                    w.asset_name = asset_name;
                    w.anim_name = anim_name;
                    w.src_frame_index = src_idx;
                    w.out_indices = std::move(kv.second);
                    w.scale_pct = pct;
                    w.scale_factor = scale_factor;
                    w.crop_bounds_scaled = scaled_crop;
                    w.src_png_path = frame_paths[src_idx];
                    w.out_normal_dir = normal_dir;
                    w.out_foreground_dir = fg_dir;
                    w.out_background_dir = bg_dir;
                    w.fx_foreground = fx_fg;
                    w.fx_background = fx_bg;
                    tasks.push_back(std::move(w));
                }

                assets_touched.insert(asset_name);
                anims_touched.insert(asset_name + "::" + anim_name);
            }

            // Only clear the frames that were flagged at the start, and only after full success.
            // This matches Python: it clears only flagged_frames, not missing-output rebuilds.
            if (!flagged.empty()) {
                to_clear.push_back(FlagClear{asset_name, anim_name, flagged});
            }
        }
    }

    // Dry run: report and exit
    if (opt.dry_run) {
        std::ostringstream ss;
        ss << "Dry run: " << tasks.size() << " tasks";
        log.info(ss.str());
        result.ok = true;
        result.stats.tasks_total = tasks.size();
        return result;
    }

    if (tasks.empty()) {
        result.ok = true;
        return result;
    }

    // Execute tasks in thread pool
    ThreadPool pool(workers);

    std::atomic<bool> any_fail{false};
    std::mutex err_mu;
    std::string first_error;

    std::atomic<std::uint64_t> pngs_written{0};
    std::atomic<std::uint64_t> pngs_skipped{0};
    std::atomic<std::uint64_t> tasks_ok{0};
    std::atomic<std::uint64_t> tasks_fail{0};

    for (const WorkItem& w : tasks) {
        pool.enqueue([&log, &w, &any_fail, &err_mu, &first_error, &pngs_written, &pngs_skipped, &tasks_ok, &tasks_fail]() {
            if (any_fail.load(std::memory_order_relaxed)) {
                // We still allow other tasks to finish, but no need to do extra work.
            }

            std::ostringstream hdr;
            hdr << "[" << w.asset_name << "/" << w.anim_name
                << " src=" << w.src_frame_index
                << " scale=" << w.scale_pct
                << " outs=" << w.out_indices.size() << "]";
            log.info(hdr.str());

            std::string err;
            auto src_img_opt = ImageCacheGenerator::LoadPngRGBA(w.src_png_path, err);
            if (!src_img_opt) {
                tasks_fail.fetch_add(1);
                any_fail.store(true);
                std::lock_guard<std::mutex> lk(err_mu);
                if (first_error.empty()) first_error = "Load failed: " + w.src_png_path.string() + " : " + err;
                log.error("Load failed: " + w.src_png_path.string() + " : " + err);
                return;
            }
            ImageRGBA src_img = *src_img_opt;

            auto resized_opt = resize_lanczos_rgba(src_img,
                                                  std::max(1, static_cast<int>(std::lround(src_img.w * w.scale_factor))),
                                                  std::max(1, static_cast<int>(std::lround(src_img.h * w.scale_factor))),
                                                  err);
            if (!resized_opt) {
                tasks_fail.fetch_add(1);
                any_fail.store(true);
                std::lock_guard<std::mutex> lk(err_mu);
                if (first_error.empty()) first_error = "Resize failed: " + w.src_png_path.string() + " : " + err;
                log.error("Resize failed: " + w.src_png_path.string() + " : " + err);
                return;
            }

            ImageRGBA img = *resized_opt;

            if (w.crop_bounds_scaled) {
                const CropBounds& cb = *w.crop_bounds_scaled;
                img = crop_rgba_margins(img, cb.left, cb.top, cb.right_margin, cb.bottom_margin);
            }

            // Apply effects once
            ImageRGBA fg_img = apply_color_effects_like_python(img, w.fx_foreground, true);
            ImageRGBA bg_img = apply_color_effects_like_python(img, w.fx_background, false);

            // Save all requested output indices
            for (int out_idx : w.out_indices) {
                fs::path npath = w.out_normal_dir / (std::to_string(out_idx) + ".png");
                fs::path fpath = w.out_foreground_dir / (std::to_string(out_idx) + ".png");
                fs::path bpath = w.out_background_dir / (std::to_string(out_idx) + ".png");

                // Python overwrites files for scheduled outputs, so we always write.
                std::string e1;
                if (!ImageCacheGenerator::SavePngRGBA(npath, img, e1)) {
                    tasks_fail.fetch_add(1);
                    any_fail.store(true);
                    std::lock_guard<std::mutex> lk(err_mu);
                    if (first_error.empty()) first_error = "Save failed: " + npath.string() + " : " + e1;
                    log.error("Save failed: " + npath.string() + " : " + e1);
                    return;
                }
                std::string e2;
                if (!ImageCacheGenerator::SavePngRGBA(fpath, fg_img, e2)) {
                    tasks_fail.fetch_add(1);
                    any_fail.store(true);
                    std::lock_guard<std::mutex> lk(err_mu);
                    if (first_error.empty()) first_error = "Save failed: " + fpath.string() + " : " + e2;
                    log.error("Save failed: " + fpath.string() + " : " + e2);
                    return;
                }
                std::string e3;
                if (!ImageCacheGenerator::SavePngRGBA(bpath, bg_img, e3)) {
                    tasks_fail.fetch_add(1);
                    any_fail.store(true);
                    std::lock_guard<std::mutex> lk(err_mu);
                    if (first_error.empty()) first_error = "Save failed: " + bpath.string() + " : " + e3;
                    log.error("Save failed: " + bpath.string() + " : " + e3);
                    return;
                }

                pngs_written.fetch_add(3);
            }

            tasks_ok.fetch_add(1);
        });
    }

    pool.wait_idle();

    result.stats.tasks_total = tasks.size();
    result.stats.tasks_succeeded = tasks_ok.load();
    result.stats.tasks_failed = tasks_fail.load();
    result.stats.pngs_written = pngs_written.load();
    result.stats.pngs_skipped_existing = pngs_skipped.load();
    result.stats.assets_touched = assets_touched.size();
    result.stats.animations_touched = anims_touched.size();

    if (any_fail.load()) {
        result.ok = false;
        result.error = first_error.empty() ? "One or more tasks failed." : first_error;
        // HARD RULE: do not write rebuild queue if any frame failed
        result.rebuild_queue_written = false;
        return result;
    }

    // Clear only originally-flagged frames (python behavior), after full success
    for (auto& c : to_clear) {
        ordered_json* anim_entry = FindAnimEntry(rebuild_queue, c.asset_name, c.anim_name, true);
        if (!anim_entry) {
            continue;
        }
        EnsureFramesArray(*anim_entry,
                          c.flagged.empty() ? 0 : (*std::max_element(c.flagged.begin(), c.flagged.end()) + 1));
        auto& frames = (*anim_entry)["frames"];
        for (int idx : c.flagged) {
            if (idx < 0 || idx >= static_cast<int>(frames.size())) continue;
            auto& entry = frames[idx];
            if (!entry.is_object()) entry = ordered_json::object();
            entry["needs_rebuild"] = false;
            rebuild_queue_dirty = true;
        }
    }

    if (rebuild_queue_dirty) {
        if (!SaveRebuildQueue(rebuild_queue_path, rebuild_queue)) {
            result.ok = false;
            result.error = "Failed to write rebuild queue: " + rebuild_queue_path.string();
            result.rebuild_queue_written = false;
            return result;
        }
        result.rebuild_queue_written = true;
    }

    result.ok = true;
    return result;
}

std::optional<fs::path> ImageCacheGenerator::FindRepoRootFrom(const fs::path& start_dir) {
    fs::path current = start_dir;
    for (;;) {
        fs::path candidate = current / "manifest.json";
        if (file_exists(candidate)) return current;

        fs::path parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return std::nullopt;
}

std::optional<fs::path> ImageCacheGenerator::ResolveManifestPath(const GeneratorOptions& opt) {
    if (!opt.manifest_path.empty()) {
        fs::path p = opt.manifest_path;
        if (p.filename() != "manifest.json") {
            // allow override to a direct file
        }
        if (file_exists(p)) return p;
        // Also allow "repo root" override
        if (fs::is_directory(p)) {
            fs::path candidate = p / "manifest.json";
            if (file_exists(candidate)) return candidate;
        }
        return std::nullopt;
    }

    fs::path cwd = fs::current_path();
    auto root = FindRepoRootFrom(cwd);
    if (!root) return std::nullopt;
    fs::path manifest = *root / "manifest.json";
    if (file_exists(manifest)) return manifest;
    return std::nullopt;
}

fs::path ImageCacheGenerator::ResolveCacheRoot(const fs::path& repo_root, const GeneratorOptions& opt) {
    if (!opt.cache_root_override.empty()) return opt.cache_root_override;
    return repo_root / CachePaths::kCacheDirName;
}

fs::path ImageCacheGenerator::ResolveAssetSourceDir(const fs::path& manifest_dir,
                                                    const fs::path& repo_root,
                                                    const std::string& asset_name,
                                                    const nlohmann::json& asset_obj) {
    // Python: if asset_directory missing, default is <manifest_dir>/resources/assets/<asset>
    if (asset_obj.is_object() && asset_obj.contains("asset_directory")) {
        std::string src = asset_obj["asset_directory"].is_string() ? asset_obj["asset_directory"].get<std::string>() : "";
        if (!src.empty()) {
            fs::path p(src);
            if (p.is_absolute()) return p;
            return manifest_dir / p;
        }
    }
    return manifest_dir / "resources" / "assets" / asset_name;
}

std::vector<std::pair<std::string, fs::path>> ImageCacheGenerator::DiscoverAnimations(const fs::path& asset_src_dir) {
    std::vector<std::pair<std::string, fs::path>> out;

    std::error_code ec;
    if (!fs::exists(asset_src_dir, ec) || ec) return out;

    // If there are any subdirectories, treat each as an animation.
    std::vector<fs::path> subdirs;
    for (auto& e : fs::directory_iterator(asset_src_dir, ec)) {
        if (ec) break;
        if (e.is_directory()) subdirs.push_back(e.path());
    }

    if (!subdirs.empty()) {
        std::sort(subdirs.begin(), subdirs.end());
        for (const auto& p : subdirs) {
            out.emplace_back(p.filename().string(), p);
        }
        return out;
    }

    // Otherwise single default animation in asset root
    out.emplace_back("default", asset_src_dir);
    return out;
}

std::vector<fs::path> ImageCacheGenerator::EnumerateSourceFrames(const fs::path& anim_src_dir) {
    std::vector<fs::path> frames;
    int idx = 0;
    for (;;) {
        fs::path candidate = anim_src_dir / (std::to_string(idx) + ".png");
        if (!file_exists(candidate)) break;
        frames.push_back(candidate);
        ++idx;
    }
    return frames;
}

void ImageCacheGenerator::EnsureFrameMetadata(std::vector<FrameMeta>& frames_meta, int source_frame_count) {
    if (source_frame_count < 0) source_frame_count = 0;
    if (static_cast<int>(frames_meta.size()) < source_frame_count) {
        frames_meta.resize(static_cast<size_t>(source_frame_count), FrameMeta{false});
    }
}

std::vector<int> ImageCacheGenerator::BuildSpeedFrameSequence(int source_frame_count, float speed_multiplier) {
    if (source_frame_count <= 0) return {};
    float m = closest_speed_multiplier(speed_multiplier);

    if (m < 1.0f) {
        int repeat = static_cast<int>(std::lround(1.0f / m));
        repeat = std::max(1, repeat);
        std::vector<int> seq;
        seq.reserve(static_cast<size_t>(source_frame_count) * static_cast<size_t>(repeat));
        for (int i = 0; i < source_frame_count; ++i) {
            for (int r = 0; r < repeat; ++r) seq.push_back(i);
        }
        return seq;
    }

    if (m > 1.0f) {
        int step = static_cast<int>(std::lround(m));
        step = std::max(1, step);
        std::vector<int> seq;
        for (int i = 0; i < source_frame_count; i += step) seq.push_back(i);
        if (seq.empty()) seq.push_back(0);
        int last = source_frame_count - 1;
        if (seq.back() != last) seq.push_back(last);
        return seq;
    }

    std::vector<int> seq(source_frame_count);
    for (int i = 0; i < source_frame_count; ++i) seq[i] = i;
    return seq;
}

std::optional<CropBounds> ImageCacheGenerator::ComputeCropBoundsForAnimation(const std::vector<fs::path>& src_frames,
                                                                            ILogger& log) {
    if (src_frames.empty()) return std::nullopt;

    int base_w = -1, base_h = -1;
    bool have_union = false;

    int union_left = 0, union_top = 0, union_right_excl = 0, union_bottom_excl = 0;

    for (const auto& p : src_frames) {
        std::string err;
        auto img_opt = LoadPngRGBA(p, err);
        if (!img_opt) {
            log.warn("Failed computing crop bounds for " + p.string() + ": " + err);
            return std::nullopt;
        }
        const ImageRGBA& img = *img_opt;

        if (base_w < 0) { base_w = img.w; base_h = img.h; }
        else if (img.w != base_w || img.h != base_h) {
            log.warn("Inconsistent frame sizes detected in " + p.parent_path().string() + "; skipping crop.");
            return std::nullopt;
        }

        int left = img.w, top = img.h, right = -1, bottom = -1;
        for (int y = 0; y < img.h; ++y) {
            for (int x = 0; x < img.w; ++x) {
                std::uint8_t a = img.pixels[(static_cast<size_t>(y) * img.w + x) * 4u + 3];
                if (a == 0) continue;
                if (x < left) left = x;
                if (y < top) top = y;
                if (x > right) right = x;
                if (y > bottom) bottom = y;
            }
        }

        if (right < left || bottom < top) {
            // bbox is empty, skip (python continues)
            continue;
        }

        // right/bottom are exclusive in python bbox
        int right_excl = right + 1;
        int bottom_excl = bottom + 1;

        if (!have_union) {
            have_union = true;
            union_left = left;
            union_top = top;
            union_right_excl = right_excl;
            union_bottom_excl = bottom_excl;
        } else {
            union_left = std::min(union_left, left);
            union_top = std::min(union_top, top);
            union_right_excl = std::max(union_right_excl, right_excl);
            union_bottom_excl = std::max(union_bottom_excl, bottom_excl);
        }
    }

    if (!have_union || base_w <= 0 || base_h <= 0) return std::nullopt;

    int right_margin = std::max(0, base_w - union_right_excl);
    int bottom_margin = std::max(0, base_h - union_bottom_excl);
    int cropped_w = base_w - union_left - right_margin;
    int cropped_h = base_h - union_top - bottom_margin;
    if (cropped_w <= 0 || cropped_h <= 0) return std::nullopt;

    CropBounds b;
    b.left = union_left;
    b.top = union_top;
    b.right_margin = right_margin;
    b.bottom_margin = bottom_margin;
    b.src_w = base_w;
    b.src_h = base_h;
    return b;
}

CropBounds ImageCacheGenerator::ScaleCropBounds(const CropBounds& b, float scale_factor) {
    CropBounds out = b;
    out.left = static_cast<int>(std::lround(static_cast<float>(b.left) * scale_factor));
    out.top = static_cast<int>(std::lround(static_cast<float>(b.top) * scale_factor));
    out.right_margin = static_cast<int>(std::lround(static_cast<float>(b.right_margin) * scale_factor));
    out.bottom_margin = static_cast<int>(std::lround(static_cast<float>(b.bottom_margin) * scale_factor));
    out.src_w = static_cast<int>(std::lround(static_cast<float>(b.src_w) * scale_factor));
    out.src_h = static_cast<int>(std::lround(static_cast<float>(b.src_h) * scale_factor));
    return out;
}

bool ImageCacheGenerator::OutputMissingAnyVariant(const fs::path& cache_root,
                                                 const std::string& asset_name,
                                                 const std::string& anim_name,
                                                 int scale_pct,
                                                 int out_index) {
    fs::path n = CachePaths::frame_png_path(cache_root, asset_name, anim_name, scale_pct, Variant::Normal, out_index);
    fs::path f = CachePaths::frame_png_path(cache_root, asset_name, anim_name, scale_pct, Variant::Foreground, out_index);
    fs::path b = CachePaths::frame_png_path(cache_root, asset_name, anim_name, scale_pct, Variant::Background, out_index);
    return !file_exists(n) || !file_exists(f) || !file_exists(b);
}

std::optional<ImageRGBA> ImageCacheGenerator::LoadPngRGBA(const fs::path& path, std::string& err) {
    err.clear();
    int w = 0, h = 0, comp = 0;
    stbi_uc* data = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
    if (!data) {
        err = std::string("stbi_load failed: ") + (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return std::nullopt;
    }

    ImageRGBA img;
    img.w = w;
    img.h = h;
    img.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    std::memcpy(img.pixels.data(), data, img.pixels.size());
    stbi_image_free(data);

    return img;
}

bool ImageCacheGenerator::SavePngRGBA(const fs::path& path, const ImageRGBA& img, std::string& err) {
    err.clear();
    if (!img.valid()) {
        err = "SavePngRGBA: invalid image";
        return false;
    }

    try {
        fs::create_directories(path.parent_path());
        fs::path tmp = path;
        tmp += ".tmp";

        int stride = img.w * 4;
        if (!stbi_write_png(tmp.string().c_str(), img.w, img.h, 4, img.pixels.data(), stride)) {
            err = "stbi_write_png failed for " + tmp.string();
            return false;
        }

        std::error_code ec;
        fs::rename(tmp, path, ec);
        if (!ec) return true;

        // Windows replace fallback
        std::error_code ec2;
        fs::remove(path, ec2);
        fs::rename(tmp, path, ec);
        if (ec) {
            std::error_code ec3;
            fs::remove(tmp, ec3);
            err = "Failed to replace file: " + path.string() + " (" + ec.message() + ")";
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        err = std::string("Exception during SavePngRGBA: ") + e.what();
        return false;
    }
}

std::optional<ImageRGBA> ImageCacheGenerator::ResizeRGBA(const ImageRGBA& src, int dst_w, int dst_h, std::string& err) {
    // Not used directly in this .cpp because we call the local Lanczos implementation,
    // but keep it implemented for completeness.
    return resize_lanczos_rgba(src, dst_w, dst_h, err);
}

std::optional<ImageRGBA> ImageCacheGenerator::ApplyCrop(const ImageRGBA& src, const CropBounds& b, std::string& err) {
    err.clear();
    if (!src.valid()) {
        err = "ApplyCrop: invalid src image";
        return std::nullopt;
    }
    ImageRGBA out = crop_rgba_margins(src, b.left, b.top, b.right_margin, b.bottom_margin);
    return out;
}

std::optional<ImageRGBA> ImageCacheGenerator::ApplyEffects(const ImageRGBA& src, const EffectsParams& fx, std::string& err) {
    err.clear();
    // You must decide foreground/background at call site for the blur behavior.
    // This wrapper assumes "foreground-like" when saturation_r/g/b are used, but the caller should not use this directly.
    ImageRGBA out = apply_color_effects_like_python(src, fx, false);
    return out;
}

} // namespace imgcache
