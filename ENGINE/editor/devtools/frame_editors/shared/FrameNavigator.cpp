#include "FrameNavigator.hpp"

#include <algorithm>
#include <string>

#include "devtools/widgets.hpp"

namespace devmode::frame_editors {

FrameNavigator::FrameNavigator() {
    btn_prev_ = std::make_unique<DMButton>("<", &DMStyles::AccentButton(), 36, DMButton::height());
    btn_next_ = std::make_unique<DMButton>(">", &DMStyles::AccentButton(), 36, DMButton::height());
    tb_frame_number_ = std::make_unique<DMTextBox>("", "0");

    update_button_states();
    update_textbox_value();
}

FrameNavigator::~FrameNavigator() = default;

void FrameNavigator::set_frame_count(int count) {
    frame_count_ = std::max(0, count);
    validate_frame_index();
    update_button_states();
}

void FrameNavigator::set_current_frame(int frame) {
    int old_frame = current_frame_;
    current_frame_ = frame;
    validate_frame_index();

    // Only notify if the frame actually changed
    if (current_frame_ != old_frame) {
        update_textbox_value();
        update_button_states();
        notify_frame_changed();
    }
}

void FrameNavigator::set_on_frame_changed(std::function<void(int)> callback) {
    on_frame_changed_ = std::move(callback);
}

void FrameNavigator::set_enabled(bool enabled) {
    enabled_ = enabled;
    update_button_states();
}

void FrameNavigator::set_rect(const SDL_Rect& rect) {
    rect_ = rect;

    const int spacing = 4;
    const int button_width = 36;
    const int textbox_width = rect.w - (button_width * 2) - (spacing * 2);

    int x_offset = rect.x;

    // Previous button
    if (btn_prev_) {
        SDL_Rect prev_rect = {x_offset, rect.y, button_width, rect.h};
        btn_prev_->set_rect(prev_rect);
        x_offset += button_width + spacing;
    }

    // Frame number textbox
    if (tb_frame_number_) {
        SDL_Rect tb_rect = {x_offset, rect.y, textbox_width, rect.h};
        tb_frame_number_->set_rect(tb_rect);
        x_offset += textbox_width + spacing;
    }

    // Next button
    if (btn_next_) {
        SDL_Rect next_rect = {x_offset, rect.y, button_width, rect.h};
        btn_next_->set_rect(next_rect);
    }
}

const SDL_Rect& FrameNavigator::get_rect() const {
    return rect_;
}

SDL_Rect FrameNavigator::get_preferred_rect() const {
    const int button_width = 36;
    const int spacing = 4;
    const int textbox_width = 60; // Minimum reasonable width
    const int total_width = (button_width * 2) + textbox_width + (spacing * 2);
    return SDL_Rect{0, 0, total_width, 32};
}

bool FrameNavigator::handle_event(const SDL_Event& e) {
    if (!enabled_) return false;

    bool consumed = false;

    // NOTE: Keyboard navigation (arrow keys) removed - now used for point selection in frame editors

    // Handle button clicks
    if (btn_prev_ && btn_prev_->handle_event(e)) {
        set_current_frame(current_frame_ - 1);
        consumed = true;
    }

    if (btn_next_ && btn_next_->handle_event(e)) {
        set_current_frame(current_frame_ + 1);
        consumed = true;
    }

    // Handle textbox input
    if (tb_frame_number_ && tb_frame_number_->handle_event(e)) {
        // Check if textbox value changed
        if (!tb_frame_number_->is_editing()) {
            // Textbox lost focus or enter was pressed
            try {
                int new_frame = std::stoi(tb_frame_number_->value());
                set_current_frame(new_frame);
            } catch (...) {
                // Invalid input, revert to current value
                update_textbox_value();
            }
        }
        consumed = true;
    }

    return consumed;
}

void FrameNavigator::render(SDL_Renderer* renderer) {
    if (!enabled_) return;

    if (btn_prev_) btn_prev_->render(renderer);
    if (tb_frame_number_) tb_frame_number_->render(renderer);
    if (btn_next_) btn_next_->render(renderer);
}

void FrameNavigator::update_button_states() {
    if (!enabled_ || frame_count_ <= 1) {
        if (btn_prev_) btn_prev_->set_text("");
        if (btn_next_) btn_next_->set_text("");
        return;
    }

    if (btn_prev_) {
        btn_prev_->set_text(current_frame_ > 0 ? "<" : "");
    }

    if (btn_next_) {
        btn_next_->set_text(current_frame_ < frame_count_ - 1 ? ">" : "");
    }
}

void FrameNavigator::validate_frame_index() {
    if (frame_count_ <= 0) {
        current_frame_ = 0;
        return;
    }

    current_frame_ = std::clamp(current_frame_, 0, frame_count_ - 1);
}

void FrameNavigator::notify_frame_changed() {
    if (on_frame_changed_) {
        on_frame_changed_(current_frame_);
    }
}

bool FrameNavigator::handle_keyboard_navigation(const SDL_Event& e) {
    if (e.type != SDL_KEYDOWN) return false;

    switch (e.key.keysym.sym) {
        case SDLK_LEFT:
            if (current_frame_ > 0) {
                set_current_frame(current_frame_ - 1);
                return true;
            }
            break;
        case SDLK_RIGHT:
            if (current_frame_ < frame_count_ - 1) {
                set_current_frame(current_frame_ + 1);
                return true;
            }
            break;
        default:
            break;
    }

    return false;
}

void FrameNavigator::update_textbox_value() {
    if (tb_frame_number_) {
        tb_frame_number_->set_value(std::to_string(current_frame_));
    }
}

}  // namespace devmode::frame_editors
