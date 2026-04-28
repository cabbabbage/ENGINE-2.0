#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <limits>

#include "widgets.hpp"

class TagEditorWidget : public Widget {
public:
    enum class Mode {
        Legacy,
        AssetInfoOverhaul
    };

    explicit TagEditorWidget(Mode mode = Mode::Legacy);
    ~TagEditorWidget() override;

    void set_mode(Mode mode);
    Mode mode() const { return mode_; }

    void set_tags(const std::vector<std::string>& tags, const std::vector<std::string>& anti_tags);
    void set_recommended_tags(const std::vector<std::string>& tags);
    void set_subject_asset_name(const std::string& asset_name);

    std::vector<std::string> tags() const;
    std::vector<std::string> anti_tags() const;

    void set_on_changed(std::function<void(const std::vector<std::string>&, const std::vector<std::string>&)> cb);

    void set_rect(const SDL_Rect& r) override;
    const SDL_Rect& rect() const override { return rect_; }
    int height_for_width(int w) const override;
    bool handle_event(const SDL_Event& e) override;
    void render(SDL_Renderer* r) const override;

private:
    enum class ChipListKind {
        None,
        Tags,
        AntiTags,
        Recommended,
        SearchInputVirtual
    };

    struct Chip {
        std::string value;
        ChipListKind kind = ChipListKind::None;
        std::unique_ptr<DMButton> button;
    };

    struct DragState {
        bool pressed = false;
        bool dragging = false;
        SDL_Point start_point{0,0};
        SDL_Point pointer{0,0};
        std::string value;
        ChipListKind source_kind = ChipListKind::None;
        ChipListKind hover_target = ChipListKind::None;
    };

    void rebuild_buttons();
    void refresh_recommendations();
    void mark_dirty();
    void layout_if_needed() const;
    int layout(int width, int origin_x, int origin_y, bool apply);
    int layout_asset_overhaul(int width, int origin_x, int origin_y, bool apply);
    int layout_grid(std::vector<Chip>& chips, int width, int origin_x, int start_y, bool apply, size_t visible_count = std::numeric_limits<size_t>::max(), const std::vector<size_t>* display_order = nullptr);
    static int label_height();
    void draw_label(SDL_Renderer* r, const std::string& text, const SDL_Rect& rect) const;

    void handle_chip_click(const Chip& chip, const SDL_Event& e, const std::function<void(const std::string&)>& on_click, bool& used);

    void add_tag(const std::string& value);
    void add_anti_tag(const std::string& value);
    void remove_tag(const std::string& value);
    void remove_anti_tag(const std::string& value);
    static std::string normalize(const std::string& value);

    void notify_changed();
    void reset_toggle_state();
    void update_toggle_labels();
    void update_search_filter();
    void clear_search();
    void add_search_text_as_tag();
    static bool event_targets_rect(const SDL_Event& e, const SDL_Rect& rect);
    static bool starts_with_casefold(const std::string& value, const std::string& prefix);

    void refresh_recommendations_asset_mode();
    void rebuild_recommended_from_search();
    bool handle_event_asset_overhaul(const SDL_Event& e);
    void render_asset_overhaul(SDL_Renderer* r) const;
    Chip* hit_chip_at(const SDL_Point& p);
    const Chip* hit_chip_at(const SDL_Point& p) const;
    ChipListKind drop_target_at(const SDL_Point& p) const;
    void clear_drag_state();
    bool move_chip_between_lists(const std::string& value, ChipListKind source, ChipListKind target);
    bool handle_chip_double_click(const Chip& chip);
    void clear_asset_session_state();

    SDL_Rect rect_{0,0,0,0};
    mutable bool layout_dirty_ = true;
    Mode mode_ = Mode::Legacy;

    std::set<std::string> tags_;
    std::set<std::string> anti_tags_;
    std::vector<std::string> recommended_tags_;
    std::vector<std::string> recommended_anti_;
    std::vector<std::string> external_recommended_tags_;

    mutable SDL_Rect tags_label_rect_{0,0,0,0};
    mutable SDL_Rect anti_label_rect_{0,0,0,0};
    mutable SDL_Rect rec_tags_label_rect_{0,0,0,0};
    mutable SDL_Rect rec_anti_label_rect_{0,0,0,0};
    mutable SDL_Rect tags_drop_rect_{0,0,0,0};
    mutable SDL_Rect anti_drop_rect_{0,0,0,0};
    mutable SDL_Rect rec_drop_rect_{0,0,0,0};

    std::vector<Chip> tag_chips_;
    std::vector<Chip> anti_chips_;
    std::vector<Chip> rec_tag_chips_;
    std::vector<Chip> rec_anti_chips_;
    std::vector<size_t> filtered_tag_order_;
    std::unordered_map<std::string, double> recommendation_context_scores_;
    std::unordered_map<std::string, int> recommendation_popularity_;
    std::set<std::string> hidden_recommended_;
    std::set<std::string> session_recommended_;
    bool show_search_virtual_chip_ = false;
    std::string search_virtual_value_;
    DragState drag_state_;

    bool show_all_tag_recs_ = false;
    bool show_all_anti_recs_ = false;
    std::unique_ptr<DMButton> show_more_tags_btn_;
    std::unique_ptr<DMButton> show_more_anti_btn_;
    std::unique_ptr<DMTextBox> tag_search_box_;
    std::unique_ptr<DMButton> add_tag_btn_;
    std::unique_ptr<DMCheckbox> add_as_anti_checkbox_;
    std::unique_ptr<CheckboxWidget> add_as_anti_widget_;
    std::unique_ptr<DMButton> browse_tags_btn_;
    std::unique_ptr<ButtonWidget> browse_tags_widget_;
    std::string search_input_;
    std::string search_query_;
    std::string subject_asset_name_;
    bool show_browse_tags_ = false;

    std::function<void(const std::vector<std::string>&, const std::vector<std::string>&)> on_changed_;

private:
    void update_browse_mode();

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
    friend struct TagEditorWidgetTestAccess;
#endif
};

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
struct TagEditorWidgetTestAccess {
    static void set_query(TagEditorWidget& widget, const std::string& query);
    static const std::vector<std::string>& recommended_tags(const TagEditorWidget& widget);
    static bool has_search_virtual_chip(const TagEditorWidget& widget);
    static const std::string& search_virtual_value(const TagEditorWidget& widget);
    static void rebuild_recommendations(TagEditorWidget& widget);
    static std::vector<std::string> top_similar_asset_names(const TagEditorWidget& widget);
    static bool pruning_matches_exact(const TagEditorWidget& widget);
};
#endif
