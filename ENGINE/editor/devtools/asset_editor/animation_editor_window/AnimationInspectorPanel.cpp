#include "AnimationInspectorPanel.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/ttf_render_utils.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <functional>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "AnimationDocument.hpp"
#include "AudioPanel.hpp"
#include "OnEndSelector.hpp"
#include "PlaybackSettingsPanel.hpp"
#include "PreviewProvider.hpp"
#include "PreviewTimeline.hpp"
#include "SourceConfigPanel.hpp"
#include "json_coercion.hpp"
#include "string_utils.hpp"
#include "dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/widgets.hpp"
#include "assets/asset/animation.hpp"
#include <nlohmann/json.hpp>

namespace animation_editor {

namespace {

constexpr int kInspectorPadding    = 14;
constexpr int kInspectorItemGap    = 10;
constexpr int kInspectorSectionGap = 14;
constexpr int kSectionHeaderHeight = 30;

constexpr int kPreviewHeight = 80;
constexpr int kHeaderButtonWidth = 160;
constexpr int kMinToggleButtonWidth = 120;
constexpr int kScrollWheelStep = 20;
constexpr int kScrollbarWidth = 8;
constexpr int kScrollbarMinThumbHeight = 28;

int preview_controls_height() {
    return 0;
}

class ClipScope {
  public:
    ClipScope(SDL_Renderer* renderer, const SDL_Rect& clip)
        : renderer_(renderer) {
        if (!renderer_ || clip.w <= 0 || clip.h <= 0) {
            return;
        }
        previous_clip_enabled_ = SDL_RenderClipEnabled(renderer_);
        if (previous_clip_enabled_) {
            SDL_GetRenderClipRect(renderer_, &previous_clip_);
        }
        SDL_SetRenderClipRect(renderer_, &clip);
        active_ = true;
    }

    ~ClipScope() { restore(); }

    void restore() {
        if (!renderer_ || !active_) {
            return;
        }
        if (previous_clip_enabled_) {
            SDL_SetRenderClipRect(renderer_, &previous_clip_);
        } else {
            SDL_SetRenderClipRect(renderer_, nullptr);
        }
        active_ = false;
    }

  private:
    SDL_Renderer* renderer_ = nullptr;
    SDL_Rect previous_clip_{0, 0, 0, 0};
    bool previous_clip_enabled_ = false;
    bool active_ = false;
};

void render_label(SDL_Renderer* renderer, const DMLabelStyle& style, const std::string& text, int x, int y, SDL_Color color) {
    if (!renderer || text.empty()) {
        return;
    }

    TTF_Font* font = style.open_font();
    if (!font) {
        return;
    }

    SDL_Surface* surface = ttf_util::RenderTextBlended(font, text.c_str(), color);
    if (!surface) {
        TTF_CloseFont(font);
        return;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture) {
        SDL_Rect dst{x, y, surface->w, surface->h};
        sdl_render::Texture(renderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }

    SDL_DestroySurface(surface);
    TTF_CloseFont(font);
}

void render_label(SDL_Renderer* renderer, const std::string& text, int x, int y, SDL_Color color) {
    render_label(renderer, DMStyles::Label(), text, x, y, color);
}

int text_width(const DMLabelStyle& style, const std::string& text) {
    TTF_Font* font = style.open_font();
    if (!font) {
        return 0;
    }
    int width = 0;
    int height = 0;
    if (!ttf_util::GetStringSize(font, text, &width, &height)) {
        width = 0;
    }
    TTF_CloseFont(font);
    return width;
}

int resolve_wheel_delta(const SDL_MouseWheelEvent& wheel) {
    int delta = wheel.integer_y;
    if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        delta = -delta;
    }
    if (delta == 0) {
        float precise = wheel.y;
        if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            precise = -precise;
        }
        delta = static_cast<int>(std::round(precise));
        if (delta == 0 && precise != 0.0f) {
            delta = precise > 0.0f ? 1 : -1;
        }
    }
    return delta;
}

bool is_pointer_event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_MOTION:
            return true;
        default:
            return false;
    }
}

void draw_section_header(SDL_Renderer* renderer,
                         const SDL_Rect& rect,
                         const std::string& title) {
    if (!renderer || rect.w <= 0 || rect.h <= 0) {
        return;
    }

    dm_draw::DrawBeveledRect(renderer, rect, DMStyles::CornerRadius(), DMStyles::BevelDepth(), DMStyles::PanelHeader(), DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());

    const int text_x = rect.x + DMSpacing::small_gap() * 2;
    const int text_y = rect.y + std::max(0, (rect.h - DMStyles::Label().font_size) / 2);
    render_label(renderer, title, text_x, text_y, DMStyles::Label().color);

}

struct LayoutCursor {
    int logical_y = 0;
    int scroll = 0;

    LayoutCursor(int logical_start, int scroll_offset)
        : logical_y(logical_start), scroll(scroll_offset) {}

    int visual_y() const { return logical_y - scroll; }
    void advance(int delta) { logical_y += delta; }
};

}

AnimationInspectorPanel::AnimationInspectorPanel() {
    source_frames_button_ = std::make_unique<DMButton>("Frames", &DMStyles::AccentButton(), 120, DMButton::height());
    source_animation_button_ = std::make_unique<DMButton>("Animation", &DMStyles::HeaderButton(), 120, DMButton::height());
    scroll_controller_.set_step_pixels(kScrollWheelStep);
}

void AnimationInspectorPanel::set_document(std::shared_ptr<AnimationDocument> document) {
    document_ = std::move(document);
    reset_payload_notification_state();
    rebuild_widgets();
}

void AnimationInspectorPanel::set_animation_id(const std::string& animation_id) {
    animation_id_ = animation_id;
    reset_payload_notification_state();
    rebuild_widgets();
}

