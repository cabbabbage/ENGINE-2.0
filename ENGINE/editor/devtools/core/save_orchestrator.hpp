#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace devmode::core {

class SaveOrchestrator {
public:
    enum class Reason {
        AutoSave,
        FocusChange,
        HotReload,
        StateChange,
    };

    enum class Status {
        Saving,
        Saved,
        SaveFailed,
        ConflictDetected,
    };

    struct Request {
        std::string document_id;
        Reason reason = Reason::StateChange;
        bool readonly = false;
        std::function<bool()> conflict_check;
        std::function<bool()> disk_available_check;
        std::function<bool(std::chrono::milliseconds timeout)> formatter_or_lint;
        bool formatter_non_blocking = true;
        std::chrono::milliseconds formatter_timeout{60};
        std::function<bool()> atomic_write;
        std::function<std::size_t()> checksum;
        std::function<void(std::uint64_t)> on_version_increment;
    };

    struct Result {
        bool success = false;
        bool conflict = false;
        bool formatter_failed = false;
        bool verification_failed = false;
        std::uint64_t version = 0;
        std::string message;
    };

    using TelemetrySink = std::function<void(const std::string&, Reason, Status, const Result&)>;
    using StatusSink = std::function<void(Status, const std::string&)>;

    void set_telemetry_sink(TelemetrySink sink);
    void set_status_sink(StatusSink sink);

    Result save(const Request& request);

    static const char* reason_to_string(Reason reason);

private:
    void emit_status(Status status, const std::string& message) const;
    void emit_telemetry(const Request& request, Status status, const Result& result) const;

    mutable std::mutex mutex_;
    std::atomic<std::uint64_t> version_counter_{0};
    TelemetrySink telemetry_sink_;
    StatusSink status_sink_;
};

}
