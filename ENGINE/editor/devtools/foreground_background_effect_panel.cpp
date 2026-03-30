#include "devtools/foreground_background_effect_panel.hpp"
#include "utils/sdl_render_conversions.hpp"

#include "core/AssetsManager.hpp"
#include "core/manifest/manifest_loader.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/widgets.hpp"
#include "assets/asset/asset_info.hpp"

#include <SDL3/SDL_timer.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <locale>
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

class SliderColumnWidget : public Widget {
public:
    explicit SliderColumnWidget(std::vector<Widget*> children, int spacing)
        : children_(std::move(children)), spacing_(spacing) {}

    void set_rect(const SDL_Rect& r) override {
        rect_ = r;
        int y = r.y;
        bool first = true;
        for (auto* child : children_) {
            if (!child) {
                continue;
            }
            if (!first) {
                y += spacing_;
            }
            first = false;
            const int child_h = std::max(0, child->height_for_width(r.w));
            child->set_rect(SDL_Rect{r.x, y, r.w, child_h});
            y += child_h;
        }
    }

    const SDL_Rect& rect() const override { return rect_; }

    int height_for_width(int width) const override {
        int total = 0;
        bool first = true;
        for (auto* child : children_) {
            if (!child) {
                continue;
            }
            if (!first) {
                total += spacing_;
            }
            first = false;
            total += std::max(0, child->height_for_width(width));
        }
        return total;
    }

    bool handle_event(const SDL_Event& e) override {
        bool handled = false;
        for (auto* child : children_) {
            if (child) {
                handled = child->handle_event(e) || handled;
            }
        }
        return handled;
    }

    void render(SDL_Renderer* renderer) const override {
        for (auto* child : children_) {
            if (child) {
                child->render(renderer);
            }
        }
    }

private:
    SDL_Rect rect_{0, 0, 0, 0};
    std::vector<Widget*> children_;
    int spacing_ = 0;
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
        const int clamped = std::max(64, height);
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
        title_style.font_size = 14;
        title_style.color = SDL_Color{218, 226, 244, 255};

        DMLabelStyle status_style = DMStyles::Label();
        status_style.font_size = 12;
        status_style.color = SDL_Color{171, 182, 205, 255};

        const int pad = 6;
        const int title_y = rect_.y + pad;
        DrawLabelText(renderer, title_, rect_.x + pad, title_y, title_style);

        const int status_y = rect_.y + rect_.h - pad - status_style.font_size;
        if (!status_.empty()) {
            DrawLabelText(renderer, status_, rect_.x + pad, status_y, status_style);
        }

        SDL_Rect image_area{rect_.x + pad,
                            title_y + title_style.font_size + 5,
                            std::max(0, rect_.w - pad * 2),
                            std::max(0, status_y - (title_y + title_style.font_size + 5) - 5)};

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

fs::path preview_images_root() {
    return project_cache_root() / "preview_images";
}

bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && !ec;
}

#if defined(_WIN32)
#define VIBBLE_POPEN _popen
#define VIBBLE_PCLOSE _pclose
#else
#define VIBBLE_POPEN popen
#define VIBBLE_PCLOSE pclose
#endif