void AnimationInspectorPanel::set_bounds(const SDL_Rect& bounds) {
    bounds_ = bounds;
    scroll_controller_.set_bounds(bounds_);
    layout_dirty_ = true;
}

void AnimationInspectorPanel::set_preview_provider(std::shared_ptr<PreviewProvider> provider) {
    preview_provider_ = std::move(provider);
}

void AnimationInspectorPanel::set_task_queue(std::shared_ptr<AsyncTaskQueue> tasks) {
    task_queue_ = std::move(tasks);
    apply_dependencies();
}

void AnimationInspectorPanel::set_source_folder_picker(PathPicker picker) {
    folder_picker_ = std::move(picker);
    apply_dependencies();
}

void AnimationInspectorPanel::set_source_animation_picker(AnimationPicker picker) {
    animation_picker_ = std::move(picker);
    apply_dependencies();
}

void AnimationInspectorPanel::set_source_gif_picker(PathPicker picker) {
    gif_picker_ = std::move(picker);
    apply_dependencies();
}

void AnimationInspectorPanel::set_source_png_sequence_picker(MultiPathPicker picker) {
    png_sequence_picker_ = std::move(picker);
    apply_dependencies();
}

void AnimationInspectorPanel::set_source_status_callback(StatusCallback callback) {
    status_callback_ = std::move(callback);
    apply_dependencies();
}

void AnimationInspectorPanel::set_frame_edit_callback(FrameEditCallback callback) {
    frame_edit_callback_ = std::move(callback);
    apply_dependencies();
}

void AnimationInspectorPanel::set_frame_mode_edit_callback(FrameModeEditCallback callback) {
    frame_mode_edit_callback_ = std::move(callback);
    apply_dependencies();
}

void AnimationInspectorPanel::set_navigate_to_animation_callback(AnimationNavigateCallback callback) {
    navigate_to_animation_callback_ = std::move(callback);
    apply_dependencies();
}

void AnimationInspectorPanel::set_audio_importer(std::shared_ptr<AudioImporter> importer) {
    audio_importer_ = std::move(importer);
    apply_dependencies();
}

void AnimationInspectorPanel::set_audio_file_picker(AudioFilePicker picker) {
    audio_file_picker_ = std::move(picker);
    apply_dependencies();
}

void AnimationInspectorPanel::set_manifest_store(devmode::core::ManifestStore* store) {
    manifest_store_ = store;
}

int AnimationInspectorPanel::height_for_width(int width) const {
    const int padding = kInspectorPadding;
    const int item_gap = kInspectorItemGap;
    const int section_gap = kInspectorSectionGap;
    const int header_height = std::max(DMTextBox::height(), DMButton::height());
    const int content_width = std::max(0, width - padding * 2);

    int total = padding;
    total += header_height;
    total += section_gap;

    total += kSectionHeaderHeight;
    total += item_gap;
    total += DMButton::height();
    if (source_config_) {
        int source_height = source_config_->preferred_height(content_width);
        if (source_height > 0) {
            total += item_gap;
            total += source_height;
        }
    }

    total += section_gap;
    total += kSectionHeaderHeight;
    total += item_gap;
    total += kPreviewHeight;

    auto add_section = [&](auto* widget) {
        if (!widget) {
            return;
        }
        total += section_gap;
        total += kSectionHeaderHeight;
        int height = widget->preferred_height(content_width);
        if (height > 0) {
            total += item_gap;
            total += height;
        }
    };

    add_section(playback_settings_.get());
    add_section(on_end_selector_.get());
    add_section(audio_panel_.get());

    total += padding;
    return total;
}

void AnimationInspectorPanel::update() {
    refresh_preview_metadata();
    ensure_preview_controls();
    layout_widgets();

    if (rename_pending_ && name_box_ && !name_box_->is_editing()) {
        commit_rename();
    }

    refresh_start_indicator();

    if (source_config_) {
        source_config_->update();
    }

    bool current_mode_animation = source_config_ && source_config_->use_animation_reference();
    if (current_mode_animation != source_uses_animation_) {
        source_uses_animation_ = current_mode_animation;
        update_source_mode_button_styles();
        layout_dirty_ = true;
    }

    update_preview_playback();

    if (playback_settings_) playback_settings_->update();
    if (on_end_selector_) on_end_selector_->update();
    if (audio_panel_) audio_panel_->update();
    notify_payload_change_if_needed();
}

void AnimationInspectorPanel::apply_dropdown_selections() {
    if (source_config_) {
        source_config_->commit_animation_dropdown_selection();
    }
    notify_payload_change_if_needed();
}

void AnimationInspectorPanel::render(SDL_Renderer* renderer) const {
    if (!renderer) {
        return;
    }

    refresh_preview_metadata();
    layout_widgets();
    update_scrollbar_geometry();

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    ui::draw_panel_background(renderer, bounds_);

    dm_draw::DrawBeveledRect(renderer, header_rect_, DMStyles::CornerRadius(), DMStyles::BevelDepth(), DMStyles::PanelHeader(), DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());

    if (name_box_) name_box_->render(renderer);
    if (start_button_) start_button_->render(renderer);

    if (is_start_animation_) {
        const DMLabelStyle& style = DMStyles::Label();
        SDL_Color accent = DMStyles::AccentButton().text;
        render_label(renderer, "Start Animation", header_rect_.x + kInspectorPadding, header_rect_.y + header_rect_.h - style.font_size - DMSpacing::small_gap(), accent);
    }

    {
        ClipScope content_clip(renderer, bounds_);
        ClipScope scroll_clip(renderer, scrollable_bounds_);

        draw_section_header(renderer, source_section_header_rect_, "Source");
        if (source_frames_button_) source_frames_button_->render(renderer);
        if (source_animation_button_) source_animation_button_->render(renderer);
        if (source_config_ && source_rect_.h > 0 && source_rect_.w > 0) {
            source_config_->render(renderer);
        }

        draw_section_header(renderer, preview_section_rect_, "Preview");
        render_preview(renderer);

        if (playback_settings_) {
            draw_section_header(renderer, playback_header_rect_, "Playback");
            playback_settings_->render(renderer);
        }
        if (on_end_selector_) {
            draw_section_header(renderer, on_end_header_rect_, "On End");
            on_end_selector_->render(renderer);
        }
        if (audio_panel_) {
            draw_section_header(renderer, audio_header_rect_, "Audio");
            audio_panel_->render(renderer);
        }
    }

    render_scrollbar(renderer);
    render_overlays(renderer);
}

