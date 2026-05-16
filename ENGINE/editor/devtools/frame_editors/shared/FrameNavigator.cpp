#include "FrameNavigator.hpp"

#include "devtools/sdl_modal_dialog.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "devtools/asset_editor/animation_editor_window/PreviewProvider.hpp"
#include "devtools/dm_icons.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/font_cache.hpp"
#include "utils/sdl_mouse_utils.hpp"

namespace devmode::frame_editors {
namespace {
constexpr int kThumbSize = 64;
const int kThumbSpacing = DMSpacing::small_gap();
const int kBarPadding = DMSpacing::item_gap();
constexpr int kThumbCorner = 8;
constexpr int kBadgeHeight = 16;
constexpr int kBadgePadding = 4;
const int kApplyButtonGap = DMSpacing::item_gap();
constexpr int kBottomButtonHeight = 40;
constexpr int kSaveExitButtonWidth = 150;
const int kNavigatorGap = DMSpacing::small_gap();

SDL_FRect ToFRect(const SDL_Rect& rect) {
    return SDL_FRect{
        static_cast<float>(rect.x),
        static_cast<float>(rect.y),
        static_cast<float>(rect.w),
        static_cast<float>(rect.h)
    };
}

float content_width(int frame_count) {
    if (frame_count <= 0) return 0.0f;
    const float stride = static_cast<float>(kThumbSize + kThumbSpacing);
    return static_cast<float>(frame_count) * stride - static_cast<float>(kThumbSpacing);
}
}  // namespace

FrameNavigator::FrameNavigator() {
    const DMButtonStyle* nav_style = &DMStyles::IconButton();
    const DMButtonStyle* apply_style = &DMStyles::AccentButton();
    btn_prev_ = std::make_unique<DMButton>(std::string(DMIcons::NavLeft()), nav_style, kThumbSize, kThumbSize);
    btn_next_ = std::make_unique<DMButton>(std::string(DMIcons::NavRight()), nav_style, kThumbSize, kThumbSize);
    btn_apply_next_ = std::make_unique<DMButton>("Apply To Next", apply_style, kBottomButtonHeight * 3, kBottomButtonHeight);
    btn_apply_animation_ = std::make_unique<DMButton>("Apply To Selected", apply_style, kBottomButtonHeight * 3, kBottomButtonHeight);
    btn_apply_all_ = std::make_unique<DMButton>("Apply To All", apply_style, kBottomButtonHeight * 3, kBottomButtonHeight);
    btn_save_exit_ = std::make_unique<DMButton>("Exit", &DMStyles::DeleteButton(), kSaveExitButtonWidth, kBottomButtonHeight);
    update_button_states();
}

FrameNavigator::~FrameNavigator() = default;

void FrameNavigator::set_frame_count(int count) {
    frame_count_ = std::max(0, count);
    validate_frame_index();
    update_button_states();
    clamp_scroll();
    ensure_frame_visible(current_frame_);
}

void FrameNavigator::set_current_frame(int frame) {
    int old_frame = current_frame_;
    current_frame_ = frame;
    validate_frame_index();
    clamp_scroll();
    ensure_frame_visible(current_frame_);

    if (current_frame_ != old_frame) {
        update_button_states();
        notify_frame_changed();
    }
}

void FrameNavigator::set_on_frame_changed(std::function<void(int)> callback) {
    on_frame_changed_ = std::move(callback);
}

void FrameNavigator::set_on_before_change(std::function<bool(int, int)> callback) {
    on_before_change_ = std::move(callback);
}

void FrameNavigator::set_on_apply_next(std::function<void()> callback) {
    on_apply_next_ = std::move(callback);
}

void FrameNavigator::set_on_apply_animation(std::function<void()> callback) {
    on_apply_animation_ = std::move(callback);
}

void FrameNavigator::set_on_apply_all(std::function<void()> callback) {
    on_apply_all_ = std::move(callback);
}

void FrameNavigator::set_on_save_and_exit(std::function<void()> callback) {
    on_save_and_exit_ = std::move(callback);
}

void FrameNavigator::set_confirmation_handler(std::function<bool(const std::string&, const std::string&)> callback) {
    on_confirm_ = std::move(callback);
}

void FrameNavigator::set_preview_source(std::weak_ptr<animation_editor::PreviewProvider> provider,
                                        const std::string& animation_id) {
    preview_provider_ = std::move(provider);
    animation_id_ = animation_id;
}

void FrameNavigator::set_enabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled_) {
        reset_hover();
    }
    update_button_states();
}

