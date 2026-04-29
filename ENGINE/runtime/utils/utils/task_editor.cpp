#include "utils/task_editor.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#    include <windows.h>
#    include <objbase.h>
#    include <shobjidl_core.h>
#    include <wrl/client.h>
#endif

#include "devtools/dm_styles.hpp"
#include "devtools/widgets.hpp"
#include "devtools/font_cache.hpp"
#include "utils/log.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/sdl_render_conversions.hpp"

namespace {

std::string format_timestamp(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local_tm;
#if defined(_WIN32)
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
    return std::string(buffer);
}

inline std::string strip_carriage_return(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

struct ScopedComInit {
    ScopedComInit() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        initialized = SUCCEEDED(hr);
    }
    ~ScopedComInit() {
        if (initialized) {
            CoUninitialize();
        }
    }
    bool ok() const { return initialized; }

private:
    bool initialized = false;
};

bool collect_paths_from_array(ComPtr<IShellItemArray> array, std::vector<std::filesystem::path>& out_paths) {
    if (!array) {
        return false;
    }
    DWORD count = 0;
    if (FAILED(array->GetCount(&count))) {
        return false;
    }
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(array->GetItemAt(i, &item)) || !item) {
            continue;
        }
        PWSTR raw_path = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) || !raw_path) {
            continue;
        }
        out_paths.emplace_back(std::filesystem::path(raw_path));
        CoTaskMemFree(raw_path);
    }
    return !out_paths.empty();
}
#endif

constexpr int kRowControlTopPadding = 8;
constexpr int kRowControlHeight = 34;
constexpr int kAttachmentLabelSpacing = 6;
constexpr int kAttachmentLabelHeight = 20;
constexpr int kDescriptionVerticalSpacing = 8;

} // namespace

TaskEditor::TaskEditor(std::filesystem::path repo_root)
    : repo_root_(std::move(repo_root)),
      csv_path_(repo_root_ / "docs/tasks.csv") {
    add_button_ = std::make_unique<DMButton>("Add Task", &DMStyles::CreateButton(), 0, DMButton::height());
    close_button_ = std::make_unique<DMButton>("Close", &DMStyles::DeleteButton(), 0, DMButton::height());
    attachment_pick_files_ = std::make_unique<DMButton>("Select Files", &DMStyles::CreateButton(), 0, DMButton::height());
    attachment_pick_folder_ = std::make_unique<DMButton>("Select Folder", &DMStyles::CreateButton(), 0, DMButton::height());
    attachment_cancel_button_ = std::make_unique<DMButton>("Cancel", &DMStyles::DeleteButton(), 0, DMButton::height());
    load_tasks();
}

TaskEditor::~TaskEditor() = default;

void TaskEditor::open() {
    if (is_open_) {
        return;
    }
    is_open_ = true;
    layout_dirty_ = true;
    scroll_offset_ = 0;
    load_tasks();
}

void TaskEditor::close() {
    if (!is_open_) {
        return;
    }
    is_open_ = false;
    attachment_dialog_open_ = false;
}

void TaskEditor::open_with_new_task_attachment(const std::string& attachment_path) {
    if (!is_open_) {
        open();
    }
    add_task_with_attachment(attachment_path);
}

void TaskEditor::update() {
    (void)is_open_;
}