void AnimationInspectorPanel::set_scrub_mode(bool enable) {
    if (scrub_mode_ == enable) {
        return;
    }
    scrub_mode_ = enable;
    if (scrub_mode_) {
        preview_scrubbing_active_ = false;
        was_playing_before_scrub_ = false;
        if (preview_timeline_) {
            preview_timeline_->pause();
        }
    } else if (preview_timeline_) {
        preview_timeline_->play();
    }
}

void AnimationInspectorPanel::set_scrub_frame(int frame) {
    scrub_frame_ = frame;
    if (frame_count_ > 0) {
        scrub_frame_ = std::clamp(scrub_frame_, 0, frame_count_ - 1);
    }
    if (scrub_mode_) {
        sync_timeline_to_slider(scrub_frame_);
    }
}

bool AnimationInspectorPanel::handle_event(const SDL_Event& e) {
    layout_widgets();
    update_scrollbar_geometry();

    if (is_pointer_event(e)) {
        SDL_Point p;
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            p.x = e.motion.x;
            p.y = e.motion.y;
        } else {
            p.x = e.button.x;
            p.y = e.button.y;
        }
        const bool inside_bounds = SDL_PointInRect(&p, &bounds_) != 0;
        if (!inside_bounds) {
            bool allow_out_of_bounds = false;
            if (source_config_ && source_config_->allow_out_of_bounds_pointer_events()) {
                allow_out_of_bounds = true;
            }
            if (!allow_out_of_bounds && on_end_selector_ && on_end_selector_->allow_out_of_bounds_pointer_events()) {
                allow_out_of_bounds = true;
            }
            if (!allow_out_of_bounds) {
                return false;
            }
        }
    }

    bool handled = false;
    bool was_editing = name_box_ && name_box_->is_editing();

    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_TAB) {
            auto order = focus_order();
            if (!order.empty()) {
                int direction = (e.key.mod & SDL_KMOD_SHIFT) ? -1 : 1;
                int count = static_cast<int>(order.size());
                int next = focus_index_;
                if (next < 0 || next >= count) {
                    next = (direction > 0) ? -1 : 0;
                }
                next += direction;
                if (next < 0) {
                    next = count - 1;
                } else if (next >= count) {
                    next = 0;
                }
                set_focus(order[next]);
                handled = true;
            }
        } else {
            auto order = focus_order();
            if (focus_index_ >= 0 && focus_index_ < static_cast<int>(order.size())) {
                FocusTarget target = order[focus_index_];
                if ((e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER ||
                     e.key.key == SDLK_SPACE) &&
                    !(target == FocusTarget::kName && name_box_ && name_box_->is_editing())) {
                    activate_focus_target(target);
                    handled = true;
                }
            }
        }
    } else if (e.type == SDL_EVENT_TEXT_INPUT) {

    }

    if (scrollbar_visible_ && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        if (SDL_PointInRect(&p, &scrollbar_track_)) {
            const int track_range = std::max(0, scrollbar_track_.h - scrollbar_thumb_.h);
            int relative = std::clamp(p.y - scrollbar_track_.y - scrollbar_thumb_.h / 2, 0, track_range);
            float ratio = (track_range > 0) ? static_cast<float>(relative) / static_cast<float>(track_range) : 0.0f;
            int max_scroll = std::max(0, content_height_ - bounds_.h);
            int new_scroll = static_cast<int>(std::round(ratio * max_scroll));
            scroll_controller_.set_scroll(new_scroll);
            layout_dirty_ = true;
            handled = true;
        }
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        FocusTarget clicked = FocusTarget::kNone;
        if (name_box_ && SDL_PointInRect(&p, &name_box_->rect())) {
            clicked = FocusTarget::kName;
        } else if (start_button_ && SDL_PointInRect(&p, &start_button_->rect())) {
            clicked = FocusTarget::kStart;
        } else if (source_frames_button_ && SDL_PointInRect(&p, &source_frames_button_->rect())) {
            clicked = FocusTarget::kSourceFrames;
        } else if (source_animation_button_ && SDL_PointInRect(&p, &source_animation_button_->rect())) {
            clicked = FocusTarget::kSourceAnimation;
        }
        set_focus(clicked);
    }

    if (widget_registry_.handle_event(e)) {
        handled = true;
    }

    if (source_config_ && source_config_->handle_event(e)) handled = true;
    if (playback_settings_ && playback_settings_->handle_event(e)) handled = true;
    if (on_end_selector_ && on_end_selector_->handle_event(e)) handled = true;
    if (audio_panel_ && audio_panel_->handle_event(e)) handled = true;

    if (was_editing && name_box_ && !name_box_->is_editing()) {
        rename_pending_ = true;
    }

    if (handle_scroll_wheel(e)) {
        handled = true;
    }

    return handled;
}