void FrameNavigator::set_rect(const SDL_Rect& rect) {
    rect_ = rect;
    rect_.h = std::max(rect_.h, get_preferred_rect().h);

    const int top_y = rect_.y + kBarPadding;
    const int bottom_apply_y = rect_.y + rect_.h - kBarPadding - kBottomButtonHeight;
    const int save_button_y = bottom_apply_y - DMSpacing::small_gap() - kBottomButtonHeight;

    const int left_x = rect_.x + kBarPadding;
    const int right_x = rect_.x + rect_.w - kBarPadding;
    const int inner_width = std::max(0, rect_.w - 2 * kBarPadding);
    const int strip_width = std::max(0, inner_width - 2 * kThumbSize - 2 * kNavigatorGap);
    const int strip_x = left_x + kThumbSize + kNavigatorGap;

    if (btn_prev_) {
        btn_prev_->set_rect(SDL_Rect{left_x, top_y, kThumbSize, kThumbSize});
    }
    if (btn_next_) {
        btn_next_->set_rect(SDL_Rect{right_x - kThumbSize, top_y, kThumbSize, kThumbSize});
    }

    strip_rect_ = SDL_Rect{
        strip_x,
        top_y,
        strip_width,
        kThumbSize
    };

    if (btn_save_exit_) {
        btn_save_exit_->set_rect(SDL_Rect{
            rect_.x + rect_.w - kBarPadding - kSaveExitButtonWidth,
            save_button_y,
            kSaveExitButtonWidth,
            kBottomButtonHeight
        });
    }

    DMButton* apply_buttons[3] = {btn_apply_next_.get(), btn_apply_animation_.get(), btn_apply_all_.get()};
    if (strip_rect_.w > 0) {
        int actual_gap = kApplyButtonGap;
        if (strip_rect_.w < kApplyButtonGap * 2) {
            actual_gap = std::max(0, strip_rect_.w / 4);
        }
        const int gap_total = actual_gap * 2;
        const int available_width = std::max(0, strip_rect_.w - gap_total);
        const int base_width = available_width / 3;
        const int remainder = available_width - base_width * 3;
        int extra[3] = {0, 0, 0};
        for (int i = 0; i < remainder && i < 3; ++i) {
            extra[i] = 1;
        }

        int apply_x = strip_rect_.x;
        for (int i = 0; i < 3; ++i) {
            int width = base_width + extra[i];
            width = std::max(width, 1);
            if (apply_buttons[i]) {
                SDL_Rect btn_rect{apply_x, bottom_apply_y, width, kBottomButtonHeight};
                apply_buttons[i]->set_rect(btn_rect);
            }
            apply_x += width;
            if (i < 2) {
                apply_x += actual_gap;
            }
        }
    }

    clamp_scroll();
    ensure_frame_visible(current_frame_);
}

const SDL_Rect& FrameNavigator::get_rect() const {
    return rect_;
}

SDL_Rect FrameNavigator::get_preferred_rect() const {
    const int bottom_rows = kBottomButtonHeight * 2 + DMSpacing::small_gap();
    return SDL_Rect{0, 0, 0, kBarPadding + kThumbSize + bottom_rows + kBarPadding};
}