void TaskEditor::render(SDL_Renderer* renderer) {
    if (!is_open_) {
        return;
    }

    SDL_Rect screen_rect{0, 0, 0, 0};
    if (!SDL_GetCurrentRenderOutputSize(renderer, &screen_rect.w, &screen_rect.h)) {
        return;
    }
    if (layout_dirty_) {
        layout_ui(screen_rect);
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
    sdl_render::FillRect(renderer, &screen_rect);

    SDL_SetRenderDrawColor(renderer, 30, 30, 35, 230);
    sdl_render::FillRect(renderer, &popup_rect_);
    SDL_SetRenderDrawColor(renderer, 80, 80, 90, 255);
    sdl_render::Rect(renderer, &popup_rect_);

    if (add_button_) {
        add_button_->render(renderer);
    }
    if (close_button_) {
        close_button_->render(renderer);
    }

    DMLabelStyle header_style = DMStyles::Label();
    header_style.font_size += 2;
    const auto metrics = compute_column_metrics();
    constexpr std::array<const char*, kColumns> headers = {"Updated", "Type", "Severity", "Description", "Created By", "Attach", "Delete"};
    DMFontCache::instance().draw_text(renderer, header_style, "Tasks", table_rect_.x, table_rect_.y - (header_style.font_size * 2) - 8);
    DMFontCache::instance().draw_text(renderer, header_style, headers[ColumnLastUpdated], metrics.starts[ColumnLastUpdated], table_rect_.y - header_style.font_size - 4);
    DMFontCache::instance().draw_text(renderer, header_style, headers[ColumnType], metrics.starts[ColumnType], table_rect_.y - header_style.font_size - 4);
    DMFontCache::instance().draw_text(renderer, header_style, headers[ColumnSeverity], metrics.starts[ColumnSeverity], table_rect_.y - header_style.font_size - 4);
    DMFontCache::instance().draw_text(renderer, header_style, headers[ColumnCreatedBy], metrics.starts[ColumnCreatedBy], table_rect_.y - header_style.font_size - 4);
    DMFontCache::instance().draw_text(renderer, header_style, headers[ColumnAttachment], metrics.starts[ColumnAttachment], table_rect_.y - header_style.font_size - 4);
    DMFontCache::instance().draw_text(renderer, header_style, headers[ColumnDelete], metrics.starts[ColumnDelete], table_rect_.y - header_style.font_size - 4);

    SDL_SetRenderClipRect(renderer, &table_rect_);
    for (size_t i = 0; i < tasks_.size(); ++i) {
        int row_top = table_rect_.y - scroll_offset_ + row_offsets_.at(i);
        SDL_Rect row_rect{table_rect_.x + 2, row_top, table_rect_.w - 4, row_height_};
        SDL_SetRenderDrawColor(renderer, 36, 36, 42, 235);
        sdl_render::FillRect(renderer, &row_rect);
        SDL_SetRenderDrawColor(renderer, 82, 82, 96, 255);
        sdl_render::Rect(renderer, &row_rect);

        set_row_widget_rects(i, row_top, metrics);
        auto& rw = row_widgets_.at(i);
        if (rw.type_dropdown) rw.type_dropdown->render(renderer);
        if (rw.severity_dropdown) rw.severity_dropdown->render(renderer);
        if (rw.description_box) rw.description_box->render(renderer);
        if (rw.created_by_box) rw.created_by_box->render(renderer);
        if (rw.attach_button) rw.attach_button->render(renderer);
        if (rw.delete_button) rw.delete_button->render(renderer);

        const DMLabelStyle label_style = DMStyles::Label();
        const int text_y = row_top + kRowControlTopPadding + 4;
        DMFontCache::instance().draw_text(renderer, label_style, tasks_[i].last_updated, metrics.starts[ColumnLastUpdated], text_y);
        const std::string attachment_label = tasks_[i].attachment_path.empty() ? "(none)" : tasks_[i].attachment_path;
        const int attachment_label_y = row_top + kRowControlTopPadding + kRowControlHeight + kAttachmentLabelSpacing;
        DMFontCache::instance().draw_text(renderer, label_style, attachment_label, metrics.starts[ColumnAttachment], attachment_label_y);
    }

    SDL_SetRenderClipRect(renderer, nullptr);
    DMDropdown::render_active_options(renderer);

    if (attachment_dialog_open_) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 40, 40, 45, 240);
        sdl_render::FillRect(renderer, &attachment_dialog_rect_);
        SDL_SetRenderDrawColor(renderer, 120, 120, 140, 255);
        sdl_render::Rect(renderer, &attachment_dialog_rect_);
        const int button_width = attachment_dialog_rect_.w - 24;
        const int button_x = attachment_dialog_rect_.x + 12;
        int button_y = attachment_dialog_rect_.y + 16;
        SDL_Rect button_rect{button_x, button_y, button_width, DMButton::height()};
        attachment_pick_files_->set_rect(button_rect);
        attachment_pick_files_->render(renderer);
        button_y += DMButton::height() + 10;
        button_rect.y = button_y;
        attachment_pick_folder_->set_rect(button_rect);
        attachment_pick_folder_->render(renderer);
        button_y += DMButton::height() + 10;
        button_rect.y = button_y;
        attachment_cancel_button_->set_rect(button_rect);
        attachment_cancel_button_->render(renderer);
    }
}

