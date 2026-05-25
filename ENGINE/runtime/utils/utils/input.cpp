#include "utils/input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include "utils/frame_stats_recorder.hpp"

namespace {
Input::Button to_button(Uint8 sdl_button) {
    switch (sdl_button) {
    case SDL_BUTTON_LEFT:   return Input::LEFT;
    case SDL_BUTTON_RIGHT:  return Input::RIGHT;
    case SDL_BUTTON_MIDDLE: return Input::MIDDLE;
    case SDL_BUTTON_X1:     return Input::X1;
    case SDL_BUTTON_X2:     return Input::X2;
    default:                return Input::COUNT;
    }
}

bool live_scancode_down(const bool* live_keys, int key_count, SDL_Scancode sc) {
    return live_keys && sc >= 0 && sc < key_count && live_keys[sc];
}

void record_key_metric(runtime_stats::FrameStatsRecorder& frame_stats,
                       const char* prefix,
                       const char* name,
                       bool value) {
    frame_stats.set(std::string(prefix) + name, value);
}
}

void Input::handleEvent(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_MINIMIZED:
        keyboard_focus_active_ = false;
        clear_all_state();
        break;

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        keyboard_focus_active_ = true;
        break;

    case SDL_EVENT_MOUSE_MOTION:
        dx_ = static_cast<int>(std::lround(e.motion.xrel));
        dy_ = static_cast<int>(std::lround(e.motion.yrel));
        x_ = static_cast<int>(std::lround(e.motion.x));
        y_ = static_cast<int>(std::lround(e.motion.y));
        mouse_motion_dirty_ = true;
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        x_ = static_cast<int>(std::lround(e.button.x));
        y_ = static_cast<int>(std::lround(e.button.y));
        bool down = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        Button button = to_button(e.button.button);
        if (button != COUNT) {
            buttons_[button] = down;
            button_state_dirty_ = true;
            if (!down) {

                clickBuffer_[button] = 3;
                click_buffer_active_ = true;
            }
        }
        break;
    }

    case SDL_EVENT_MOUSE_WHEEL:
        {
            x_ = static_cast<int>(std::lround(e.wheel.mouse_x));
            y_ = static_cast<int>(std::lround(e.wheel.mouse_y));
            int wheel_x = e.wheel.integer_x;
            int wheel_y = e.wheel.integer_y;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                wheel_x = -wheel_x;
                wheel_y = -wheel_y;
            }
            scrollX_ += wheel_x;
            scrollY_ += wheel_y;
        }
        scroll_dirty_ = true;
        break;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        SDL_Scancode sc = e.key.scancode;
        if (sc >= 0 && sc < SDL_SCANCODE_COUNT) {
            keys_down_[sc] = (e.type == SDL_EVENT_KEY_DOWN);
            mark_scancode_dirty(sc);
        }
        break;
    }

    default:
        break;
    }
}

std::uint32_t Input::sync_live_keyboard_state() {
    int key_count = 0;
    const bool* live_keys = SDL_GetKeyboardState(&key_count);
    std::uint32_t changed_count = 0;

    if (live_keys && key_count > 0) {
        const int count = std::min(key_count, static_cast<int>(SDL_SCANCODE_COUNT));
        for (int i = 0; i < count; ++i) {
            const SDL_Scancode sc = static_cast<SDL_Scancode>(i);
            const bool live_down = keyboard_focus_active_ && live_keys[i];
            if (keys_down_[sc] == live_down) {
                continue;
            }
            keys_down_[sc] = live_down;
            mark_scancode_dirty(sc);
            ++changed_count;
        }
    }

    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    frame_stats.set("input.keyboard_reconciled", changed_count > 0);
    frame_stats.set("input.keyboard_reconciled_changed_count", changed_count);
    frame_stats.set("input.keyboard_focus_active", keyboard_focus_active_);
    frame_stats.set("input.focus_loss_cleared", focus_loss_cleared_since_sync_);
    focus_loss_cleared_since_sync_ = false;

    record_key_metric(frame_stats, "input.live.", "w", keyboard_focus_active_ && live_scancode_down(live_keys, key_count, SDL_SCANCODE_W));
    record_key_metric(frame_stats, "input.live.", "a", keyboard_focus_active_ && live_scancode_down(live_keys, key_count, SDL_SCANCODE_A));
    record_key_metric(frame_stats, "input.live.", "s", keyboard_focus_active_ && live_scancode_down(live_keys, key_count, SDL_SCANCODE_S));
    record_key_metric(frame_stats, "input.live.", "d", keyboard_focus_active_ && live_scancode_down(live_keys, key_count, SDL_SCANCODE_D));
    record_key_metric(frame_stats, "input.live.", "space", keyboard_focus_active_ && live_scancode_down(live_keys, key_count, SDL_SCANCODE_SPACE));
    record_key_metric(frame_stats, "input.stored.", "w", keys_down_[SDL_SCANCODE_W]);
    record_key_metric(frame_stats, "input.stored.", "a", keys_down_[SDL_SCANCODE_A]);
    record_key_metric(frame_stats, "input.stored.", "s", keys_down_[SDL_SCANCODE_S]);
    record_key_metric(frame_stats, "input.stored.", "d", keys_down_[SDL_SCANCODE_D]);
    record_key_metric(frame_stats, "input.stored.", "space", keys_down_[SDL_SCANCODE_SPACE]);
    return changed_count;
}

