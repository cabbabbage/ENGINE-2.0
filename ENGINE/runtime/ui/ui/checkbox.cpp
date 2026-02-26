#include "checkbox.hpp"

#include "ui/styles.hpp"
#include "widget_text_renderer.hpp"

Checkbox::Checkbox(const std::string& label, bool value)
    : WidgetBase(SDL_Rect{0, 0, width(), height()}), label_(label), value_(value) {}

void Checkbox::set_position(SDL_Point p) { WidgetBase::set_position(p); }
void Checkbox::set_rect(const SDL_Rect& r) { WidgetBase::set_rect(r); }
const SDL_Rect& Checkbox::rect() const { return WidgetBase::rect(); }

void Checkbox::set_label(const std::string& s) { label_ = s; }
const std::string& Checkbox::label() const { return label_; }

void Checkbox::set_value(bool v) { value_ = v; }
bool Checkbox::value() const { return value_; }

bool Checkbox::handle_event(const SDL_Event& e) {
    bool toggled = false;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        update_hover(e.motion);
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        update_hover(e.button);
        if (hovered()) {
            value_ = !value_;
            toggled = true;
        }
    }
    return toggled;
}

void Checkbox::render(SDL_Renderer* r) const {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    if (!label_.empty()) {
        WidgetTextRenderer label_renderer(TextStyles::SmallMain());
        label_renderer.Render(r, label_, rect_.x, rect_.y);
    }

    const int box_size = rect_.h - 6;
    SDL_Rect box{
        rect_.x + rect_.w - box_size - 4,
        rect_.y + 3,
        box_size,
        box_size
    };

    SDL_Color bg = Styles::Slate();
    bg.a = 160;
    WidgetBase::FillRect(r, box, bg);

    SDL_Color border_on = Styles::Gold();
    SDL_Color border_off = Styles::GoldDim();
    SDL_Color frame = hovered() ? border_on : border_off;
    WidgetBase::StrokeRect(r, box, frame);

    if (value_) {
        SDL_Rect inner{ box.x + 4, box.y + 4, box.w - 8, box.h - 8 };
        SDL_Color fill = Styles::Ivory();
        WidgetBase::FillRect(r, inner, fill);
    }
}

int Checkbox::width() { return 300; }
int Checkbox::height() { return 28; }



