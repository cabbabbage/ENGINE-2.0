#include "map_assets_modals.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_set>

#include <SDL3_ttf/SDL_ttf.h>

#include "DockableCollapsible.hpp"
#include "DockManager.hpp"
#include "core/AssetsManager.hpp"
#include "dm_styles.hpp"
#include "tag_library.hpp"
#include "spawn_groups/spawn_group_utils.hpp"
#include "spawn_groups/widgets/CandidateEditorPieGraphWidget.hpp"
#include "utils/input.hpp"
#include "utils/grid.hpp"
#include "widgets.hpp"

using nlohmann::json;

namespace {

bool is_integral(double value) {
    if (!std::isfinite(value)) return false;
    const double rounded = std::round(value);
    return std::fabs(value - rounded) < 1e-9;
}

double read_candidate_weight(const json& candidate) {
    if (candidate.is_object()) {
        const auto weight_it = candidate.find("chance");
        if (weight_it != candidate.end()) {
            if (weight_it->is_number_float()) return weight_it->get<double>();
            if (weight_it->is_number_integer()) return static_cast<double>(weight_it->get<int>());
        }
        const auto alt_it = candidate.find("weight");
        if (alt_it != candidate.end()) {
            if (alt_it->is_number_float()) return alt_it->get<double>();
            if (alt_it->is_number_integer()) return static_cast<double>(alt_it->get<int>());
        }
    } else if (candidate.is_number_float()) {
        return candidate.get<double>();
    } else if (candidate.is_number_integer()) {
        return static_cast<double>(candidate.get<int>());
    }
    return 0.0;
}

constexpr std::string_view kNullCandidateName = "null";

std::string trim_copy(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

bool is_null_candidate_name(std::string_view name) {
    if (name.size() != kNullCandidateName.size()) {
        return false;
    }
    for (size_t i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != kNullCandidateName[i]) {
            return false;
        }
    }
    return true;
}

std::string read_string_field_no_throw(const json& obj,
                                       const char* key,
                                       std::string fallback = {}) {
    if (!obj.is_object() || key == nullptr) {
        return fallback;
    }
    auto it = obj.find(key);
    if (it == obj.end()) {
        return fallback;
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    return fallback;
}

std::string candidate_name_from_json(const json& candidate) {
    if (candidate.is_object()) {
        return read_string_field_no_throw(candidate, "name");
    }
    if (candidate.is_string()) {
        return candidate.get<std::string>();
    }
    return std::string{};
}

bool candidate_is_null_entry(const json& candidate) {
    return is_null_candidate_name(trim_copy(candidate_name_from_json(candidate)));
}

std::string canonical_candidate_identity(const json& candidate) {
    std::string name = trim_copy(candidate_name_from_json(candidate));
    if (name.empty()) {
        return std::string{};
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return name;
}

json make_candidate_entry_from_search_value(const std::string& raw_value, double weight) {
    json candidate = json::object();
    std::string value = trim_copy(raw_value);
    if (value.empty()) {
        value = std::string{kNullCandidateName};
    }

    std::string tag_name;
    bool is_tag = false;
    if (!value.empty() && value.front() == '#') {
        tag_name = trim_copy(std::string_view(value).substr(1));
        is_tag = !tag_name.empty();
    }

    if (is_tag) {
        const std::string display = "#" + tag_name;
        candidate["name"] = display;
        candidate["tag"] = true;
        candidate["tag_name"] = tag_name;
        candidate["display_name"] = display;
    } else {
        candidate["name"] = value;
    }

    if (is_integral(weight)) {
        candidate["chance"] = static_cast<int>(std::llround(weight));
    } else {
        candidate["chance"] = weight;
    }

    return candidate;
}

std::vector<SearchAssets::Result> build_candidate_search_extra_results() {
    std::vector<SearchAssets::Result> results;
    results.reserve(16);

    SearchAssets::Result null_res;
    null_res.label = "null";
    null_res.value = "null";
    null_res.is_tag = false;
    results.push_back(std::move(null_res));

    std::unordered_set<std::string> seen;
    for (const auto& tag : TagLibrary::instance().tags()) {
        std::string normalized = trim_copy(tag);
        if (normalized.empty()) {
            continue;
        }
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (!seen.insert(normalized).second) {
            continue;
        }

        SearchAssets::Result tag_res;
        tag_res.label = "#" + normalized;
        tag_res.value = normalized;
        tag_res.is_tag = true;
        tag_res.tags = {normalized};
        results.push_back(std::move(tag_res));
    }

    return results;
}

constexpr int kMapWideGridResolutionMin = 5;
constexpr int kMapWideGridResolutionMax = 10;
constexpr int kMapWidePositionJitterMin = 0;
constexpr int kMapWidePositionJitterMax = 500;

bool ensure_null_candidate_entry(json& entry) {
    return vibble::spawn_group_codec::sanitize_spawn_group_candidates(entry);
}

bool is_pointer_or_wheel_event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_WHEEL:
            return true;
        default:
            return false;
    }
}

bool is_text_or_key_event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            return true;
        default:
            return false;
    }
}

class LabelWidget : public Widget {
public:
    LabelWidget() = default;
    explicit LabelWidget(std::string text, SDL_Color color = DMStyles::Label().color, bool subtle = false)
        : text_(std::move(text)), color_(color), subtle_(subtle) {}

    void set_text(const std::string& text) { text_ = text; }
    void set_color(SDL_Color color) { color_ = color; }
    void set_subtle(bool subtle) { subtle_ = subtle; }

    void set_rect(const SDL_Rect& r) override { rect_ = r; }
    const SDL_Rect& rect() const override { return rect_; }
    int height_for_width(int) const override { return DMCheckbox::height(); }

    bool handle_event(const SDL_Event&) override { return false; }

    void render(SDL_Renderer* renderer) const override {
        if (!renderer) return;
        DMLabelStyle style = DMStyles::Label();
        SDL_Color color = subtle_ ? SDL_Color{static_cast<Uint8>(style.color.r / 2),
                                              static_cast<Uint8>(style.color.g / 2), static_cast<Uint8>(style.color.b / 2), style.color.a} : style.color;
        if (color_.a != 0) color = color_;
        TTF_Font* font = TTF_OpenFont(style.font_path.c_str(), style.font_size);
        if (!font) return;
        SDL_Surface* surface = ttf_util::RenderTextBlended(font, text_.c_str(), color);
        if (!surface) {
            TTF_CloseFont(font);
            return;
        }
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (texture) {
            SDL_Rect dst{rect_.x, rect_.y, surface->w, surface->h};
            sdl_render::Texture(renderer, texture, nullptr, &dst);
            SDL_DestroyTexture(texture);
        }
        SDL_DestroySurface(surface);
        TTF_CloseFont(font);
    }

private:
    std::string text_{};
    SDL_Color color_{0, 0, 0, 0};
    bool subtle_ = false;
    SDL_Rect rect_{0, 0, 0, 0};
};

class CallbackTextBoxWidget : public Widget {
public:
    CallbackTextBoxWidget(std::unique_ptr<DMTextBox> box,
                          std::function<void(const std::string&)> on_change,
                          bool full_row)
        : box_(std::move(box)), on_change_(std::move(on_change)), full_row_(full_row) {
        if (box_) {
            box_->set_on_height_changed([this]() { this->request_layout(); });
        }
    }

    ~CallbackTextBoxWidget() override {
        if (box_) {
            box_->set_on_height_changed(nullptr);
        }
    }

    void set_rect(const SDL_Rect& r) override {
        if (box_) box_->set_rect(r);
        rect_cache_ = r;
    }

    const SDL_Rect& rect() const override {
        if (box_) return box_->rect();
        return rect_cache_;
    }

    int height_for_width(int w) const override {
        return box_ ? box_->preferred_height(w) : DMTextBox::height();
    }