std::string quote_shell_arg(const std::string& value) {
    std::string escaped = "\"";
    escaped.reserve(value.size() + 8);
    for (char ch : value) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

std::string trim_copy(const std::string& text) {
    std::size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(start, end - start);
}

std::string escape_json_for_shell(const std::string& json_text) {
    std::string escaped;
    escaped.reserve(json_text.size() * 2);
    for (char ch : json_text) {
        if (ch == '"' || ch == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

fs::path resolve_depth_cue_script_path() {
    const fs::path root_candidate = fs::path(PROJECT_ROOT) / "scripts" / "depth_cue_effects.py";
    if (file_exists(root_candidate)) {
        return root_candidate;
    }
    const fs::path cwd_candidate = fs::current_path() / "scripts" / "depth_cue_effects.py";
    if (file_exists(cwd_candidate)) {
        return cwd_candidate;
    }
    return {};
}

std::string make_preview_request_id(const std::string& asset_name, const char* layer) {
    static std::atomic<std::uint64_t> seq{1};
    std::string id = std::string("preview_") + layer + "_" + std::to_string(seq.fetch_add(1, std::memory_order_relaxed)) +
                     "_" + asset_name;
    for (char& ch : id) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if ((uch >= 'a' && uch <= 'z') ||
            (uch >= 'A' && uch <= 'Z') ||
            (uch >= '0' && uch <= '9') ||
            ch == '_' || ch == '-' || ch == '.') {
            continue;
        }
        ch = '_';
    }
    return id;
}

} // namespace

ForegroundBackgroundEffectPanel::ForegroundBackgroundEffectPanel(Assets* assets, int x, int y)
    : DockableCollapsible("Depth Cue FX Editor", false, x, y),
      assets_(assets) {
    set_padding(std::max(6, DMSpacing::panel_padding() / 3));
    set_row_gap(std::max(2, DMSpacing::small_gap() / 2));
    set_col_gap(std::max(6, DMSpacing::small_gap() + 2));
    set_close_button_enabled(true);
    set_header_button_style(&DMStyles::AccentButton());
    set_scroll_enabled(false);
    set_visible(false);

    set_on_close([this]() { this->on_panel_closed(); });

    build_ui();
    rebuild_asset_options();
    refresh_from_camera();
}

ForegroundBackgroundEffectPanel::~ForegroundBackgroundEffectPanel() {
    destroy_preview_textures();
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

    configure_slider_set(fg_sliders_, "FG", PreviewSide::Foreground);
    configure_slider_set(bg_sliders_, "BG", PreviewSide::Background);

    fg_slider_column_widget_ = make_slider_column_widget(fg_sliders_);
    bg_slider_column_widget_ = make_slider_column_widget(bg_sliders_);

    fg_preview_ = std::make_unique<PreviewPaneWidget>("FG Preview");
    bg_preview_ = std::make_unique<PreviewPaneWidget>("BG Preview");

    apply_button_ = std::make_unique<DMButton>("Apply", &DMStyles::AccentButton(), 0, DMButton::height());
    apply_button_widget_ = std::make_unique<ButtonWidget>(apply_button_.get(), [this]() { this->apply_and_queue_rebuild(); });

    restore_defaults_button_ = std::make_unique<DMButton>("Reset All", &DMStyles::WarnButton(), 0, DMButton::height());
    restore_defaults_button_widget_ = std::make_unique<ButtonWidget>(restore_defaults_button_.get(), [this]() { this->restore_defaults(); });

    restore_fg_defaults_button_ = std::make_unique<DMButton>("Reset FG", &DMStyles::HeaderButton(), 0, DMButton::height());
    restore_fg_defaults_button_widget_ = std::make_unique<ButtonWidget>(restore_fg_defaults_button_.get(), [this]() {
        this->restore_defaults_for_side(PreviewSide::Foreground);
    });

    restore_bg_defaults_button_ = std::make_unique<DMButton>("Reset BG", &DMStyles::HeaderButton(), 0, DMButton::height());
    restore_bg_defaults_button_widget_ = std::make_unique<ButtonWidget>(restore_bg_defaults_button_.get(), [this]() {
        this->restore_defaults_for_side(PreviewSide::Background);
    });

    discard_button_ = std::make_unique<DMButton>("Close", &DMStyles::DeleteButton(), 0, DMButton::height());
    discard_button_widget_ = std::make_unique<ButtonWidget>(discard_button_.get(), [this]() { this->close(); });

    edit_depth_settings_button_ = std::make_unique<DMButton>("Edit Depth", &DMStyles::AccentButton(), 0, DMButton::height());
    edit_depth_settings_button_widget_ = std::make_unique<ButtonWidget>(
        edit_depth_settings_button_.get(),
        [this]() { this->enter_live_depth_settings_editor(); });

    fill_spacer_ = std::make_unique<FlexibleSpacerWidget>(0);

    rebuild_rows();
}

void ForegroundBackgroundEffectPanel::rebuild_rows() {
    Rows rows;
    if (asset_dropdown_widget_) {
        rows.push_back({asset_dropdown_widget_.get()});
    }

    paired_rows_.clear();
    Row compact_main_row;
    if (fg_slider_column_widget_) {
        compact_main_row.push_back(fg_slider_column_widget_.get());
    }
    if (fg_preview_) {
        compact_main_row.push_back(fg_preview_.get());
    }
    if (bg_slider_column_widget_) {
        compact_main_row.push_back(bg_slider_column_widget_.get());
    }
    if (bg_preview_) {
        compact_main_row.push_back(bg_preview_.get());
    }
    if (!compact_main_row.empty()) {
        rows.push_back(std::move(compact_main_row));
    }

    if (fill_spacer_) {
        rows.push_back({fill_spacer_.get()});
    }

    Row action_row;
    if (restore_fg_defaults_button_widget_) {
        action_row.push_back(restore_fg_defaults_button_widget_.get());
    }
    if (restore_bg_defaults_button_widget_) {
        action_row.push_back(restore_bg_defaults_button_widget_.get());
    }
    if (restore_defaults_button_widget_) {
        action_row.push_back(restore_defaults_button_widget_.get());
    }
    if (apply_button_widget_) {
        action_row.push_back(apply_button_widget_.get());
    }
    if (edit_depth_settings_button_widget_) {
        action_row.push_back(edit_depth_settings_button_widget_.get());
    }
    if (discard_button_widget_) {
        action_row.push_back(discard_button_widget_.get());
    }
    if (!action_row.empty()) {
        rows.push_back(std::move(action_row));
    }

    set_rows(rows);
}

void ForegroundBackgroundEffectPanel::rebuild_asset_options() {
    const std::string previous_selection = selected_asset_;

    asset_names_.clear();
    if (assets_) {
        const auto& all_assets = assets_->library().all();
        asset_names_.reserve(all_assets.size());
        for (const auto& entry : all_assets) {
            asset_names_.push_back(entry.first);
        }
        std::sort(asset_names_.begin(), asset_names_.end());
    }

    if (!asset_names_.empty()) {
        auto it = std::find(asset_names_.begin(), asset_names_.end(), previous_selection);
        selected_asset_ = (it != asset_names_.end()) ? *it : asset_names_.front();
    } else {
        selected_asset_.clear();
    }

    recreate_asset_dropdown();
    rebuild_rows();

    preview_source_dirty_ = true;
    schedule_preview_rebuild(true, true, 0);
}

void ForegroundBackgroundEffectPanel::recreate_asset_dropdown() {
    std::vector<std::string> options = asset_names_;
    if (options.empty()) {
        options.emplace_back("No assets available");
    }

    int selected_index = 0;
    if (!asset_names_.empty() && !selected_asset_.empty()) {
        auto it = std::find(asset_names_.begin(), asset_names_.end(), selected_asset_);
        if (it != asset_names_.end()) {
            selected_index = static_cast<int>(std::distance(asset_names_.begin(), it));
        }
    }

    asset_dropdown_ = std::make_unique<DMDropdown>("Preview Asset", options, selected_index);
    asset_dropdown_->set_on_selection_changed([this](int index) { this->handle_asset_selection(index); });

    asset_dropdown_widget_ = std::make_unique<DropdownWidget>(asset_dropdown_.get());
    asset_dropdown_widget_->set_tooltip(
        "Preview source is the selected asset frame. Generated previews are written to cache/preview_images.");
}

void ForegroundBackgroundEffectPanel::handle_asset_selection(int index) {
    if (asset_names_.empty()) {
        selected_asset_.clear();
        preview_source_dirty_ = true;
        schedule_preview_rebuild(true, true, 0);
        return;
    }

    const int clamped = std::clamp(index, 0, static_cast<int>(asset_names_.size()) - 1);
    selected_asset_ = asset_names_[static_cast<std::size_t>(clamped)];

    preview_source_dirty_ = true;
    fg_preview_status_ = "Source changed. Regenerating...";
    bg_preview_status_ = "Source changed. Regenerating...";
    schedule_preview_rebuild(true, true, 0);
}

void ForegroundBackgroundEffectPanel::configure_slider_set(SliderSet& set,
                                                           const std::string&,
                                                           PreviewSide side) {
    auto configure_slider = [this, side](std::unique_ptr<FloatSliderWidget>& target,
                                         const std::string& label,
                                         float min,
                                         float max,
                                         float step,
                                         int decimals) {
        target = std::make_unique<FloatSliderWidget>(label, min, max, step, 0.0f, decimals);
        target->set_on_value_changed([this, side](float) {
            this->on_slider_changed(side);
        });
    };

    configure_slider(set.contrast, "Contrast", -1.0f, 1.0f, 0.02f, 2);
    configure_slider(set.brightness, "Brightness", -1.0f, 1.0f, 0.02f, 2);
    configure_slider(set.blur, "Blur / Sharpen", -1.0f, 1.0f, 0.02f, 2);
    configure_slider(set.saturation_r, "Red Saturation", -1.0f, 1.0f, 0.02f, 2);
    configure_slider(set.saturation_g, "Green Saturation", -1.0f, 1.0f, 0.02f, 2);
    configure_slider(set.saturation_b, "Blue Saturation", -1.0f, 1.0f, 0.02f, 2);
    configure_slider(set.hue, "Hue Shift (deg)", -180.0f, 180.0f, 1.0f, 0);
}

std::unique_ptr<Widget> ForegroundBackgroundEffectPanel::make_slider_column_widget(const SliderSet& set) {
    std::vector<Widget*> children;
    children.reserve(7);
    if (set.contrast) {
        children.push_back(set.contrast.get());
    }
    if (set.brightness) {
        children.push_back(set.brightness.get());
    }
    if (set.blur) {
        children.push_back(set.blur.get());
    }
    if (set.saturation_r) {
        children.push_back(set.saturation_r.get());
    }
    if (set.saturation_g) {
        children.push_back(set.saturation_g.get());
    }
    if (set.saturation_b) {
        children.push_back(set.saturation_b.get());
    }
    if (set.hue) {
        children.push_back(set.hue.get());
    }
    return std::make_unique<SliderColumnWidget>(std::move(children), std::max(0, DMSpacing::small_gap() - 4));
}

void ForegroundBackgroundEffectPanel::set_slider_values(SliderSet& set,
                                                         const camera_effects::ImageEffectSettings& settings) {
    if (set.contrast) {
        set.contrast->set_value(settings.contrast);
    }
    if (set.brightness) {
        set.brightness->set_value(settings.brightness);
    }
    if (set.blur) {
        set.blur->set_value(settings.blur);
    }
    if (set.saturation_r) {
        set.saturation_r->set_value(settings.saturation_red);
    }
    if (set.saturation_g) {
        set.saturation_g->set_value(settings.saturation_green);
    }
    if (set.saturation_b) {
        set.saturation_b->set_value(settings.saturation_blue);
    }
    if (set.hue) {
        set.hue->set_value(settings.hue);
    }
}

camera_effects::ImageEffectSettings ForegroundBackgroundEffectPanel::read_slider_values(const SliderSet& set) const {
    camera_effects::ImageEffectSettings settings{};
    if (set.contrast) {
        settings.contrast = set.contrast->value();
    }
    if (set.brightness) {
        settings.brightness = set.brightness->value();
    }
    if (set.blur) {
        settings.blur = set.blur->value();
    }
    if (set.saturation_r) {
        settings.saturation_red = set.saturation_r->value();
    }
    if (set.saturation_g) {
        settings.saturation_green = set.saturation_g->value();
    }
    if (set.saturation_b) {
        settings.saturation_blue = set.saturation_b->value();
    }
    if (set.hue) {
        settings.hue = set.hue->value();
    }
    camera_effects::ClampImageEffectSettings(settings);
    return settings;
}

void ForegroundBackgroundEffectPanel::on_slider_changed(PreviewSide side) {
    if (side == PreviewSide::Foreground) {
        draft_fg_ = read_slider_values(fg_sliders_);
        fg_preview_status_ = "Updating preview...";
    } else {
        draft_bg_ = read_slider_values(bg_sliders_);
        bg_preview_status_ = "Updating preview...";
    }

    refresh_unsaved_state();
    schedule_preview_rebuild(side == PreviewSide::Foreground,
                             side == PreviewSide::Background,
                             0);
}

void ForegroundBackgroundEffectPanel::schedule_preview_rebuild(bool fg, bool bg, Uint32 delay_ms) {
    const Uint64 now = SDL_GetTicks();
    if (fg) {
        fg_preview_pending_ = true;
        fg_preview_due_ms_ = now + delay_ms;
    }
    if (bg) {
        bg_preview_pending_ = true;
        bg_preview_due_ms_ = now + delay_ms;
    }
}

void ForegroundBackgroundEffectPanel::update_pending_previews(Uint64 now_ms) {
    if (!can_render_preview()) {
        return;
    }

    if (fg_preview_pending_ && now_ms >= fg_preview_due_ms_) {
        fg_preview_pending_ = false;
        rebuild_preview(PreviewSide::Foreground);
    }

    if (bg_preview_pending_ && now_ms >= bg_preview_due_ms_) {
        bg_preview_pending_ = false;
        rebuild_preview(PreviewSide::Background);
    }
}

void ForegroundBackgroundEffectPanel::rebuild_preview(PreviewSide side) {
    if (!can_render_preview()) {
        return;
    }

    if (!ensure_preview_source()) {
        if (side == PreviewSide::Foreground) {
            if (fg_preview_status_.empty()) {
                fg_preview_status_ = "No preview source available.";
            }
        } else {
            if (bg_preview_status_.empty()) {
                bg_preview_status_ = "No preview source available.";
            }
        }
        sync_preview_widgets();
        return;
    }

    std::string output_path;
    std::string error;

    const camera_effects::ImageEffectSettings settings =
        (side == PreviewSide::Foreground) ? draft_fg_ : draft_bg_;

    if (!generate_preview_with_python(side, preview_source_path_, settings, output_path, error)) {
        if (side == PreviewSide::Foreground) {
            fg_preview_status_ = "Preview generation failed: " + error;
        } else {
            bg_preview_status_ = "Preview generation failed: " + error;
        }
        sync_preview_widgets();
        return;
    }

    load_side_preview_texture(side, output_path);

    if (side == PreviewSide::Foreground) {
        fg_preview_status_ = has_unsaved_changes_ ? "Live preview (unsaved draft)." : "Preview is up to date.";
    } else {
        bg_preview_status_ = has_unsaved_changes_ ? "Live preview (unsaved draft)." : "Preview is up to date.";
    }

    sync_preview_widgets();
}

bool ForegroundBackgroundEffectPanel::ensure_preview_source() {
    if (!preview_source_dirty_ && !preview_source_path_.empty()) {
        std::error_code ec;
        if (fs::exists(preview_source_path_, ec) && !ec) {
            return true;
        }
    }

    std::string source_path;
    if (!resolve_preview_source_path(source_path)) {
        fg_preview_status_ = "Unable to resolve preview source frame.";
        bg_preview_status_ = fg_preview_status_;
        return false;
    }

    preview_source_path_ = source_path;
    preview_source_dirty_ = false;

    destroy_base_preview_texture();

    if (!assets_ || !assets_->renderer()) {
        fg_preview_status_ = "No renderer available for preview.";
        bg_preview_status_ = fg_preview_status_;
        return false;
    }

    SDL_Texture* texture = IMG_LoadTexture(assets_->renderer(), preview_source_path_.c_str());
    if (!texture) {
        fg_preview_status_ = "Failed to load preview source texture.";
        bg_preview_status_ = fg_preview_status_;
        return false;
    }

    int tex_w = 0;
    int tex_h = 0;
    if (!query_texture_size(texture, tex_w, tex_h)) {
        SDL_DestroyTexture(texture);
        fg_preview_status_ = "Failed to query preview source texture size.";
        bg_preview_status_ = fg_preview_status_;
        return false;
    }

    base_preview_texture_ = texture;
    base_preview_w_ = tex_w;
    base_preview_h_ = tex_h;
    return true;
}

bool ForegroundBackgroundEffectPanel::resolve_preview_source_path(std::string& out_path) const {
    out_path.clear();

    auto try_normal_dir = [&](const fs::path& normal_dir) -> bool {
        std::error_code ec;
        if (!fs::exists(normal_dir, ec) || !fs::is_directory(normal_dir, ec)) {
            return false;
        }

        const fs::path first_frame = normal_dir / "0.png";
        if (fs::exists(first_frame, ec) && fs::is_regular_file(first_frame, ec)) {
            out_path = first_frame.string();
            return true;
        }

        const auto pngs = sorted_pngs(normal_dir);
        if (!pngs.empty()) {
            out_path = pngs.front().string();
            return true;
        }
        return false;
    };

    if (!selected_asset_.empty()) {
        const fs::path cache_anim_root = project_cache_root() / selected_asset_ / "animations";
        const auto anim_dirs = sorted_directories(cache_anim_root);

        for (const auto& anim_dir : anim_dirs) {
            if (try_normal_dir(anim_dir / "scale_100" / "normal")) {
                return true;
            }
        }

        for (const auto& anim_dir : anim_dirs) {
            const auto scale_dirs = sorted_directories(anim_dir);
            for (const auto& scale_dir : scale_dirs) {
                const std::string scale_name = scale_dir.filename().string();
                if (scale_name.rfind("scale_", 0) != 0) {
                    continue;
                }
                if (try_normal_dir(scale_dir / "normal")) {
                    return true;
                }
            }
        }

        const fs::path source_asset_root = fs::path(PROJECT_ROOT) / "resources" / "assets" / selected_asset_;
        const auto source_anim_dirs = sorted_directories(source_asset_root);
        for (const auto& anim_dir : source_anim_dirs) {
            const fs::path first = anim_dir / "0.png";
            std::error_code ec;
            if (fs::exists(first, ec) && fs::is_regular_file(first, ec)) {
                out_path = first.string();
                return true;
            }
            const auto pngs = sorted_pngs(anim_dir);
            if (!pngs.empty()) {
                out_path = pngs.front().string();
                return true;
            }
        }

        const fs::path root_first = source_asset_root / "0.png";
        std::error_code ec;
        if (fs::exists(root_first, ec) && fs::is_regular_file(root_first, ec)) {
            out_path = root_first.string();
            return true;
        }
        const auto root_pngs = sorted_pngs(source_asset_root);
        if (!root_pngs.empty()) {
            out_path = root_pngs.front().string();
            return true;
        }
    }

    return false;
}

bool ForegroundBackgroundEffectPanel::generate_preview_with_python(
    PreviewSide side,
    const std::string& input_source_path,
    const camera_effects::ImageEffectSettings& settings,
    std::string& out_path,
    std::string& error) const {
    out_path.clear();
    error.clear();

    if (input_source_path.empty()) {
        error = "preview input is empty";
        return false;
    }

    const fs::path script_path = resolve_depth_cue_script_path();
    if (script_path.empty()) {
        error = "depth_cue_effects.py not found";
        return false;
    }

    const char* layer_name = (side == PreviewSide::Foreground) ? "foreground" : "background";

    std::error_code ec;
    const fs::path preview_dir = preview_images_root();
    fs::create_directories(preview_dir, ec);
    if (ec) {
        error = "failed to create preview output directory: " + ec.message();
        return false;
    }

    nlohmann::json payload = nlohmann::json::object();
    payload["layer"] = layer_name;
    payload["contrast"] = settings.contrast;
    payload["brightness"] = settings.brightness;
    payload["blur"] = settings.blur;
    payload["saturation_red"] = settings.saturation_red;
    payload["saturation_green"] = settings.saturation_green;
    payload["saturation_blue"] = settings.saturation_blue;
    payload["hue"] = settings.hue;
    payload["save_mode"] = "preview";
    payload["request_id"] = make_preview_request_id(selected_asset_, layer_name);

    const std::string payload_arg = "'" + escape_json_for_shell(payload.dump()) + "'";
    const std::string command = std::string("python ") +
                                quote_shell_arg(script_path.string()) + " " +
                                quote_shell_arg(input_source_path) + " " +
                                payload_arg + " 2>&1";

    FILE* pipe = VIBBLE_POPEN(command.c_str(), "r");
    if (!pipe) {
        error = "failed to launch depth_cue_effects.py";
        return false;
    }

    std::string output;
    char buffer[512];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output += buffer;
    }
    const int rc = VIBBLE_PCLOSE(pipe);
    if (rc != 0) {
        std::string details = trim_copy(output);
        error = "depth_cue_effects.py exited with code " + std::to_string(rc);
        if (!details.empty()) {
            constexpr std::size_t kDetailLimit = 180;
            if (details.size() > kDetailLimit) {
                details = details.substr(details.size() - kDetailLimit);
            }
            error += " (" + details + ")";
        }
        return false;
    }

    std::string output_path;
    std::istringstream output_stream(output);
    std::string line;
    while (std::getline(output_stream, line)) {
        const std::string trimmed = trim_copy(line);
        if (!trimmed.empty()) {
            output_path = trimmed;
        }
    }
    if (output_path.empty()) {
        error = "depth_cue_effects.py produced no output path";
        return false;
    }

    if ((output_path.front() == '"' && output_path.back() == '"') ||
        (output_path.front() == '\'' && output_path.back() == '\'')) {
        output_path = output_path.substr(1, output_path.size() - 2);
    }

    fs::path produced_path(output_path);
    if (!fs::exists(produced_path, ec) || ec) {
        error = "preview output was not produced";
        return false;
    }

    const fs::path expected_path = preview_dir / (std::string(layer_name) + ".png");
    if (!fs::exists(expected_path, ec) || ec) {
        error = "expected preview cache image not found: " + expected_path.string();
        return false;
    }

    out_path = expected_path.string();
    return true;
}

void ForegroundBackgroundEffectPanel::load_side_preview_texture(PreviewSide side, const std::string& image_path) {
    if (!assets_ || !assets_->renderer()) {
        return;
    }

    SDL_Texture* texture = IMG_LoadTexture(assets_->renderer(), image_path.c_str());
    if (!texture) {
        return;
    }

    int width = 0;
    int height = 0;
    if (!query_texture_size(texture, width, height)) {
        SDL_DestroyTexture(texture);
        return;
    }

    if (side == PreviewSide::Foreground) {
        destroy_side_preview_texture(PreviewSide::Foreground);
        fg_preview_texture_ = texture;
        fg_preview_w_ = width;
        fg_preview_h_ = height;
    } else {
        destroy_side_preview_texture(PreviewSide::Background);
        bg_preview_texture_ = texture;
        bg_preview_w_ = width;
        bg_preview_h_ = height;
    }
}

void ForegroundBackgroundEffectPanel::sync_preview_widgets() {
    auto* fg_widget = dynamic_cast<PreviewPaneWidget*>(fg_preview_.get());
    if (fg_widget) {
        SDL_Texture* texture = fg_preview_texture_ ? fg_preview_texture_ : base_preview_texture_;
        const int width = fg_preview_texture_ ? fg_preview_w_ : base_preview_w_;
        const int height = fg_preview_texture_ ? fg_preview_h_ : base_preview_h_;
        fg_widget->set_texture(texture, width, height);
        fg_widget->set_status(fg_preview_status_);
    }

    auto* bg_widget = dynamic_cast<PreviewPaneWidget*>(bg_preview_.get());
    if (bg_widget) {
        SDL_Texture* texture = bg_preview_texture_ ? bg_preview_texture_ : base_preview_texture_;
        const int width = bg_preview_texture_ ? bg_preview_w_ : base_preview_w_;
        const int height = bg_preview_texture_ ? bg_preview_h_ : base_preview_h_;
        bg_widget->set_texture(texture, width, height);
        bg_widget->set_status(bg_preview_status_);
    }
}

void ForegroundBackgroundEffectPanel::destroy_preview_textures() {
    destroy_base_preview_texture();
    destroy_side_preview_texture(PreviewSide::Foreground);
    destroy_side_preview_texture(PreviewSide::Background);
}

void ForegroundBackgroundEffectPanel::destroy_base_preview_texture() {
    if (base_preview_texture_) {
        SDL_DestroyTexture(base_preview_texture_);
        base_preview_texture_ = nullptr;
    }
    base_preview_w_ = 0;
    base_preview_h_ = 0;
}

void ForegroundBackgroundEffectPanel::destroy_side_preview_texture(PreviewSide side) {
    if (side == PreviewSide::Foreground) {
        if (fg_preview_texture_) {
            SDL_DestroyTexture(fg_preview_texture_);
            fg_preview_texture_ = nullptr;
        }
        fg_preview_w_ = 0;
        fg_preview_h_ = 0;
        return;
    }

    if (bg_preview_texture_) {
        SDL_DestroyTexture(bg_preview_texture_);
        bg_preview_texture_ = nullptr;
    }
    bg_preview_w_ = 0;
    bg_preview_h_ = 0;
}

void ForegroundBackgroundEffectPanel::load_committed_settings_from_manifest() {
    committed_fg_ = camera_effects::ImageEffectSettings{};
    committed_bg_ = camera_effects::ImageEffectSettings{};

    try {
        nlohmann::json manifest_json = manifest::load_manifest().raw;
        if (!manifest_json.contains("image_effects") || !manifest_json["image_effects"].is_object()) {
            return;
        }

        const nlohmann::json& image_effects = manifest_json["image_effects"];

        auto load_side = [&](const char* key) -> camera_effects::ImageEffectSettings {
            camera_effects::ImageEffectSettings settings{};
            if (!image_effects.contains(key) || !image_effects[key].is_object()) {
                return settings;
            }

            const nlohmann::json& obj = image_effects[key];
            settings.contrast = obj.value("contrast", settings.contrast);
            settings.brightness = obj.value("brightness", settings.brightness);
            settings.blur = obj.value("blur", settings.blur);
            settings.saturation_red = obj.value("saturation_red", settings.saturation_red);
            settings.saturation_green = obj.value("saturation_green", settings.saturation_green);
            settings.saturation_blue = obj.value("saturation_blue", settings.saturation_blue);
            settings.hue = obj.value("hue", settings.hue);
            camera_effects::ClampImageEffectSettings(settings);
            return settings;
        };

        committed_fg_ = load_side("foreground");
        committed_bg_ = load_side("background");
    } catch (...) {
        committed_fg_ = camera_effects::ImageEffectSettings{};
        committed_bg_ = camera_effects::ImageEffectSettings{};
    }
}

void ForegroundBackgroundEffectPanel::save_committed_settings_to_manifest(
    const camera_effects::ImageEffectSettings& fg,
    const camera_effects::ImageEffectSettings& bg) {
    manifest::ManifestData data = manifest::load_manifest();
    nlohmann::json& manifest_json = data.raw;

    auto write_settings = [](const camera_effects::ImageEffectSettings& settings) {
        nlohmann::json out = nlohmann::json::object();
        out["contrast"] = settings.contrast;
        out["brightness"] = settings.brightness;
        out["blur"] = settings.blur;
        out["saturation_red"] = settings.saturation_red;
        out["saturation_green"] = settings.saturation_green;
        out["saturation_blue"] = settings.saturation_blue;
        out["hue"] = settings.hue;
        return out;
    };

    if (!manifest_json.contains("image_effects") || !manifest_json["image_effects"].is_object()) {
        manifest_json["image_effects"] = nlohmann::json::object();
    }

    manifest_json["image_effects"]["foreground"] = write_settings(fg);
    manifest_json["image_effects"]["background"] = write_settings(bg);

    data.raw = manifest_json;
    if (manifest_json.contains("assets") && manifest_json["assets"].is_object()) {
        data.assets = manifest_json["assets"];
    }
    if (manifest_json.contains("maps") && manifest_json["maps"].is_object()) {
        data.maps = manifest_json["maps"];
    }

    manifest::save_manifest(data);
}

void ForegroundBackgroundEffectPanel::apply_and_queue_rebuild() {
    draft_fg_ = read_slider_values(fg_sliders_);
    draft_bg_ = read_slider_values(bg_sliders_);

    try {
        save_committed_settings_to_manifest(draft_fg_, draft_bg_);

        committed_fg_ = draft_fg_;
        committed_bg_ = draft_bg_;
        refresh_unsaved_state();

        if (assets_) {
            for (const auto& [asset_name, info] : assets_->library().all()) {
                (void)asset_name;
                if (!info) {
                    continue;
                }
                info->mark_all_animation_textures_on_close(
                    static_cast<std::uint8_t>(AssetInfo::kTextureVariantForeground |
                                              AssetInfo::kTextureVariantBackground));
                info->mark_dirty();
            }
        }

        fg_preview_status_ = "Applied. Foreground rebuild marked for exit save.";
        bg_preview_status_ = "Applied. Background rebuild marked for exit save.";
    } catch (const std::exception& e) {
        fg_preview_status_ = std::string("Apply failed: ") + e.what();
        bg_preview_status_ = fg_preview_status_;
    } catch (...) {
        fg_preview_status_ = "Apply failed due to unknown error.";
        bg_preview_status_ = fg_preview_status_;
    }

    sync_preview_widgets();
}

void ForegroundBackgroundEffectPanel::restore_defaults() {
    draft_fg_ = camera_effects::ImageEffectSettings{};
    draft_bg_ = camera_effects::ImageEffectSettings{};

    set_slider_values(fg_sliders_, draft_fg_);
    set_slider_values(bg_sliders_, draft_bg_);

    fg_preview_status_ = "Foreground reset to defaults (draft only).";
    bg_preview_status_ = "Background reset to defaults (draft only).";

    refresh_unsaved_state();
    schedule_preview_rebuild(true, true, 0);
}

void ForegroundBackgroundEffectPanel::restore_defaults_for_side(PreviewSide side) {
    if (side == PreviewSide::Foreground) {
        draft_fg_ = camera_effects::ImageEffectSettings{};
        set_slider_values(fg_sliders_, draft_fg_);
        fg_preview_status_ = "Foreground reset to defaults (draft only).";
    } else {
        draft_bg_ = camera_effects::ImageEffectSettings{};
        set_slider_values(bg_sliders_, draft_bg_);
        bg_preview_status_ = "Background reset to defaults (draft only).";
    }

    refresh_unsaved_state();
    schedule_preview_rebuild(side == PreviewSide::Foreground,
                             side == PreviewSide::Background,
                             0);
}

void ForegroundBackgroundEffectPanel::discard_unsaved_changes() {
    refresh_from_committed();
}

void ForegroundBackgroundEffectPanel::enter_live_depth_settings_editor() {
    close();
    if (edit_depth_settings_callback_ && !edit_depth_callback_running_) {
        edit_depth_callback_running_ = true;
        edit_depth_settings_callback_();
        edit_depth_callback_running_ = false;
    }
}

void ForegroundBackgroundEffectPanel::refresh_from_committed() {
    draft_fg_ = committed_fg_;
    draft_bg_ = committed_bg_;

    set_slider_values(fg_sliders_, draft_fg_);
    set_slider_values(bg_sliders_, draft_bg_);

    refresh_unsaved_state();

    if (is_visible()) {
        fg_preview_status_ = "Showing committed foreground settings.";
        bg_preview_status_ = "Showing committed background settings.";
    }

    schedule_preview_rebuild(true, true, 0);
}

bool ForegroundBackgroundEffectPanel::settings_equal(const camera_effects::ImageEffectSettings& a,
                                                     const camera_effects::ImageEffectSettings& b,
                                                     float epsilon) const {
    return std::fabs(a.contrast - b.contrast) <= epsilon &&
           std::fabs(a.brightness - b.brightness) <= epsilon &&
           std::fabs(a.blur - b.blur) <= epsilon &&
           std::fabs(a.saturation_red - b.saturation_red) <= epsilon &&
           std::fabs(a.saturation_green - b.saturation_green) <= epsilon &&
           std::fabs(a.saturation_blue - b.saturation_blue) <= epsilon &&
           std::fabs(a.hue - b.hue) <= epsilon;
}

void ForegroundBackgroundEffectPanel::refresh_unsaved_state() {
    has_unsaved_changes_ =
        !settings_equal(draft_fg_, committed_fg_) ||
        !settings_equal(draft_bg_, committed_bg_);
    update_title_state();
}

void ForegroundBackgroundEffectPanel::update_title_state() {
    set_title(has_unsaved_changes_ ? "Depth Cue FX Editor*" : "Depth Cue FX Editor");
}

void ForegroundBackgroundEffectPanel::sync_modal_geometry(int screen_w, int screen_h) {
    if (screen_w <= 0 || screen_h <= 0) {
        return;
    }

    const SDL_Rect target_rect{0, 0, screen_w, screen_h};
    if (rect().x != target_rect.x || rect().y != target_rect.y || rect().w != target_rect.w || rect().h != target_rect.h) {
        set_rect(target_rect);
    }

    const int header_height = show_header() ? (DMButton::height() + DMSpacing::header_gap()) : 0;
    const int available_body = std::max(0, screen_h - (padding_ * 2) - header_height);
    set_available_height_override(available_body);

    const int content_width = std::max(1, screen_w - padding_ * 2);
    auto* fg_widget = dynamic_cast<PreviewPaneWidget*>(fg_preview_.get());
    auto* bg_widget = dynamic_cast<PreviewPaneWidget*>(bg_preview_.get());
    constexpr int kPreviewMinHeight = 72;
    constexpr int kPreviewMaxHeight = 180;
    if (fg_widget) {
        fg_widget->set_preferred_height(kPreviewMinHeight);
    }
    if (bg_widget) {
        bg_widget->set_preferred_height(kPreviewMinHeight);
    }

    const int base_content_without_fill = estimate_content_height_without_fill(content_width);
    const int available_for_growth = std::max(0, available_body - base_content_without_fill);
    const int preview_height = std::clamp(kPreviewMinHeight + available_for_growth,
                                          kPreviewMinHeight,
                                          kPreviewMaxHeight);
    if (fg_widget) {
        fg_widget->set_preferred_height(preview_height);
    }
    if (bg_widget) {
        bg_widget->set_preferred_height(preview_height);
    }

    const int content_without_fill = estimate_content_height_without_fill(content_width);
    constexpr int kBottomActionRowLift = 14;
    const int fill_height = std::max(0, available_body - content_without_fill - kBottomActionRowLift);
    if (auto* flexible = dynamic_cast<FlexibleSpacerWidget*>(fill_spacer_.get())) {
        flexible->set_height(fill_height);
    }

    last_modal_body_height_ = available_body;
}

int ForegroundBackgroundEffectPanel::estimate_content_height_without_fill(int content_width) const {
    int total_height = 0;
    int counted_rows = 0;

    for (const auto& row : rows_) {
        if (row.empty()) {
            continue;
        }

        bool row_has_non_fill_widget = false;
        int row_height = 0;
        const int cols = static_cast<int>(row.size());
        const int col_width = std::max(1, (content_width - std::max(0, cols - 1) * col_gap_) / std::max(1, cols));

        for (Widget* widget : row) {
            if (!widget || widget == fill_spacer_.get()) {
                continue;
            }
            row_has_non_fill_widget = true;
            row_height = std::max(row_height, std::max(0, widget->height_for_width(col_width)));
        }

        if (!row_has_non_fill_widget) {
            continue;
        }

        total_height += row_height;
        ++counted_rows;
    }

    if (counted_rows > 1) {
        total_height += (counted_rows - 1) * row_gap_;
    }

    return total_height;
}

bool ForegroundBackgroundEffectPanel::can_render_preview() const {
    return is_visible() && is_expanded() && assets_ && assets_->renderer();
}

void ForegroundBackgroundEffectPanel::on_panel_closed() {
    discard_unsaved_changes();

    fg_preview_pending_ = false;
    bg_preview_pending_ = false;

    if (close_callback_ && !close_callback_running_) {
        close_callback_running_ = true;
        close_callback_();
        close_callback_running_ = false;
    }
}

void ForegroundBackgroundEffectPanel::refresh_from_camera() {
    load_committed_settings_from_manifest();
    refresh_from_committed();
    preview_source_dirty_ = true;
    schedule_preview_rebuild(true, true, 0);
}
