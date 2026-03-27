#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include "DockableCollapsible.hpp"
#include "devtools/float_slider_widget.hpp"
#include "rendering/render/image_effect_settings.hpp"

class Assets;
class DMDropdown;
class DropdownWidget;
class DMButton;
class ButtonWidget;
class Widget;
class Input;

class ForegroundBackgroundEffectPanel : public DockableCollapsible {
public:
    explicit ForegroundBackgroundEffectPanel(Assets* assets, int x = 0, int y = 0);
    ~ForegroundBackgroundEffectPanel() override;

    void set_assets(Assets* assets);
    void refresh_from_camera();

    void update(const Input& input, int screen_w, int screen_h) override;
    bool handle_event(const SDL_Event& e) override;
    void render(SDL_Renderer* renderer) const override;

    void open();

    using CloseCallback = std::function<void()>;
    void set_close_callback(CloseCallback callback) { close_callback_ = std::move(callback); }
    using EditDepthSettingsCallback = std::function<void()>;
    void set_edit_depth_settings_callback(EditDepthSettingsCallback callback) {
        edit_depth_settings_callback_ = std::move(callback);
    }
    void close();
    bool is_point_inside(int x, int y) const;

private:
    enum class PreviewSide {
        Foreground,
        Background
    };

    struct SliderSet {
        std::unique_ptr<FloatSliderWidget> contrast;
        std::unique_ptr<FloatSliderWidget> brightness;
        std::unique_ptr<FloatSliderWidget> blur;
        std::unique_ptr<FloatSliderWidget> saturation_r;
        std::unique_ptr<FloatSliderWidget> saturation_g;
        std::unique_ptr<FloatSliderWidget> saturation_b;
        std::unique_ptr<FloatSliderWidget> hue;
    };

    void layout_custom_content(int screen_w, int screen_h) const override;

    void build_ui();
    void rebuild_rows();
    void rebuild_asset_options();
    void recreate_asset_dropdown();
    void handle_asset_selection(int index);

    void configure_slider_set(SliderSet& set, const std::string& prefix, PreviewSide side);
    void set_slider_values(SliderSet& set, const camera_effects::ImageEffectSettings& settings);
    camera_effects::ImageEffectSettings read_slider_values(const SliderSet& set) const;
    void on_slider_changed(PreviewSide side);
    std::unique_ptr<Widget> make_slider_column_widget(const SliderSet& set);

    void schedule_preview_rebuild(bool fg, bool bg, Uint32 delay_ms = 0);
    void update_pending_previews(Uint64 now_ms);
    void rebuild_preview(PreviewSide side);
    bool ensure_preview_source();
    bool resolve_preview_source_path(std::string& out_path) const;
    bool generate_preview_with_python(PreviewSide side,
                                      const std::string& input_source_path,
                                      const camera_effects::ImageEffectSettings& settings,
                                      std::string& out_path,
                                      std::string& error) const;
    void load_side_preview_texture(PreviewSide side, const std::string& image_path);
    void sync_preview_widgets();
    void destroy_preview_textures();
    void destroy_base_preview_texture();
    void destroy_side_preview_texture(PreviewSide side);

    void load_committed_settings_from_manifest();
    void save_committed_settings_to_manifest(const camera_effects::ImageEffectSettings& fg,
                                             const camera_effects::ImageEffectSettings& bg);
    void apply_and_queue_rebuild();
    void restore_defaults();
    void restore_defaults_for_side(PreviewSide side);
    void discard_unsaved_changes();
    void refresh_from_committed();
    void enter_live_depth_settings_editor();

    bool settings_equal(const camera_effects::ImageEffectSettings& a,
                        const camera_effects::ImageEffectSettings& b,
                        float epsilon = 1e-5f) const;
    void refresh_unsaved_state();
    void update_title_state();
    void sync_modal_geometry(int screen_w, int screen_h);
    int estimate_content_height_without_fill(int content_width) const;
    bool can_render_preview() const;

    void on_panel_closed();

private:
    Assets* assets_ = nullptr;

    std::vector<std::string> asset_names_;
    std::string selected_asset_;

    std::unique_ptr<Widget> header_spacer_;
    std::unique_ptr<DMDropdown> asset_dropdown_;
    std::unique_ptr<DropdownWidget> asset_dropdown_widget_;

    std::unique_ptr<Widget> fg_label_;
    std::unique_ptr<Widget> bg_label_;
    std::vector<std::unique_ptr<Widget>> paired_rows_;
    std::unique_ptr<Widget> fill_spacer_;
    SliderSet fg_sliders_{};
    SliderSet bg_sliders_{};
    std::unique_ptr<Widget> fg_slider_column_widget_;
    std::unique_ptr<Widget> bg_slider_column_widget_;
    std::unique_ptr<Widget> fg_preview_;
    std::unique_ptr<Widget> bg_preview_;

    std::unique_ptr<DMButton> apply_button_;
    std::unique_ptr<ButtonWidget> apply_button_widget_;
    std::unique_ptr<DMButton> restore_defaults_button_;
    std::unique_ptr<ButtonWidget> restore_defaults_button_widget_;
    std::unique_ptr<DMButton> restore_fg_defaults_button_;
    std::unique_ptr<ButtonWidget> restore_fg_defaults_button_widget_;
    std::unique_ptr<DMButton> restore_bg_defaults_button_;
    std::unique_ptr<ButtonWidget> restore_bg_defaults_button_widget_;
    std::unique_ptr<DMButton> discard_button_;
    std::unique_ptr<ButtonWidget> discard_button_widget_;
    std::unique_ptr<DMButton> edit_depth_settings_button_;
    std::unique_ptr<ButtonWidget> edit_depth_settings_button_widget_;

    SDL_Texture* base_preview_texture_ = nullptr;
    int base_preview_w_ = 0;
    int base_preview_h_ = 0;

    SDL_Texture* fg_preview_texture_ = nullptr;
    int fg_preview_w_ = 0;
    int fg_preview_h_ = 0;

    SDL_Texture* bg_preview_texture_ = nullptr;
    int bg_preview_w_ = 0;
    int bg_preview_h_ = 0;

    camera_effects::ImageEffectSettings committed_fg_{};
    camera_effects::ImageEffectSettings committed_bg_{};
    camera_effects::ImageEffectSettings draft_fg_{};
    camera_effects::ImageEffectSettings draft_bg_{};

    bool has_unsaved_changes_ = false;

    bool preview_source_dirty_ = true;
    bool fg_preview_pending_ = false;
    bool bg_preview_pending_ = false;
    Uint64 fg_preview_due_ms_ = 0;
    Uint64 bg_preview_due_ms_ = 0;

    std::string preview_source_path_;

    std::string fg_preview_status_;
    std::string bg_preview_status_;

    int modal_screen_w_ = 0;
    int modal_screen_h_ = 0;
    int last_modal_body_height_ = -1;

    CloseCallback close_callback_;
    bool close_callback_running_ = false;
    EditDepthSettingsCallback edit_depth_settings_callback_;
    bool edit_depth_callback_running_ = false;
};
