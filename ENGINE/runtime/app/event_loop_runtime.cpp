#include "event_loop_runtime.hpp"

#include "utils/frame_stats_recorder.hpp"

namespace app::event_loop_runtime {

bool is_resize_or_scale_event(Uint32 event_type) {
    switch (event_type) {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
#ifdef SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
#endif
#ifdef SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED
    case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
#endif
        return true;
    default:
        return false;
    }
}

bool is_critical_runtime_event(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
#ifdef SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
#endif
#ifdef SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED
    case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
#endif
        return true;
    default:
        return false;
    }
}

bool event_uses_pointer_coordinates(Uint32 event_type) {
    switch (event_type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
        return true;
    default:
        return false;
    }
}

EventPumpFrameState pump_events(SDL_Renderer* renderer,
                                EventPumpConfig config,
                                std::deque<SDL_Event>& deferred_events,
                                const std::function<void(SDL_Event&)>& process_event,
                                Uint64 event_begin_counter) {
    EventPumpFrameState state{};

    while (!deferred_events.empty()) {
        SDL_Event deferred = deferred_events.front();
        deferred_events.pop_front();
        process_event(deferred);
        ++state.event_count;
        if (renderer && is_resize_or_scale_event(deferred.type)) {
            state.resize_or_scale_seen = true;
        }
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        const bool over_count = state.event_count >= config.max_events_per_frame;
        const bool over_time = runtime_stats::FrameStatsRecorder::elapsed_ms(event_begin_counter,
                                                                              SDL_GetPerformanceCounter()) >= config.max_poll_ms_per_frame;
        if (!is_critical_runtime_event(e) && (over_count || over_time)) {
            deferred_events.push_back(e);
            ++state.deferred_event_count;
            state.event_budget_hit_count = state.event_budget_hit_count || over_count;
            state.event_budget_hit_time = state.event_budget_hit_time || over_time;
            continue;
        }

        process_event(e);
        ++state.event_count;
        if (renderer && is_resize_or_scale_event(e.type)) {
            state.resize_or_scale_seen = true;
        }
    }

    return state;
}

} // namespace app::event_loop_runtime