bool FrameNavigator::handle_event(const SDL_Event& e) {
    if (!enabled_) return false;

    bool consumed = false;

    if (btn_prev_ && btn_prev_->handle_event(e)) {
        request_frame_change(current_frame_ - 1);
        consumed = true;
    }

    if (btn_next_ && btn_next_->handle_event(e)) {
        request_frame_change(current_frame_ + 1);
        consumed = true;
    }
    if (btn_apply_next_ && btn_apply_next_->handle_event(e)) {
        handle_apply_next();
        consumed = true;
    }
    if (btn_apply_animation_ && btn_apply_animation_->handle_event(e)) {
        handle_apply_animation();
        consumed = true;
    }
    if (btn_apply_all_ && btn_apply_all_->handle_event(e)) {
        handle_apply_all();
        consumed = true;
    }
    if (btn_save_exit_ && btn_save_exit_->handle_event(e)) {
        handle_save_and_exit();
        consumed = true;
    }

    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
        update_hover(p);
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        int idx = frame_index_at_point(p);
        pressed_thumb_index_ = idx;
        if (idx >= 0) {
            consumed = true;
        }
        update_hover(p);
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        int idx = frame_index_at_point(p);
        if (idx >= 0 && idx == pressed_thumb_index_) {
            request_frame_change(idx);
            consumed = true;
        }
        pressed_thumb_index_ = -1;
        update_hover(p);
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        SDL_Point p = sdl_mouse_util::WheelPoint(e.wheel);
        if (SDL_PointInRect(&p, &strip_rect_)) {
            int delta_x = e.wheel.x;
            int delta_y = e.wheel.y;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                delta_x = -delta_x;
                delta_y = -delta_y;
            }
            const int steps = (delta_x != 0) ? delta_x : delta_y;
            const float scroll_step = static_cast<float>(kThumbSize + kThumbSpacing);
            scroll_offset_ = std::clamp(scroll_offset_ - static_cast<float>(steps) * scroll_step,
                                        0.0f,
                                        std::max(0.0f, content_width(frame_count_) - static_cast<float>(strip_rect_.w)));
            consumed = true;
        }
    }

    return consumed;
}

void FrameNavigator::render(SDL_Renderer* renderer) {
    if (!enabled_ || !renderer) return;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    render_background(renderer);
    render_thumbnails(renderer);
    if (btn_prev_) btn_prev_->render(renderer);
    if (btn_next_) btn_next_->render(renderer);
    if (btn_apply_next_) btn_apply_next_->render(renderer);
    if (btn_apply_animation_) btn_apply_animation_->render(renderer);
    if (btn_apply_all_) btn_apply_all_->render(renderer);
    if (btn_save_exit_) btn_save_exit_->render(renderer);
}

void FrameNavigator::request_frame_change(int frame) {
    if (!enabled_ || frame_count_ <= 0) {
        return;
    }
    int clamped = std::clamp(frame, 0, frame_count_ - 1);
    if (clamped == current_frame_) {
        return;
    }
    if (on_before_change_) {
        if (!on_before_change_(current_frame_, clamped)) {
            return;
        }
    }
    set_current_frame(clamped);
}

void FrameNavigator::ensure_frame_visible(int frame) {
    if (strip_rect_.w <= 0 || frame_count_ <= 0) return;
    const float stride = static_cast<float>(kThumbSize + kThumbSpacing);
    const float start = static_cast<float>(frame) * stride;
    const float end = start + static_cast<float>(kThumbSize);
    const float view_start = scroll_offset_;
    const float view_end = scroll_offset_ + static_cast<float>(strip_rect_.w);

    if (start < view_start) {
        scroll_offset_ = std::max(0.0f, start - static_cast<float>(kThumbSpacing));
    } else if (end > view_end) {
        scroll_offset_ = end - static_cast<float>(strip_rect_.w) + static_cast<float>(kThumbSpacing);
    }

    clamp_scroll();
}

void FrameNavigator::clamp_scroll() {
    const float max_scroll = std::max(0.0f, content_width(frame_count_) - static_cast<float>(strip_rect_.w));
    scroll_offset_ = std::clamp(scroll_offset_, 0.0f, max_scroll);
}

int FrameNavigator::frame_index_at_point(const SDL_Point& p) const {
    if (!SDL_PointInRect(&p, &strip_rect_)) {
        return -1;
    }
    const float stride = static_cast<float>(kThumbSize + kThumbSpacing);
    float rel_x = static_cast<float>(p.x - strip_rect_.x) + scroll_offset_;
    if (rel_x < 0.0f) return -1;
    int idx = static_cast<int>(rel_x / stride);
    float offset_in_tile = rel_x - static_cast<float>(idx) * stride;
    if (offset_in_tile > static_cast<float>(kThumbSize)) {
        return -1;
    }
    if (idx < 0 || idx >= frame_count_) return -1;
    return idx;
}

