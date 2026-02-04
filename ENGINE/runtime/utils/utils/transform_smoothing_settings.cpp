#include "transform_smoothing_settings.hpp"

#include <mutex>
#include <string>
#include <cmath>

#include "devtools/dev_ui_settings.hpp"

namespace transform_smoothing {

namespace {

struct CachedParams {
    TransformSmoothingParams asset_alpha{};
    bool initialized = false;
};

CachedParams& cache() {
    static CachedParams cached;
    return cached;
}

std::mutex& cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

TransformSmoothingMethod clamp_method(int raw, TransformSmoothingMethod fallback) {
    switch (raw) {
    case static_cast<int>(TransformSmoothingMethod::None): return TransformSmoothingMethod::None;
    case static_cast<int>(TransformSmoothingMethod::Lerp): return TransformSmoothingMethod::Lerp;
    case static_cast<int>(TransformSmoothingMethod::CriticallyDampedSpring): return TransformSmoothingMethod::CriticallyDampedSpring;
    default:
        return fallback;
    }
}

TransformSmoothingParams sanitized(const TransformSmoothingParams& params) {
    TransformSmoothingParams result = params;
    if (!std::isfinite(result.lerp_rate) || result.lerp_rate < 0.0f) {
        result.lerp_rate = 0.0f;
    }
    if (!std::isfinite(result.spring_frequency) || result.spring_frequency < 0.0f) {
        result.spring_frequency = 0.0f;
    }
    if (!std::isfinite(result.max_step) || result.max_step < 0.0f) {
        result.max_step = 0.0f;
    }
    if (!std::isfinite(result.snap_threshold) || result.snap_threshold < 0.0f) {
        result.snap_threshold = 0.0f;
    }
    return result;
}

TransformSmoothingParams load_params(std::string_view prefix,
                                     const TransformSmoothingParams& defaults) {
    TransformSmoothingParams params = defaults;

    const std::string base(prefix);
    const std::string method_key       = base + ".method";
    const std::string lerp_key         = base + ".lerp_rate";
    const std::string spring_key       = base + ".spring_frequency";
    const std::string max_step_key     = base + ".max_step";
    const std::string snap_key         = base + ".snap_threshold";

    const int method_value = static_cast<int>(devmode::ui_settings::load_number( method_key, static_cast<int>(defaults.method)));
    params.method          = clamp_method(method_value, defaults.method);

    params.lerp_rate        = static_cast<float>(devmode::ui_settings::load_number( lerp_key, defaults.lerp_rate));
    params.spring_frequency = static_cast<float>(devmode::ui_settings::load_number( spring_key, defaults.spring_frequency));
    params.max_step         = static_cast<float>(devmode::ui_settings::load_number( max_step_key, defaults.max_step));
    params.snap_threshold   = static_cast<float>(devmode::ui_settings::load_number( snap_key, defaults.snap_threshold));

    params = sanitized(params);

    devmode::ui_settings::save_number(method_key, static_cast<int>(params.method));
    devmode::ui_settings::save_number(lerp_key, params.lerp_rate);
    devmode::ui_settings::save_number(spring_key, params.spring_frequency);
    devmode::ui_settings::save_number(max_step_key, params.max_step);
    devmode::ui_settings::save_number(snap_key, params.snap_threshold);

    return params;
}

void ensure_loaded() {
    auto& cached = cache();
    if (cached.initialized) {
        return;
    }

    cached.asset_alpha = load_params(
        "render.smoothing.asset.alpha",
        TransformSmoothingParams{
            TransformSmoothingMethod::Lerp,
            8.0f,
            0.0f,
            2.0f,
            0.01f});

    cached.initialized = true;
}

void store_params(std::string_view prefix, const TransformSmoothingParams& params) {
    const std::string base(prefix);
    const std::string method_key       = base + ".method";
    const std::string lerp_key         = base + ".lerp_rate";
    const std::string spring_key       = base + ".spring_frequency";
    const std::string max_step_key     = base + ".max_step";
    const std::string snap_key         = base + ".snap_threshold";

    devmode::ui_settings::save_number(method_key, static_cast<int>(params.method));
    devmode::ui_settings::save_number(lerp_key, params.lerp_rate);
    devmode::ui_settings::save_number(spring_key, params.spring_frequency);
    devmode::ui_settings::save_number(max_step_key, params.max_step);
    devmode::ui_settings::save_number(snap_key, params.snap_threshold);
}

}

const TransformSmoothingParams& asset_alpha_params() {
    std::lock_guard<std::mutex> lock(cache_mutex());
    ensure_loaded();
    return cache().asset_alpha;
}

void set_asset_alpha_params(const TransformSmoothingParams& params) {
    std::lock_guard<std::mutex> lock(cache_mutex());
    ensure_loaded();
    cache().asset_alpha = sanitized(params);
    store_params("render.smoothing.asset.alpha", cache().asset_alpha);
}

void reload_from_settings() {
    std::lock_guard<std::mutex> lock(cache_mutex());
    cache().initialized = false;
    ensure_loaded();
}

}