    bool handle_event(const SDL_Event& e) override {
        if (!box_) return false;
        std::string before = box_->value();
        bool used = box_->handle_event(e);
        if (used) {
            std::string after = box_->value();
            if (after != before && on_change_) {
                on_change_(after);
            }
        }
        return used;
    }

    void render(SDL_Renderer* renderer) const override {
        if (box_) box_->render(renderer);
    }

    bool wants_full_row() const override { return full_row_; }

    void set_value(const std::string& value) {
        if (box_) box_->set_value(value);
    }

private:
    std::unique_ptr<DMTextBox> box_{};
    std::function<void(const std::string&)> on_change_{};
    bool full_row_ = false;
    SDL_Rect rect_cache_{0, 0, 0, 0};
};

class CandidateListPanelImpl : public DockableCollapsible {
public:
    using SaveCallback = std::function<void()>;
    using RegenCallback = std::function<void(const json&)>;

    CandidateListPanelImpl() : DockableCollapsible("Spawn Group Candidates", true) {
        set_scroll_enabled(true);
        set_floating_content_width(520);
        set_cell_width(460);
        set_row_gap(10);
        set_col_gap(12);
        set_padding(12);
    }

    void set_screen_dimensions(int width, int height) {
        screen_w_ = std::max(width, 0);
        screen_h_ = std::max(height, 0);
        const int kMinVisibleHeight = 320;
        const int kHeightMargin = 200;
        int visible_height = kMinVisibleHeight;
        if (screen_h_ > 0) {
            visible_height = std::max(kMinVisibleHeight, screen_h_ - kHeightMargin);
        }
        set_visible_height(visible_height);
        set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
        if (pie_widget_) {
            pie_widget_->set_screen_dimensions(screen_w_, screen_h_);
        }
    }

    void set_manifest_store(devmode::core::ManifestStore* store) {
        manifest_store_ = store;
        if (pie_widget_) {
            pie_widget_->set_manifest_store(manifest_store_);
        }
    }

    void set_assets(Assets* assets) {
        assets_ = assets;
        if (pie_widget_) {
            pie_widget_->set_assets(assets_);
        }
    }

    void bind(json* entry,
              std::string default_display_name,
              std::string ownership_label,
              std::optional<SDL_Color> ownership_color,
              SaveCallback on_save,
              RegenCallback on_regen) {
        entry_ = entry;
        default_display_name_ = std::move(default_display_name);
        ownership_label_ = std::move(ownership_label);
        ownership_color_ = ownership_color;
        save_callback_ = std::move(on_save);
        regen_callback_ = std::move(on_regen);

        if (!ownership_label_.empty()) {
            if (!ownership_label_widget_) ownership_label_widget_ = std::make_unique<LabelWidget>();
            ownership_label_widget_->set_text(ownership_label_);
            if (ownership_color_) {
                ownership_label_widget_->set_color(*ownership_color_);
                ownership_label_widget_->set_subtle(false);
            } else {
                ownership_label_widget_->set_subtle(true);
            }
        }

        if (!display_name_widget_) display_name_widget_ = std::make_unique<LabelWidget>();
        if (!instructions_label_) {
            instructions_label_ = std::make_unique<LabelWidget>(
                "Adjust the grid resolution and candidate weights for the map-wide batch.", DMStyles::Label().color, true);
        }
        if (!candidate_set_header_) {
            candidate_set_header_ = std::make_unique<LabelWidget>();
        }
        if (!regen_button_) {
            regen_button_ = std::make_unique<DMButton>("Regenerate Map Assets",
                                                       &DMStyles::AccentButton(),
                                                       200,
                                                       DMButton::height());
            regen_button_widget_ =
                std::make_unique<ButtonWidget>(regen_button_.get(), [this]() { this->handle_regen(); });
        }
        if (!pie_widget_) {
            pie_widget_ = std::make_unique<CandidateEditorPieGraphWidget>();
        }
        pie_widget_->set_screen_dimensions(screen_w_, screen_h_);
        pie_widget_->set_manifest_store(manifest_store_);
        pie_widget_->set_assets(assets_);
        auto search_extras = std::make_shared<std::vector<SearchAssets::Result>>(build_candidate_search_extra_results());
        pie_widget_->set_search_extra_results_provider([search_extras]() { return *search_extras; });
        pie_widget_->set_on_request_layout([this]() { this->layout(); });
        // Regular modal writes directly to the section; apply immediately
        pie_widget_->set_defer_adjust_until_release(false);
        pie_widget_->set_on_adjust([this](int index, double delta) { adjust_candidate_weight(index, delta); });
        pie_widget_->set_on_delete([this](int index) { remove_candidate(index); });
        if (regen_callback_) {
            pie_widget_->set_on_regenerate([this]() { this->handle_regen(); });
        } else {
            pie_widget_->set_on_regenerate({});
        }
        pie_widget_->set_on_add_candidate([this](const std::string& value) { this->add_candidate_from_search(value); });

        const int resolution = current_grid_resolution();
        if (!grid_resolution_stepper_) {
            grid_resolution_stepper_ = std::make_unique<DMNumericStepper>("Grid Resolution (2^r px)",
                                                                          kMapWideGridResolutionMin,
                                                                          kMapWideGridResolutionMax,
                                                                          resolution);
            grid_resolution_stepper_->set_step(1);
            grid_resolution_stepper_->set_on_change([this](int value) { this->update_grid_resolution(value); });
            grid_resolution_widget_ = std::make_unique<StepperWidget>(grid_resolution_stepper_.get());
        } else {
            grid_resolution_stepper_->set_value(resolution);
        }

        const int jitter = current_position_jitter();
        if (!position_jitter_stepper_) {
            position_jitter_stepper_ = std::make_unique<DMNumericStepper>("Position Jitter (px)",
                                                                          kMapWidePositionJitterMin,
                                                                          kMapWidePositionJitterMax,
                                                                          jitter);
            position_jitter_stepper_->set_step(1);
            position_jitter_stepper_->set_on_change([this](int value) { this->update_position_jitter(value); });
            position_jitter_widget_ = std::make_unique<StepperWidget>(position_jitter_stepper_.get());
        } else {
            position_jitter_stepper_->set_value(jitter);
        }

        if (entry_) {
            (*entry_)["grid_resolution"] = resolution;
            (*entry_)["position_jitter_px"] = jitter;
        }

        if (!ownership_label_.empty()) {
            set_title(ownership_label_ + " Candidates");
        } else {
            set_title("Map-wide Candidates");
        }

        rebuild_rows(true);
    }

    void notify_save(bool force_rebuild) {
        if (!entry_) return;
        bool sanitized = sanitize_entry();
        if (save_callback_) save_callback_();
        if (force_rebuild || sanitized) {
            rebuild_rows(false);
        } else if (pie_widget_) {
            pie_widget_->set_candidates_from_json(*entry_);
        }
    }

    bool handle_event(const SDL_Event& e) override {
        if (route_embedded_search_event(e)) {
            return true;
        }

        return DockableCollapsible::handle_event(e);
    }

    void update(const Input& input, int screen_w, int screen_h) override {
        screen_w_ = std::max(screen_w, 0);
        screen_h_ = std::max(screen_h, 0);
        if (pie_widget_) {
            pie_widget_->set_screen_dimensions(screen_w_, screen_h_);
        }
        const SDL_Point pointer{input.getX(), input.getY()};
        const bool suppress_parent_scroll =
            pie_widget_ && pie_widget_->is_search_point_inside(pointer.x, pointer.y);
        if (suppress_parent_scroll) {
            set_scroll_enabled(false);
        }
        DockableCollapsible::update(input, screen_w, screen_h);
        if (suppress_parent_scroll) {
            set_scroll_enabled(true);
        }
        if (pie_widget_) {
            pie_widget_->update_search(input);
        }
    }

protected:
    std::string_view lock_settings_namespace() const override { return "map_assets"; }
    std::string_view lock_settings_id() const override { return "candidates"; }

private:
    bool route_embedded_search_event(const SDL_Event& e) {
        if (!pie_widget_ || !pie_widget_->is_search_visible()) {
            return false;
        }
        if (!is_pointer_or_wheel_event(e) && !is_text_or_key_event(e)) {
            return false;
        }
        return pie_widget_->handle_event(e);
    }

