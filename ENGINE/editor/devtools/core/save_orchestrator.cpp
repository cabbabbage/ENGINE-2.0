#include "devtools/core/save_orchestrator.hpp"

namespace devmode::core {

void SaveOrchestrator::set_telemetry_sink(TelemetrySink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    telemetry_sink_ = std::move(sink);
}

void SaveOrchestrator::set_status_sink(StatusSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_sink_ = std::move(sink);
}

SaveOrchestrator::Result SaveOrchestrator::save(const Request& request) {
    std::lock_guard<std::mutex> lock(mutex_);

    Result result;
    if (request.document_id.empty() || !request.atomic_write) {
        result.message = "Save failed";
        emit_status(Status::SaveFailed, result.message);
        emit_telemetry(request, Status::SaveFailed, result);
        return result;
    }

    emit_status(Status::Saving, "Saving...");

    if (request.readonly) {
        result.message = "Save failed: readonly";
        emit_status(Status::SaveFailed, result.message);
        emit_telemetry(request, Status::SaveFailed, result);
        return result;
    }

    if (request.conflict_check && request.conflict_check()) {
        result.conflict = true;
        result.message = "Conflict detected";
        emit_status(Status::ConflictDetected, result.message);
        emit_telemetry(request, Status::ConflictDetected, result);
        return result;
    }

    if (request.disk_available_check && !request.disk_available_check()) {
        result.message = "Save failed: disk unavailable";
        emit_status(Status::SaveFailed, result.message);
        emit_telemetry(request, Status::SaveFailed, result);
        return result;
    }

    if (request.formatter_or_lint) {
        const bool formatter_ok = request.formatter_or_lint(request.formatter_timeout);
        if (!formatter_ok) {
            result.formatter_failed = true;
            if (!request.formatter_non_blocking) {
                result.message = "Save failed: formatter";
                emit_status(Status::SaveFailed, result.message);
                emit_telemetry(request, Status::SaveFailed, result);
                return result;
            }
        }
    }

    const std::size_t before_checksum = request.checksum ? request.checksum() : 0u;

    if (!request.atomic_write()) {
        result.message = "Save failed";
        emit_status(Status::SaveFailed, result.message);
        emit_telemetry(request, Status::SaveFailed, result);
        return result;
    }

    const std::size_t after_checksum = request.checksum ? request.checksum() : before_checksum;
    if (request.checksum && before_checksum != after_checksum) {
        result.verification_failed = true;
        result.message = "Save failed: checksum mismatch";
        emit_status(Status::SaveFailed, result.message);
        emit_telemetry(request, Status::SaveFailed, result);
        return result;
    }

    result.success = true;
    result.version = ++version_counter_;
    result.message = "Saved";
    if (request.on_version_increment) {
        request.on_version_increment(result.version);
    }

    emit_status(Status::Saved, result.message);
    emit_telemetry(request, Status::Saved, result);
    return result;
}

const char* SaveOrchestrator::reason_to_string(Reason reason) {
    switch (reason) {
        case Reason::AutoSave:
            return "autosave";
        case Reason::FocusChange:
            return "focus-change";
        case Reason::HotReload:
            return "hot-reload";
        case Reason::StateChange:
            return "state-change";
    }
    return "state-change";
}

void SaveOrchestrator::emit_status(Status status, const std::string& message) const {
    if (status_sink_) {
        status_sink_(status, message);
    }
}

void SaveOrchestrator::emit_telemetry(const Request& request,
                                      Status status,
                                      const Result& result) const {
    if (telemetry_sink_) {
        telemetry_sink_(request.document_id, request.reason, status, result);
    }
}

}