bool TaskEditor::handle_event(const SDL_Event& event) {
    if (!is_open_) {
        return false;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        last_mouse_pos_.x = static_cast<int>(event.motion.x);
        last_mouse_pos_.y = static_cast<int>(event.motion.y);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        last_mouse_pos_.x = static_cast<int>(event.button.x);
        last_mouse_pos_.y = static_cast<int>(event.button.y);
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        float mx = 0.0f;
        float my = 0.0f;
        SDL_GetMouseState(&mx, &my);
        last_mouse_pos_.x = static_cast<int>(mx);
        last_mouse_pos_.y = static_cast<int>(my);
    }

    if (attachment_dialog_open_) {
        if (attachment_pick_files_ && attachment_pick_files_->handle_event(event)) {
            std::vector<std::filesystem::path> paths;
            if (pick_files(paths)) {
                copy_selected_paths(attachment_dialog_row_, paths);
            }
            attachment_dialog_open_ = false;
            return true;
        }
        if (attachment_pick_folder_ && attachment_pick_folder_->handle_event(event)) {
            std::vector<std::filesystem::path> paths;
            if (pick_folder(paths)) {
                copy_selected_paths(attachment_dialog_row_, paths);
            }
            attachment_dialog_open_ = false;
            return true;
        }
        if (attachment_cancel_button_ && attachment_cancel_button_->handle_event(event)) {
            attachment_dialog_open_ = false;
            return true;
        }
        return true;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        close();
        return true;
    }

    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        SDL_Point pt{last_mouse_pos_.x, last_mouse_pos_.y};
        if (SDL_PointInRect(&pt, &table_rect_)) {
            scroll_offset_ -= event.wheel.y * (row_height_ / 2);
            clamp_scroll();
            return true;
        }
    }

    bool consumed = false;
    if (add_button_ && add_button_->handle_event(event)) {
        add_task();
        consumed = true;
    }
    if (close_button_ && close_button_->handle_event(event)) {
        close();
        return true;
    }

    const ColumnMetrics metrics = compute_column_metrics();
    for (size_t i = 0; i < tasks_.size(); ++i) {
        int row_top = table_rect_.y - scroll_offset_ + row_offsets_.at(i);
        set_row_widget_rects(i, row_top, metrics);
        auto& rw = row_widgets_.at(i);
        if (rw.type_dropdown && rw.type_dropdown->handle_event(event)) {
            const int sel = rw.type_dropdown->selected();
            const int clamped = std::clamp(sel, 0, static_cast<int>(type_options_.size()) - 1);
            if (tasks_[i].type != type_options_[clamped]) {
                tasks_[i].type = type_options_[clamped];
                mark_row_updated(i);
            }
            consumed = true;
        }
        if (rw.severity_dropdown && rw.severity_dropdown->handle_event(event)) {
            const int sel = rw.severity_dropdown->selected();
            const int clamped = std::clamp(sel, 0, static_cast<int>(severity_options_.size()) - 1);
            if (tasks_[i].severity != severity_options_[clamped]) {
                tasks_[i].severity = severity_options_[clamped];
                mark_row_updated(i);
            }
            consumed = true;
        }
        if (rw.description_box && rw.description_box->handle_event(event)) {
            const std::string value = rw.description_box->value();
            if (tasks_[i].description != value) {
                tasks_[i].description = value;
                mark_row_updated(i);
            }
            consumed = true;
        }
        if (rw.created_by_box && rw.created_by_box->handle_event(event)) {
            const std::string value = rw.created_by_box->value();
            if (tasks_[i].created_by != value) {
                tasks_[i].created_by = value;
                mark_row_updated(i);
            }
            consumed = true;
        }
        if (rw.attach_button && rw.attach_button->handle_event(event)) {
            open_attachment_picker(i);
            consumed = true;
            break;
        }
        if (rw.delete_button && rw.delete_button->handle_event(event)) {
            delete_task(i);
            consumed = true;
            break;
        }
    }

    if (consumed) {
        return true;
    }

    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    default:
        break;
    }
    return false;
}