    bool sanitize_entry() {
        if (!entry_) return false;
        bool changed = devmode::spawn::ensure_spawn_group_entry_defaults(*entry_, default_display_name_);
        changed = devmode::spawn::sanitize_spawn_group_candidates(*entry_) || changed;
        return changed;
    }

    void rebuild_rows(bool ensure_sanitized) {
        if (!entry_) {
            set_rows({});
            return;
        }

        if (ensure_sanitized) sanitize_entry();

        if (pie_widget_) {
            pie_widget_->set_candidates_from_json(*entry_);
        }

        DockableCollapsible::Rows rows;

        if (ownership_label_widget_) {
            rows.push_back({ownership_label_widget_.get()});
        }

        const std::string display_name = read_string_field_no_throw(*entry_, "display_name", default_display_name_);
        if (display_name_widget_) {
            display_name_widget_->set_text("Spawn group: " + display_name);
            display_name_widget_->set_subtle(true);
            rows.push_back({display_name_widget_.get()});
        }

        if (instructions_label_) {
            instructions_label_->set_subtle(true);
            rows.push_back({instructions_label_.get()});
        }

        if (regen_button_widget_) {
            rows.push_back({regen_button_widget_.get()});
        }

        if (candidate_set_header_) {
            candidate_set_header_->set_text("Candidate Set 1");
            candidate_set_header_->set_subtle(false);
            rows.push_back({candidate_set_header_.get()});
        }

        if (grid_resolution_widget_ && grid_resolution_stepper_) {
            const int resolution = current_grid_resolution();
            grid_resolution_stepper_->set_value(resolution);
            rows.push_back({grid_resolution_widget_.get()});
        }

        if (position_jitter_widget_ && position_jitter_stepper_) {
            const int jitter = current_position_jitter();
            position_jitter_stepper_->set_value(jitter);
            rows.push_back({position_jitter_widget_.get()});
        }

        if (pie_widget_) {
            rows.push_back({pie_widget_.get()});
        }

        set_rows(rows);
    }

    void adjust_candidate_weight(int index, double delta) {
        if (!entry_ || std::abs(delta) < 1e-9) return;
        devmode::spawn::ensure_spawn_group_entry_defaults(*entry_, default_display_name_);
        auto& candidates = (*entry_)["candidates"];
        if (!candidates.is_array() || index < 0 || index >= static_cast<int>(candidates.size())) return;
        auto& candidate = candidates[index];
        if (!candidate.is_object()) {
            candidate = json::object();
        }
        double current = read_candidate_weight(candidate);
        double next = std::max(0.0, current + delta);
        if (is_integral(next)) {
            candidate["chance"] = static_cast<int>(std::llround(next));
        } else {
            candidate["chance"] = next;
        }
        notify_save(true);
    }

    void remove_candidate(int index) {
        if (!entry_ || index < 0) return;
        ensure_null_candidate_entry(*entry_);
        auto& candidates = (*entry_)["candidates"];
        if (!candidates.is_array() || index >= static_cast<int>(candidates.size())) return;
        if (vibble::spawn_group_codec::is_null_candidate_entry(
                candidates[static_cast<std::size_t>(index)])) {
            return;
        }
        auto it = candidates.begin() + static_cast<json::difference_type>(index);
        candidates.erase(it);
        ensure_null_candidate_entry(*entry_);
        notify_save(true);
    }

    void add_candidate_from_search(const std::string& label) {
        if (!entry_) return;
        if (label.empty()) return;
        auto& candidates = (*entry_)["candidates"];
        if (!candidates.is_array()) candidates = json::array();

        double max_weight = 0.0;
        for (const auto& candidate : candidates) {
            max_weight = std::max(max_weight, std::max(0.0, read_candidate_weight(candidate)));
        }

        double new_weight = max_weight > 0.0 ? max_weight * 0.05 : 5.0;
        if (new_weight <= 0.0) {
            new_weight = 5.0;
        }

        json candidate = make_candidate_entry_from_search_value(label, new_weight);
        const std::string new_identity = canonical_candidate_identity(candidate);
        if (!new_identity.empty()) {
            for (const auto& existing : candidates) {
                if (canonical_candidate_identity(existing) == new_identity) {
                    return;
                }
            }
        }

        candidates.push_back(std::move(candidate));
        notify_save(true);
    }

    void handle_regen() {
        if (!entry_) return;
        const bool sanitized = sanitize_entry();
        if (sanitized && pie_widget_) {
            pie_widget_->set_candidates_from_json(*entry_);
        }
        if (save_callback_) {
            save_callback_();
        }
        if (regen_callback_) {
            regen_callback_(*entry_);
        }
    }

    int current_grid_resolution() const {
        if (!entry_) return kMapWideGridResolutionMin;
        int value = vibble::spawn_group_codec::read_int_field(*entry_, "grid_resolution", kMapWideGridResolutionMin);
        return clamp_map_grid_resolution(value);
    }

    static int clamp_map_grid_resolution(int value) {
        int clamped = std::clamp(value, kMapWideGridResolutionMin, kMapWideGridResolutionMax);
        clamped = vibble::grid::clamp_resolution(clamped);
        return std::clamp(clamped, kMapWideGridResolutionMin, kMapWideGridResolutionMax);
    }

    void update_grid_resolution(int value) {
        if (!entry_) return;
        const int resolved = clamp_map_grid_resolution(value);
        (*entry_)["grid_resolution"] = resolved;
        notify_save(false);
    }

    json* entry_ = nullptr;
    std::string default_display_name_{};
    std::string ownership_label_{};
    std::optional<SDL_Color> ownership_color_{};
    SaveCallback save_callback_{};
    RegenCallback regen_callback_{};

    int screen_w_ = 1920;
    int screen_h_ = 1080;
    devmode::core::ManifestStore* manifest_store_ = nullptr;
    Assets* assets_ = nullptr;

    std::unique_ptr<LabelWidget> ownership_label_widget_{};
    std::unique_ptr<LabelWidget> display_name_widget_{};
    std::unique_ptr<LabelWidget> instructions_label_{};
    std::unique_ptr<LabelWidget> candidate_set_header_{};
    std::unique_ptr<CandidateEditorPieGraphWidget> pie_widget_{};
    std::unique_ptr<DMButton> regen_button_{};
    std::unique_ptr<ButtonWidget> regen_button_widget_{};
    std::unique_ptr<DMNumericStepper> grid_resolution_stepper_{};
    std::unique_ptr<StepperWidget> grid_resolution_widget_{};
    std::unique_ptr<DMNumericStepper> position_jitter_stepper_{};
    std::unique_ptr<StepperWidget> position_jitter_widget_{};

    int current_position_jitter() const {
        if (!entry_) return kMapWidePositionJitterMin;
        int value = vibble::spawn_group_codec::read_int_field(*entry_, "position_jitter_px", kMapWidePositionJitterMin);
        return std::clamp(value, kMapWidePositionJitterMin, kMapWidePositionJitterMax);
    }

    void update_position_jitter(int value) {
        if (!entry_) return;
        const int clamped = std::clamp(value, kMapWidePositionJitterMin, kMapWidePositionJitterMax);
        (*entry_)["position_jitter_px"] = clamped;
        notify_save(false);
    }
};

class BoundaryCandidateListPanelImpl : public DockableCollapsible {
public:
    using SaveCallback = std::function<void()>;
    using RegenCallback = std::function<void(const json&)>;

