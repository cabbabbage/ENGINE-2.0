#include "devtools/foreground_background_effect_panel.hpp"
#include "utils/sdl_render_conversions.hpp"

#include "core/AssetsManager.hpp"
#include "core/manifest/manifest_loader.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/widgets.hpp"
#include "utils/rebuild_queue.hpp"

#include <SDL3/SDL_timer.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

class LocalSpacerWidget : public Widget {
public:
    explicit LocalSpacerWidget(int h) : height_(h) {}

    void set_rect(const SDL_Rect& r) override { rect_ = r; }
    const SDL_Rect& rect() const override { return rect_; }
    int height_for_width(int) const override { return height_; }
    bool handle_event(const SDL_Event&) override { return false; }
    void render(SDL_Renderer*) const override {}
    bool wants_full_row() const override { return true; }

private:
    SDL_Rect rect_{0, 0, 0, 0};
    int height_ = 0;
};

class FlexibleSpacerWidget : public Widget {
public:
    explicit FlexibleSpacerWidget(int h) : height_(h) {}

    void set_height(int height) {
        const int clamped = std::max(0, height);
        if (height_ == clamped) {
            return;
        }
        height_ = clamped;
        request_layout();
    }

    void set_rect(const SDL_Rect& r) override { rect_ = r; }
    const SDL_Rect& rect() const override { return rect_; }
    int height_for_width(int) const override { return height_; }
    bool handle_event(const SDL_Event&) override { return false; }
    void render(SDL_Renderer*) const override {}
    bool wants_full_row() const override { return true; }

private:
    SDL_Rect rect_{0, 0, 0, 0};
    int height_ = 0;
};

class SectionLabelWidget : public Widget {
public:
    explicit SectionLabelWidget(std::string text) : text_(std::move(text)) {
        style_ = DMStyles::Label();
        style_.font_size = std::max(18, style_.font_size + 2);
    }

    void set_rect(const SDL_Rect& r) override { rect_ = r; }
    const SDL_Rect& rect() const override { return rect_; }
    int height_for_width(int) const override { return DMButton::height(); }
    bool handle_event(const SDL_Event&) override { return false; }

    void render(SDL_Renderer* renderer) const override {
        if (!renderer) {
            return;
        }
        const int text_y = rect_.y + std::max(0, (rect_.h - style_.font_size) / 2);
        DrawLabelText(renderer, text_, rect_.x, text_y, style_);
    }

private:
    SDL_Rect rect_{0, 0, 0, 0};
    std::string text_;
    DMLabelStyle style_{};
};

class PairRowWidget : public Widget {
public:
    PairRowWidget(Widget* left, Widget* right) : left_(left), right_(right) {}

    void set_rect(const SDL_Rect& r) override {
        rect_ = r;
        constexpr int kGap = 12;
        const int gap = std::min(kGap, std::max(0, r.w / 10));
        const int left_width = std::max(0, (r.w - gap) / 2);
        const int right_width = std::max(0, r.w - left_width - gap);

        if (left_) {
            left_->set_rect(SDL_Rect{r.x, r.y, left_width, r.h});
        }
        if (right_) {
            right_->set_rect(SDL_Rect{r.x + left_width + gap, r.y, right_width, r.h});
        }
    }

    const SDL_Rect& rect() const override { return rect_; }

    int height_for_width(int width) const override {
        constexpr int kGap = 12;
        const int gap = std::min(kGap, std::max(0, width / 10));
        const int left_width = std::max(1, (width - gap) / 2);
        const int right_width = std::max(1, width - left_width - gap);

        int left_height = 0;
        int right_height = 0;
        if (left_) {
            left_height = std::max(0, left_->height_for_width(left_width));
        }
        if (right_) {
            right_height = std::max(0, right_->height_for_width(right_width));
        }
        return std::max(left_height, right_height);
    }

    bool handle_event(const SDL_Event& e) override {
        bool handled = false;
        if (right_) {
            handled = right_->handle_event(e) || handled;
        }
        if (left_) {
            handled = left_->handle_event(e) || handled;
        }
        return handled;
    }

    void render(SDL_Renderer* renderer) const override {
        if (left_) {
            left_->render(renderer);
        }
        if (right_) {
            right_->render(renderer);
        }
    }