void Input::update() {
    if (button_state_dirty_ || click_buffer_active_ || button_transition_active_) {
        bool any_click_active = false;
        bool any_transition = false;
        for (int i = 0; i < COUNT; ++i) {
            pressed_[i]     = (!prevButtons_[i] && buttons_[i]);
            released_[i]    = (prevButtons_[i] && !buttons_[i]);
            if (pressed_[i] || released_[i]) {
                any_transition = true;
            }
            prevButtons_[i] = buttons_[i];
            if (clickBuffer_[i] > 0) {
                --clickBuffer_[i];
                if (clickBuffer_[i] > 0) {
                    any_click_active = true;
                }
            }
        }
        click_buffer_active_ = any_click_active;
        button_transition_active_ = any_transition;
        button_state_dirty_ = false;
    }

    if (!pressed_scancode_buffer_.empty()) {
        for (SDL_Scancode sc : pressed_scancode_buffer_) {
            keys_pressed_[sc] = false;
        }
        pressed_scancode_buffer_.clear();
    }

    if (!released_scancode_buffer_.empty()) {
        for (SDL_Scancode sc : released_scancode_buffer_) {
            keys_released_[sc] = false;
        }
        released_scancode_buffer_.clear();
    }

    if (!dirty_scancodes_.empty()) {
        for (SDL_Scancode sc : dirty_scancodes_) {
            const bool is_down   = keys_down_[sc];
            const bool was_down  = prev_keys_down_[sc];
            const bool pressed   = (!was_down && is_down);
            const bool released  = (was_down && !is_down);
            keys_pressed_[sc]    = pressed;
            keys_released_[sc]   = released;
            if (pressed) {
                pressed_scancode_buffer_.push_back(sc);
            }
            if (released) {
                released_scancode_buffer_.push_back(sc);
            }
            prev_keys_down_[sc]     = is_down;
            scancode_dirty_flags_[sc] = false;
        }
        dirty_scancodes_.clear();
    }

    if (mouse_motion_dirty_ || dx_ != 0 || dy_ != 0) {
        dx_ = dy_ = 0;
        mouse_motion_dirty_ = false;
    }

    if (scroll_dirty_ || scrollX_ != 0 || scrollY_ != 0) {
        scrollX_ = scrollY_ = 0;
        scroll_dirty_ = false;
    }
}

bool Input::wasClicked(Button b) const {
    return clickBuffer_[b] > 0;
}

void Input::clearClickBuffer() {
    for (int i = 0; i < COUNT; ++i) {
        clickBuffer_[i] = 0;
    }
    click_buffer_active_ = false;
}

void Input::consumeMouseButton(Button b) {
    if (b < 0 || b >= COUNT) return;
    prevButtons_[b] = buttons_[b];
    pressed_[b] = false;
    released_[b] = false;
    clickBuffer_[b] = 0;
    refresh_click_buffer_active();
    refresh_button_transition_active();
}

void Input::consumeAllMouseButtons() {
    for (int i = 0; i < COUNT; ++i) {
        consumeMouseButton(static_cast<Button>(i));
    }
}

void Input::consumeScroll() {
    scrollX_ = 0;
    scrollY_ = 0;
    scroll_dirty_ = false;
}

void Input::consumeMotion() {
    dx_ = 0;
    dy_ = 0;
    mouse_motion_dirty_ = false;
}

void Input::consumeEvent(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        Button button = to_button(e.button.button);
        if (button != COUNT) {
            consumeMouseButton(button);
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        consumeScroll();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        consumeMotion();
        break;
    default:
        break;
    }
}

void Input::setScancodeDownForTest(SDL_Scancode sc, bool down) {
    if (sc < 0 || sc >= SDL_SCANCODE_COUNT) {
        return;
    }
    keyboard_focus_active_ = true;
    if (keys_down_[sc] != down) {
        keys_down_[sc] = down;
        mark_scancode_dirty(sc);
    }
}

