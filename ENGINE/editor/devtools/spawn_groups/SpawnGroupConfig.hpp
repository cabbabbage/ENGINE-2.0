#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include <deque>

#include <nlohmann/json.hpp>

#include "DockableCollapsible.hpp"

class Input;
class SpawnGroupLabelWidget;
class Assets;
namespace devmode::core { class ManifestStore; }

class SpawnGroupConfig : public DockableCollapsible {
    struct Entry;
public:
    using DockableCollapsible::open;

    enum class ChangeReason {
        Loaded,
        LoadFailed,
        EntryEdited,
        EntryAdded,
        EntryDeleted,
        EntryReordered,
        RegenerateRequested,
        CommitFailed
    };

    enum class PatchType {
        Add,
        Update,
        Delete,
        Reorder
    };

    struct ChangeSummary {
        bool method_changed = false;
        bool quantity_changed = false;
        bool candidates_changed = false;
        std::string method;
        bool resolution_changed = false;
        int resolution = 0;
    };

    struct PatchOperation {
        PatchType type = PatchType::Update;
        std::string spawn_id;
        nlohmann::json entry = nlohmann::json::object();
        size_t from_index = 0;
        size_t to_index = 0;
    };

    struct ChangeEvent {
        ChangeReason reason = ChangeReason::EntryEdited;
        std::string spawn_id;
        nlohmann::json entry = nlohmann::json::object();
        ChangeSummary summary{};
        std::vector<PatchOperation> patches{};
        std::string error_message;
    };

    struct Callbacks {
        std::function<bool(const std::vector<PatchOperation>&, std::string&)> on_commit;
        std::function<void(const ChangeEvent&)> on_change_event;
        std::function<void(const std::string&)> on_error;
        std::function<void(const std::string&, SDL_Point)> on_open_floating;
    };

    class EntryController {
    public:
        void set_ownership_label(const std::string& label, SDL_Color color);
        void clear_ownership_label();
        void set_area_names_provider(std::function<std::vector<std::string>()> provider);
        void lock_method_to(const std::string& method);
        void clear_method_lock();
        void set_quantity_hidden(bool hidden);

    private:
        explicit EntryController(Entry* entry) : entry_(entry) {}
        Entry* entry_ = nullptr;
        friend class SpawnGroupConfig;
    };

    using ConfigureEntryCallback = std::function<void(EntryController&, const nlohmann::json&)>;

    explicit SpawnGroupConfig(bool floatable = true);
    ~SpawnGroupConfig() override;

    void set_screen_dimensions(int width, int height);

    void load(const nlohmann::json& groups,
              const std::vector<std::string>& stable_ids,
              ConfigureEntryCallback configure_entry = {});

    void load(const nlohmann::json& groups);

    void append_rows(Rows& rows);
    void set_callbacks(Callbacks cb);
    void set_on_layout_changed(std::function<void()> cb);
    void refresh_row_configuration();

    void set_embedded_mode(bool embedded);

    void set_default_resolution(int resolution);
    void set_manifest_store(class devmode::core::ManifestStore* store);
    void set_assets(class Assets* assets);
    // Reset the panel to an empty draft model.
    void clear_binding();

    void expand_group(const std::string& id);
    void collapse_group(const std::string& id);
    bool is_expanded(const std::string& id) const;

    std::vector<std::string> expanded_groups() const;
    void restore_expanded_groups(const std::vector<std::string>& ids);

    nlohmann::json to_json() const;

    void update(const Input& input, int screen_w, int screen_h) override;
    bool handle_event(const SDL_Event& e) override;
    void render(SDL_Renderer* r) const override;
    void render_content(SDL_Renderer* r) const override;

    void open(const nlohmann::json& groups, std::function<void(const nlohmann::json&)> on_save);
    void request_open_spawn_group(const std::string& id, int x, int y);
    void set_anchor(int x, int y);
    void close_embedded_search();

    void load_impl(const nlohmann::json& source,
                   std::vector<std::string> stable_ids,
                   ConfigureEntryCallback configure_entry);
    void rebuild_rows();
    void apply_configuration(Entry& entry);
    void rebuild_layout();
    void mark_layout_dirty();
    DockableCollapsible::Rows build_layout_rows();
    const nlohmann::json* current_source() const;
    void enqueue_notification(std::function<void()> cb);
    void process_pending_notifications();
    bool commit_patches(const std::vector<PatchOperation>& patches,
                        const ChangeEvent& event,
                        std::string* error_message = nullptr);
    void emit_change_event(ChangeEvent event);
    bool single_entry_mode() const { return single_entry_mode_; }

private:
    struct DragState {
        bool active = false;
        size_t source_index = 0;
        size_t hover_index = 0;
        std::vector<std::string> original_order;
        std::vector<std::string> expansion_snapshot;
        std::vector<int> entry_heights;
        SDL_Rect placeholder_rect{0, 0, 0, 0};
        SDL_Rect source_rect{0, 0, 0, 0};
        int pointer_y = 0;
        bool pointer_inside = false;
    };

    bool default_floatable_mode_ = true;
    bool embedded_mode_ = false;
    bool layout_dirty_ = true;
    int screen_w_ = 1920;
    int screen_h_ = 1080;

    std::vector<std::unique_ptr<Entry>> entries_;
    nlohmann::json source_snapshot_{};
    nlohmann::json draft_groups_{};
    std::vector<std::string> draft_stable_ids_{};
    bool editable_mode_ = false;
    ConfigureEntryCallback configure_entry_{};
    Callbacks callbacks_{};
    std::function<void()> on_layout_change_{};

    std::unordered_set<std::string> expanded_{};
    SDL_Point anchor_{0, 0};
    std::optional<std::string> pending_focus_id_{};
    std::function<void(const nlohmann::json&)> pending_save_callback_{};
    int default_resolution_ = 0;
    bool single_entry_mode_ = false;

    bool suppress_layout_change_callback_ = false;
    std::unique_ptr<DMButton> add_button_{};
    std::unique_ptr<ButtonWidget> add_button_widget_{};
    std::unique_ptr<SpawnGroupLabelWidget> empty_state_label_{};

    std::deque<std::function<void()>> pending_notifications_{};
    bool processing_notifications_ = false;

    DragState drag_state_{};
    class devmode::core::ManifestStore* manifest_store_ = nullptr;
    class Assets* assets_ = nullptr;

    Entry* find_entry_by_id(const std::string& id);
    void begin_drag(size_t index, int pointer_y);
    void cancel_drag();
    void finalize_drag(bool commit);
    void update_drag_visuals(const Input& input);
    bool should_render_entry_body(const Entry& entry) const;
    SDL_Rect slot_rect_for_index(size_t index, int fallback_height) const;
    void reorder_json(size_t from, size_t to);
    void restore_order_from_snapshot(const std::vector<std::string>& order);
    void nudge_priority(Entry& entry, int delta);
    std::string stable_id_for_index(size_t index) const;
    void queue_add_spawn_group();
    void add_spawn_group_now();

    friend class SpawnGroupConfigTestAccessor;

protected:
    std::string_view lock_settings_namespace() const override;
    std::string_view lock_settings_id() const override;
};