    bool wants_full_row() const override { return true; }

private:
    Widget* left_ = nullptr;
    Widget* right_ = nullptr;
    SDL_Rect rect_{0, 0, 0, 0};
};

class PreviewPaneWidget : public Widget {
public:
    explicit PreviewPaneWidget(std::string title) : title_(std::move(title)) {}

    void set_texture(SDL_Texture* texture, int width, int height) {
        texture_ = texture;
        texture_w_ = width;
        texture_h_ = height;
    }

    void set_status(std::string status) { status_ = std::move(status); }

    void set_preferred_height(int height) {
        const int clamped = std::max(180, height);
        if (preferred_height_ == clamped) {
            return;
        }
        preferred_height_ = clamped;
        request_layout();
    }

    void set_rect(const SDL_Rect& r) override { rect_ = r; }
    const SDL_Rect& rect() const override { return rect_; }
    int height_for_width(int) const override { return preferred_height_; }
    bool handle_event(const SDL_Event&) override { return false; }

    void render(SDL_Renderer* renderer) const override {
        if (!renderer || rect_.w <= 0 || rect_.h <= 0) {
            return;
        }

        SDL_SetRenderDrawColor(renderer, 16, 18, 24, 255);
        sdl_render::FillRect(renderer, &rect_);
        SDL_SetRenderDrawColor(renderer, 52, 58, 72, 255);
        sdl_render::Rect(renderer, &rect_);

        DMLabelStyle title_style = DMStyles::Label();
        title_style.font_size = std::max(16, title_style.font_size + 1);
        title_style.color = SDL_Color{218, 226, 244, 255};

        DMLabelStyle status_style = DMStyles::Label();
        status_style.font_size = std::max(13, status_style.font_size - 1);
        status_style.color = SDL_Color{171, 182, 205, 255};

        const int pad = 10;
        const int title_y = rect_.y + pad;
        DrawLabelText(renderer, title_, rect_.x + pad, title_y, title_style);

        const int status_y = rect_.y + rect_.h - pad - status_style.font_size;
        if (!status_.empty()) {
            DrawLabelText(renderer, status_, rect_.x + pad, status_y, status_style);
        }

        SDL_Rect image_area{rect_.x + pad,
                            title_y + title_style.font_size + 8,
                            std::max(0, rect_.w - pad * 2),
                            std::max(0, status_y - (title_y + title_style.font_size + 8) - 8)};

        SDL_SetRenderDrawColor(renderer, 9, 10, 14, 255);
        sdl_render::FillRect(renderer, &image_area);
        SDL_SetRenderDrawColor(renderer, 34, 39, 52, 255);
        sdl_render::Rect(renderer, &image_area);

        if (texture_ && texture_w_ > 0 && texture_h_ > 0 && image_area.w > 0 && image_area.h > 0) {
            const float sx = static_cast<float>(image_area.w) / static_cast<float>(texture_w_);
            const float sy = static_cast<float>(image_area.h) / static_cast<float>(texture_h_);
            const float scale = std::max(0.001f, std::min(sx, sy));
            const int draw_w = std::max(1, static_cast<int>(std::lround(static_cast<float>(texture_w_) * scale)));
            const int draw_h = std::max(1, static_cast<int>(std::lround(static_cast<float>(texture_h_) * scale)));
            SDL_Rect dst{image_area.x + (image_area.w - draw_w) / 2,
                         image_area.y + (image_area.h - draw_h) / 2,
                         draw_w,
                         draw_h};
            sdl_render::Texture(renderer, texture_, nullptr, &dst);
        } else {
            DMLabelStyle missing_style = DMStyles::Label();
            missing_style.font_size = std::max(12, missing_style.font_size - 1);
            missing_style.color = SDL_Color{126, 136, 158, 255};
            DrawLabelText(renderer, "No preview available", image_area.x + 10, image_area.y + 10, missing_style);
        }
    }

private:
    SDL_Rect rect_{0, 0, 0, 0};
    SDL_Texture* texture_ = nullptr;
    int texture_w_ = 0;
    int texture_h_ = 0;
    int preferred_height_ = 340;
    std::string title_;
    std::string status_;
};

std::vector<fs::path> sorted_directories(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return out;
    }

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_directory()) {
            out.push_back(entry.path());
        }
    }

    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    return out;
}

