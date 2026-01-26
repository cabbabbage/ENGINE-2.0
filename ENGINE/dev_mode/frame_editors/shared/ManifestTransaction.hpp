#pragma once

#include <functional>

#include "FrameEditorContext.hpp"
#include "dev_mode/asset_sections/animation_editor_window/AnimationDocument.hpp"

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
    void set_deferred_persist(bool deferred) { deferred_persist_ = deferred; }

    bool immediate_persist() const { return immediate_persist_; }
    bool deferred_persist() const { return deferred_persist_; }

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
        // Guard against re-entrancy - prevent recursive commits
        if (committing_) return true;
        committing_ = true;

        bool result = false;
        if (commit_) {
            result = commit_();
        } else {
            // For deferred persistence, apply changes and save immediately to disk
            if (deferred_persist_ && apply_) {
                if (!apply_()) {
                    committing_ = false;
                    return false;
                }
                if (validate_ && !validate_()) {
                    committing_ = false;
                    return false;
                }
                // Force immediate save to disk for deferred mode
                if (context_ && context_->document) {
                    result = context_->document->save_to_file_checked(true);
                } else {
                    result = true;
                }
            } else {
                // Regular commit behavior
                result = apply_mode_edits() && validate();
            }
        }

        committing_ = false;
        return result;
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
    bool deferred_persist_ = false;
    bool committing_ = false;  // Re-entrancy guard
};

}  // namespace devmode::frame_editors
