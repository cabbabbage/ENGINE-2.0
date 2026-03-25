// image_cache_generator.cpp
//
// Full implementation for ImageCacheGenerator (offline cache build tool).
// Mirrors the current Python pipeline behavior found in asset_tool.py and apply_color_effects.py:
//
// - Repo root discovery: search upward for manifest.json
// - Asset src resolution: default <manifest_dir>/resources/assets/<asset>, or asset_directory override
// - Animation discovery: subdirectories, else single "default"
// - Frame enumeration: 0.png, 1.png, ... stop at first missing
// - Rebuild selection: explicit runtime requests OR missing output file(s) OR force option
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

#include "d3d11_effects_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <numeric>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "utils/stb_image.h"

#include "utils/stb_image_write.h"

namespace imgcache {

namespace {

using ordered_json = nlohmann::ordered_json;
static constexpr float kPi = 3.14159265358979323846f;

static inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

static inline int clampi(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
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

static const std::vector<int>& canonical_scale_percents() {
    static const std::vector<int> kCanonicalScalePercents{100, 90, 80, 70, 60, 50, 40, 30, 20, 10};
    return kCanonicalScalePercents;
}

static bool scale_dir_is_canonical(const std::string& dir_name) {
    static const std::unordered_set<std::string> kCanonicalScaleDirs{
        "scale_100", "scale_90", "scale_80", "scale_70", "scale_60",
        "scale_50", "scale_40", "scale_30", "scale_20", "scale_10"};
    return kCanonicalScaleDirs.find(dir_name) != kCanonicalScaleDirs.end();
}

static void prune_non_canonical_scale_dirs(const fs::path& animation_cache_root, ILogger& log) {
    std::error_code ec;
    if (!fs::exists(animation_cache_root, ec) || !fs::is_directory(animation_cache_root, ec)) {
        return;
    }
    for (const auto& entry : fs::directory_iterator(animation_cache_root, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        const std::string dir_name = entry.path().filename().string();
        const std::string prefix = "scale_";
        if (dir_name.rfind(prefix, 0) != 0) {
            continue;
        }
        if (scale_dir_is_canonical(dir_name)) {
            continue;
        }
        std::error_code remove_ec;
        const auto removed = fs::remove_all(entry.path(), remove_ec);
        if (remove_ec) {
            log.warn("Failed to prune non-canonical scale directory: " + entry.path().string() +
                     " (" + remove_ec.message() + ")");
        } else if (removed > 0) {
            log.info("Pruned non-canonical scale directory: " + entry.path().string());
        }
    }
}

static std::vector<std::string> discover_asset_names_from_source_root(const fs::path& assets_root) {
    std::vector<std::string> names;
    std::error_code ec;
    if (!fs::exists(assets_root, ec) || ec || !fs::is_directory(assets_root, ec)) {
        return names;
    }

    for (const auto& entry : fs::directory_iterator(assets_root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory()) {
            continue;
        }
        const std::string asset_name = entry.path().filename().string();
        if (!asset_name.empty()) {
            names.push_back(asset_name);
        }
    }

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
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

    thread_local std::vector<float> tmp;
    const size_t tmp_size = static_cast<size_t>(dst_w) * static_cast<size_t>(src.h) * 4u;
    tmp.assign(tmp_size, 0.0f);

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
// Gaussian blur (separable), Unsharp mask (RGB), and alpha is processed too
// for blur paths so FG/BG halos are not clipped to the original alpha mask.
// -----------------------------
static std::vector<float> build_gaussian_kernel(float sigma) {
    sigma = std::max(0.01f, sigma);
    const int key = static_cast<int>(std::lround(sigma * 1000.0f));
    static std::mutex kernel_cache_mu;
    static std::unordered_map<int, std::vector<float>> kernel_cache;
    {
        std::lock_guard<std::mutex> lk(kernel_cache_mu);
        auto it = kernel_cache.find(key);
        if (it != kernel_cache.end()) {
            return it->second;
        }
    }

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

    {
        std::lock_guard<std::mutex> lk(kernel_cache_mu);
        kernel_cache[key] = k;
    }
    return k;
}

static ImageRGBA gaussian_blur_rgba(const ImageRGBA& src, float sigma) {
    ImageRGBA out = src;
    if (!src.valid()) return out;

    std::vector<float> k = build_gaussian_kernel(sigma);
    int radius = static_cast<int>(k.size() / 2);

    // Horizontal
    thread_local std::vector<float> tmp;
    const size_t tmp_size = static_cast<size_t>(src.w) * static_cast<size_t>(src.h) * 4u;
    tmp.assign(tmp_size, 0.0f);
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int t = -radius; t <= radius; ++t) {
                int sx = clampi(x + t, 0, src.w - 1);
                const std::uint8_t* sp = &src.pixels[(static_cast<size_t>(y) * src.w + sx) * 4u];
                float w = k[t + radius];
                acc[0] += u82f(sp[0]) * w;
                acc[1] += u82f(sp[1]) * w;
                acc[2] += u82f(sp[2]) * w;
                acc[3] += u82f(sp[3]) * w;
            }
            float* dp = &tmp[(static_cast<size_t>(y) * src.w + x) * 4u];
            dp[0] = acc[0];
            dp[1] = acc[1];
            dp[2] = acc[2];
            dp[3] = acc[3];
        }
    }

    // Vertical
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int t = -radius; t <= radius; ++t) {
                int sy = clampi(y + t, 0, src.h - 1);
                const float* sp = &tmp[(static_cast<size_t>(sy) * src.w + x) * 4u];
                float w = k[t + radius];
                acc[0] += sp[0] * w;
                acc[1] += sp[1] * w;
                acc[2] += sp[2] * w;
                acc[3] += sp[3] * w;
            }
            std::uint8_t* dp = &out.pixels[(static_cast<size_t>(y) * src.w + x) * 4u];
            dp[0] = f2u8(acc[0]);
            dp[1] = f2u8(acc[1]);
            dp[2] = f2u8(acc[2]);
            dp[3] = f2u8(acc[3]);
        }
    }

    return out;
}