    BoundaryCandidateListPanelImpl() : DockableCollapsible("Boundary Candidates", true) {
        set_scroll_enabled(true);
        set_floating_content_width(520);
        set_cell_width(460);
        set_row_gap(10);
        set_col_gap(12);
        set_padding(12);
    }

    void set_screen_dimensions(int width, int height) {
        screen_w_ = std::max(width, 0);
        screen_h_ = std::max(height, 0);
        const int kMinVisibleHeight = 320;
        const int kHeightMargin = 200;
        int visible_height = kMinVisibleHeight;
        if (screen_h_ > 0) {
            visible_height = std::max(kMinVisibleHeight, screen_h_ - kHeightMargin);
        }
        set_visible_height(visible_height);
        set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
        for (auto& group : group_widgets_) {
            if (group.pie_widget) {
                group.pie_widget->set_screen_dimensions(screen_w_, screen_h_);
            }
        }
    }

    void set_manifest_store(devmode::core::ManifestStore* store) {
        manifest_store_ = store;
        for (auto& group : group_widgets_) {
            if (group.pie_widget) {
                group.pie_widget->set_manifest_store(manifest_store_);
            }
        }
    }

    void set_assets(Assets* assets) {
        assets_ = assets;
        for (auto& group : group_widgets_) {
            if (group.pie_widget) {
                group.pie_widget->set_assets(assets_);
            }
        }
    }

    void bind(json* section,
              std::string default_display_name,
              std::string ownership_label,
              std::optional<SDL_Color> ownership_color,
              SaveCallback on_save,
              RegenCallback on_regen) {
        section_ = section;
        default_display_name_ = std::move(default_display_name);
        ownership_label_ = std::move(ownership_label);
        ownership_color_ = ownership_color;
        save_callback_ = std::move(on_save);
        regen_callback_ = std::move(on_regen);
        pending_rebuild_ = false;
        pending_sync_ = false;
        pending_sync_spawn_id_.reset();
        pending_remove_spawn_id_.reset();
        pie_callback_depth_ = 0;

        if (!ownership_label_.empty()) {
            if (!ownership_label_widget_) ownership_label_widget_ = std::make_unique<LabelWidget>();
            ownership_label_widget_->set_text(ownership_label_);
            if (ownership_color_) {
                ownership_label_widget_->set_color(*ownership_color_);
                ownership_label_widget_->set_subtle(false);
            } else {
                ownership_label_widget_->set_subtle(true);
            }
        }

        if (!instructions_label_) {
            instructions_label_ = std::make_unique<LabelWidget>(
                "Each candidate set requires a unique grid resolution.", DMStyles::Label().color, true);
        }

        if (!regen_button_) {
            regen_button_ = std::make_unique<DMButton>("Regenerate Boundaries",
                                                       &DMStyles::AccentButton(),
                                                       180,
                                                       DMButton::height());
            regen_button_widget_ =
                std::make_unique<ButtonWidget>(regen_button_.get(), [this]() { this->handle_global_regen(); });
        }

        if (!add_button_) {
            add_button_ = std::make_unique<DMButton>("Add New", &DMStyles::CreateButton(), 140, DMButton::height());
            add_button_widget_ = std::make_unique<ButtonWidget>(add_button_.get(), [this]() { this->add_group(); });
        }

        if (!ownership_label_.empty()) {
            set_title(ownership_label_ + " Candidates");
        } else {
            set_title("Boundary Candidates");
        }

        rebuild_rows(true);
    }

    void notify_save(bool force_rebuild, const std::string* changed_spawn_id = nullptr) {
        if (!section_) return;
        bool sanitized = sanitize_groups();
        if (save_callback_) save_callback_();
        const bool needs_rebuild = force_rebuild || sanitized;
        if (pie_callback_depth_ > 0) {
            pending_rebuild_ = pending_rebuild_ || needs_rebuild;
            if (!needs_rebuild) {
                pending_sync_ = true;
                if (changed_spawn_id && !changed_spawn_id->empty()) {
                    if (!pending_sync_spawn_id_) {
                        pending_sync_spawn_id_ = *changed_spawn_id;
                    } else if (*pending_sync_spawn_id_ != *changed_spawn_id) {
                        pending_sync_spawn_id_.reset();
                    }
                } else {
                    pending_sync_spawn_id_.reset();
                }
            } else {
                pending_sync_spawn_id_.reset();
            }
            return;
        }
        if (needs_rebuild) {
            pending_sync_spawn_id_.reset();
            rebuild_rows(false);
        } else {
            if (changed_spawn_id && !changed_spawn_id->empty()) {
                sync_group_widget(*changed_spawn_id);
            } else {
                sync_group_widgets();
            }
        }
    }

    bool handle_event(const SDL_Event& e) override {
        if (route_embedded_search_event(e)) {
            return true;
        }
        return DockableCollapsible::handle_event(e);
    }

    void update(const Input& input, int screen_w, int screen_h) override {
        screen_w_ = std::max(screen_w, 0);
        screen_h_ = std::max(screen_h, 0);
        for (auto& group : group_widgets_) {
            if (group.pie_widget) {
                group.pie_widget->set_screen_dimensions(screen_w_, screen_h_);
            }
        }
        const SDL_Point pointer{input.getX(), input.getY()};
        bool suppress_parent_scroll = false;
        for (const auto& group : group_widgets_) {
            if (!group.pie_widget) continue;
            if (group.pie_widget->is_search_point_inside(pointer.x, pointer.y)) {
                suppress_parent_scroll = true;
                break;
            }
        }
        if (suppress_parent_scroll) {
            set_scroll_enabled(false);
        }
        DockableCollapsible::update(input, screen_w, screen_h);
        if (suppress_parent_scroll) {
            set_scroll_enabled(true);
        }
        for (auto& group : group_widgets_) {
            if (group.pie_widget) {
                group.pie_widget->update_search(input);
            }
        }
        apply_pending_refresh();
    }

protected:
    std::string_view lock_settings_namespace() const override { return "map_assets"; }
    std::string_view lock_settings_id() const override { return "boundary_candidates"; }

private:
    static constexpr int kMaxBoundaryJitter = 500;
    static constexpr int kMaxSpawnFromRoomMin = 0;
    static constexpr int kMaxSpawnFromRoomMax = 2000;
    static constexpr int kMaxSpawnFromRoomDefault = 128;

    struct GroupWidgets {
        std::string spawn_id{};
        std::unique_ptr<LabelWidget> header{};
        std::unique_ptr<DMNumericStepper> resolution_stepper{};
        std::unique_ptr<StepperWidget> resolution_widget{};
        std::unique_ptr<DMNumericStepper> jitter_stepper{};
        std::unique_ptr<StepperWidget> jitter_widget{};
        std::unique_ptr<DMButton> remove_button{};
        std::unique_ptr<ButtonWidget> remove_button_widget{};
        std::unique_ptr<CandidateEditorPieGraphWidget> pie_widget{};
    };

    struct PieCallbackGuard {
        explicit PieCallbackGuard(BoundaryCandidateListPanelImpl* owner) : owner_(owner) {
            ++owner_->pie_callback_depth_;
        }
        ~PieCallbackGuard() { owner_->pie_callback_depth_ = std::max(0, owner_->pie_callback_depth_ - 1); }
        BoundaryCandidateListPanelImpl* owner_;
    };

    CandidateEditorPieGraphWidget* active_search_widget() {
        for (auto& group : group_widgets_) {
            if (group.pie_widget && group.pie_widget->is_search_visible()) {
                return group.pie_widget.get();
            }
        }
        return nullptr;
    }