void AnimationInspectorPanel::rebuild_widgets() {
    widget_registry_.reset();

    if (!document_ || animation_id_.empty()) {
        return;
    }

    if (!preview_timeline_) {
        preview_timeline_ = std::make_unique<PreviewTimeline>();
    }

    if (!name_box_) {
        name_box_ = std::make_unique<DMTextBox>("Animation ID", animation_id_);
    } else {
        name_box_->set_value(animation_id_);
    }
    if (name_box_) {
        widget_registry_.add_handler([this](const SDL_Event& ev) {
            if (!name_box_) {
                return false;
            }
            if (!name_box_->handle_event(ev)) {
                return false;
            }
            rename_pending_ = true;
            return true;
        });
    }

    if (!start_button_) {
        start_button_ = std::make_unique<DMButton>("Set as Start", &DMStyles::AccentButton(), kHeaderButtonWidth, DMButton::height());
    }
    if (start_button_) {
        widget_registry_.add_handler([this](const SDL_Event& ev) {
            if (!start_button_) {
                return false;
            }
            if (!start_button_->handle_event(ev)) {
                return false;
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) {
                activate_focus_target(FocusTarget::kStart);
            }
            return true;
        });
    }

    if (!source_config_) {
        source_config_ = std::make_unique<SourceConfigPanel>();
    }
    source_config_->set_document(document_);
    source_config_->set_animation_id(animation_id_);
    source_uses_animation_ = source_config_->use_animation_reference();

    if (!source_frames_button_) {
        source_frames_button_ = std::make_unique<DMButton>("Frames", &DMStyles::AccentButton(), 120, DMButton::height());
    }
    if (!source_animation_button_) {
        source_animation_button_ = std::make_unique<DMButton>("Animation", &DMStyles::HeaderButton(), 120, DMButton::height());
    }
    update_source_mode_button_styles();
    if (source_frames_button_) {
        widget_registry_.add_handler([this](const SDL_Event& ev) {
            if (!source_frames_button_) {
                return false;
            }
            if (!source_frames_button_->handle_event(ev)) {
                return false;
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) {
                activate_focus_target(FocusTarget::kSourceFrames);
            }
            return true;
        });
    }
    if (source_animation_button_) {
        widget_registry_.add_handler([this](const SDL_Event& ev) {
            if (!source_animation_button_) {
                return false;
            }
            if (!source_animation_button_->handle_event(ev)) {
                return false;
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) {
                activate_focus_target(FocusTarget::kSourceAnimation);
            }
            return true;
        });
    }

    if (!playback_settings_) {
        playback_settings_ = std::make_unique<PlaybackSettingsPanel>();
    }
    playback_settings_->set_document(document_);
    playback_settings_->set_animation_id(animation_id_);

    if (!on_end_selector_) {
        on_end_selector_ = std::make_unique<OnEndSelector>();
    }
    on_end_selector_->set_document(document_);
    on_end_selector_->set_animation_id(animation_id_);

    if (!audio_panel_) {
        audio_panel_ = std::make_unique<AudioPanel>();
    }
    audio_panel_->set_document(document_);
    audio_panel_->set_animation_id(animation_id_);

    rename_pending_ = false;
    refresh_start_indicator();
    layout_dirty_ = true;
    apply_dependencies();
}

void AnimationInspectorPanel::refresh_totals() {
}