static ImageRGBA unsharp_mask_rgb_preserve_alpha(const ImageRGBA& src, float radius, float percent, int threshold_u8) {
    ImageRGBA out = src;
    if (!src.valid()) return out;

    ImageRGBA blurred = gaussian_blur_rgba(src, std::max(0.01f, radius));
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

static ImageRGBA make_centered_transparent_2x_canvas(const ImageRGBA& src) {
    ImageRGBA out;
    if (!src.valid()) {
        return out;
    }

    out.w = std::max(1, src.w * 2);
    out.h = std::max(1, src.h * 2);
    out.pixels.assign(static_cast<size_t>(out.w) * static_cast<size_t>(out.h) * 4u, 0);

    const int offset_x = (out.w - src.w) / 2;
    const int offset_y = (out.h - src.h) / 2;
    const size_t src_row_bytes = static_cast<size_t>(src.w) * 4u;

    for (int y = 0; y < src.h; ++y) {
        const size_t src_off = static_cast<size_t>(y) * src_row_bytes;
        const size_t dst_off = (static_cast<size_t>(y + offset_y) * static_cast<size_t>(out.w)
            + static_cast<size_t>(offset_x)) * 4u;
        std::memcpy(out.pixels.data() + dst_off, src.pixels.data() + src_off, src_row_bytes);
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
            ImageRGBA blurred = gaussian_blur_rgba(src, radius);
            float ring_radius = std::max(1.0f, radius * 0.5f);
            int ring_percent = 80;
            int ring_threshold = 3;
            ImageRGBA ring = unsharp_mask_rgb_preserve_alpha(blurred, ring_radius, static_cast<float>(ring_percent), ring_threshold);
            return ring;
        } else {
            float radius = base_radius * 1.3f;
            ImageRGBA blurred = gaussian_blur_rgba(src, radius);

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
            ImageRGBA mask_blur = gaussian_blur_rgba(mask_img, mask_sigma);

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
                    float aa = u82f(bright.pixels[idx + 3]);
                    float bb = u82f(blurred.pixels[idx + 3]);
                    out.pixels[idx + 3] = f2u8(aa * m + bb * (1.0f - m));
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

static inline const char* to_string_effects_backend(EffectsBackend backend) {
    switch (backend) {
        case EffectsBackend::Auto: return "auto";
        case EffectsBackend::Cpu: return "cpu";
        case EffectsBackend::D3D11: return "d3d11";
    }
    return "auto";
}

static bool apply_effects_on_expanded_canvas(const ImageRGBA& expanded,
                                             const EffectsParams& fx,
                                             EffectLayerMode mode,
                                             bool use_d3d11_backend,
                                             ImageRGBA& out,
                                             std::string& err) {
    if (!expanded.valid()) {
        err = "Effects pipeline received invalid expanded source image.";
        return false;
    }

    if (use_d3d11_backend) {
        if (D3D11EffectsBackend::Instance().ApplyEffects(expanded, fx, mode, out, err) && out.valid()) {
            return true;
        }
        // Fall back to CPU per-call to keep generation robust even if D3D11 hits a transient runtime issue.
        err.clear();
    }

    const bool is_foreground = (mode == EffectLayerMode::Foreground);
    out = apply_color_effects_like_python(expanded, fx, is_foreground);
    if (!out.valid()) {
        err = "CPU effect pipeline produced invalid output";
        return false;
    }
    return true;
}

struct SourceFrameBatch {
    std::string asset_name;
    std::string anim_name;
    int src_frame_index = 0;
    fs::path src_png_path;
    std::vector<WorkItem> scale_items;
};

} // namespace

// -----------------------------
// ImageCacheGenerator public API
// -----------------------------
GenResult ImageCacheGenerator::Run(const GeneratorOptions& opt, ILogger& log) {
    auto sanitize_mask = [](std::uint8_t mask) -> std::uint8_t {
        return static_cast<std::uint8_t>(mask & kTextureVariantMaskAll);
    };
    auto merge_mask = [&](std::uint8_t& target, std::uint8_t mask) {
        target = sanitize_mask(static_cast<std::uint8_t>(target | sanitize_mask(mask)));
    };
    auto merge_frame_mask = [&](std::unordered_map<int, std::uint8_t>& target,
                                int frame_idx,
                                std::uint8_t mask) {
        if (frame_idx < 0) {
            return;
        }
        mask = sanitize_mask(mask);
        if (mask == kTextureVariantMaskNone) {
            return;
        }
        auto [it, inserted] = target.emplace(frame_idx, mask);
        if (!inserted) {
            merge_mask(it->second, mask);
        }
    };
    auto animations_payload_from_asset = [](const ordered_json& asset_meta) -> const ordered_json* {
        if (!asset_meta.is_object()) {
            return nullptr;
        }
        auto it = asset_meta.find("animations");
        if (it == asset_meta.end() || !it->is_object()) {
            return nullptr;
        }
        auto nested = it->find("animations");
        if (nested != it->end() && nested->is_object()) {
            return &(*nested);
        }
        return &(*it);
    };
    auto resolve_anim_dir_runtime = [&](const fs::path& asset_src_dir,
                                        const std::string& anim_name,
                                        const ordered_json& anim_meta,
                                        const std::unordered_map<std::string, fs::path>& discovered_anims) {
        if (anim_meta.is_object() && anim_meta.contains("source") && anim_meta["source"].is_object()) {
            const auto& source = anim_meta["source"];
            const std::string kind = source.value("kind", std::string{});
            if (kind == "folder") {
                const std::string path = source.value("path", anim_name);
                if (!path.empty()) {
                    fs::path p(path);
                    if (p.is_absolute()) {
                        return p;
                    }
                    return asset_src_dir / p;
                }
            }
        }
        auto discovered_it = discovered_anims.find(anim_name);
        if (discovered_it != discovered_anims.end()) {
            return discovered_it->second;
        }
        if (anim_name == "default") {
            return asset_src_dir;
        }
        return asset_src_dir / anim_name;
    };
    auto output_missing_mask = [&](const fs::path& cache_root,
                                   const std::string& asset_name,
                                   const std::string& anim_name,
                                   int scale_pct,
                                   int out_idx) {
        std::uint8_t mask = kTextureVariantMaskNone;
        if (!file_exists(CachePaths::frame_png_path(cache_root,
                                                    asset_name,
                                                    anim_name,
                                                    scale_pct,
                                                    Variant::Normal,
                                                    out_idx))) {
            merge_mask(mask, kTextureVariantMaskNormal);
        }
        if (!file_exists(CachePaths::frame_png_path(cache_root,
                                                    asset_name,
                                                    anim_name,
                                                    scale_pct,
                                                    Variant::Foreground,
                                                    out_idx))) {
            merge_mask(mask, kTextureVariantMaskForeground);
        }
        if (!file_exists(CachePaths::frame_png_path(cache_root,
                                                    asset_name,
                                                    anim_name,
                                                    scale_pct,
                                                    Variant::Background,
                                                    out_idx))) {
            merge_mask(mask, kTextureVariantMaskBackground);
        }
        return mask;
    };

    struct ExplicitAnimRequest {
        std::uint8_t all_frames_mask = kTextureVariantMaskNone;
        std::unordered_map<int, std::uint8_t> frame_masks;
    };

    GenResult result;
    const bool explicit_mode = opt.has_explicit_rebuild_requests();
    const bool missing_only_mode = opt.missing_only || (!explicit_mode && !opt.force_rebuild);

    auto manifest_path_opt = ResolveManifestPath(opt);
    if (!manifest_path_opt) {
        result.ok = false;
        result.error = "Could not locate manifest.json";
        return result;
    }
    const fs::path manifest_path = *manifest_path_opt;
    const fs::path manifest_dir = manifest_path.parent_path();
    const fs::path repo_root = manifest_dir;

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
    if (!manifest.is_object()) {
        result.ok = false;
        result.error = "Manifest root is not a JSON object.";
        return result;
    }

    const fs::path cache_root = ResolveCacheRoot(repo_root, opt);
    std::vector<int> scale_pcts = canonical_scale_percents();
    if (!opt.scale_percents.empty() && opt.scale_percents != scale_pcts) {
        log.warn("Ignoring custom scale percentages; generator enforces canonical 100..10 variants.");
    }

    std::uint32_t workers = opt.worker_count_override;
    if (workers == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) hc = 4;
        workers = (hc > 1) ? (hc - 1) : 1;
    }

    bool use_d3d11_effects = false;
    std::string d3d11_reason;
    if (!opt.dry_run) {
        if (opt.effects_backend == EffectsBackend::Auto || opt.effects_backend == EffectsBackend::D3D11) {
            use_d3d11_effects = D3D11EffectsBackend::Instance().IsAvailable(d3d11_reason);
            if (use_d3d11_effects) {
                log.info(std::string("Effects backend: d3d11 (requested ") + to_string_effects_backend(opt.effects_backend) + ")");
            } else {
                log.warn(std::string("D3D11 effects backend unavailable; falling back to CPU. Reason: ") +
                         (d3d11_reason.empty() ? std::string("unknown") : d3d11_reason));
            }
        }
        if (!use_d3d11_effects) {
            log.info(std::string("Effects backend: cpu (requested ") + to_string_effects_backend(opt.effects_backend) + ")");
        }
    }

    std::unordered_map<std::string, std::unordered_map<std::string, ExplicitAnimRequest>> explicit_requests;
    for (const auto& request : opt.explicit_rebuild_requests) {
        if (request.asset_name.empty() || request.animation_name.empty()) {
            continue;
        }
        auto& entry = explicit_requests[request.asset_name][request.animation_name];
        merge_mask(entry.all_frames_mask, request.all_frames_variant_mask);
        for (const auto& frame_pair : request.frame_variant_masks) {
            merge_frame_mask(entry.frame_masks, frame_pair.first, frame_pair.second);
        }
    }

    const ordered_json* manifest_assets = nullptr;
    if (manifest.contains("assets") && manifest["assets"].is_object()) {
        manifest_assets = &manifest["assets"];
    }

    std::vector<std::string> asset_names;
    if (explicit_mode) {
        asset_names.reserve(explicit_requests.size());
        for (const auto& entry : explicit_requests) {
            if (!entry.first.empty()) {
                asset_names.push_back(entry.first);
            }
        }
    } else if (!opt.filters.assets.empty()) {
        asset_names.assign(opt.filters.assets.begin(), opt.filters.assets.end());
    } else if (manifest_assets) {
        for (auto it = manifest_assets->begin(); it != manifest_assets->end(); ++it) {
            if (!it.key().empty()) {
                asset_names.push_back(it.key());
            }
        }
    } else {
        asset_names = discover_asset_names_from_source_root(manifest_dir / "resources" / "assets");
    }
    std::sort(asset_names.begin(), asset_names.end());
    asset_names.erase(std::unique(asset_names.begin(), asset_names.end()), asset_names.end());

    EffectsParams fx_fg = parse_effects_block(manifest, "foreground");
    EffectsParams fx_bg = parse_effects_block(manifest, "background");

    std::unordered_set<std::string> touched_assets_set;
    std::unordered_set<std::string> touched_anims_set;
    std::vector<fs::path> written_files;

    auto finalize_fail = [&](const std::string& message) -> GenResult {
        result.ok = false;
        result.error = message;
        result.stats.assets_touched = touched_assets_set.size();
        result.stats.animations_touched = touched_anims_set.size();
        result.touched_assets.assign(touched_assets_set.begin(), touched_assets_set.end());
        result.touched_animations.assign(touched_anims_set.begin(), touched_anims_set.end());
        std::sort(result.touched_assets.begin(), result.touched_assets.end());
        std::sort(result.touched_animations.begin(), result.touched_animations.end());
        result.written_files = std::move(written_files);
        return result;
    };

    struct AnimationTask {
        std::string asset_name;
        std::string anim_name;
        std::vector<fs::path> frame_paths;
        ExplicitAnimRequest explicit_request;
        bool has_explicit_request = false;
    };
    struct AnimationTaskResult {
        bool ok = true;
        std::string error;
        GenStats stats;
        bool touched_animation = false;
        std::string asset_name;
        std::string anim_name;
        std::vector<fs::path> written_files;
    };

    auto add_stats = [](GenStats& target, const GenStats& src) {
        target.tasks_total += src.tasks_total;
        target.tasks_succeeded += src.tasks_succeeded;
        target.tasks_failed += src.tasks_failed;
        target.pngs_written += src.pngs_written;
        target.pngs_skipped_existing += src.pngs_skipped_existing;
    };

    std::vector<AnimationTask> tasks;
    for (const auto& asset_name : asset_names) {
        if (!opt.filters.matches_asset(asset_name)) {
            continue;
        }

        ordered_json asset_meta = ordered_json::object();
        if (manifest_assets &&
            manifest_assets->contains(asset_name) &&
            (*manifest_assets)[asset_name].is_object()) {
            asset_meta = (*manifest_assets)[asset_name];
        }

        const fs::path asset_source_dir = ResolveAssetSourceDir(manifest_dir, repo_root, asset_name, asset_meta);
        const auto discovered_anims_list = DiscoverAnimations(asset_source_dir);
        std::unordered_map<std::string, fs::path> discovered_anims;
        for (const auto& discovered : discovered_anims_list) {
            if (!discovered.first.empty()) {
                discovered_anims.emplace(discovered.first, discovered.second);
            }
        }

        const ordered_json* anim_payloads = animations_payload_from_asset(asset_meta);
        std::vector<std::string> anim_names;
        if (anim_payloads && anim_payloads->is_object()) {
            for (auto it = anim_payloads->begin(); it != anim_payloads->end(); ++it) {
                if (!it.key().empty()) {
                    anim_names.push_back(it.key());
                }
            }
        }
        for (const auto& discovered : discovered_anims) {
            if (std::find(anim_names.begin(), anim_names.end(), discovered.first) == anim_names.end()) {
                anim_names.push_back(discovered.first);
            }
        }
        std::sort(anim_names.begin(), anim_names.end());
        anim_names.erase(std::unique(anim_names.begin(), anim_names.end()), anim_names.end());

        for (const auto& anim_name : anim_names) {
            if (!opt.filters.matches_anim(anim_name)) {
                continue;
            }

            const auto explicit_asset_it = explicit_requests.find(asset_name);
            const auto explicit_anim_it =
                explicit_asset_it == explicit_requests.end()
                    ? std::unordered_map<std::string, ExplicitAnimRequest>::const_iterator{}
                    : explicit_asset_it->second.find(anim_name);
            const bool has_explicit_request =
                explicit_asset_it != explicit_requests.end() &&
                explicit_anim_it != explicit_asset_it->second.end();

            if (explicit_mode && !has_explicit_request && !missing_only_mode && !opt.force_rebuild) {
                continue;
            }

            ordered_json anim_meta = ordered_json::object();
            if (anim_payloads &&
                anim_payloads->is_object() &&
                anim_payloads->contains(anim_name) &&
                (*anim_payloads)[anim_name].is_object()) {
                anim_meta = (*anim_payloads)[anim_name];
            }

            const fs::path anim_dir =
                resolve_anim_dir_runtime(asset_source_dir, anim_name, anim_meta, discovered_anims);
            const std::vector<fs::path> frame_paths = EnumerateSourceFrames(anim_dir);
            if (frame_paths.empty()) {
                log.warn("No frames found for asset '" + asset_name + "' animation '" + anim_name +
                         "' in " + anim_dir.string());
                continue;
            }

            AnimationTask task;
            task.asset_name = asset_name;
            task.anim_name = anim_name;
            task.frame_paths = frame_paths;
            task.has_explicit_request = has_explicit_request;
            if (has_explicit_request) {
                task.explicit_request = explicit_anim_it->second;
            }
            tasks.push_back(std::move(task));
        }
    }

    std::atomic<bool> abort_requested{false};
    std::vector<AnimationTaskResult> task_results(tasks.size());
    auto process_animation = [&](const AnimationTask& task) -> AnimationTaskResult {
        AnimationTaskResult task_result;
        task_result.asset_name = task.asset_name;
        task_result.anim_name = task.anim_name;
        if (abort_requested.load(std::memory_order_relaxed)) {
            return task_result;
        }

        std::string task_err;
        if (!opt.dry_run) {
            prune_non_canonical_scale_dirs(cache_root / task.asset_name / "animations" / task.anim_name, log);
        }

        for (int frame_idx = 0; frame_idx < static_cast<int>(task.frame_paths.size()); ++frame_idx) {
            if (!opt.filters.matches_source_frame(frame_idx) ||
                abort_requested.load(std::memory_order_relaxed)) {
                continue;
            }

            bool source_loaded = false;
            ImageRGBA source_image;
            bool expanded_loaded = false;
            ImageRGBA expanded_source;
            bool fg_ready = false;
            ImageRGBA fg_base;
            bool bg_ready = false;
            ImageRGBA bg_base;

            for (int pct : scale_pcts) {
                if (abort_requested.load(std::memory_order_relaxed)) {
                    break;
                }

                const int out_idx = frame_idx;
                std::uint8_t write_mask = kTextureVariantMaskNone;

                if (opt.force_rebuild) {
                    write_mask = kTextureVariantMaskAll;
                }
                if (task.has_explicit_request) {
                    merge_mask(write_mask, task.explicit_request.all_frames_mask);
                    auto frame_it = task.explicit_request.frame_masks.find(frame_idx);
                    if (frame_it != task.explicit_request.frame_masks.end()) {
                        merge_mask(write_mask, frame_it->second);
                    }
                }
                if (missing_only_mode) {
                    merge_mask(write_mask,
                               output_missing_mask(cache_root, task.asset_name, task.anim_name, pct, out_idx));
                }

                write_mask = sanitize_mask(write_mask);
                if (write_mask == kTextureVariantMaskNone) {
                    continue;
                }

                task_result.touched_animation = true;
                ++task_result.stats.tasks_total;

                if (opt.dry_run) {
                    ++task_result.stats.tasks_succeeded;
                    continue;
                }

                if (!source_loaded) {
                    auto source_opt = LoadPngRGBA(task.frame_paths[frame_idx], task_err);
                    if (!source_opt) {
                        ++task_result.stats.tasks_failed;
                        task_result.ok = false;
                        task_result.error = "Load failed: " + task.frame_paths[frame_idx].string() + " : " + task_err;
                        abort_requested.store(true, std::memory_order_relaxed);
                        return task_result;
                    }
                    source_image = std::move(*source_opt);
                    source_loaded = true;
                }

                const fs::path normal_dir =
                    CachePaths::variant_dir(cache_root, task.asset_name, task.anim_name, pct, Variant::Normal);
                const fs::path fg_dir =
                    CachePaths::variant_dir(cache_root, task.asset_name, task.anim_name, pct, Variant::Foreground);
                const fs::path bg_dir =
                    CachePaths::variant_dir(cache_root, task.asset_name, task.anim_name, pct, Variant::Background);
                std::error_code ec;
                fs::create_directories(normal_dir, ec);
                fs::create_directories(fg_dir, ec);
                fs::create_directories(bg_dir, ec);

                const fs::path normal_path = normal_dir / (std::to_string(out_idx) + ".png");
                const fs::path fg_path = fg_dir / (std::to_string(out_idx) + ".png");
                const fs::path bg_path = bg_dir / (std::to_string(out_idx) + ".png");

                const float scale_factor = static_cast<float>(pct) / 100.0f;
                const int normal_w = std::max(1, static_cast<int>(std::lround(source_image.w * scale_factor)));
                const int normal_h = std::max(1, static_cast<int>(std::lround(source_image.h * scale_factor)));
                const int effects_w = std::max(1, normal_w * 2);
                const int effects_h = std::max(1, normal_h * 2);

                if ((write_mask & kTextureVariantMaskNormal) != 0u) {
                    auto normal_opt = resize_lanczos_rgba(source_image, normal_w, normal_h, task_err);
                    if (!normal_opt) {
                        ++task_result.stats.tasks_failed;
                        task_result.ok = false;
                        task_result.error = "Normal resize failed: " + task.frame_paths[frame_idx].string() + " : " + task_err;
                        abort_requested.store(true, std::memory_order_relaxed);
                        return task_result;
                    }
                    if (!SavePngRGBA(normal_path, *normal_opt, task_err)) {
                        ++task_result.stats.tasks_failed;
                        task_result.ok = false;
                        task_result.error = "Save failed: " + normal_path.string() + " : " + task_err;
                        abort_requested.store(true, std::memory_order_relaxed);
                        return task_result;
                    }
                    task_result.written_files.push_back(normal_path);
                    ++task_result.stats.pngs_written;
                }

                if ((write_mask & kTextureVariantMaskForeground) != 0u) {
                    if (!fg_ready) {
                        if (!expanded_loaded) {
                            expanded_source = make_centered_transparent_2x_canvas(source_image);
                            if (!expanded_source.valid()) {
                                ++task_result.stats.tasks_failed;
                                task_result.ok = false;
                                task_result.error = "Expand canvas failed: " + task.frame_paths[frame_idx].string();
                                abort_requested.store(true, std::memory_order_relaxed);
                                return task_result;
                            }
                            expanded_loaded = true;
                        }
                        if (!apply_effects_on_expanded_canvas(expanded_source,
                                                              fx_fg,
                                                              EffectLayerMode::Foreground,
                                                              use_d3d11_effects,
                                                              fg_base,
                                                              task_err)) {
                            ++task_result.stats.tasks_failed;
                            task_result.ok = false;
                            task_result.error = "Foreground effects failed: " + task.frame_paths[frame_idx].string() + " : " + task_err;
                            abort_requested.store(true, std::memory_order_relaxed);
                            return task_result;
                        }
                        fg_ready = true;
                    }
                    ImageRGBA fg_scaled = fg_base;
                    if (pct != 100) {
                        auto fg_opt = resize_lanczos_rgba(fg_base, effects_w, effects_h, task_err);
                        if (!fg_opt) {
                            ++task_result.stats.tasks_failed;
                            task_result.ok = false;
                            task_result.error = "Foreground downscale failed: " + task.frame_paths[frame_idx].string() + " : " + task_err;
                            abort_requested.store(true, std::memory_order_relaxed);
                            return task_result;
                        }
                        fg_scaled = std::move(*fg_opt);
                    }
                    if (!SavePngRGBA(fg_path, fg_scaled, task_err)) {
                        ++task_result.stats.tasks_failed;
                        task_result.ok = false;
                        task_result.error = "Save failed: " + fg_path.string() + " : " + task_err;
                        abort_requested.store(true, std::memory_order_relaxed);
                        return task_result;
                    }
                    task_result.written_files.push_back(fg_path);
                    ++task_result.stats.pngs_written;
                }

                if ((write_mask & kTextureVariantMaskBackground) != 0u) {
                    if (!bg_ready) {
                        if (!expanded_loaded) {
                            expanded_source = make_centered_transparent_2x_canvas(source_image);
                            if (!expanded_source.valid()) {
                                ++task_result.stats.tasks_failed;
                                task_result.ok = false;
                                task_result.error = "Expand canvas failed: " + task.frame_paths[frame_idx].string();
                                abort_requested.store(true, std::memory_order_relaxed);
                                return task_result;
                            }
                            expanded_loaded = true;
                        }
                        if (!apply_effects_on_expanded_canvas(expanded_source,
                                                              fx_bg,
                                                              EffectLayerMode::Background,
                                                              use_d3d11_effects,
                                                              bg_base,
                                                              task_err)) {
                            ++task_result.stats.tasks_failed;
                            task_result.ok = false;
                            task_result.error = "Background effects failed: " + task.frame_paths[frame_idx].string() + " : " + task_err;
                            abort_requested.store(true, std::memory_order_relaxed);
                            return task_result;
                        }
                        bg_ready = true;
                    }
                    ImageRGBA bg_scaled = bg_base;
                    if (pct != 100) {
                        auto bg_opt = resize_lanczos_rgba(bg_base, effects_w, effects_h, task_err);
                        if (!bg_opt) {
                            ++task_result.stats.tasks_failed;
                            task_result.ok = false;
                            task_result.error = "Background downscale failed: " + task.frame_paths[frame_idx].string() + " : " + task_err;
                            abort_requested.store(true, std::memory_order_relaxed);
                            return task_result;
                        }
                        bg_scaled = std::move(*bg_opt);
                    }
                    if (!SavePngRGBA(bg_path, bg_scaled, task_err)) {
                        ++task_result.stats.tasks_failed;
                        task_result.ok = false;
                        task_result.error = "Save failed: " + bg_path.string() + " : " + task_err;
                        abort_requested.store(true, std::memory_order_relaxed);
                        return task_result;
                    }
                    task_result.written_files.push_back(bg_path);
                    ++task_result.stats.pngs_written;
                }

                ++task_result.stats.tasks_succeeded;
            }
        }

        return task_result;
    };

    if (tasks.size() <= 1 || workers <= 1) {
        for (std::size_t idx = 0; idx < tasks.size(); ++idx) {
            task_results[idx] = process_animation(tasks[idx]);
        }
    } else {
        const std::uint32_t worker_count = std::max<std::uint32_t>(
            1u,
            std::min<std::uint32_t>(workers, static_cast<std::uint32_t>(tasks.size())));
        ThreadPool pool(worker_count);
        for (std::size_t idx = 0; idx < tasks.size(); ++idx) {
            pool.enqueue([&, idx]() {
                task_results[idx] = process_animation(tasks[idx]);
            });
        }
        pool.wait_idle();
    }

    std::string first_error;
    for (auto& task_result : task_results) {
        add_stats(result.stats, task_result.stats);
        written_files.insert(written_files.end(),
                             std::make_move_iterator(task_result.written_files.begin()),
                             std::make_move_iterator(task_result.written_files.end()));
        if (task_result.touched_animation) {
            touched_assets_set.insert(task_result.asset_name);
            touched_anims_set.insert(task_result.asset_name + "::" + task_result.anim_name);
        }
        if (!task_result.ok && first_error.empty()) {
            first_error = task_result.error;
        }
    }

    if (!first_error.empty()) {
        return finalize_fail(first_error);
    }

    result.stats.assets_touched = touched_assets_set.size();
    result.stats.animations_touched = touched_anims_set.size();
    result.touched_assets.assign(touched_assets_set.begin(), touched_assets_set.end());
    result.touched_animations.assign(touched_anims_set.begin(), touched_anims_set.end());
    std::sort(result.touched_assets.begin(), result.touched_assets.end());
    std::sort(result.touched_animations.begin(), result.touched_animations.end());
    result.written_files = std::move(written_files);
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

    static std::once_flag png_write_tuning_once;
    std::call_once(png_write_tuning_once, []() {
        // Cache artifacts are regenerated as needed; favor encode throughput over max compression.
        stbi_write_png_compression_level = 3;
        stbi_write_force_png_filter = 0;
    });

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

std::optional<ImageRGBA> ImageCacheGenerator::ApplyEffects(const ImageRGBA& src,
                                                           const EffectsParams& fx,
                                                           EffectLayerMode mode,
                                                           std::string& err) {
    err.clear();
    if (!src.valid()) {
        err = "ApplyEffects: invalid input image";
        return std::nullopt;
    }

    ImageRGBA expanded = make_centered_transparent_2x_canvas(src);
    if (!expanded.valid()) {
        err = "ApplyEffects: failed to create centered transparent 2x canvas";
        return std::nullopt;
    }

    const bool is_foreground = (mode == EffectLayerMode::Foreground);
    ImageRGBA out = apply_color_effects_like_python(expanded, fx, is_foreground);
    if (!out.valid()) {
        err = "ApplyEffects: effect pipeline produced invalid output";
        return std::nullopt;
    }

    return out;
}

} // namespace imgcache