void TaskEditor::load_tasks() {
    tasks_.clear();
    if (!std::filesystem::exists(csv_path_)) {
        save_tasks();
    }
    std::ifstream in(csv_path_);
    if (!in.is_open()) {
        vibble::log::error("[TaskEditor] Unable to read " + csv_path_.string());
        return;
    }
    std::string line;
    bool first_line = true;
    while (std::getline(in, line)) {
        line = strip_carriage_return(line);
        if (line.empty()) {
            continue;
        }
        if (first_line && line.rfind("type", 0) == 0) {
            first_line = false;
            continue;
        }
        first_line = false;
        const auto fields = parse_csv_line(line);
        if (fields.size() < kCsvColumns) {
            continue;
        }
        TaskRow row;
        row.type = fields[0];
        row.last_updated = fields[1];
        row.severity = fields[2];
        row.description = fields[3];
        row.created_by = fields[4];
        row.attachment_path = fields[5];
        tasks_.push_back(std::move(row));
    }
    rebuild_widgets();
    layout_dirty_ = true;
    clamp_scroll();
}

void TaskEditor::save_tasks() {
    const std::filesystem::path tmp_path = csv_path_.string() + ".tmp";
    std::ofstream out(tmp_path, std::ios::trunc);
    if (!out.is_open()) {
        vibble::log::error("[TaskEditor] Unable to write " + tmp_path.string());
        return;
    }
    out << "type,last_updated,severity,description,created_by,attachment_path\n";
    for (const auto& row : tasks_) {
        out << escape_csv_field(row.type) << ','
            << escape_csv_field(row.last_updated) << ','
            << escape_csv_field(row.severity) << ','
            << escape_csv_field(row.description) << ','
            << escape_csv_field(row.created_by) << ','
            << escape_csv_field(row.attachment_path) << '\n';
    }
    out.close();
    std::error_code ec;
    std::filesystem::rename(tmp_path, csv_path_, ec);
    if (ec) {
        ec.clear();
        std::filesystem::remove(csv_path_, ec);
        ec.clear();
        std::filesystem::rename(tmp_path, csv_path_, ec);
    }
    if (ec) {
        vibble::log::error("[TaskEditor] Unable to persist " + csv_path_.string() + ": " + ec.message());
        std::filesystem::remove(tmp_path, ec);
    }
}