void AnimationInspectorPanel::layout_widgets() const {
    if (!layout_dirty_) {
        return;
    }

    auto* self = const_cast<AnimationInspectorPanel*>(this);
    self->layout_dirty_ = false;

    const int padding = kInspectorPadding;
    const int item_gap = kInspectorItemGap;
    const int section_gap = kInspectorSectionGap;
    const int button_gap = DMSpacing::small_gap();

    const int width = std::max(0, bounds_.w - padding * 2);
    const int x = bounds_.x + padding;

    const int button_height = DMButton::height();
    int action_width = start_button_ ? std::min(kHeaderButtonWidth, width) : 0;
    int name_left = x;
    int name_right = x + width - action_width;
    if (action_width > 0) {
        name_right -= button_gap;
    }
    int name_width = std::max(0, name_right - name_left);

    int name_height = DMTextBox::height();
    if (name_box_) {
        name_height = name_box_->height_for_width(name_width);
        SDL_Rect rect{name_left, bounds_.y + padding, name_width, name_height};
        name_box_->set_rect(rect);
    }

    if (start_button_) {
        int effective_button_width = std::min(kHeaderButtonWidth, width);
        SDL_Rect rect{x + width - effective_button_width, bounds_.y + padding, effective_button_width, button_height};
        start_button_->set_rect(rect);
    }

    const int header_content_height = std::max(name_height, button_height);
    const int header_total_height = header_content_height + padding;
    self->header_rect_ = SDL_Rect{bounds_.x, bounds_.y, bounds_.w, header_total_height};

    const int content_top = bounds_.y + header_total_height + section_gap;
    const int scroll_height = std::max(0, bounds_.y + bounds_.h - content_top);
    self->scrollable_bounds_ = SDL_Rect{x, content_top, width, scroll_height};
    self->scroll_controller_.set_bounds(self->scrollable_bounds_);
    const int scroll = self->scroll_controller_.scroll();
    LayoutCursor cursor(content_top, scroll);

    self->source_section_header_rect_ = SDL_Rect{x, cursor.visual_y(), width, kSectionHeaderHeight};
    self->source_section_rect_ = self->source_section_header_rect_;
    cursor.advance(kSectionHeaderHeight);

    cursor.advance(item_gap);
    const int selector_height = DMButton::height();
    const int selector_gap = DMSpacing::small_gap();
    self->source_selector_rect_ = SDL_Rect{x, cursor.visual_y(), width, selector_height};
    int frames_width = std::max(0, (width - selector_gap) / 2);
    int animation_width = std::max(0, width - frames_width - selector_gap);
    if (source_frames_button_) {
        SDL_Rect rect{x, cursor.visual_y(), frames_width, selector_height};
        source_frames_button_->set_rect(rect);
    }
    if (source_animation_button_) {
        SDL_Rect rect{x + frames_width + selector_gap, cursor.visual_y(), animation_width, selector_height};
        source_animation_button_->set_rect(rect);
    }
    cursor.advance(selector_height);

    int source_height = source_config_ ? source_config_->preferred_height(width) : 0;
    if (source_height > 0) {
        cursor.advance(item_gap);
        self->source_rect_ = SDL_Rect{x, cursor.visual_y(), width, source_height};
        if (source_config_) {
            source_config_->set_bounds(self->source_rect_);
        }
        cursor.advance(source_height);
    } else {
        self->source_rect_ = SDL_Rect{x, cursor.visual_y(), width, 0};
        if (source_config_) {
            source_config_->set_bounds(self->source_rect_);
        }
    }
    self->source_section_rect_.h = std::max(0, cursor.visual_y() - self->source_section_rect_.y);

    cursor.advance(section_gap);
    self->preview_section_rect_ = SDL_Rect{x, cursor.visual_y(), width, kSectionHeaderHeight};
    cursor.advance(kSectionHeaderHeight);
    cursor.advance(item_gap);

    self->preview_controls_rect_ = SDL_Rect{x, cursor.visual_y(), width, 0};

    self->preview_rect_ = SDL_Rect{x, cursor.visual_y(), width, kPreviewHeight};
    cursor.advance(kPreviewHeight);

    auto place_static_section = [&](auto* widget,
                                    SDL_Rect& header_rect,
                                    SDL_Rect& content_rect,
                                    SDL_Rect& section_rect) {
        if (!widget) {
            auto reset_rect = [&](SDL_Rect& rect) { rect = SDL_Rect{x, cursor.visual_y(), width, 0}; };
            reset_rect(header_rect);
            reset_rect(content_rect);
            reset_rect(section_rect);
            return;
        }

        cursor.advance(section_gap);
        section_rect = SDL_Rect{x, cursor.visual_y(), width, 0};
        header_rect = SDL_Rect{x, cursor.visual_y(), width, kSectionHeaderHeight};
        cursor.advance(kSectionHeaderHeight);
        int content_height = widget->preferred_height(width);
        if (content_height > 0) {
            cursor.advance(item_gap);
            content_rect = SDL_Rect{x, cursor.visual_y(), width, content_height};
            widget->set_bounds(content_rect);
            cursor.advance(content_height);
        } else {
            content_rect = SDL_Rect{x, cursor.visual_y(), width, 0};
            widget->set_bounds(content_rect);
        }

        section_rect.h = std::max(0, cursor.visual_y() - section_rect.y);
    };

    place_static_section(playback_settings_.get(), self->playback_header_rect_, self->playback_rect_, self->playback_section_rect_);
    place_static_section(on_end_selector_.get(), self->on_end_header_rect_, self->on_end_rect_, self->on_end_section_rect_);
    place_static_section(audio_panel_.get(), self->audio_header_rect_, self->audio_rect_, self->audio_section_rect_);

    self->content_height_ = cursor.logical_y + padding - content_top;
    const int previous_scroll = scroll;
    self->scroll_controller_.set_content_height(self->content_height_);
    if (self->scroll_controller_.scroll() != previous_scroll) {
        self->layout_dirty_ = true;
    }

    refresh_focus_index();
}

void AnimationInspectorPanel::ensure_preview_controls() {
    if (!preview_timeline_) {
        preview_timeline_ = std::make_unique<PreviewTimeline>();
    }
    preview_timeline_->set_frame_count(std::max(1, frame_count_));
    preview_timeline_->set_fps(static_cast<float>(kBaseAnimationFps));
    if (!scrub_mode_ && !preview_timeline_->is_playing()) {
        preview_timeline_->play();
    }
}

void AnimationInspectorPanel::update_preview_playback() {
    if (!preview_timeline_) {
        return;
    }

    preview_timeline_->set_frame_count(std::max(1, frame_count_));
    preview_timeline_->set_fps(static_cast<float>(kBaseAnimationFps));

    if (scrub_mode_) {
        preview_timeline_->pause();
        current_frame_ = std::clamp(scrub_frame_, 0, std::max(0, frame_count_ - 1));
    } else {
        if (!preview_timeline_->is_playing()) {
            preview_timeline_->play();
        }
        preview_timeline_->update();
        int timeline_frame = std::clamp(preview_timeline_->current_frame(), 0, std::max(0, frame_count_ - 1));
        current_frame_ = display_frame_from_timeline(timeline_frame);
    }
}

void AnimationInspectorPanel::render_preview_controls(SDL_Renderer* renderer) const {
    (void)renderer;
}