void Input::setMousePositionForTest(int x, int y) {
    dx_ = x - x_;
    dy_ = y - y_;
    x_ = x;
    y_ = y;
    mouse_motion_dirty_ = true;
}

void Input::setMouseButtonDownForTest(Button button, bool down) {
    if (button < 0 || button >= COUNT) {
        return;
    }
    if (buttons_[button] == down) {
        return;
    }
    buttons_[button] = down;
    button_state_dirty_ = true;
    if (!down) {
        clickBuffer_[button] = 3;
        click_buffer_active_ = true;
    }
}

void Input::applyCodexPlaytestDriverForTest(std::uint64_t frame_id, int screen_w, int screen_h) {
    struct PlaytestSegment {
        std::uint64_t index = 0;
        std::uint64_t start_frame = 0;
        std::uint64_t length_frames = 1;
        int x = 0;
        int y = -1;
        bool sprint = false;
        bool long_hold = true;
    };

    auto next_random = [](std::uint32_t& state) {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        return state;
    };

    auto choose_segment = [&](PlaytestSegment previous, std::uint32_t& random_state) {
        PlaytestSegment next = previous;
        ++next.index;
        next.start_frame = frame_id;

        const bool force_long_hold = (next.index % 6u) == 0u;
        const std::uint32_t duration_roll = next_random(random_state);
        if (force_long_hold) {
            next.long_hold = true;
            next.length_frames = 180u + (duration_roll % 301u);
        } else if ((duration_roll % 5u) == 0u) {
            next.long_hold = false;
            next.length_frames = 60u + (duration_roll % 91u);
        } else {
            next.long_hold = false;
            next.length_frames = 5u + (duration_roll % 32u);
        }

        static constexpr SDL_Point directions[] = {
            SDL_Point{0, -1},
            SDL_Point{1, 0},
            SDL_Point{0, 1},
            SDL_Point{-1, 0},
            SDL_Point{1, -1},
            SDL_Point{-1, -1},
            SDL_Point{1, 1},
            SDL_Point{-1, 1}
        };
        const std::uint32_t direction_roll = next_random(random_state);
        const SDL_Point chosen = directions[direction_roll % (sizeof(directions) / sizeof(directions[0]))];
        next.x = chosen.x;
        next.y = chosen.y;
        next.sprint = next.long_hold && ((next_random(random_state) % 3u) == 0u);
        return next;
    };

    static std::uint32_t random_state = 0xC0D3CAFEu;
    static PlaytestSegment segment{};
    static bool initialized = false;
    static std::uint64_t last_frame_id = 0;
    if (!initialized || frame_id <= last_frame_id) {
        initialized = true;
        random_state = 0xC0D3CAFEu;
        segment = PlaytestSegment{};
        segment.index = 0;
        segment.start_frame = frame_id;
        segment.length_frames = 210u;
        segment.x = 0;
        segment.y = -1;
        segment.sprint = false;
        segment.long_hold = true;
    } else if (frame_id - segment.start_frame >= segment.length_frames) {
        segment = choose_segment(segment, random_state);
    }
    last_frame_id = frame_id;

    constexpr SDL_Scancode movement_keys[] = {
        SDL_SCANCODE_W,
        SDL_SCANCODE_A,
        SDL_SCANCODE_S,
        SDL_SCANCODE_D,
        SDL_SCANCODE_UP,
        SDL_SCANCODE_LEFT,
        SDL_SCANCODE_DOWN,
        SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_SPACE,
        SDL_SCANCODE_LSHIFT,
        SDL_SCANCODE_E
    };
    for (SDL_Scancode scancode : movement_keys) {
        setScancodeDownForTest(scancode, false);
    }

    if (segment.x > 0) {
        setScancodeDownForTest(SDL_SCANCODE_D, true);
    } else if (segment.x < 0) {
        setScancodeDownForTest(SDL_SCANCODE_A, true);
    }
    if (segment.y < 0) {
        setScancodeDownForTest(SDL_SCANCODE_W, true);
    } else if (segment.y > 0) {
        setScancodeDownForTest(SDL_SCANCODE_S, true);
    }
    if (segment.sprint) {
        setScancodeDownForTest(SDL_SCANCODE_LSHIFT, true);
    }

    const std::uint64_t segment_frame = frame_id - segment.start_frame;
    const bool pulse_dash = (segment.long_hold && (segment_frame % 150u) < 10u) ||
                            (!segment.long_hold && segment.length_frames > 18u && segment_frame < 4u);
    const bool pulse_interact = !segment.long_hold && (segment.index % 7u) == 0u &&
                                segment_frame >= 2u && segment_frame < 10u;
    setScancodeDownForTest(SDL_SCANCODE_SPACE, pulse_dash);
    setScancodeDownForTest(SDL_SCANCODE_E, pulse_interact);

    const int safe_w = std::max(1, screen_w);
    const int safe_h = std::max(1, screen_h);
    const std::uint64_t mouse_phase = (frame_id / 60u) % 4u;
    const int mouse_x = (mouse_phase == 0u) ? (safe_w * 3) / 4
                      : (mouse_phase == 2u) ? safe_w / 4
                      : safe_w / 2;
    const int mouse_y = (mouse_phase == 1u) ? safe_h / 4
                      : (mouse_phase == 3u) ? (safe_h * 3) / 4
                      : safe_h / 2;
    setMousePositionForTest(mouse_x, mouse_y);

    const bool melee_down = (frame_id % 210u) < 6u;
    setMouseButtonDownForTest(Input::LEFT, melee_down);

    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    frame_stats.set("codex_playtest.input_driver", true);
    frame_stats.set("codex_playtest.phase", segment.index % 8u);
    frame_stats.set("codex_playtest.segment_index", segment.index);
    frame_stats.set("codex_playtest.segment_kind", segment.long_hold ? "long" : "burst");
    frame_stats.set("codex_playtest.segment_length_frames", segment.length_frames);
    frame_stats.set("codex_playtest.segment_frame", segment_frame);
    frame_stats.set("codex_playtest.mouse_x", mouse_x);
    frame_stats.set("codex_playtest.mouse_y", mouse_y);
    record_key_metric(frame_stats, "input.stored.", "w", keys_down_[SDL_SCANCODE_W]);
    record_key_metric(frame_stats, "input.stored.", "a", keys_down_[SDL_SCANCODE_A]);
    record_key_metric(frame_stats, "input.stored.", "s", keys_down_[SDL_SCANCODE_S]);
    record_key_metric(frame_stats, "input.stored.", "d", keys_down_[SDL_SCANCODE_D]);
    record_key_metric(frame_stats, "input.stored.", "space", keys_down_[SDL_SCANCODE_SPACE]);
}