    bool route_embedded_search_event(const SDL_Event& e) {
        CandidateEditorPieGraphWidget* widget = active_search_widget();
        if (!widget) {
            return false;
        }
        if (!is_pointer_or_wheel_event(e) && !is_text_or_key_event(e)) {
            return false;
        }
        return widget->handle_event(e);
    }

    json& ensure_candidate_selectors() {
        if (!section_) {
            static json empty = json::array();
            return empty;
        }
        if (!section_->is_object()) {
            *section_ = json::object();
        }
        auto it = section_->find("boundary_area_selectors");
        if (it == section_->end() || !it->is_array()) {
            (*section_)["boundary_area_selectors"] = json::array();
        }
        return (*section_)["boundary_area_selectors"];
    }

    int current_max_spawn_from_room() const {
        if (!section_ || !section_->is_object()) {
            return kMaxSpawnFromRoomDefault;
        }
        auto it = section_->find("max_spawn_from_room");
        if (it == section_->end() || !it->is_number()) {
            return kMaxSpawnFromRoomDefault;
        }
        int value = kMaxSpawnFromRoomDefault;
        try {
            if (it->is_number_integer()) {
                value = it->get<int>();
            } else {
                value = static_cast<int>(std::lround(it->get<double>()));
            }
        } catch (...) {
            value = kMaxSpawnFromRoomDefault;
        }
        return std::clamp(value, kMaxSpawnFromRoomMin, kMaxSpawnFromRoomMax);
    }

    void set_max_spawn_from_room(int value) {
        if (!section_) return;
        if (!section_->is_object()) {
            *section_ = json::object();
        }
        const int clamped = std::clamp(value, kMaxSpawnFromRoomMin, kMaxSpawnFromRoomMax);
        (*section_)["max_spawn_from_room"] = clamped;
        notify_save(false);
        if (assets_) {
            assets_->notify_dynamic_spawn_distance_changed();
        }
    }

    static int clamp_jitter(int value) {
        if (!std::isfinite(static_cast<double>(value))) return 0;
        return std::clamp(value, 0, kMaxBoundaryJitter);
    }

    json* find_group_by_spawn_id(const std::string& spawn_id) {
        if (!section_) return nullptr;
        auto& selectors = ensure_candidate_selectors();
        if (!selectors.is_array()) return nullptr;
        for (auto& entry : selectors) {
            if (!entry.is_object()) continue;
            if (read_string_field_no_throw(entry, "spawn_id") == spawn_id) {
                return &entry;
            }
        }
        return nullptr;
    }

    int resolve_unique_resolution(int desired, const std::unordered_set<int>& used, int current) const {
        const int clamped = vibble::grid::clamp_resolution(desired);
        if (used.find(clamped) == used.end()) return clamped;
        const bool prefer_down = clamped < current;
        for (int offset = 1; offset <= vibble::grid::kMaxResolution; ++offset) {
            int up = clamped + offset;
            int down = clamped - offset;
            if (prefer_down) {
                if (down >= 0 && used.find(down) == used.end()) return down;
                if (up <= vibble::grid::kMaxResolution && used.find(up) == used.end()) return up;
            } else {
                if (up <= vibble::grid::kMaxResolution && used.find(up) == used.end()) return up;
                if (down >= 0 && used.find(down) == used.end()) return down;
            }
        }
        return clamped;
    }

    bool sanitize_groups() {
        if (!section_) return false;
        bool changed = false;
        if (!section_->is_object()) {
            *section_ = json::object();
            changed = true;
        }
        const int max_spawn_from_room = current_max_spawn_from_room();
        if (!section_->contains("max_spawn_from_room") ||
            !(*section_)["max_spawn_from_room"].is_number_integer() ||
            (*section_)["max_spawn_from_room"].get<int>() != max_spawn_from_room) {
            (*section_)["max_spawn_from_room"] = max_spawn_from_room;
            changed = true;
        }
        auto& selectors = ensure_candidate_selectors();
        if (!selectors.is_array()) {
            selectors = json::array();
            changed = true;
        }
        if (selectors.empty()) {
            json entry = json::object();
            devmode::spawn::ensure_spawn_group_entry_defaults(entry, default_display_name_);
            entry["grid_resolution"] = vibble::grid::clamp_resolution(5);
            entry["jitter"] = 0;
            selectors.push_back(std::move(entry));
            changed = true;
        }

        for (auto& entry : selectors) {
            if (!entry.is_object()) {
                entry = json::object();
                changed = true;
            }
            changed = devmode::spawn::ensure_spawn_group_entry_defaults(entry, default_display_name_) || changed;
            if (ensure_null_candidate_entry(entry)) changed = true;
            int grid_resolution = vibble::spawn_group_codec::read_int_field(entry, "grid_resolution", 5);
            grid_resolution = vibble::grid::clamp_resolution(grid_resolution);
            if (!entry.contains("grid_resolution") || !entry["grid_resolution"].is_number_integer() ||
                entry["grid_resolution"].get<int>() != grid_resolution) {
                entry["grid_resolution"] = grid_resolution;
                changed = true;
            }

            int jitter = clamp_jitter(vibble::spawn_group_codec::read_int_field(entry, "jitter", 0));
            if (!entry.contains("jitter") || !entry["jitter"].is_number_integer() ||
                entry["jitter"].get<int>() != jitter) {
                entry["jitter"] = jitter;
                changed = true;
            }
        }

        std::unordered_set<int> used;
        for (auto& entry : selectors) {
            if (!entry.is_object()) continue;
            int grid_resolution = vibble::spawn_group_codec::read_int_field(entry, "grid_resolution", 5);
            int unique = resolve_unique_resolution(grid_resolution, used, grid_resolution);
            if (unique != grid_resolution) {
                entry["grid_resolution"] = unique;
                changed = true;
            }
            used.insert(unique);
        }

        return changed;
    }

