#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

#include <SDL3/SDL.h>

#include "EditorUIPrimitives.hpp"

namespace animation_editor {

class AnimationDocument;
class PreviewProvider;

class AnimationListPanel {
  public:
    struct ExternalRow {
        std::string id;
        int level = 0;
        bool missing_source = false;
        bool selectable = true;
    };

    AnimationListPanel();

    struct ExternalRowsFingerprint {
        std::size_t count = 0;
        std::uint64_t rolling_hash = 0;

        bool operator==(const ExternalRowsFingerprint& other) const {
            return count == other.count && rolling_hash == other.rolling_hash;
        }
    };

    void set_document(std::shared_ptr<AnimationDocument> document);
    void set_external_rows(std::optional<std::vector<ExternalRow>> rows);
    void set_show_delete_button(bool show);
    void set_bounds(const SDL_Rect& bounds);
    void set_preview_provider(std::shared_ptr<PreviewProvider> provider);
    void set_selected_animation_id(const std::optional<std::string>& animation_id);
    void set_on_selection_changed(std::function<void(const std::optional<std::string>&)> callback);
    void set_on_context_menu(std::function<void(const std::optional<std::string>&, const SDL_Point&)> callback);
    void set_on_delete_animation(std::function<void(const std::string&)> callback);

    void update();
    void render(SDL_Renderer* renderer) const;
    bool handle_event(const SDL_Event& e);
    bool is_point_inside(int x, int y) const;
    const std::optional<std::string>& debug_selected_animation_id() const { return selected_animation_id_; }
    int debug_row_count() const { return static_cast<int>(display_rows_.size()); }
    const std::string& debug_last_hit_result() const { return last_hit_result_; }

  private:
    enum class HitTarget {
        Outside,
        Empty,
        Row,
        Delete,
    };

    struct HitTestResult {
        HitTarget target = HitTarget::Outside;
        std::optional<size_t> row_index;
        std::string animation_id;
    };

    void rebuild_rows();
    void layout_rows();
    void scroll_selection_into_view();
    HitTestResult hit_test(const SDL_Point& p) const;
    std::optional<size_t> row_index_at_point(const SDL_Point& p) const;
    void ensure_layout() const;
    void record_hit_result(const char* phase, const SDL_Point& p, const HitTestResult& hit) const;

  private:
    struct DisplayRow {
        std::string id;
        int level = 0;
        bool missing_source = false;
        bool selectable = true;
};
    struct RowGeometry {
        SDL_Rect outer{0, 0, 0, 0};
        SDL_Rect delete_button_rel{0, 0, 0, 0};
        SDL_Rect preview_rel{0, 0, 0, 0};
        int content_offset_x = 0;
        int content_offset_y = 0;
        int content_height = 0;
};

    std::shared_ptr<AnimationDocument> document_;
    std::optional<std::vector<ExternalRow>> external_rows_;
    std::optional<ExternalRowsFingerprint> last_external_rows_fingerprint_;
    std::uint64_t row_update_applied_count_ = 0;
    std::uint64_t row_update_skipped_count_ = 0;
    std::vector<RowGeometry> row_geometry_;
    std::vector<DisplayRow> display_rows_;
    std::optional<std::string> start_animation_id_;
    std::shared_ptr<PreviewProvider> preview_provider_;
    std::function<void(const std::optional<std::string>&)> on_selection_changed_;
    std::function<void(const std::optional<std::string>&, const SDL_Point&)> on_context_menu_;
    std::function<void(const std::string&)> on_delete_animation_;
    std::optional<std::string> selected_animation_id_;
    std::optional<size_t> hovered_row_;
    std::optional<size_t> hovered_delete_row_;
    SDL_Rect bounds_{0, 0, 0, 0};
    int content_height_ = 0;
    mutable bool layout_dirty_ = true;
    ui::ScrollController scroll_controller_;
    std::optional<std::uint64_t> last_document_revision_;
    mutable std::string last_hit_result_ = "none";
    bool show_delete_button_ = true;

    std::unordered_map<std::string, std::string> root_for_id_;
};

}