void Input::set_screen_to_world_mapper(ScreenToWorldFunction fn) {
    screen_to_world_fn_ = std::move(fn);
}

void Input::clear_screen_to_world_mapper() {
    screen_to_world_fn_ = {};
}

std::optional<SDL_Point> Input::screen_to_world(SDL_Point screen) const {
    if (!screen_to_world_fn_) {
        return std::nullopt;
    }
    return screen_to_world_fn_(screen);
}

std::optional<SDL_Point> Input::mouse_world_position() const {
    SDL_Point screen{x_, y_};
    return screen_to_world(screen);
}

void Input::refresh_click_buffer_active() {
    click_buffer_active_ = false;
    for (int i = 0; i < COUNT; ++i) {
        if (clickBuffer_[i] > 0) {
            click_buffer_active_ = true;
            break;
        }
    }
}

void Input::refresh_button_transition_active() {
    button_transition_active_ = false;
    for (int i = 0; i < COUNT; ++i) {
        if (pressed_[i] || released_[i]) {
            button_transition_active_ = true;
            break;
        }
    }
}

void Input::mark_scancode_dirty(SDL_Scancode sc) {
    if (sc < 0 || sc >= SDL_SCANCODE_COUNT) {
        return;
    }
    if (!scancode_dirty_flags_[sc]) {
        scancode_dirty_flags_[sc] = true;
        dirty_scancodes_.push_back(sc);
    }
}

void Input::clear_all_state() {
    for (int i = 0; i < COUNT; ++i) {
        if (buttons_[i]) {
            button_state_dirty_ = true;
        }
        buttons_[i] = false;
        pressed_[i] = false;
        released_[i] = false;
        clickBuffer_[i] = 0;
    }
    click_buffer_active_ = false;

    for (int i = 0; i < SDL_SCANCODE_COUNT; ++i) {
        const SDL_Scancode sc = static_cast<SDL_Scancode>(i);
        if (keys_down_[sc]) {
            keys_down_[sc] = false;
            mark_scancode_dirty(sc);
        }
    }

    dx_ = dy_ = 0;
    scrollX_ = scrollY_ = 0;
    mouse_motion_dirty_ = false;
    scroll_dirty_ = false;
    focus_loss_cleared_since_sync_ = true;
    refresh_button_transition_active();
}

bool Input::has_activity() const {
    if (mouse_motion_dirty_ ||
        button_state_dirty_ ||
        scroll_dirty_ ||
        click_buffer_active_ ||
        button_transition_active_ ||
        !dirty_scancodes_.empty()) {
        return true;
    }

    for (int i = 0; i < COUNT; ++i) {
        if (buttons_[i]) {
            return true;
        }
    }

    for (bool down : keys_down_) {
        if (down) {
            return true;
        }
    }

    return false;
}

