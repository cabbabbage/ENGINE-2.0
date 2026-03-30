#pragma once

namespace runtime::dev_mode_policy {

inline bool should_advance_animation_for_asset(bool dev_mode,
                                               bool runtime_updates_enabled,
                                               bool frame_editor_session_active,
                                               bool is_frame_editor_target) {
    if (!dev_mode) {
        return true;
    }

    if (frame_editor_session_active) {
        return is_frame_editor_target;
    }

    return runtime_updates_enabled;
}

} // namespace runtime::dev_mode_policy
