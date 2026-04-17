#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include "assets/asset/anchor_point.hpp"

class DMButton;
class DMTextBox;
class DMSlider;
class DMCheckbox;
class DMDropdown;

class RoomOvalPointChildEditorPanel {
public:
    struct PointDetailValues {
        float rotation_degrees = 0.0f;
        bool hidden = false;
        bool resolve_x = true;
        AnchorScalingMethod scaling_method = AnchorScalingMethod::Parent;
        std::vector<std::string> tags;
        std::vector<std::string> anti_tags;
    };

    using ApplyPointDetailsCallback = std::function<void(const PointDetailValues&)>;

    RoomOvalPointChildEditorPanel();
    ~RoomOvalPointChildEditorPanel();

    void set_visible(bool visible);
    bool is_visible() const { return visible_; }

    void set_screen_dimensions(int width, int height);
    void set_panel_bounds_override(const SDL_Rect& bounds);
    void clear_panel_bounds_override();

    void set_point_selected(bool selected);
    bool point_selected() const { return point_selected_; }
    void set_resolved_asset_name(const std::string& asset_name);
    void set_point_detail_values(const PointDetailValues& values);
    void set_recommendation_pool(const std::vector<std::string>& tags);

    void set_on_apply_point_details(ApplyPointDetailsCallback callback);

    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;
    bool is_point_inside(int x, int y) const;

private:
    enum class TagKind {
        Positive,
        Negative,
        Recommended,
    };

    struct TagChip {
        std::string value;
        TagKind kind = TagKind::Recommended;
        std::unique_ptr<DMButton> button;
    };

    static bool point_in_rect(int x, int y, const SDL_Rect& rect);
    static int scaling_method_to_dropdown_index(AnchorScalingMethod method);
    static AnchorScalingMethod dropdown_index_to_scaling_method(int index);
    static std::string normalize_tag(std::string_view raw);
    static std::string to_lower_copy(std::string value);
    static bool starts_with(std::string_view value, std::string_view prefix);

    void update_layout() const;
    int layout_tag_chips(std::vector<TagChip>& chips,
                         int content_x,
                         int content_w,
                         int start_y,
                         bool apply) const;
    void set_tag_values(const std::vector<std::string>& positive,
                        const std::vector<std::string>& negative);
    void refresh_recommendations();
    void rebuild_tag_buttons();
    void emit_point_detail_change();
    PointDetailValues collect_point_detail_values() const;

    void add_positive_tag(const std::string& tag);
    void add_negative_tag(const std::string& tag);
    void remove_positive_tag(const std::string& tag);
    void remove_negative_tag(const std::string& tag);

private:
    bool visible_ = false;
    bool point_selected_ = false;
    int screen_w_ = 0;
    int screen_h_ = 0;
    mutable bool layout_dirty_ = true;

    bool panel_bounds_override_active_ = false;
    SDL_Rect panel_bounds_override_{0, 0, 0, 0};
    mutable SDL_Rect panel_rect_{364, 56, 332, 760};

    mutable SDL_Rect header_rect_{0, 0, 0, 0};
    mutable SDL_Rect source_rect_{0, 0, 0, 0};
    mutable SDL_Rect title_rect_{0, 0, 0, 0};
    mutable SDL_Rect positive_label_rect_{0, 0, 0, 0};
    mutable SDL_Rect negative_label_rect_{0, 0, 0, 0};
    mutable SDL_Rect recommended_label_rect_{0, 0, 0, 0};

    std::string resolved_asset_name_;
    std::string search_input_;
    std::string search_query_;

    std::vector<std::string> recommendation_pool_;
    std::vector<std::string> recommended_tags_;
    std::set<std::string> positive_tags_;
    std::set<std::string> negative_tags_;

    std::unique_ptr<DMSlider> point_rotation_slider_;
    std::unique_ptr<DMCheckbox> point_hidden_checkbox_;
    std::unique_ptr<DMButton> advanced_options_button_;
    std::unique_ptr<DMCheckbox> point_resolve_x_checkbox_;
    std::unique_ptr<DMDropdown> point_scaling_method_dropdown_;
    std::unique_ptr<DMTextBox> search_box_;
    bool advanced_options_expanded_ = false;

    std::vector<TagChip> positive_tag_chips_;
    std::vector<TagChip> negative_tag_chips_;
    std::vector<TagChip> recommended_tag_chips_;

    ApplyPointDetailsCallback on_apply_point_details_;

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
    friend struct RoomOvalPointChildEditorPanelTestAccess;
#endif
};

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
struct RoomOvalPointChildEditorPanelTestAccess {
    static void set_query(RoomOvalPointChildEditorPanel& panel, const std::string& query);
    static void set_recommendation_pool(RoomOvalPointChildEditorPanel& panel,
                                        const std::vector<std::string>& pool);
    static void set_point_detail_values(RoomOvalPointChildEditorPanel& panel,
                                        const RoomOvalPointChildEditorPanel::PointDetailValues& values);
    static void left_click_recommended(RoomOvalPointChildEditorPanel& panel, const std::string& tag);
    static void right_click_recommended(RoomOvalPointChildEditorPanel& panel, const std::string& tag);
    static void click_positive(RoomOvalPointChildEditorPanel& panel, const std::string& tag);
    static void click_negative(RoomOvalPointChildEditorPanel& panel, const std::string& tag);
    static std::vector<std::string> positive_tags(const RoomOvalPointChildEditorPanel& panel);
    static std::vector<std::string> negative_tags(const RoomOvalPointChildEditorPanel& panel);
    static const std::vector<std::string>& recommended_tags(const RoomOvalPointChildEditorPanel& panel);
};
#endif