void AnimationInspectorPanel::render_preview(SDL_Renderer* renderer) const {
    if (!renderer || preview_rect_.w <= 0 || preview_rect_.h <= 0) {
        return;
    }

    dm_draw::DrawBeveledRect(renderer, preview_rect_, DMStyles::CornerRadius(), DMStyles::BevelDepth(), DMStyles::PanelHeader(), DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());

    SDL_Rect preview_clip = preview_rect_;
    const int preview_inset = DMStyles::BevelDepth();
    preview_clip.x += preview_inset;
    preview_clip.y += preview_inset;
    preview_clip.w = std::max(0, preview_clip.w - preview_inset * 2);
    preview_clip.h = std::max(0, preview_clip.h - preview_inset * 2);

    auto draw_contents = [&]() {
        int max_frame = std::max(0, frame_count_ - 1);
        int frame_to_render = std::clamp(current_frame_, 0, max_frame);
        SDL_Texture* texture = preview_provider_ ? preview_provider_->get_frame_texture(renderer, animation_id_, frame_to_render) : nullptr;
        if (texture) {
            int tex_w = 0;
            int tex_h = 0;
            float tex_wf = 0.0f;
            float tex_hf = 0.0f;
            if (SDL_GetTextureSize(texture, &tex_wf, &tex_hf)) {
                tex_w = static_cast<int>(std::lround(tex_wf));
                tex_h = static_cast<int>(std::lround(tex_hf));
            }
            const int padding = kInspectorPadding;
            int avail_w = std::max(1, preview_rect_.w - padding * 2);
            int avail_h = std::max(1, preview_rect_.h - padding * 2);
            float scale = std::min(avail_w / static_cast<float>(tex_w), avail_h / static_cast<float>(tex_h));
            int draw_w = std::max(1, static_cast<int>(tex_w * scale));
            int draw_h = std::max(1, static_cast<int>(tex_h * scale));
            SDL_Rect dst{preview_rect_.x + (preview_rect_.w - draw_w) / 2,
                         preview_rect_.y + (preview_rect_.h - draw_h) / 2, draw_w, draw_h};

            SDL_FlipMode flip_flags = SDL_FLIP_NONE;
            if (preview_flip_x_) flip_flags = static_cast<SDL_FlipMode>(flip_flags | SDL_FLIP_HORIZONTAL);
            if (preview_flip_y_) flip_flags = static_cast<SDL_FlipMode>(flip_flags | SDL_FLIP_VERTICAL);

            sdl_render::TextureRotated(renderer, texture, nullptr, &dst, 0.0, nullptr, flip_flags);
        } else {
            const DMLabelStyle& style = DMStyles::Label();
            const std::string text = "No Preview Available";
            int label_w = text_width(style, text);
            SDL_Color color = style.color;
            render_label(renderer, text, preview_rect_.x + (preview_rect_.w - label_w) / 2, preview_rect_.y + preview_rect_.h / 2 - style.font_size / 2, color);
        }

};

    if (preview_clip.w > 0 && preview_clip.h > 0) {
        ClipScope scope(renderer, preview_clip);
        draw_contents();
    } else {
        draw_contents();
    }
}

void AnimationInspectorPanel::sync_slider_to_current_frame() {
}

void AnimationInspectorPanel::sync_timeline_to_slider(int display_frame) {
    int clamped = std::clamp(display_frame, 0, std::max(0, frame_count_ - 1));
    current_frame_ = clamped;
    int timeline_frame = timeline_frame_from_display(clamped);
    if (preview_timeline_) {
        preview_timeline_->set_current_frame(timeline_frame);
    }
}

int AnimationInspectorPanel::display_frame_from_timeline(int timeline_frame) const {
    int max_frame = std::max(0, frame_count_ - 1);
    timeline_frame = std::clamp(timeline_frame, 0, max_frame);
    if (!preview_reverse_) {
        return timeline_frame;
    }
    return max_frame - timeline_frame;
}

int AnimationInspectorPanel::timeline_frame_from_display(int display_frame) const {
    int max_frame = std::max(0, frame_count_ - 1);
    display_frame = std::clamp(display_frame, 0, max_frame);
    if (!preview_reverse_) {
        return display_frame;
    }
    return max_frame - display_frame;
}

void AnimationInspectorPanel::update_scrollbar_geometry() const {
    auto* self = const_cast<AnimationInspectorPanel*>(this);
    self->scrollbar_visible_ = false;
    self->scrollbar_track_ = SDL_Rect{0, 0, 0, 0};
    self->scrollbar_thumb_ = SDL_Rect{0, 0, 0, 0};

    if (scrollable_bounds_.h <= 0) {
        return;
    }
    const int max_scroll = std::max(0, content_height_ - scrollable_bounds_.h);
    if (max_scroll <= 0) {
        return;
    }

    const int inset = DMSpacing::small_gap();
    SDL_Rect track{bounds_.x + bounds_.w - kScrollbarWidth - inset,
                   scrollable_bounds_.y + inset,
                   kScrollbarWidth,
                   std::max(0, scrollable_bounds_.h - inset * 2)};
    if (track.h <= 0 || track.w <= 0) {
        return;
    }

    float visible_ratio = static_cast<float>(scrollable_bounds_.h) / static_cast<float>(content_height_);
    visible_ratio = std::clamp(visible_ratio, 0.05f, 1.0f);
    int thumb_h = std::max(kScrollbarMinThumbHeight, static_cast<int>(std::round(track.h * visible_ratio)));
    thumb_h = std::min(thumb_h, track.h);

    const int track_range = std::max(0, track.h - thumb_h);
    float scroll_ratio = (max_scroll > 0) ? static_cast<float>(scroll_controller_.scroll()) / static_cast<float>(max_scroll) : 0.0f;
    scroll_ratio = std::clamp(scroll_ratio, 0.0f, 1.0f);
    int thumb_y = track.y + static_cast<int>(std::round(track_range * scroll_ratio));

    SDL_Rect thumb{track.x, thumb_y, track.w, thumb_h};

    self->scrollbar_track_ = track;
    self->scrollbar_thumb_ = thumb;
    self->scrollbar_visible_ = true;
}