void FrameNavigator::update_hover(const SDL_Point& p) {
    int idx = frame_index_at_point(p);
    hovered_index_ = idx;
}

void FrameNavigator::reset_hover() {
    hovered_index_ = -1;
    pressed_thumb_index_ = -1;
}

SDL_Rect FrameNavigator::compute_thumb_rect(int index) const {
    const float stride = static_cast<float>(kThumbSize + kThumbSpacing);
    const float x = static_cast<float>(strip_rect_.x) + static_cast<float>(index) * stride - scroll_offset_;
    return SDL_Rect{
        static_cast<int>(std::lround(x)),
        strip_rect_.y,
        kThumbSize,
        kThumbSize
    };
}

void FrameNavigator::render_background(SDL_Renderer* renderer) const {
    SDL_Color bar_bg = DMStyles::PanelHeader();
    SDL_SetRenderDrawColor(renderer, bar_bg.r, bar_bg.g, bar_bg.b, bar_bg.a);
    SDL_FRect rect_f = ToFRect(rect_);
    SDL_RenderFillRect(renderer, &rect_f);

    SDL_Color strip_bg = DMStyles::PanelBG();
    SDL_SetRenderDrawColor(renderer, strip_bg.r, strip_bg.g, strip_bg.b, strip_bg.a);
    SDL_FRect strip_f = ToFRect(strip_rect_);
    SDL_RenderFillRect(renderer, &strip_f);

    SDL_Color border = DMStyles::Border();
    SDL_Rect bottom_line{rect_.x, rect_.y + rect_.h - 2, rect_.w, 2};
    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
    SDL_FRect bottom_line_f = ToFRect(bottom_line);
    SDL_RenderFillRect(renderer, &bottom_line_f);
}

void FrameNavigator::render_thumbnails(SDL_Renderer* renderer) {
    if (strip_rect_.w <= 0 || strip_rect_.h <= 0) return;

    SDL_Rect prev_clip{};
    SDL_GetRenderClipRect(renderer, &prev_clip);
    const bool was_clipping = SDL_RenderClipEnabled(renderer);
    SDL_SetRenderClipRect(renderer, &strip_rect_);

    for (int i = 0; i < frame_count_; ++i) {
        SDL_Rect thumb = compute_thumb_rect(i);
        if (thumb.x + thumb.w < strip_rect_.x || thumb.x > strip_rect_.x + strip_rect_.w) {
            continue;
        }

        SDL_Color base = DMStyles::PanelHeader();
        dm_draw::DrawRoundedSolidRect(renderer, thumb, kThumbCorner, base);

        SDL_Texture* tex = nullptr;
        if (auto provider = preview_provider_.lock()) {
            tex = provider->get_frame_texture(renderer, animation_id_, i);
        }

        if (tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            if (SDL_GetTextureSize(tex, &tex_w, &tex_h) && tex_w > 0.0f && tex_h > 0.0f) {
                const float inset = 6.0f;
                const float avail_w = static_cast<float>(thumb.w) - inset * 2.0f;
                const float avail_h = static_cast<float>(thumb.h) - inset * 2.0f;
                const float scale = std::min(avail_w / tex_w, avail_h / tex_h);
                SDL_FRect dst{
                    static_cast<float>(thumb.x) + (static_cast<float>(thumb.w) - tex_w * scale) * 0.5f,
                    static_cast<float>(thumb.y) + (static_cast<float>(thumb.h) - tex_h * scale) * 0.5f,
                    tex_w * scale,
                    tex_h * scale
                };
                SDL_RenderTexture(renderer, tex, nullptr, &dst);
            }
        }

        const bool active = (i == current_frame_);
        if (active) {
            SDL_Color glow = DMStyles::AccentButton().bg;
            glow.a = static_cast<Uint8>(std::clamp<int>(static_cast<int>(glow.a * 0.45f), 0, 255));
            dm_draw::DrawRoundedSolidRect(renderer, thumb, kThumbCorner, glow);
        }

        SDL_Color border = active ? DMStyles::AccentButton().border : DMStyles::Border();
        dm_draw::DrawRoundedOutline(renderer, thumb, kThumbCorner, 2, border);

        if (hovered_index_ == i) {
            SDL_Color hover = DMStyles::HighlightColor();
            dm_draw::DrawRoundedOutline(renderer, thumb, kThumbCorner, 2, hover);
        }

        render_badge(renderer, thumb, i, active);
    }

    if (was_clipping) {
        SDL_SetRenderClipRect(renderer, &prev_clip);
    } else {
        SDL_SetRenderClipRect(renderer, nullptr);
    }
}