std::vector<fs::path> sorted_pngs(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return out;
    }

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() == ".png") {
            out.push_back(entry.path());
        }
    }

    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    return out;
}

bool query_texture_size(SDL_Texture* texture, int& width, int& height) {
    width = 0;
    height = 0;
    if (!texture) {
        return false;
    }

    float wf = 0.0f;
    float hf = 0.0f;
    if (!SDL_GetTextureSize(texture, &wf, &hf)) {
        return false;
    }

    width = static_cast<int>(std::lround(wf));
    height = static_cast<int>(std::lround(hf));
    return width > 0 && height > 0;
}

fs::path project_cache_root() {
    return fs::path(PROJECT_ROOT) / "cache";
}

const char* variant_label(ForegroundBackgroundEffectPanel::PreviewSide side) {
    return side == ForegroundBackgroundEffectPanel::PreviewSide::Foreground ? "foreground" : "background";
}

} // namespace

ForegroundBackgroundEffectPanel::ForegroundBackgroundEffectPanel(Assets* assets, int x, int y)
    : DockableCollapsible("Depth Cue FX Editor", false, x, y),
      assets_(assets) {
    set_padding(DMSpacing::panel_padding());
    set_row_gap(DMSpacing::item_gap());
    set_col_gap(DMSpacing::item_gap());
    set_close_button_enabled(true);
    set_header_button_style(&DMStyles::AccentButton());
    set_scroll_enabled(true);
    set_visible(false);

    std::error_code ec;
    preview_temp_root_ = fs::temp_directory_path(ec) / "engine_depth_cue_preview";
    if (preview_temp_root_.empty()) {
        preview_temp_root_ = project_cache_root() / "_depth_cue_preview";
    }

    set_on_close([this]() { this->on_panel_closed(); });

    header_spacer_ = std::make_unique<LocalSpacerWidget>(DMSpacing::header_gap());
    build_ui();
    rebuild_asset_options();
    refresh_from_camera();
}

ForegroundBackgroundEffectPanel::~ForegroundBackgroundEffectPanel() {
    destroy_preview_textures();

    std::error_code ec;
    fs::remove_all(preview_temp_root_, ec);
}

void ForegroundBackgroundEffectPanel::set_assets(Assets* assets) {
    assets_ = assets;
    destroy_preview_textures();
    rebuild_asset_options();
    preview_source_dirty_ = true;
    schedule_preview_rebuild(true, true, 0);
}

void ForegroundBackgroundEffectPanel::open() {
    load_committed_settings_from_manifest();
    refresh_from_committed();
    preview_source_dirty_ = true;
    schedule_preview_rebuild(true, true, 0);

    set_visible(true);
    DockableCollapsible::open();

    if (modal_screen_w_ > 0 && modal_screen_h_ > 0) {
        sync_modal_geometry(modal_screen_w_, modal_screen_h_);
    }
}

void ForegroundBackgroundEffectPanel::close() {
    DockableCollapsible::close();
}

bool ForegroundBackgroundEffectPanel::is_point_inside(int x, int y) const {
    return DockableCollapsible::is_point_inside(x, y);
}

void ForegroundBackgroundEffectPanel::update(const Input& input, int screen_w, int screen_h) {
    modal_screen_w_ = screen_w;
    modal_screen_h_ = screen_h;

    if (!is_visible()) {
        return;
    }

    sync_modal_geometry(screen_w, screen_h);
    DockableCollapsible::update(input, screen_w, screen_h);

    update_pending_previews(SDL_GetTicks());
    sync_preview_widgets();
}

bool ForegroundBackgroundEffectPanel::handle_event(const SDL_Event& e) {
    if (!is_visible()) {
        return false;
    }

    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
        close();
        return true;
    }

    return DockableCollapsible::handle_event(e);
}

void ForegroundBackgroundEffectPanel::render(SDL_Renderer* renderer) const {
    if (!renderer || !is_visible()) {
        return;
    }

    DockableCollapsible::render(renderer);
    DMDropdown::render_active_options(renderer);
}

void ForegroundBackgroundEffectPanel::layout_custom_content(int, int) const {
    // All content now participates in regular row layout to ensure correct clipping.
}

