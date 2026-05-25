#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace runtime_stats {

class FrameStatsRecorder {
public:
    // Temporary kill switch for runtime frame-stats CSV recording.
    // Re-enable by flipping this back to `true`.
    static constexpr bool kRuntimeFrameStatsDebugEnabled = false;

    class ScopedTimer {
    public:
        explicit ScopedTimer(std::string metric);
        ~ScopedTimer();

        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;

    private:
        std::string metric_;
        Uint64 begin_counter_ = 0;
        bool active_ = false;
    };

    static FrameStatsRecorder& instance();

    bool enabled() const;
    void begin_run();
    void begin_run(const std::filesystem::path& output_path);
    void shutdown();

    void begin_frame(std::uint64_t frame_id);
    void end_frame();
    void flush();
    void mark_stage(const char* stage, bool idle_wait = false);

    void set(const std::string& metric, const std::string& value);
    void set(const std::string& metric, const char* value);
    void set(const std::string& metric, bool value);
    void set(const std::string& metric, int value);
    void set(const std::string& metric, std::uint32_t value);
    void set(const std::string& metric, std::uint64_t value);
    void set(const std::string& metric, double value);
    void add(const std::string& metric, double value);

    static double elapsed_ms(Uint64 begin_counter, Uint64 end_counter);

private:
    FrameStatsRecorder() = default;
};

} // namespace runtime_stats
