#include "utils/frame_stats_recorder.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace runtime_stats {
namespace {

constexpr std::size_t kFlushIntervalFrames = 120;

struct FrameSnapshot {
    std::uint64_t frame_id = 0;
    std::unordered_map<std::string, std::string> values;
};

struct RecorderState {
    std::mutex mutex;
    std::filesystem::path output_path;
    std::vector<FrameSnapshot> frames;
    std::vector<std::string> metric_order;
    std::unordered_set<std::string> known_metrics;
    FrameSnapshot current_frame;
    bool started = false;
    bool frame_open = false;
};

RecorderState& state() {
    static RecorderState s;
    return s;
}

std::filesystem::path default_output_path() {
#if defined(PROJECT_ROOT)
    return std::filesystem::path(PROJECT_ROOT) / "runtime_frame_stats.csv";
#else
    return std::filesystem::path("ENGINE") / "runtime_frame_stats.csv";
#endif
}

void remember_metric_locked(RecorderState& s, const std::string& metric) {
    if (metric.empty()) {
        return;
    }
    if (s.known_metrics.insert(metric).second) {
        s.metric_order.push_back(metric);
    }
}

std::string escape_csv(const std::string& value) {
    const bool needs_quotes =
        value.find_first_of(",\"\r\n") != std::string::npos;
    if (!needs_quotes) {
        return value;
    }

    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char ch : value) {
        if (ch == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

std::string number_to_string(double value) {
    if (!std::isfinite(value)) {
        return {};
    }
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss << std::setprecision(6) << value;
    std::string out = ss.str();
    while (!out.empty() && out.back() == '0') {
        out.pop_back();
    }
    if (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    return out.empty() ? std::string{"0"} : out;
}

void write_csv_locked(RecorderState& s) {
    if (!s.started || s.output_path.empty()) {
        return;
    }

    std::error_code ec;
    const std::filesystem::path parent = s.output_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        ec.clear();
    }

    const std::filesystem::path tmp_path = s.output_path.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::out | std::ios::trunc);
        if (!out.good()) {
            return;
        }

        out << "metric";
        for (const FrameSnapshot& frame : s.frames) {
            out << ",frame_" << frame.frame_id;
        }
        out << '\n';

        for (const std::string& metric : s.metric_order) {
            out << escape_csv(metric);
            for (const FrameSnapshot& frame : s.frames) {
                out << ',';
                const auto it = frame.values.find(metric);
                if (it != frame.values.end()) {
                    out << escape_csv(it->second);
                }
            }
            out << '\n';
        }
    }

    std::filesystem::rename(tmp_path, s.output_path, ec);
    if (ec) {
        ec.clear();
        std::filesystem::remove(s.output_path, ec);
        ec.clear();
        std::filesystem::rename(tmp_path, s.output_path, ec);
    }
}

void set_locked(RecorderState& s, const std::string& metric, std::string value) {
    if (!s.started || !s.frame_open || metric.empty()) {
        return;
    }
    remember_metric_locked(s, metric);
    s.current_frame.values[metric] = std::move(value);
}

} // namespace

FrameStatsRecorder::ScopedTimer::ScopedTimer(std::string metric)
    : metric_(std::move(metric)) {
    if (!FrameStatsRecorder::instance().enabled() || metric_.empty()) {
        return;
    }
    begin_counter_ = SDL_GetPerformanceCounter();
    active_ = true;
}

FrameStatsRecorder::ScopedTimer::~ScopedTimer() {
    if (!active_) {
        return;
    }
    const double ms = FrameStatsRecorder::elapsed_ms(begin_counter_, SDL_GetPerformanceCounter());
    FrameStatsRecorder::instance().add(metric_, ms);
}

FrameStatsRecorder& FrameStatsRecorder::instance() {
    static FrameStatsRecorder recorder;
    return recorder;
}

bool FrameStatsRecorder::enabled() const {
    return kRuntimeFrameStatsDebugEnabled;
}

void FrameStatsRecorder::begin_run() {
    begin_run(default_output_path());
}

void FrameStatsRecorder::begin_run(const std::filesystem::path& output_path) {
    if (!enabled()) {
        return;
    }

    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.output_path = output_path;
    s.frames.clear();
    s.metric_order.clear();
    s.known_metrics.clear();
    s.current_frame = FrameSnapshot{};
    s.started = true;
    s.frame_open = false;

    std::error_code ec;
    const std::filesystem::path parent = s.output_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream clear_file(s.output_path, std::ios::out | std::ios::trunc);
}

void FrameStatsRecorder::shutdown() {
    if (!enabled()) {
        return;
    }

    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.frame_open) {
        s.frames.push_back(std::move(s.current_frame));
        s.current_frame = FrameSnapshot{};
        s.frame_open = false;
    }
    write_csv_locked(s);
    s.started = false;
}

void FrameStatsRecorder::begin_frame(std::uint64_t frame_id) {
    if (!enabled()) {
        return;
    }

    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started) {
        s.output_path = default_output_path();
        s.started = true;
    }
    if (s.frame_open) {
        s.frames.push_back(std::move(s.current_frame));
    }
    s.current_frame = FrameSnapshot{};
    s.current_frame.frame_id = frame_id;
    s.frame_open = true;
}

void FrameStatsRecorder::end_frame() {
    if (!enabled()) {
        return;
    }

    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started || !s.frame_open) {
        return;
    }
    s.frames.push_back(std::move(s.current_frame));
    s.current_frame = FrameSnapshot{};
    s.frame_open = false;
    if ((s.frames.size() % kFlushIntervalFrames) == 0) {
        write_csv_locked(s);
    }
}

void FrameStatsRecorder::flush() {
    if (!enabled()) {
        return;
    }

    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    write_csv_locked(s);
}

void FrameStatsRecorder::set(const std::string& metric, const std::string& value) {
    if (!enabled()) {
        return;
    }
    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    set_locked(s, metric, value);
}

void FrameStatsRecorder::set(const std::string& metric, const char* value) {
    set(metric, value ? std::string(value) : std::string{});
}

void FrameStatsRecorder::set(const std::string& metric, bool value) {
    set(metric, value ? std::string{"1"} : std::string{"0"});
}

void FrameStatsRecorder::set(const std::string& metric, int value) {
    set(metric, std::to_string(value));
}

void FrameStatsRecorder::set(const std::string& metric, std::uint32_t value) {
    set(metric, std::to_string(value));
}

void FrameStatsRecorder::set(const std::string& metric, std::uint64_t value) {
    set(metric, std::to_string(value));
}

void FrameStatsRecorder::set(const std::string& metric, double value) {
    set(metric, number_to_string(value));
}

void FrameStatsRecorder::add(const std::string& metric, double value) {
    if (!enabled() || metric.empty() || !std::isfinite(value)) {
        return;
    }

    RecorderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started || !s.frame_open) {
        return;
    }
    remember_metric_locked(s, metric);
    double current = 0.0;
    const auto it = s.current_frame.values.find(metric);
    if (it != s.current_frame.values.end()) {
        try {
            current = std::stod(it->second);
        } catch (...) {
            current = 0.0;
        }
    }
    s.current_frame.values[metric] = number_to_string(current + value);
}

double FrameStatsRecorder::elapsed_ms(Uint64 begin_counter, Uint64 end_counter) {
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0 || end_counter <= begin_counter) {
        return 0.0;
    }
    return (static_cast<double>(end_counter - begin_counter) * 1000.0) /
           static_cast<double>(frequency);
}

} // namespace runtime_stats
