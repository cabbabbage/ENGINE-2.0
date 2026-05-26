#include <cassert>

namespace {

struct AimRouterState {
    bool normal_mode_active = false;
    float yaw_degrees = 0.0f;
    float pitch_degrees = 0.0f;

    int normal_mode_aim_updates = 0;
    int dev_mode_cursor_aim_updates = 0;

    void tick(bool dev_mode, bool has_mouse_world_target, float dx, float dy, float camera_seed_yaw) {
        if (!dev_mode && !normal_mode_active) {
            yaw_degrees = camera_seed_yaw;
            pitch_degrees = 0.0f; // clear stale pitch when returning from dev mode
            normal_mode_active = true;
        } else if (dev_mode) {
            normal_mode_active = false;
        }

        if (!dev_mode) {
            yaw_degrees += dx * 0.20f;
            pitch_degrees -= dy * 0.20f;
            ++normal_mode_aim_updates;
        } else if (has_mouse_world_target) {
            ++dev_mode_cursor_aim_updates;
        }
    }
};

} // namespace

int main() {
    AimRouterState state;

    // Start in normal mode: normal aim updates should run and cursor aim should not.
    state.tick(false, true, 10.0f, -10.0f, 30.0f);
    assert(state.normal_mode_aim_updates == 1);
    assert(state.dev_mode_cursor_aim_updates == 0);
    assert(state.normal_mode_active);

    // Enter dev mode: cursor aim updates should run and normal aim updates should stop.
    const float yaw_before_dev = state.yaw_degrees;
    const float pitch_before_dev = state.pitch_degrees;
    state.tick(true, true, 999.0f, 999.0f, 60.0f);
    assert(state.normal_mode_aim_updates == 1);
    assert(state.dev_mode_cursor_aim_updates == 1);
    assert(!state.normal_mode_active);
    assert(state.yaw_degrees == yaw_before_dev);
    assert(state.pitch_degrees == pitch_before_dev);

    // Re-enter normal mode: yaw reseeds and pitch resets (no stale carryover from previous normal run).
    state.tick(false, true, 0.0f, 0.0f, 75.0f);
    assert(state.normal_mode_active);
    assert(state.yaw_degrees == 75.0f);
    assert(state.pitch_degrees == 0.0f);
    assert(state.normal_mode_aim_updates == 2);
    assert(state.dev_mode_cursor_aim_updates == 1);
}
