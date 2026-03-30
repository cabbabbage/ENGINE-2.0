#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace devmode::core {

class ManifestStore;

class DevSaveCoordinator {
public:
    enum class IntentKind {
        ManifestAsset,
        MapEntry,
        Tags,
        Custom,
    };

    enum class Priority {
        Debounced,
        Immediate,
    };

    struct Telemetry {
        std::uint32_t writes_this_frame = 0;
        std::uint64_t flush_count = 0;
        std::uint64_t coalesced_updates = 0;
        std::uint64_t total_intents = 0;
    };

    using NoticeSink = std::function<void(bool /*success*/, const std::string& /*message*/)>;

    DevSaveCoordinator();

    void set_manifest_store(ManifestStore* store);
    void set_notice_sink(NoticeSink sink);

    void begin_frame();
    void tick();
    void flush_now(const std::string& reason = {});

    void enqueue_manifest_asset(const std::string& asset_key,
                                nlohmann::json payload,
                                Priority priority,
                                const std::string& label,
                                std::function<void()> on_success = {});

    void enqueue_custom(IntentKind kind,
                        const std::string& key,
                        std::function<bool(ManifestStore&)> apply,
                        Priority priority,
                        const std::string& label,
                        std::function<void()> on_success = {});

    const Telemetry& telemetry() const { return telemetry_; }

private:
    struct PendingIntent {
        IntentKind kind = IntentKind::Custom;
        std::string key;
        std::string label;
        Priority priority = Priority::Debounced;
        std::function<bool(ManifestStore&)> apply;
        std::function<void()> on_success;
        std::chrono::steady_clock::time_point deadline;
    };

    using Clock = std::chrono::steady_clock;

    Clock::time_point now() const;
    void schedule_deadline(const PendingIntent& intent);
    bool process(bool force, const std::string& reason);
    void publish_notice(bool success,
                        std::size_t writes,
                        const std::vector<std::string>& labels,
                        const std::string& reason);

private:
    ManifestStore* store_ = nullptr;
    NoticeSink notice_sink_;
    std::chrono::milliseconds debounce_{120};
    std::vector<PendingIntent> intents_;
    std::unordered_map<std::string, std::size_t> index_;
    std::optional<Clock::time_point> next_deadline_;
    Telemetry telemetry_;
    bool processing_ = false;
};

}
