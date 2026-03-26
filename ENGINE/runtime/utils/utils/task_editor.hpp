#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class DMButton;
class DMTextBox;
class DMDropdown;

class TaskEditor {
public:
    explicit TaskEditor(std::filesystem::path repo_root);
    ~TaskEditor();

    void open();
    void close();
    bool is_open() const { return is_open_; }
    void open_with_new_task_attachment(const std::string& attachment_path);

    void update();
    void render(SDL_Renderer* renderer);
    bool handle_event(const SDL_Event& event);

private:
    static constexpr int kColumns = 7;
    static constexpr size_t kCsvColumns = 6;
    enum ColumnId {
        ColumnLastUpdated,
        ColumnType,
        ColumnSeverity,
        ColumnDescription,
        ColumnCreatedBy,
        ColumnAttachment,
        ColumnDelete
    };

    struct ColumnMetrics {
        std::array<int, kColumns> starts{};
        std::array<int, kColumns> widths{};
    };

    struct TaskRow {
        std::string type;
        std::string last_updated;
        std::string severity;
        std::string description;
        std::string created_by;
        std::string attachment_path;
    };

    struct RowWidgets {
        std::unique_ptr<DMDropdown> type_dropdown;
        std::unique_ptr<DMDropdown> severity_dropdown;
        std::unique_ptr<DMTextBox> description_box;
        std::unique_ptr<DMTextBox> created_by_box;
        std::unique_ptr<DMButton> attach_button;
        std::unique_ptr<DMButton> delete_button;
    };

    void load_tasks();
    void save_tasks();
    void rebuild_widgets();
    void layout_ui(const SDL_Rect& screen_rect);
    void clamp_scroll();
    ColumnMetrics compute_column_metrics() const;
    void set_row_widget_rects(size_t index, int row_top, const ColumnMetrics& metrics);

    void add_task();
    void add_task_with_attachment(const std::string& attachment_path);
    void delete_task(size_t index);
    void mark_row_updated(size_t index);
    std::string timestamp_now() const;

    void open_attachment_picker(size_t row_index);
    bool pick_files(std::vector<std::filesystem::path>& out_paths);
    bool pick_folder(std::vector<std::filesystem::path>& out_paths);
    bool copy_selected_paths(size_t row_index, const std::vector<std::filesystem::path>& selections);

    static std::vector<std::string> parse_csv_line(const std::string& line);
    static std::string escape_csv_field(const std::string& value);

    bool is_open_ = false;
    bool layout_dirty_ = true;

    SDL_Rect popup_rect_{};
    SDL_Rect header_rect_{};
    SDL_Rect table_rect_{};
    SDL_Rect attachment_dialog_rect_{};

    std::unique_ptr<DMButton> add_button_;
    std::unique_ptr<DMButton> close_button_;
    std::unique_ptr<DMButton> attachment_pick_files_;
    std::unique_ptr<DMButton> attachment_pick_folder_;
    std::unique_ptr<DMButton> attachment_cancel_button_;

    std::filesystem::path repo_root_;
    std::filesystem::path csv_path_;
    std::vector<TaskRow> tasks_;
    std::vector<RowWidgets> row_widgets_;
    std::vector<int> row_offsets_;

    int scroll_offset_ = 0;
    const int row_height_ = 160;
    const int row_gap_ = 14;

    std::vector<std::string> type_options_{"issue", "modification", "feature", "design", "research", "media"};
    std::vector<std::string> severity_options_{"low", "medium", "high"};

    bool attachment_dialog_open_ = false;
    size_t attachment_dialog_row_ = 0;

    SDL_Point last_mouse_pos_{0, 0};
};