    void rebuild_rows(bool ensure_sanitized) {
        if (!section_) {
            set_rows({});
            return;
        }

        if (ensure_sanitized) sanitize_groups();

        auto& selectors = ensure_candidate_selectors();
        if (!selectors.is_array()) {
            set_rows({});
            return;
        }

        group_widgets_.clear();

        DockableCollapsible::Rows rows;

        if (ownership_label_widget_) {
            rows.push_back({ownership_label_widget_.get()});
        }

        if (instructions_label_) {
            instructions_label_->set_subtle(true);
            rows.push_back({instructions_label_.get()});
        }

        if (regen_button_widget_) {
            rows.push_back({regen_button_widget_.get()});
        }

        if (add_button_widget_) {
            rows.push_back({add_button_widget_.get()});
        }

        const int max_spawn_from_room = current_max_spawn_from_room();
        if (!max_spawn_from_room_stepper_) {
            max_spawn_from_room_stepper_ = std::make_unique<DMNumericStepper>(
                "max_spawn_from_room",
                kMaxSpawnFromRoomMin,
                kMaxSpawnFromRoomMax,
                max_spawn_from_room);
            max_spawn_from_room_stepper_->set_step(1);
            max_spawn_from_room_stepper_->set_on_change([this](int value) {
                this->set_max_spawn_from_room(value);
            });
            max_spawn_from_room_widget_ =
                std::make_unique<StepperWidget>(max_spawn_from_room_stepper_.get());
        }
        if (max_spawn_from_room_stepper_) {
            max_spawn_from_room_stepper_->set_value(max_spawn_from_room);
        }
        if (max_spawn_from_room_widget_) {
            rows.push_back({max_spawn_from_room_widget_.get()});
        }

        auto search_extras = std::make_shared<std::vector<SearchAssets::Result>>(build_candidate_search_extra_results());
        int index = 0;
        for (auto& entry : selectors) {
            if (!entry.is_object()) {
                ++index;
                continue;
            }

            GroupWidgets group;
            group.spawn_id = read_string_field_no_throw(entry, "spawn_id");

            group.header = std::make_unique<LabelWidget>();
            group.header->set_text("Candidate Set " + std::to_string(index + 1));
            group.header->set_subtle(false);
            rows.push_back({group.header.get()});

            const int grid_resolution = vibble::spawn_group_codec::read_int_field(entry, "grid_resolution", 5);
            group.resolution_stepper = std::make_unique<DMNumericStepper>("Grid Resolution (3^r spacing)",
                                                                          0,
                                                                          vibble::grid::kMaxResolution,
                                                                          grid_resolution);
            group.resolution_stepper->set_step(1);
            group.resolution_stepper->set_on_change([this, spawn_id = group.spawn_id](int value) {
                this->update_group_resolution(spawn_id, value);
            });
            group.resolution_widget = std::make_unique<StepperWidget>(group.resolution_stepper.get());

            const int jitter = clamp_jitter(vibble::spawn_group_codec::read_int_field(entry, "jitter", 0));
            group.jitter_stepper = std::make_unique<DMNumericStepper>("Jitter (px)",
                                                                      0,
                                                                      kMaxBoundaryJitter,
                                                                      jitter);
            group.jitter_stepper->set_step(1);
            group.jitter_stepper->set_on_change([this, spawn_id = group.spawn_id](int value) {
                this->update_group_jitter(spawn_id, value);
            });
            group.jitter_widget = std::make_unique<StepperWidget>(group.jitter_stepper.get());

            group.remove_button = std::make_unique<DMButton>("Remove", &DMStyles::WarnButton(), 90, DMButton::height());
            group.remove_button_widget = std::make_unique<ButtonWidget>(
                group.remove_button.get(),
                [this, spawn_id = group.spawn_id]() { this->queue_remove_group(spawn_id); });

            rows.push_back({group.resolution_widget.get(), group.remove_button_widget.get()});
            rows.push_back({group.jitter_widget.get()});

            group.pie_widget = std::make_unique<CandidateEditorPieGraphWidget>();
            group.pie_widget->set_screen_dimensions(screen_w_, screen_h_);
            group.pie_widget->set_manifest_store(manifest_store_);
            group.pie_widget->set_assets(assets_);
            group.pie_widget->set_search_extra_results_provider([search_extras]() { return *search_extras; });
            group.pie_widget->set_on_request_layout([this]() { this->layout(); });
            // Boundary edits update JSON only; the panel rebuild button applies catalog changes.
            group.pie_widget->set_defer_adjust_until_release(true);
            group.pie_widget->set_on_adjust([this, spawn_id = group.spawn_id](int idx, double delta) {
                this->adjust_candidate_weight(spawn_id, idx, delta);
            });
            group.pie_widget->set_on_delete([this, spawn_id = group.spawn_id](int idx) {
                this->remove_candidate(spawn_id, idx);
            });
            group.pie_widget->set_on_add_candidate([this, spawn_id = group.spawn_id](const std::string& value) {
                this->add_candidate_from_search(spawn_id, value);
            });
            // Single regen button at panel level; keep per-group regen disabled.
            group.pie_widget->set_on_regenerate({});
            ensure_null_candidate_entry(entry);
            group.pie_widget->set_candidates_from_json(entry);
            rows.push_back({group.pie_widget.get()});

            group_widgets_.push_back(std::move(group));
            ++index;
        }

        if (selectors.empty()) {
            add_group();
            return;
        }

        set_rows(rows);
    }

    void sync_group_widgets() {
        if (!section_) return;
        auto& selectors = ensure_candidate_selectors();
        if (!selectors.is_array()) return;
        for (auto& group : group_widgets_) {
            if (!group.pie_widget) continue;
            json* entry = find_group_by_spawn_id(group.spawn_id);
            if (!entry) continue;
            ensure_null_candidate_entry(*entry);
            group.pie_widget->set_candidates_from_json(*entry);
        }
    }

    void sync_group_widget(const std::string& spawn_id) {
        if (spawn_id.empty()) {
            sync_group_widgets();
            return;
        }
        json* entry = find_group_by_spawn_id(spawn_id);
        if (!entry) {
            return;
        }
        ensure_null_candidate_entry(*entry);
        const int resolved_resolution =
            vibble::spawn_group_codec::read_int_field(*entry, "grid_resolution", 5);
        const int resolved_jitter =
            clamp_jitter(vibble::spawn_group_codec::read_int_field(*entry, "jitter", 0));
        for (auto& group : group_widgets_) {
            if (group.spawn_id != spawn_id) {
                continue;
            }
            if (group.pie_widget) {
                group.pie_widget->set_candidates_from_json(*entry);
            }
            if (group.resolution_stepper) {
                group.resolution_stepper->set_value(resolved_resolution);
            }
            if (group.jitter_stepper) {
                group.jitter_stepper->set_value(resolved_jitter);
            }
            break;
        }
    }

    void add_group() {
        if (!section_) return;
        auto& selectors = ensure_candidate_selectors();
        json entry = json::object();
        devmode::spawn::ensure_spawn_group_entry_defaults(entry, default_display_name_);
        ensure_null_candidate_entry(entry);

        std::unordered_set<int> used;
        for (const auto& existing : selectors) {
            if (!existing.is_object()) continue;
            used.insert(vibble::spawn_group_codec::read_int_field(existing, "grid_resolution", 5));
        }
        const int desired_resolution = vibble::spawn_group_codec::read_int_field(entry, "grid_resolution", 5);
        int resolution = resolve_unique_resolution(desired_resolution, used, desired_resolution);
        entry["grid_resolution"] = resolution;
        entry["jitter"] = 0;

        selectors.push_back(std::move(entry));
        notify_save(true);
    }

    void remove_group(const std::string& spawn_id) {
        if (!section_) return;
        auto& selectors = ensure_candidate_selectors();
        if (!selectors.is_array() || selectors.size() <= 1) {
            return;
        }
        auto it = std::remove_if(selectors.begin(), selectors.end(),
            [&](const json& entry) {
                return entry.is_object() && read_string_field_no_throw(entry, "spawn_id") == spawn_id;
            });
        if (it != selectors.end()) {
            selectors.erase(it, selectors.end());
            notify_save(true);
        }
    }

    void queue_remove_group(const std::string& spawn_id) {
        if (spawn_id.empty()) {
            return;
        }
        pending_remove_spawn_id_ = spawn_id;
    }

    void update_group_resolution(const std::string& spawn_id, int parsed_value) {
        json* entry = find_group_by_spawn_id(spawn_id);
        if (!entry) return;
        int current = vibble::spawn_group_codec::read_int_field(*entry, "grid_resolution", 5);
        int desired = vibble::grid::clamp_resolution(parsed_value);

        std::unordered_set<int> used;
        auto& selectors = ensure_candidate_selectors();
        for (const auto& other : selectors) {
            if (!other.is_object()) continue;
            if (read_string_field_no_throw(other, "spawn_id") == spawn_id) continue;
            used.insert(vibble::spawn_group_codec::read_int_field(other, "grid_resolution", 5));
        }

        int resolved = resolve_unique_resolution(desired, used, current);
        (*entry)["grid_resolution"] = resolved;
        notify_save(false, &spawn_id);

        for (auto& group : group_widgets_) {
            if (group.spawn_id == spawn_id && group.resolution_stepper) {
                group.resolution_stepper->set_value(resolved);
                break;
            }
        }
    }

    void update_group_jitter(const std::string& spawn_id, int parsed_value) {
        json* entry = find_group_by_spawn_id(spawn_id);
        if (!entry) return;
        const int clamped = clamp_jitter(parsed_value);
        (*entry)["jitter"] = clamped;
        notify_save(false, &spawn_id);

        for (auto& group : group_widgets_) {
            if (group.spawn_id == spawn_id && group.jitter_stepper) {
                group.jitter_stepper->set_value(clamped);
                break;
            }
        }
    }