void AnimationInspectorPanel::render_scrollbar(SDL_Renderer* renderer) const {
    if (!renderer) {
        return;
    }
    update_scrollbar_geometry();
    if (!scrollbar_visible_) {
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Color track_color = DMStyles::PanelHeader();
    track_color.a = static_cast<Uint8>(std::min(120, static_cast<int>(track_color.a)));
    SDL_SetRenderDrawColor(renderer, track_color.r, track_color.g, track_color.b, track_color.a);
    sdl_render::FillRect(renderer, &scrollbar_track_);

    SDL_Color thumb_color = DMStyles::AccentButton().hover_bg;
    SDL_SetRenderDrawColor(renderer, thumb_color.r, thumb_color.g, thumb_color.b, 230);
    sdl_render::FillRect(renderer, &scrollbar_thumb_);
}

void AnimationInspectorPanel::render_overlays(SDL_Renderer* renderer) const {
    if (!renderer) {
        return;
    }
}

bool AnimationInspectorPanel::handle_scroll_wheel(const SDL_Event& e) {
    if (e.type != SDL_EVENT_MOUSE_WHEEL) {
        return false;
    }
    if (scrollable_bounds_.h <= 0) {
        return false;
    }
    int mx = 0;
    int my = 0;
    sdl_mouse_util::GetMouseState(&mx, &my);
    SDL_Point mouse{mx, my};
    if (!SDL_PointInRect(&mouse, &bounds_)) {
        return false;
    }

    bool over_source = source_config_ && SDL_PointInRect(&mouse, &source_rect_);
    bool dropdown_expanded = source_config_ && source_config_->allow_out_of_bounds_pointer_events();
    if (over_source && dropdown_expanded) {
        return false;
    }

    int delta = resolve_wheel_delta(e.wheel);
    if (delta == 0) {
        return false;
    }
    if (!scroll_controller_.apply_wheel_delta(delta)) {
        return false;
    }
    layout_dirty_ = true;
    return true;
}

void AnimationInspectorPanel::apply_dependencies() {
    if (source_config_) {
        source_config_->set_task_queue(task_queue_);
        source_config_->set_folder_picker(folder_picker_);
        source_config_->set_animation_picker(animation_picker_);
        source_config_->set_gif_picker(gif_picker_);
        source_config_->set_png_sequence_picker(png_sequence_picker_);
        source_config_->set_status_callback(status_callback_);

        source_config_->set_on_source_changed([this](const std::string& id) {
            if (playback_settings_) {
                playback_settings_->set_document(document_);
                playback_settings_->set_animation_id(id);
            }
            this->layout_dirty_ = true;
            notify_payload_change_if_needed(true);
        });
    }

    if (audio_panel_) {
        audio_panel_->set_importer(audio_importer_);
        audio_panel_->set_file_picker(audio_file_picker_);
    }

    notify_payload_change_if_needed(true);
}

void AnimationInspectorPanel::update_source_mode_button_styles() {
    bool animation_mode = source_config_ && source_config_->use_animation_reference();
    if (source_frames_button_) {
        source_frames_button_->set_style(animation_mode ? &DMStyles::HeaderButton() : &DMStyles::AccentButton());
    }
    if (source_animation_button_) {
        source_animation_button_->set_style(animation_mode ? &DMStyles::AccentButton() : &DMStyles::HeaderButton());
    }
}

void AnimationInspectorPanel::refresh_preview_metadata() const {
    auto* self = const_cast<AnimationInspectorPanel*>(this);
    if (!document_ || animation_id_.empty()) {
        self->preview_signature_.clear();
        self->preview_reverse_ = false;
        self->preview_flip_x_ = false;
        self->preview_flip_y_ = false;
        return;
    }

    auto payload_dump = document_->animation_payload(animation_id_);
    std::string signature = payload_dump.has_value() ? *payload_dump : std::string{};
    if (signature == self->preview_signature_) {
        return;
    }

    int previous_frame_count = self->frame_count_;
    self->preview_signature_ = signature;
    self->preview_reverse_ = false;
    self->preview_flip_x_ = false;
    self->preview_flip_y_ = false;
    self->frame_count_ = 1;

    if (!payload_dump.has_value()) {
        return;
    }

    nlohmann::json payload = nlohmann::json::parse(*payload_dump, nullptr, false);
    if (!payload.is_object()) {
        return;
    }

    bool derived = false;
    if (payload.contains("source") && payload["source"].is_object()) {
        const nlohmann::json& source = payload["source"];
        std::string kind = source.value("kind", std::string{});
        if (kind == "animation") {
            derived = true;
        }
    }

    if (derived) {
        self->preview_reverse_ = payload.value("reverse_source", false);
        self->preview_flip_x_ = payload.value("flipped_source", false);
        self->preview_flip_y_ = payload.value("flip_vertical_source", false);
        if (payload.contains("derived_modifiers") && payload["derived_modifiers"].is_object()) {
            const auto& modifiers = payload["derived_modifiers"];
            self->preview_reverse_ = modifiers.value("reverse", self->preview_reverse_);
            self->preview_flip_x_ = modifiers.value("flipX", self->preview_flip_x_);
            self->preview_flip_y_ = modifiers.value("flipY", self->preview_flip_y_);
        } else {
            self->preview_flip_y_ = payload.value("flip_vertical_source", false);
        }
    } else {
        self->preview_reverse_ = payload.value("reverse_source", false);
        self->preview_flip_x_ = payload.value("flipped_source", false);
        self->preview_flip_y_ = false;
    }

    if (payload.contains("number_of_frames")) {
        self->frame_count_ = json_coercion::read_int_field_like(payload, "number_of_frames", 1);
        if (self->frame_count_ <= 0) self->frame_count_ = 1;
    }
    if (self->frame_count_ != previous_frame_count) {
        self->preview_slider_max_frame_ = -1;
    }

}

void AnimationInspectorPanel::reset_payload_notification_state() {
    notified_payload_animation_id_.clear();
    notified_payload_signature_.clear();
}

void AnimationInspectorPanel::notify_payload_change_if_needed(bool force_notify) {
    if (!on_animation_properties_changed_ || !document_ || animation_id_.empty()) {
        return;
    }

    auto payload_json = document_->animation_payload_json(animation_id_);
    if (!payload_json.has_value() || !payload_json->is_object()) {
        return;
    }

    std::string signature = payload_json->dump();
    const bool changed = force_notify ||
                         notified_payload_animation_id_ != animation_id_ ||
                         notified_payload_signature_ != signature;
    if (!changed) {
        return;
    }

    notified_payload_animation_id_ = animation_id_;
    notified_payload_signature_ = signature;
    on_animation_properties_changed_(animation_id_, *payload_json);
}

std::vector<AnimationInspectorPanel::FocusTarget> AnimationInspectorPanel::focus_order() const {
    std::vector<FocusTarget> order;
    if (name_box_) order.push_back(FocusTarget::kName);
    if (start_button_) order.push_back(FocusTarget::kStart);
    if (source_frames_button_) order.push_back(FocusTarget::kSourceFrames);
    if (source_animation_button_) order.push_back(FocusTarget::kSourceAnimation);
    return order;
}

void AnimationInspectorPanel::set_focus(FocusTarget target) {
    current_focus_target_ = target;
    if (target == FocusTarget::kNone) {
        focus_index_ = -1;
        return;
    }
    auto order = focus_order();
    focus_index_ = -1;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == target) {
            focus_index_ = static_cast<int>(i);
            break;
        }
    }
    if (focus_index_ >= 0) {
        announce_focus(target);
    } else {
        current_focus_target_ = FocusTarget::kNone;
    }
}