void FrameNavigator::render_badge(SDL_Renderer* renderer, const SDL_Rect& thumb_rect, int index, bool active) const {
    const SDL_Color badge_bg = active ? DMStyles::AccentButton().bg : DMStyles::PanelHeader();
    const SDL_Color badge_border = active ? DMStyles::AccentButton().border : DMStyles::Border();
    SDL_Rect badge{
        thumb_rect.x + kBadgePadding,
        thumb_rect.y + thumb_rect.h - kBadgeHeight - kBadgePadding,
        thumb_rect.w - kBadgePadding * 2,
        kBadgeHeight
    };
    SDL_SetRenderDrawColor(renderer, badge_bg.r, badge_bg.g, badge_bg.b, badge_bg.a);
    SDL_FRect badge_f = ToFRect(badge);
    SDL_RenderFillRect(renderer, &badge_f);
    SDL_SetRenderDrawColor(renderer, badge_border.r, badge_border.g, badge_border.b, badge_border.a);
    SDL_RenderRect(renderer, &badge_f);

    const SDL_Color text_color = active ? DMStyles::AccentButton().text : DMStyles::Label().color;
    DMLabelStyle label_style{DMStyles::Label().font_path, 12, text_color};
    const std::string label = std::to_string(index);
    SDL_Point text_size = DMFontCache::instance().measure_text(label_style, label);
    const int text_x = badge.x + badge.w - text_size.x - 4;
    const int text_y = badge.y + (badge.h - text_size.y) / 2;
    DMFontCache::instance().draw_text(renderer, label_style, label, text_x, text_y);
}

bool FrameNavigator::confirm_action(const std::string& title, const std::string& message) const {
    if (on_confirm_) {
        return on_confirm_(title, message);
    }

    return devmode::dialogs::confirm(parent_window_, title, message, "Yes", "No", false);
}

void FrameNavigator::handle_apply_next() {
    if (on_apply_next_) {
        on_apply_next_();
    }
}

void FrameNavigator::handle_apply_animation() {
    if (on_apply_animation_) {
        const std::string title = "Apply To Selected Animations";
        const std::string msg = "Apply current anchor set to every frame in selected animation scope?";
        if (!confirm_action(title, msg)) {
            return;
        }
        on_apply_animation_();
    }
}

void FrameNavigator::handle_apply_all() {
    if (on_apply_all_) {
        const std::string title = "Apply To All Animations";
        const std::string msg = "Apply current anchor set to every frame in every animation for this asset?";
        if (!confirm_action(title, msg)) {
            return;
        }
        on_apply_all_();
    }
}

void FrameNavigator::handle_save_and_exit() {
    if (on_save_and_exit_) {
        on_save_and_exit_();
    }
}

void FrameNavigator::update_button_states() {
    prev_enabled_ = enabled_ && frame_count_ > 0 && current_frame_ > 0;
    next_enabled_ = enabled_ && frame_count_ > 0 && current_frame_ < frame_count_ - 1;

    if (btn_prev_) {
        btn_prev_->set_text(std::string(DMIcons::NavLeft()));
        btn_prev_->set_style(prev_enabled_ ? &DMStyles::IconButton() : &DMStyles::ListButton());
    }

    if (btn_next_) {
        btn_next_->set_text(std::string(DMIcons::NavRight()));
        btn_next_->set_style(next_enabled_ ? &DMStyles::IconButton() : &DMStyles::ListButton());
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

}  // namespace devmode::frame_editors