void TaskEditor::rebuild_widgets() {
    row_widgets_.clear();
    row_widgets_.resize(tasks_.size());
    for (size_t i = 0; i < tasks_.size(); ++i) {
        TaskRow& row = tasks_[i];
        auto& rw = row_widgets_[i];
        if (!rw.type_dropdown) {
            rw.type_dropdown = std::make_unique<DMDropdown>("", type_options_);
        }
        const auto type_iter = std::find(type_options_.begin(), type_options_.end(), row.type);
        const int type_index = static_cast<int>(std::distance(type_options_.begin(), type_iter));
        rw.type_dropdown->set_selected(std::clamp(type_index, 0, static_cast<int>(type_options_.size()) - 1));

        if (!rw.severity_dropdown) {
            rw.severity_dropdown = std::make_unique<DMDropdown>("", severity_options_);
        }
        const auto severity_iter = std::find(severity_options_.begin(), severity_options_.end(), row.severity);
        const int severity_index = static_cast<int>(std::distance(severity_options_.begin(), severity_iter));
        rw.severity_dropdown->set_selected(std::clamp(severity_index, 0, static_cast<int>(severity_options_.size()) - 1));

        if (!rw.description_box) {
            rw.description_box = std::make_unique<DMTextBox>("", row.description);
        } else {
            rw.description_box->set_value(row.description);
        }
        if (!rw.created_by_box) {
            rw.created_by_box = std::make_unique<DMTextBox>("", row.created_by);
        } else {
            rw.created_by_box->set_value(row.created_by);
        }
        if (!rw.attach_button) {
            rw.attach_button = std::make_unique<DMButton>("Attach", &DMStyles::CreateButton(), 0, DMButton::height());
        }
        if (!rw.delete_button) {
            rw.delete_button = std::make_unique<DMButton>("Remove", &DMStyles::DeleteButton(), 0, DMButton::height());
        }
    }
    row_offsets_.resize(tasks_.size());
    for (size_t i = 0; i < row_offsets_.size(); ++i) {
        row_offsets_[i] = static_cast<int>(i) * (row_height_ + row_gap_);
    }
}

void TaskEditor::layout_ui(const SDL_Rect& screen_rect) {
    const int width = std::min(1500, screen_rect.w - 40);
    const int height = std::min(920, screen_rect.h - 40);
    popup_rect_.w = width;
    popup_rect_.h = height;
    popup_rect_.x = (screen_rect.w - width) / 2;
    popup_rect_.y = (screen_rect.h - height) / 2;

    header_rect_.x = popup_rect_.x + 12;
    header_rect_.y = popup_rect_.y + 12;
    header_rect_.w = popup_rect_.w - 24;
    header_rect_.h = DMButton::height();

    if (add_button_) {
        const int add_w = std::max(add_button_->preferred_width(), 120);
        const int close_w = close_button_ ? std::max(close_button_->preferred_width(), 100) : 0;
        const int gap = close_button_ ? 8 : 0;
        const int close_x = header_rect_.x + header_rect_.w - close_w;
        const int add_x = close_x - gap - add_w;
        add_button_->set_rect({add_x, header_rect_.y, add_w, header_rect_.h});
        if (close_button_) {
            close_button_->set_rect({close_x, header_rect_.y, close_w, header_rect_.h});
        }
    }

    table_rect_.x = popup_rect_.x + 12;
    table_rect_.y = header_rect_.y + header_rect_.h + 12;
    table_rect_.w = popup_rect_.w - 24;
    table_rect_.h = popup_rect_.h - (table_rect_.y - popup_rect_.y) - 16;

    attachment_dialog_rect_.w = 320;
    attachment_dialog_rect_.h = 180;
    attachment_dialog_rect_.x = popup_rect_.x + (popup_rect_.w - attachment_dialog_rect_.w) / 2;
    attachment_dialog_rect_.y = popup_rect_.y + (popup_rect_.h - attachment_dialog_rect_.h) / 2;

    row_offsets_.resize(tasks_.size());
    for (size_t i = 0; i < row_offsets_.size(); ++i) {
        row_offsets_[i] = static_cast<int>(i) * (row_height_ + row_gap_);
    }

    layout_dirty_ = false;
    clamp_scroll();
}