void AnimationInspectorPanel::announce_focus(FocusTarget target) const {
    if (!status_callback_) {
        return;
    }

    switch (target) {
        case FocusTarget::kName:
            status_callback_("Focus: Animation name. Press Enter to begin editing.");
            break;
        case FocusTarget::kStart:
            status_callback_("Focus: Mark animation as start. Press Enter or Space to apply.");
            break;
        case FocusTarget::kSourceFrames:
            status_callback_("Focus: Select frame-based source mode. Press Enter or Space to choose.");
            break;
        case FocusTarget::kSourceAnimation:
            status_callback_("Focus: Select animation reference mode. Press Enter or Space to choose.");
            break;
        case FocusTarget::kNone:
        default:
            break;
    }
}

void AnimationInspectorPanel::activate_focus_target(FocusTarget target) {
    switch (target) {
        case FocusTarget::kName:
            if (status_callback_) {
                status_callback_("Press Enter inside the name field to begin editing.");
            }
            break;
        case FocusTarget::kStart:
            if (document_) {
                document_->set_start_animation(animation_id_);
            }
            refresh_start_indicator();
            if (status_callback_) {
                status_callback_("Animation marked as start animation.");
            }
            break;

        case FocusTarget::kSourceFrames:
            if (source_config_) {
                source_config_->set_source_mode(SourceConfigPanel::SourceMode::kFrames);
                source_uses_animation_ = source_config_->use_animation_reference();
                update_source_mode_button_styles();
                layout_dirty_ = true;
                if (status_callback_) {
                    status_callback_("Source mode set to Frames.");
                }
            }
            break;
        case FocusTarget::kSourceAnimation:
            if (source_config_) {
                source_config_->set_source_mode(SourceConfigPanel::SourceMode::kAnimation);
                source_uses_animation_ = source_config_->use_animation_reference();
                update_source_mode_button_styles();
                layout_dirty_ = true;
                if (status_callback_) {
                    status_callback_("Source mode set to Animation.");
                }
            }
            break;
        case FocusTarget::kNone:
        default:
            break;
    }
}

void AnimationInspectorPanel::refresh_focus_index() const {
    auto* self = const_cast<AnimationInspectorPanel*>(this);
    auto order = focus_order();
    self->focus_index_ = -1;
    if (self->current_focus_target_ == FocusTarget::kNone) {
        return;
    }
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == self->current_focus_target_) {
            self->focus_index_ = static_cast<int>(i);
            return;
        }
    }
    self->current_focus_target_ = FocusTarget::kNone;
}

void AnimationInspectorPanel::commit_rename() {
    if (!rename_pending_ || !document_ || !name_box_) {
        rename_pending_ = false;
        if (name_box_) {
            name_box_->set_value(animation_id_);
        }
        return;
    }

    std::string desired = strings::trim_copy(name_box_->value());
    if (desired.empty() || desired == animation_id_) {
        name_box_->set_value(animation_id_);
        rename_pending_ = false;
        return;
    }
    if (strings::is_reserved_animation_name(desired)) {
        name_box_->set_value(animation_id_);
        rename_pending_ = false;
        if (status_callback_) {
            status_callback_("Animation name '" + desired + "' is reserved.");
        }
        return;
    }

    const std::string old_id = animation_id_;
    auto before = document_->animation_ids();
    document_->rename_animation(animation_id_, desired);
    auto after = document_->animation_ids();

    std::string new_id = desired;
    if (std::find(after.begin(), after.end(), desired) == after.end()) {
        for (const auto& id : after) {
            if (std::find(before.begin(), before.end(), id) == before.end()) {
                new_id = id;
                break;
            }
        }
    }

    animation_id_ = new_id;
    name_box_->set_value(animation_id_);
    rename_pending_ = false;

    if (preview_provider_) {
        preview_provider_->invalidate(old_id);
        preview_provider_->invalidate(animation_id_);
    }

    refresh_totals();
    refresh_start_indicator();
    layout_dirty_ = true;
}

void AnimationInspectorPanel::refresh_start_indicator() {
    bool new_state = false;
    if (document_) {
        auto start = document_->start_animation();
        new_state = start.has_value() && *start == animation_id_;
    }

    is_start_animation_ = new_state;

    if (start_button_) {
        if (is_start_animation_) {
            start_button_->set_text("Start Animation");
            start_button_->set_style(&DMStyles::HeaderButton());
        } else {
            start_button_->set_text("Set as Start");
            start_button_->set_style(&DMStyles::AccentButton());
        }
    }
}

}