    void adjust_candidate_weight(const std::string& spawn_id, int index, double delta) {
        PieCallbackGuard guard(this);
        if (std::abs(delta) < 1e-9) return;
        json* entry = find_group_by_spawn_id(spawn_id);
        if (!entry) return;
        devmode::spawn::ensure_spawn_group_entry_defaults(*entry, default_display_name_);
        auto& candidates = (*entry)["candidates"];
        if (!candidates.is_array() || index < 0 || index >= static_cast<int>(candidates.size())) return;
        auto& candidate = candidates[index];
        if (!candidate.is_object()) {
            candidate = json::object();
        }
        double current = read_candidate_weight(candidate);
        double next = std::max(0.0, current + delta);
        if (is_integral(next)) {
            candidate["chance"] = static_cast<int>(std::llround(next));
        } else {
            candidate["chance"] = next;
        }
        notify_save(false, &spawn_id);
    }

    void remove_candidate(const std::string& spawn_id, int index) {
        PieCallbackGuard guard(this);
        json* entry = find_group_by_spawn_id(spawn_id);
        if (!entry || index < 0) return;
        ensure_null_candidate_entry(*entry);
        auto& candidates = (*entry)["candidates"];
        if (!candidates.is_array() || index >= static_cast<int>(candidates.size())) return;
        if (vibble::spawn_group_codec::is_null_candidate_entry(
                candidates[static_cast<std::size_t>(index)])) {
            return;
        }
        auto it = candidates.begin() + static_cast<json::difference_type>(index);
        candidates.erase(it);
        ensure_null_candidate_entry(*entry);
        notify_save(false, &spawn_id);
    }

    void add_candidate_from_search(const std::string& spawn_id, const std::string& label) {
        PieCallbackGuard guard(this);
        if (label.empty()) return;
        json* entry = find_group_by_spawn_id(spawn_id);
        if (!entry) return;
        auto& candidates = (*entry)["candidates"];
        if (!candidates.is_array()) candidates = json::array();

        double max_weight = 0.0;
        for (const auto& candidate : candidates) {
            max_weight = std::max(max_weight, std::max(0.0, read_candidate_weight(candidate)));
        }

        double new_weight = max_weight > 0.0 ? max_weight * 0.05 : 5.0;
        if (new_weight <= 0.0) {
            new_weight = 5.0;
        }

        json candidate = make_candidate_entry_from_search_value(label, new_weight);
        const std::string new_identity = canonical_candidate_identity(candidate);
        if (!new_identity.empty()) {
            for (const auto& existing : candidates) {
                if (canonical_candidate_identity(existing) == new_identity) {
                    return;
                }
            }
        }

        candidates.push_back(std::move(candidate));
        ensure_null_candidate_entry(*entry);
        notify_save(false, &spawn_id);
    }

    void handle_global_regen() {
        if (!section_) return;
        const bool sanitized = sanitize_groups();
        if (sanitized) {
            rebuild_rows(false);
        } else {
            sync_group_widgets();
        }
        int current_seed = 0;
        try {
            current_seed = vibble::spawn_group_codec::read_int_field(*section_, "regen_seed", 0);
        } catch (...) {
            current_seed = 0;
        }
        (*section_)["regen_seed"] = std::max(0, current_seed) + 1;
        if (save_callback_) {
            save_callback_();
        }
        if (regen_callback_) {
            regen_callback_(*section_);
        }
    }

    void handle_regen(const std::string& spawn_id) {
        json* entry = find_group_by_spawn_id(spawn_id);
        if (!entry) return;
        const bool sanitized = devmode::spawn::sanitize_spawn_group_candidates(*entry);
        if (sanitized) {
            sync_group_widgets();
        }
        if (save_callback_) {
            save_callback_();
        }
        if (regen_callback_) {
            regen_callback_(*entry);
        }
    }

    json* section_ = nullptr;
    std::string default_display_name_{};
    std::string ownership_label_{};
    std::optional<SDL_Color> ownership_color_{};
    SaveCallback save_callback_{};
    RegenCallback regen_callback_{};

    int screen_w_ = 1920;
    int screen_h_ = 1080;
    devmode::core::ManifestStore* manifest_store_ = nullptr;
    Assets* assets_ = nullptr;

    std::unique_ptr<LabelWidget> ownership_label_widget_{};
    std::unique_ptr<LabelWidget> instructions_label_{};
    std::unique_ptr<DMButton> regen_button_{};
    std::unique_ptr<ButtonWidget> regen_button_widget_{};
    std::unique_ptr<DMButton> add_button_{};
    std::unique_ptr<ButtonWidget> add_button_widget_{};
    std::unique_ptr<DMNumericStepper> max_spawn_from_room_stepper_{};
    std::unique_ptr<StepperWidget> max_spawn_from_room_widget_{};
    std::vector<GroupWidgets> group_widgets_{};
    int pie_callback_depth_ = 0;
    bool pending_rebuild_ = false;
    bool pending_sync_ = false;
    std::optional<std::string> pending_sync_spawn_id_{};
    std::optional<std::string> pending_remove_spawn_id_{};

    void apply_pending_refresh() {
        if (pending_remove_spawn_id_ && !pending_remove_spawn_id_->empty()) {
            const std::string spawn_id = *pending_remove_spawn_id_;
            pending_remove_spawn_id_.reset();
            remove_group(spawn_id);
            return;
        }
        if (pending_rebuild_) {
            pending_rebuild_ = false;
            pending_sync_ = false;
            pending_sync_spawn_id_.reset();
            rebuild_rows(false);
            return;
        }
        if (pending_sync_) {
            pending_sync_ = false;
            if (pending_sync_spawn_id_ && !pending_sync_spawn_id_->empty()) {
                sync_group_widget(*pending_sync_spawn_id_);
            } else {
                sync_group_widgets();
            }
            pending_sync_spawn_id_.reset();
        }
    }
};

}

class CandidateListPanel : public CandidateListPanelImpl {
public:
    using CandidateListPanelImpl::CandidateListPanelImpl;
};

class BoundaryCandidateListPanel : public BoundaryCandidateListPanelImpl {
public:
    using BoundaryCandidateListPanelImpl::BoundaryCandidateListPanelImpl;
};

SingleSpawnGroupModal::SingleSpawnGroupModal() = default;
SingleSpawnGroupModal::~SingleSpawnGroupModal() = default;

void SingleSpawnGroupModal::set_on_close(std::function<void()> cb) {
    on_close_ = std::move(cb);
    if (panel_) {
        panel_->set_on_close([this]() {
            if (on_close_) on_close_();
        });
    }
}

void SingleSpawnGroupModal::ensure_single_group(json& section,
                                                const std::string& default_display_name) {
    if (!section.is_object()) {
        section = json::object();
    }
    auto& groups = devmode::spawn::ensure_spawn_groups_array(section);
    if (groups.empty()) {
        json entry = json::object();
        devmode::spawn::ensure_spawn_group_entry_defaults(entry, default_display_name);
        groups.push_back(std::move(entry));
    } else {
        devmode::spawn::ensure_spawn_group_entry_defaults(groups[0], default_display_name);
        if (groups.size() > 1) {
            json first = groups[0];
            groups = json::array();
            groups.push_back(std::move(first));
        }
    }
}

