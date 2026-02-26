#pragma once

#include <SDL3/SDL.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "devtools/widgets.hpp"

namespace animation_editor {

class AnimationDocument;

class AnimationOptionsPanel {
  public:
    AnimationOptionsPanel();

    void set_document(std::shared_ptr<AnimationDocument> document);
    void set_animation_id(const std::string& animation_id);
    void set_bounds(const SDL_Rect& bounds);
    void update();
    void render(SDL_Renderer* renderer) const;
    bool handle_event(const SDL_Event& e);
    int preferred_height(int width) const;
    void set_on_animation_properties_changed(std::function<void(const std::string&, const nlohmann::json&)> callback);

  private:
    void ensure_widgets();
    void layout_widgets() const;
    void sync_from_document();
    void apply_changes_from_controls();
    float selected_speed() const;

  private:
    std::shared_ptr<AnimationDocument> document_;
    std::string animation_id_;
    SDL_Rect bounds_{0, 0, 0, 0};
    mutable SDL_Rect label_rect_{0, 0, 0, 0};
    mutable bool layout_dirty_ = true;
    std::unique_ptr<DMDropdown> speed_dropdown_;
    std::unique_ptr<DMCheckbox> crop_checkbox_;
    float cached_speed_ = 1.0f;
    bool cached_crop_ = false;
    std::string payload_signature_;
    bool syncing_ = false;
    std::function<void(const std::string&, const nlohmann::json&)> on_animation_properties_changed_;
};

}
