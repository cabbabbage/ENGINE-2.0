#include "map_layers_container_configurator.hpp"

#include <utility>

namespace map_layers::container_configurator {

void configure_callbacks(SlidingWindowContainer& container,
                         LayoutCallback layout,
                         RenderCallback render,
                         EventCallback event,
                         UpdateCallback update) {
    container.set_layout_function(std::move(layout));
    container.set_render_function(std::move(render));
    container.set_event_function(std::move(event));
    container.set_update_function(std::move(update));
}

void apply_default_panel_options(SlidingWindowContainer& container,
                                 bool blocks_editor_interactions,
                                 bool close_button_enabled) {
    container.set_header_visible(true);
    container.set_scrollbar_visible(true);
    container.set_close_button_enabled(close_button_enabled);
    container.set_blocks_editor_interactions(blocks_editor_interactions);
}

void clear_callbacks(SlidingWindowContainer& container, bool reset_editor_blocking) {
    container.set_layout_function({});
    container.set_render_function({});
    container.set_event_function({});
    container.set_update_function({});
    if (reset_editor_blocking) {
        container.set_blocks_editor_interactions(false);
    }
}

}  // namespace map_layers::container_configurator