void ForegroundBackgroundEffectPanel::build_ui() {
    recreate_asset_dropdown();

    fg_label_ = std::make_unique<SectionLabelWidget>("Foreground Effects");
    bg_label_ = std::make_unique<SectionLabelWidget>("Background Effects");

    configure_slider_set(fg_sliders_, "FG", PreviewSide::Foreground);
    configure_slider_set(bg_sliders_, "BG", PreviewSide::Background);

    fg_preview_ = std::make_unique<PreviewPaneWidget>("Foreground Preview");
    bg_preview_ = std::make_unique<PreviewPaneWidget>("Background Preview");

    apply_button_ = std::make_unique<DMButton>("Apply + Queue Rebuild", &DMStyles::AccentButton(), 0, DMButton::height());
    apply_button_widget_ = std::make_unique<ButtonWidget>(apply_button_.get(), [this]() { this->apply_and_queue_rebuild(); });

    restore_defaults_button_ = std::make_unique<DMButton>("Reset Both", &DMStyles::WarnButton(), 0, DMButton::height());
    restore_defaults_button_widget_ = std::make_unique<ButtonWidget>(restore_defaults_button_.get(), [this]() { this->restore_defaults(); });

    restore_fg_defaults_button_ = std::make_unique<DMButton>("Reset FG", &DMStyles::HeaderButton(), 0, DMButton::height());
    restore_fg_defaults_button_widget_ = std::make_unique<ButtonWidget>(restore_fg_defaults_button_.get(), [this]() {
        this->restore_defaults_for_side(PreviewSide::Foreground);
    });

    restore_bg_defaults_button_ = std::make_unique<DMButton>("Reset BG", &DMStyles::HeaderButton(), 0, DMButton::height());
    restore_bg_defaults_button_widget_ = std::make_unique<ButtonWidget>(restore_bg_defaults_button_.get(), [this]() {
        this->restore_defaults_for_side(PreviewSide::Background);
    });

    discard_button_ = std::make_unique<DMButton>("Cancel", &DMStyles::DeleteButton(), 0, DMButton::height());
    discard_button_widget_ = std::make_unique<ButtonWidget>(discard_button_.get(), [this]() { this->close(); });

    fill_spacer_ = std::make_unique<FlexibleSpacerWidget>(0);

    rebuild_rows();
}

void ForegroundBackgroundEffectPanel::rebuild_rows() {
    Rows rows;
    if (header_spacer_) {
        rows.push_back({header_spacer_.get()});
    }

    if (asset_dropdown_widget_) {
        rows.push_back({asset_dropdown_widget_.get()});
    }

    paired_rows_.clear();

    auto add_pair_row = [&](Widget* left, Widget* right) {
        if (!left && !right) {
            return;
        }
        paired_rows_.push_back(std::make_unique<PairRowWidget>(left, right));
        rows.push_back({paired_rows_.back().get()});
    };

    add_pair_row(fg_label_.get(), bg_label_.get());
    add_pair_row(fg_sliders_.contrast.get(), bg_sliders_.contrast.get());
    add_pair_row(fg_sliders_.brightness.get(), bg_sliders_.brightness.get());
    add_pair_row(fg_sliders_.blur.get(), bg_sliders_.blur.get());
    add_pair_row(fg_sliders_.saturation_r.get(), bg_sliders_.saturation_r.get());
    add_pair_row(fg_sliders_.saturation_g.get(), bg_sliders_.saturation_g.get());
    add_pair_row(fg_sliders_.saturation_b.get(), bg_sliders_.saturation_b.get());
    add_pair_row(fg_sliders_.hue.get(), bg_sliders_.hue.get());
    add_pair_row(restore_fg_defaults_button_widget_.get(), restore_bg_defaults_button_widget_.get());
    add_pair_row(fg_preview_.get(), bg_preview_.get());

    Row action_row;
    if (apply_button_widget_) {
        action_row.push_back(apply_button_widget_.get());
    }
    if (restore_defaults_button_widget_) {
        action_row.push_back(restore_defaults_button_widget_.get());
    }
    if (discard_button_widget_) {
        action_row.push_back(discard_button_widget_.get());
    }
    if (!action_row.empty()) {
        rows.push_back(std::move(action_row));
    }

    if (fill_spacer_) {
        rows.push_back({fill_spacer_.get()});
    }

    set_rows(rows);
}
