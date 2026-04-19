#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

class DMButton;
class DMTextBox;

namespace animation_editor {

class AnimationDocument;

class AnimationTagsPanel {
  public:
    AnimationTagsPanel();
    ~AnimationTagsPanel();

    void set_document(std::shared_ptr<AnimationDocument> document);
    void set_animation_id(const std::string& animation_id);
    void set_bounds(const SDL_Rect& bounds);
    void set_status_callback(std::function<void(const std::string&)> callback);
    void set_on_tags_changed(std::function<void(const std::vector<std::string>&)> callback);

    void update();
    void render(SDL_Renderer* renderer) const;
    bool handle_event(const SDL_Event& e);

    int preferred_height(int width) const;

  private:
    struct Chip {
        std::string value;
        std::unique_ptr<DMButton> button;
    };

    void sync_tags_from_document();
    void refresh_recommendation_pool();
    void refresh_recommendations();
    void rebuild_buttons();
    void persist_tags_to_document();
    void mark_layout_dirty();
    void ensure_layout() const;
    int layout_for_width(int width, int origin_x, int origin_y, bool apply) const;
    int layout_chip_grid(std::vector<Chip>& chips, int width, int origin_x, int start_y, bool apply) const;

    static std::string normalize_tag(std::string_view raw);

    std::shared_ptr<AnimationDocument> document_;
    std::string animation_id_;
    SDL_Rect bounds_{0, 0, 0, 0};
    mutable bool layout_dirty_ = true;

    std::set<std::string> tags_;
    std::vector<std::string> recommendation_pool_;
    std::vector<std::string> recommended_tags_;

    std::unique_ptr<DMTextBox> search_box_;
    std::string search_input_;
    std::string search_query_;
    std::string payload_signature_;
    std::uint64_t recommendation_pool_version_ = 0;

    mutable SDL_Rect search_rect_{0, 0, 0, 0};
    mutable SDL_Rect tags_label_rect_{0, 0, 0, 0};
    mutable SDL_Rect rec_label_rect_{0, 0, 0, 0};
    mutable SDL_Rect tags_content_rect_{0, 0, 0, 0};
    mutable SDL_Rect rec_content_rect_{0, 0, 0, 0};

    std::vector<Chip> tag_chips_;
    std::vector<Chip> rec_chips_;

    std::function<void(const std::string&)> status_callback_;
    std::function<void(const std::vector<std::string>&)> on_tags_changed_;

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
    friend struct AnimationTagsPanelTestAccess;
#endif
};

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
struct AnimationTagsPanelTestAccess {
    static void set_query(AnimationTagsPanel& panel, const std::string& query);
    static void add_tag(AnimationTagsPanel& panel, const std::string& tag);
    static void remove_tag(AnimationTagsPanel& panel, const std::string& tag);
    static std::vector<std::string> tags(const AnimationTagsPanel& panel);
    static const std::vector<std::string>& recommended_tags(const AnimationTagsPanel& panel);
    static void refresh_pool(AnimationTagsPanel& panel);
};
#endif

}  // namespace animation_editor
