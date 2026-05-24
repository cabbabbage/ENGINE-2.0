#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <deque>
#include <functional>

namespace app::event_loop_runtime {

struct EventPumpConfig {
    int max_events_per_frame = 1000;
    int max_poll_ms_per_frame = 4;
};

struct EventPumpFrameState {
    int event_count = 0;
    int deferred_event_count = 0;
    bool event_budget_hit_count = false;
    bool event_budget_hit_time = false;
    bool resize_or_scale_seen = false;
};

bool is_resize_or_scale_event(Uint32 event_type);
bool is_critical_runtime_event(const SDL_Event& e);
bool event_uses_pointer_coordinates(Uint32 event_type);

EventPumpFrameState pump_events(SDL_Renderer* renderer,
                                EventPumpConfig config,
                                std::deque<SDL_Event>& deferred_events,
                                const std::function<void(SDL_Event&)>& process_event,
                                Uint64 event_begin_counter);

} // namespace app::event_loop_runtime
