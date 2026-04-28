#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>

namespace app::frame_pacing {

inline constexpr double kTargetFps = 24.0;
inline constexpr double kTargetFrameSeconds = 1.0 / kTargetFps;

inline double target_frame_counts(double perf_frequency) {
    return kTargetFrameSeconds * perf_frequency;
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
    oss << std::fixed << std::setprecision(2) << kTargetFps
        << " FPS (" << std::setprecision(3)
        << (kTargetFrameSeconds * 1000.0) << " ms/frame)";
    return oss.str();
}

} // namespace app::frame_pacing
