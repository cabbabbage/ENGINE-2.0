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
        return false;
    }
    if (runtime_updates_enabled) {
        return true;
    }
    return is_frame_editor_target;
}

inline bool should_allow_movement_for_asset(bool dev_mode) {
    return !dev_mode;
}

} // namespace runtime::dev_mode_policy
