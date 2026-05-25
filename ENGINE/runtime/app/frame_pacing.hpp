#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

namespace app::frame_pacing {

inline constexpr double kDefaultRuntimeFps = 60.0;
inline constexpr double kDeterministicFps = 24.0;
inline constexpr double kMinTargetFps = 30.0;
inline constexpr double kMaxTargetFps = 240.0;

inline bool env_flag_enabled(const char* name, bool default_value) {
    if (!name || !*name) {
        return default_value;
    }
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return default_value;
    }

    std::string value(raw);
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value == "1" || value == "true" || value == "yes" || value == "on" || value == "y" || value == "t") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off" || value == "n" || value == "f") {
        return false;
    }
    return default_value;
}

inline double target_fps() {
    static const double fps = []() {
        const bool deterministic_mode = env_flag_enabled("VIBBLE_CODEX_PLAYTEST_INPUT", false) ||
                                        env_flag_enabled("CI", false);
        const double default_fps = deterministic_mode ? kDeterministicFps : kDefaultRuntimeFps;
        const char* raw_target_fps = std::getenv("VIBBLE_TARGET_FPS");
        if (!raw_target_fps || !*raw_target_fps) {
            return std::clamp(default_fps, kMinTargetFps, kMaxTargetFps);
        }
        try {
            const double requested_fps = std::stod(raw_target_fps);
            return std::clamp(requested_fps, kMinTargetFps, kMaxTargetFps);
        } catch (...) {
            return std::clamp(default_fps, kMinTargetFps, kMaxTargetFps);
        }
    }();
    return fps;
}

inline double target_frame_seconds() {
    return 1.0 / target_fps();
}

inline double target_frame_counts(double perf_frequency) {
    return target_frame_seconds() * perf_frequency;
}

inline double remaining_frame_counts(Uint64 frame_begin,
                                     double target_counts,
                                     double perf_frequency) {
    if (perf_frequency <= 0.0) {
        return 0.0;
    }
    const Uint64 frame_end = SDL_GetPerformanceCounter();
    const double work_counts = static_cast<double>(frame_end - frame_begin);
    return target_counts - work_counts;
}

inline void delay_from_remaining_counts(double remaining_counts,
                                        double perf_frequency) {
    if (remaining_counts <= 0.0 || perf_frequency <= 0.0) {
        return;
    }
    const double remaining_ms = (remaining_counts * 1000.0) / perf_frequency;
    if (remaining_ms >= 1.0) {
        SDL_Delay(static_cast<Uint32>(remaining_ms));
    }
}

inline std::string target_summary() {
    std::ostringstream oss;
    const double effective_fps = target_fps();
    oss << std::fixed << std::setprecision(2) << effective_fps
        << " FPS (" << std::setprecision(3)
        << (target_frame_seconds() * 1000.0) << " ms/frame)";
    return oss.str();
}

} // namespace app::frame_pacing