void TaskEditor::clamp_scroll() {
    const int total_height = static_cast<int>(tasks_.size()) * (row_height_ + row_gap_);
    const int max_scroll = std::max(0, total_height - table_rect_.h);
    scroll_offset_ = std::clamp(scroll_offset_, 0, max_scroll);
}

TaskEditor::ColumnMetrics TaskEditor::compute_column_metrics() const {
    ColumnMetrics metrics;
    const int gap = 10;
    const int left = table_rect_.x + 12;
    int x = left;

    metrics.starts[ColumnLastUpdated] = x;
    metrics.widths[ColumnLastUpdated] = 220;
    x += metrics.widths[ColumnLastUpdated] + gap;

    metrics.starts[ColumnType] = x;
    metrics.widths[ColumnType] = 130;
    x += metrics.widths[ColumnType] + gap;

    metrics.starts[ColumnSeverity] = x;
    metrics.widths[ColumnSeverity] = 130;
    x += metrics.widths[ColumnSeverity] + gap;

    metrics.starts[ColumnCreatedBy] = x;
    metrics.widths[ColumnCreatedBy] = 220;
    x += metrics.widths[ColumnCreatedBy] + gap;

    metrics.starts[ColumnAttachment] = x;
    metrics.widths[ColumnAttachment] = 110;
    x += metrics.widths[ColumnAttachment] + gap;

    metrics.starts[ColumnDelete] = x;
    metrics.widths[ColumnDelete] = 100;

    metrics.starts[ColumnDescription] = left;
    metrics.widths[ColumnDescription] = std::max(360, table_rect_.w - 24);

    return metrics;
}

void TaskEditor::set_row_widget_rects(size_t index, int row_top, const ColumnMetrics& metrics) {
    if (index >= row_widgets_.size()) {
        return;
    }
    auto& rw = row_widgets_[index];
    const int top_y = row_top + kRowControlTopPadding;
    const int control_h = kRowControlHeight;
    const int attachment_label_y = top_y + control_h + kAttachmentLabelSpacing;
    const int desc_y = attachment_label_y + kAttachmentLabelHeight + kDescriptionVerticalSpacing;
    const int desc_h = std::max(row_height_ - (desc_y - row_top) - kDescriptionVerticalSpacing, 40);
    if (rw.type_dropdown) {
        rw.type_dropdown->set_rect({metrics.starts[ColumnType], top_y, metrics.widths[ColumnType], control_h});
    }
    if (rw.severity_dropdown) {
        rw.severity_dropdown->set_rect({metrics.starts[ColumnSeverity], top_y, metrics.widths[ColumnSeverity], control_h});
    }
    if (rw.description_box) {
        rw.description_box->set_rect({metrics.starts[ColumnDescription], desc_y, metrics.widths[ColumnDescription], desc_h});
    }
    if (rw.created_by_box) {
        rw.created_by_box->set_rect({metrics.starts[ColumnCreatedBy], top_y, metrics.widths[ColumnCreatedBy], control_h});
    }
    if (rw.attach_button) {
        rw.attach_button->set_rect({metrics.starts[ColumnAttachment], top_y, metrics.widths[ColumnAttachment], control_h});
    }
    if (rw.delete_button) {
        rw.delete_button->set_rect({metrics.starts[ColumnDelete], top_y, metrics.widths[ColumnDelete], control_h});
    }
}

void TaskEditor::add_task() {
    add_task_with_attachment("");
}

void TaskEditor::add_task_with_attachment(const std::string& attachment_path) {
    TaskRow row;
    row.type = type_options_.front();
    row.severity = severity_options_.front();
    row.last_updated = timestamp_now();
    row.attachment_path = attachment_path;
    tasks_.insert(tasks_.begin(), std::move(row));
    rebuild_widgets();
    layout_dirty_ = true;
    scroll_offset_ = 0;
    save_tasks();
}

void TaskEditor::delete_task(size_t index) {
    if (index >= tasks_.size()) {
        return;
    }
    tasks_.erase(tasks_.begin() + index);
    rebuild_widgets();
    layout_dirty_ = true;
    clamp_scroll();
    save_tasks();
}

