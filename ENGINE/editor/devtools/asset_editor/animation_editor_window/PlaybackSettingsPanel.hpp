#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <nlohmann/json.hpp>

#include "devtools/widgets.hpp"

class DMCheckbox;

namespace animation_editor {

class AnimationDocument;

using DMCheckbox = ::DMCheckbox;

class PlaybackSettingsPanel {
  public:
    PlaybackSettingsPanel();

    void set_document(std::shared_ptr<AnimationDocument> document);
    void set_animation_id(const std::string& animation_id);
    void set_bounds(const SDL_Rect& bounds);

    void update();
    void render(SDL_Renderer* renderer) const;
    bool handle_event(const SDL_Event& e);

    int preferred_height(int width) const;

  private:
    struct PlaybackState {
        bool invert_x = false;
        bool invert_y = false;
        bool invert_z = false;
        bool reverse_source = false;
        bool inherit_data = true;
        bool invert_frames_horizontal = false;
        bool invert_frames_vertical = false;
        bool locked = false;
        bool random_start = false;

        bool operator==(const PlaybackState& other) const {
            return invert_x == other.invert_x &&
                   invert_y == other.invert_y &&
                   invert_z == other.invert_z &&
                   reverse_source == other.reverse_source &&
                   inherit_data == other.inherit_data &&
                   invert_frames_horizontal == other.invert_frames_horizontal &&
                   invert_frames_vertical == other.invert_frames_vertical &&
                   locked == other.locked &&
                   random_start == other.random_start;
        }

        bool operator!=(const PlaybackState& other) const { return !(*this == other); }
};

    void ensure_widgets();
    void layout_widgets() const;
    void apply_state_to_controls(const PlaybackState& state);
    PlaybackState read_controls() const;
    void handle_controls_changed();
    void sync_from_document();
    void commit_changes(const PlaybackState& desired_state);
    static std::optional<std::string> fetch_payload(const AnimationDocument* document, const std::string& animation_id);
    PlaybackState payload_to_state(const nlohmann::json& payload);
    void apply_state_to_payload(nlohmann::json& payload, const PlaybackState& state);
    void update_inherited_state(const nlohmann::json& payload);
    void refresh_inherited_message();
    bool inherit_controls_visible_for_state(const PlaybackState& state) const;
    bool random_start_visible_for_state(const PlaybackState& state) const;
    bool inversion_controls_visible_for_state(const PlaybackState& state) const;
    bool inherit_controls_visible() const { return inherit_controls_visible_for_state(state_); }
    bool random_start_visible() const { return random_start_visible_for_state(state_); }
    bool inversion_controls_visible() const { return inversion_controls_visible_for_state(state_); }

  private:
    std::shared_ptr<AnimationDocument> document_;
    std::string animation_id_;
    SDL_Rect bounds_{0, 0, 0, 0};

    std::unique_ptr<DMCheckbox> invert_x_checkbox_;
    std::unique_ptr<DMCheckbox> invert_y_checkbox_;
    std::unique_ptr<DMCheckbox> invert_z_checkbox_;
    std::unique_ptr<DMCheckbox> invert_frames_horizontal_checkbox_;
    std::unique_ptr<DMCheckbox> invert_frames_vertical_checkbox_;
    std::unique_ptr<DMCheckbox> inherit_geometry_checkbox_;
    std::unique_ptr<DMCheckbox> reverse_checkbox_;
    std::unique_ptr<DMCheckbox> locked_checkbox_;
    std::unique_ptr<DMCheckbox> random_start_checkbox_;

    PlaybackState state_{};
    PlaybackState document_state_{};
    bool has_document_state_ = false;
    mutable bool layout_dirty_ = true;
    bool is_syncing_ui_ = false;
    bool derived_from_animation_ = false;
    bool source_chain_resolves_to_frames_ = false;
    std::string derived_source_id_;
    std::vector<std::string> inherited_message_lines_;
    std::vector<std::string> inherited_modifiers_;
    mutable SDL_Rect inherited_message_rect_{0, 0, 0, 0};
    mutable SDL_Rect invert_frames_helper_rect_{0, 0, 0, 0};

    DMWidgetTooltipState info_tooltip_{};
};

}


