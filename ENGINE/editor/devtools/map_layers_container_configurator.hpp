#pragma once

#include <functional>

#include "SlidingWindowContainer.hpp"

class Input;
union SDL_Event;
struct SDL_Renderer;

namespace map_layers::container_configurator {

using LayoutCallback = std::function<int(const SlidingWindowContainer::LayoutContext&)>;
using RenderCallback = std::function<void(SDL_Renderer*)>;
using EventCallback = std::function<bool(const SDL_Event&)>;
using UpdateCallback = std::function<void(const Input&, int, int)>;

void configure_callbacks(SlidingWindowContainer& container,
                         LayoutCallback layout,
                         RenderCallback render,
                         EventCallback event,
                         UpdateCallback update);

void apply_default_panel_options(SlidingWindowContainer& container,
                                 bool blocks_editor_interactions,
                                 bool close_button_enabled = false);

void clear_callbacks(SlidingWindowContainer& container, bool reset_editor_blocking = true);

}  // namespace map_layers::container_configurator