void TaskEditor::mark_row_updated(size_t index) {
    if (index >= tasks_.size()) {
        return;
    }
    tasks_[index].last_updated = timestamp_now();
    save_tasks();
}

std::string TaskEditor::timestamp_now() const {
    return format_timestamp(std::chrono::system_clock::now());
}

void TaskEditor::open_attachment_picker(size_t row_index) {
    if (row_index >= tasks_.size()) {
        return;
    }
    attachment_dialog_row_ = row_index;
    attachment_dialog_open_ = true;
}

bool TaskEditor::pick_files(std::vector<std::filesystem::path>& out_paths) {
#if defined(_WIN32)
    ScopedComInit com_init;
    if (!com_init.ok()) {
        return false;
    }
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM;
    dialog->SetOptions(options);
    if (SUCCEEDED(dialog->Show(nullptr))) {
        ComPtr<IShellItemArray> results;
        if (SUCCEEDED(dialog->GetResults(&results))) {
            return collect_paths_from_array(results, out_paths);
        }
    }
    return false;
#else
    (void)out_paths;
    return false;
#endif
}

bool TaskEditor::pick_folder(std::vector<std::filesystem::path>& out_paths) {
#if defined(_WIN32)
    ScopedComInit com_init;
    if (!com_init.ok()) {
        return false;
    }
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM;
    dialog->SetOptions(options);
    if (SUCCEEDED(dialog->Show(nullptr))) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR raw_path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) && raw_path) {
                out_paths.emplace_back(std::filesystem::path(raw_path));
                CoTaskMemFree(raw_path);
                return true;
            }
        }
    }
    return false;
#else
    (void)out_paths;
    return false;
#endif
}

bool TaskEditor::copy_selected_paths(size_t row_index, const std::vector<std::filesystem::path>& selections) {
    if (row_index >= tasks_.size() || selections.empty()) {
        return false;
    }
    const auto attachments_root = repo_root_ / "tasks_attachments";
    std::error_code ec;
    std::filesystem::create_directories(attachments_root, ec);
    if (ec) {
        vibble::log::error("[TaskEditor] Unable to create attachments directory: " + ec.message());
        return false;
    }
    const auto folder_name = "task_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const auto dest_dir = attachments_root / folder_name;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        vibble::log::error("[TaskEditor] Unable to create task folder: " + ec.message());
        return false;
    }
    for (const auto& path : selections) {
        if (!std::filesystem::exists(path)) {
            continue;
        }
        const auto target = dest_dir / path.filename();
        ec.clear();
        if (std::filesystem::is_directory(path)) {
            std::filesystem::copy(path, target, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
        } else {
            std::filesystem::copy_file(path, target, std::filesystem::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            vibble::log::error("[TaskEditor] Failed to copy attachment: " + ec.message());
            continue;
        }
    }
    const auto relative = std::filesystem::relative(dest_dir, repo_root_, ec);
    if (ec) {
        tasks_[row_index].attachment_path = dest_dir.string();
    } else {
        tasks_[row_index].attachment_path = relative.generic_string();
    }
    mark_row_updated(row_index);
    return true;
}

std::vector<std::string> TaskEditor::parse_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                current.push_back('"');
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (ch == ',' && !quoted) {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    fields.push_back(current);
    return fields;
}

std::string TaskEditor::escape_csv_field(const std::string& value) {
    const bool needs_quotes = value.find_first_of(",\"\n\r") != std::string::npos;
    std::string result;
    if (needs_quotes) {
        result.push_back('"');
    }
    for (char ch : value) {
        if (ch == '"') {
            result.push_back('"');
            result.push_back('"');
        } else {
            result.push_back(ch);
        }
    }
    if (needs_quotes) {
        result.push_back('"');
    }
    return result;
}
