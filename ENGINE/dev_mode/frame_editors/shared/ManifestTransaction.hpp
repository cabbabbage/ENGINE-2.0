#pragma once

#include <functional>

#include "FrameEditorContext.hpp"

namespace devmode::frame_editors {

class ManifestTransaction {
public:
    using ApplyCallback = std::function<bool()>;
    using ValidateCallback = std::function<bool()>;
    using CommitCallback = std::function<bool()>;
    using RollbackCallback = std::function<void()>;

    void begin(const FrameEditorContext& context) {
        context_ = &context;
        active_ = true;
    }

    void set_apply_callback(ApplyCallback cb) { apply_ = std::move(cb); }
    void set_validate_callback(ValidateCallback cb) { validate_ = std::move(cb); }
    void set_commit_callback(CommitCallback cb) { commit_ = std::move(cb); }
    void set_rollback_callback(RollbackCallback cb) { rollback_ = std::move(cb); }
    void set_immediate_persist(bool immediate) { immediate_persist_ = immediate; }

    bool immediate_persist() const { return immediate_persist_; }

    bool apply_mode_edits() {
        if (apply_) return apply_();
        return true;
    }

    bool validate() {
        if (validate_) return validate_();
        return true;
    }

    bool commit() {
        if (!active_) return false;
        if (commit_) return commit_();
        return apply_mode_edits() && validate();
    }

    void rollback() {
        if (rollback_) rollback_();
        active_ = false;
    }

    bool active() const { return active_; }
    const FrameEditorContext* context() const { return context_; }

private:
    const FrameEditorContext* context_ = nullptr;
    ApplyCallback apply_;
    ValidateCallback validate_;
    CommitCallback commit_;
    RollbackCallback rollback_;
    bool active_ = false;
    bool immediate_persist_ = false;
};

}  // namespace devmode::frame_editors