void SingleSpawnGroupModal::open(json& map_info,
                                 const std::string& section_key,
                                 const std::string& default_display_name,
                                 const std::string& ownership_label,
                                 SDL_Color ownership_color,
                                 SaveCallback on_save,
                                 RegenCallback on_regen) {
    map_info_ = &map_info;
    on_save_ = std::move(on_save);
    on_regen_ = std::move(on_regen);
    section_ = &(*map_info_)[section_key];
    ensure_single_group(*section_, default_display_name);

    auto& groups = (*section_)["spawn_groups"];
    entry_ = &groups.front();

    if (!panel_) panel_ = std::make_unique<CandidateListPanel>();
    panel_->set_screen_dimensions(screen_w_, screen_h_);
    panel_->set_manifest_store(manifest_store_);
    panel_->set_assets(assets_);
    panel_->bind(entry_,
                 default_display_name,
                 ownership_label,
                 ownership_label.empty() ? std::optional<SDL_Color>{} : std::optional<SDL_Color>{ownership_color},
                 [this]() {
                     if (on_save_) on_save_();
                 },
                 [this](const json& updated_entry) {
                     if (on_regen_) on_regen_(updated_entry);
                 });

    if (panel_) {
        panel_->set_on_close([this]() {
            if (on_close_) on_close_();
        });
    }

    panel_->open();
    panel_->force_pointer_ready();
    position_initialized_ = false;
    ensure_visible_position();
}

void SingleSpawnGroupModal::close() {
    if (panel_) panel_->close();
}

bool SingleSpawnGroupModal::visible() const {
    return panel_ && panel_->is_visible();
}

void SingleSpawnGroupModal::update(const Input& input) {
    if (panel_) panel_->update(input, screen_w_, screen_h_);
}

bool SingleSpawnGroupModal::handle_event(const SDL_Event& e) {
    if (!panel_) return false;
    return panel_->handle_event(e);
}

void SingleSpawnGroupModal::render(SDL_Renderer* r) const {
    if (panel_) panel_->render(r);
}

bool SingleSpawnGroupModal::is_point_inside(int x, int y) const {
    if (!panel_) return false;
    return panel_->is_point_inside(x, y);
}

void SingleSpawnGroupModal::set_screen_dimensions(int width, int height) {
    screen_w_ = std::max(width, 0);
    screen_h_ = std::max(height, 0);
    if (panel_) panel_->set_screen_dimensions(screen_w_, screen_h_);
    position_initialized_ = false;
    ensure_visible_position();
}

void SingleSpawnGroupModal::set_manifest_store(devmode::core::ManifestStore* store) {
    manifest_store_ = store;
    if (panel_) {
        panel_->set_manifest_store(manifest_store_);
    }
}

void SingleSpawnGroupModal::set_assets(Assets* assets) {
    assets_ = assets;
    if (panel_) {
        panel_->set_assets(assets_);
    }
}

void SingleSpawnGroupModal::set_floating_stack_key(std::string key) {
    stack_key_ = std::move(key);
}

void SingleSpawnGroupModal::set_on_open_area(
    std::function<void(const std::string&, const std::string&)> cb) {
    on_open_area_ = std::move(cb);
}

void SingleSpawnGroupModal::ensure_visible_position() {
    if (!panel_) return;
    if (position_initialized_) return;

    panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});

    SDL_Rect rect = panel_->rect();
    constexpr int kPreferredWidth = 360;
    constexpr int kPreferredHeight = 420;

    DockManager::PanelInfo info;
    info.panel = panel_.get();
    info.force_layout = true;
    info.preferred_width = rect.w > 0 ? std::max(rect.w, kPreferredWidth) : kPreferredWidth;
    int resolved_height = rect.h > 0 ? rect.h : panel_->height();
    if (resolved_height <= 0) {
        resolved_height = kPreferredHeight;
    }
    info.preferred_height = std::max(resolved_height, kPreferredHeight);

    std::vector<DockManager::PanelInfo> panels;
    panels.push_back(info);
    DockManager::instance().layoutAll(panels);

    position_initialized_ = true;
}

BoundarySpawnGroupModal::BoundarySpawnGroupModal() = default;
BoundarySpawnGroupModal::~BoundarySpawnGroupModal() = default;

void BoundarySpawnGroupModal::set_on_close(std::function<void()> cb) {
    on_close_ = std::move(cb);
    if (panel_) {
        panel_->set_on_close([this]() {
            if (on_close_) on_close_();
        });
    }
}

void BoundarySpawnGroupModal::open(nlohmann::json& map_info,
                                   const std::string& section_key,
                                   const std::string& default_display_name,
                                   const std::string& ownership_label,
                                   SDL_Color ownership_color,
                                   SaveCallback on_save,
                                   RegenCallback on_regen) {
    map_info_ = &map_info;
    on_save_ = std::move(on_save);
    on_regen_ = std::move(on_regen);
    section_ = &(*map_info_)[section_key];

    if (!panel_) panel_ = std::make_unique<BoundaryCandidateListPanel>();
    panel_->set_screen_dimensions(screen_w_, screen_h_);
    panel_->set_manifest_store(manifest_store_);
    panel_->set_assets(assets_);
    panel_->bind(section_,
                 default_display_name,
                 ownership_label,
                 ownership_label.empty() ? std::optional<SDL_Color>{} : std::optional<SDL_Color>{ownership_color},
                 [this]() {
                     if (on_save_) on_save_();
                 },
                 [this](const json& updated_entry) {
                     if (on_regen_) on_regen_(updated_entry);
                 });

    if (panel_) {
        panel_->set_on_close([this]() {
            if (on_close_) on_close_();
        });
    }

    panel_->open();
    panel_->force_pointer_ready();
    position_initialized_ = false;
    ensure_visible_position();
}

void BoundarySpawnGroupModal::close() {
    if (panel_) panel_->close();
}

bool BoundarySpawnGroupModal::visible() const {
    return panel_ && panel_->is_visible();
}

void BoundarySpawnGroupModal::update(const Input& input) {
    if (panel_) panel_->update(input, screen_w_, screen_h_);
}

bool BoundarySpawnGroupModal::handle_event(const SDL_Event& e) {
    if (!panel_) return false;
    return panel_->handle_event(e);
}

void BoundarySpawnGroupModal::render(SDL_Renderer* r) const {
    if (panel_) panel_->render(r);
}

bool BoundarySpawnGroupModal::is_point_inside(int x, int y) const {
    if (!panel_) return false;
    return panel_->is_point_inside(x, y);
}

void BoundarySpawnGroupModal::set_screen_dimensions(int width, int height) {
    screen_w_ = std::max(width, 0);
    screen_h_ = std::max(height, 0);
    if (panel_) panel_->set_screen_dimensions(screen_w_, screen_h_);
    position_initialized_ = false;
    ensure_visible_position();
}

void BoundarySpawnGroupModal::set_manifest_store(devmode::core::ManifestStore* store) {
    manifest_store_ = store;
    if (panel_) {
        panel_->set_manifest_store(manifest_store_);
    }
}

void BoundarySpawnGroupModal::set_assets(Assets* assets) {
    assets_ = assets;
    if (panel_) {
        panel_->set_assets(assets_);
    }
}

void BoundarySpawnGroupModal::set_floating_stack_key(std::string key) {
    stack_key_ = std::move(key);
}

void BoundarySpawnGroupModal::set_on_open_area(
    std::function<void(const std::string&, const std::string&)> cb) {
    on_open_area_ = std::move(cb);
}

void BoundarySpawnGroupModal::ensure_visible_position() {
    if (!panel_) return;
    if (position_initialized_) return;

    panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});

    SDL_Rect rect = panel_->rect();
    constexpr int kPreferredWidth = 380;
    constexpr int kPreferredHeight = 520;

    DockManager::PanelInfo info;
    info.panel = panel_.get();
    info.force_layout = true;
    info.preferred_width = rect.w > 0 ? std::max(rect.w, kPreferredWidth) : kPreferredWidth;
    int resolved_height = rect.h > 0 ? rect.h : panel_->height();
    if (resolved_height <= 0) {
        resolved_height = kPreferredHeight;
    }
    info.preferred_height = std::max(resolved_height, kPreferredHeight);

    std::vector<DockManager::PanelInfo> panels;
    panels.push_back(info);
    DockManager::instance().layoutAll(panels);

    position_initialized_ = true;
}



