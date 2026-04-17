#include "AnimationTagsPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "AnimationDocument.hpp"
#include "core/manifest/manifest_loader.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/widgets.hpp"
#include "tag_utils.hpp"

namespace animation_editor {

namespace {

constexpr int kPanelPadding = 8;
constexpr int kPanelGap = 10;
constexpr int kChipMinWidth = 120;
constexpr int kChipMaxWidth = 180;

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void append_tags_from_json(const nlohmann::json& node, std::unordered_set<std::string>* values) {
    if (!values) {
        return;
    }
    auto append_if_string = [&](const nlohmann::json& value) {
        if (!value.is_string()) {
            return;
        }
        const std::string normalized = tag_utils::normalize(value.get<std::string>());
        if (!normalized.empty()) {
            values->insert(normalized);
        }
    };

    if (node.is_array()) {
        for (const auto& entry : node) {
            append_if_string(entry);
        }
    } else {
        append_if_string(node);
    }
}

const nlohmann::json* locate_animation_payloads(const nlohmann::json& asset_payload) {
    if (!asset_payload.is_object()) {
        return nullptr;
    }

    auto animations_it = asset_payload.find("animations");
    if (animations_it == asset_payload.end() || !animations_it->is_object()) {
        return nullptr;
    }

    auto nested_it = animations_it->find("animations");
    if (nested_it != animations_it->end() && nested_it->is_object()) {
        return &(*nested_it);
    }
    return &(*animations_it);
}

std::vector<std::string> build_recommendation_pool_from_manifest() {
    std::unordered_set<std::string> deduped;
    manifest::ManifestData manifest_data;
    try {
        manifest_data = manifest::load_manifest();
    } catch (...) {
        return {};
    }

    if (!manifest_data.assets.is_object()) {
        return {};
    }

    for (auto asset_it = manifest_data.assets.begin(); asset_it != manifest_data.assets.end(); ++asset_it) {
        const auto* animation_payloads = locate_animation_payloads(asset_it.value());
        if (!animation_payloads || !animation_payloads->is_object()) {
            continue;
        }
        for (auto anim_it = animation_payloads->begin(); anim_it != animation_payloads->end(); ++anim_it) {
            if (!anim_it.value().is_object()) {
                continue;
            }
            if (!anim_it.value().contains("tags")) {
                continue;
            }
            append_tags_from_json(anim_it.value()["tags"], &deduped);
        }
    }

    std::vector<std::string> pool(deduped.begin(), deduped.end());
    std::sort(pool.begin(), pool.end());
    return pool;
}

std::vector<std::string> sorted_tags_vector(const std::set<std::string>& tags) {
    return std::vector<std::string>(tags.begin(), tags.end());
}

}  // namespace

AnimationTagsPanel::AnimationTagsPanel() {
    search_box_ = std::make_unique<DMTextBox>("Find Tag", "");
    refresh_recommendation_pool();
    refresh_recommendations();
    rebuild_buttons();
}

AnimationTagsPanel::~AnimationTagsPanel() = default;

void AnimationTagsPanel::set_document(std::shared_ptr<AnimationDocument> document) {
    document_ = std::move(document);
    payload_signature_.clear();
    sync_tags_from_document();
}

void AnimationTagsPanel::set_animation_id(const std::string& animation_id) {
    if (animation_id_ == animation_id) {
        return;
    }
    animation_id_ = animation_id;
    payload_signature_.clear();
    sync_tags_from_document();
}

void AnimationTagsPanel::set_bounds(const SDL_Rect& bounds) {
    bounds_ = bounds;
    mark_layout_dirty();
}

void AnimationTagsPanel::set_status_callback(std::function<void(const std::string&)> callback) {
    status_callback_ = std::move(callback);
}

void AnimationTagsPanel::set_on_tags_changed(std::function<void(const std::vector<std::string>&)> callback) {
    on_tags_changed_ = std::move(callback);
}

void AnimationTagsPanel::update() {
    sync_tags_from_document();

    const std::uint64_t current_pool_version = tag_utils::tag_version();
    if (recommendation_pool_version_ != current_pool_version) {
        refresh_recommendation_pool();
    }

    if (search_box_) {
        const std::string value = search_box_->value();
        if (value != search_input_) {
            search_input_ = value;
            search_query_ = to_lower_copy(search_input_);
            refresh_recommendations();
            rebuild_buttons();
            mark_layout_dirty();
        }
    }
}

void AnimationTagsPanel::render(SDL_Renderer* renderer) const {
    if (!renderer || bounds_.w <= 0 || bounds_.h <= 0) {
        return;
    }

    ensure_layout();

    if (search_box_ && search_rect_.w > 0 && search_rect_.h > 0) {
        search_box_->render(renderer);
    }

    const DMLabelStyle& label_style = DMStyles::Label();
    DMFontCache::instance().draw_text(renderer,
                                      label_style,
                                      "Assigned Tags",
                                      tags_label_rect_.x,
                                      tags_label_rect_.y);
    DMFontCache::instance().draw_text(renderer,
                                      label_style,
                                      "Recommended Tags",
                                      rec_label_rect_.x,
                                      rec_label_rect_.y);

    for (const auto& chip : tag_chips_) {
        if (chip.button) {
            chip.button->render(renderer);
        }
    }
    for (const auto& chip : rec_chips_) {
        if (chip.button) {
            chip.button->render(renderer);
        }
    }
}

bool AnimationTagsPanel::handle_event(const SDL_Event& e) {
    ensure_layout();

    bool used = false;
    if (search_box_) {
        if (search_box_->handle_event(e)) {
            used = true;
            const std::string value = search_box_->value();
            if (value != search_input_) {
                search_input_ = value;
                search_query_ = to_lower_copy(search_input_);
                refresh_recommendations();
                rebuild_buttons();
                mark_layout_dirty();
            }
        }
    }

    std::optional<std::string> add_tag;
    std::optional<std::string> remove_tag;
    for (auto& chip : rec_chips_) {
        if (!chip.button) {
            continue;
        }
        if (chip.button->handle_event(e)) {
            used = true;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                add_tag = chip.value;
            }
        }
    }
    for (auto& chip : tag_chips_) {
        if (!chip.button) {
            continue;
        }
        if (chip.button->handle_event(e)) {
            used = true;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                remove_tag = chip.value;
            }
        }
    }

    if (add_tag.has_value()) {
        const std::string normalized = normalize_tag(*add_tag);
        if (!normalized.empty() && tags_.insert(normalized).second) {
            persist_tags_to_document();
            refresh_recommendations();
            rebuild_buttons();
            mark_layout_dirty();
        }
    }
    if (remove_tag.has_value()) {
        const std::string normalized = normalize_tag(*remove_tag);
        if (!normalized.empty() && tags_.erase(normalized) > 0) {
            persist_tags_to_document();
            refresh_recommendations();
            rebuild_buttons();
            mark_layout_dirty();
        }
    }

    return used;
}

int AnimationTagsPanel::preferred_height(int width) const {
    const int panel_width = std::max(0, width);
    return layout_for_width(panel_width, 0, 0, false);
}

void AnimationTagsPanel::sync_tags_from_document() {
    if (!document_ || animation_id_.empty()) {
        if (!tags_.empty()) {
            tags_.clear();
            refresh_recommendations();
            rebuild_buttons();
            mark_layout_dirty();
        }
        payload_signature_.clear();
        return;
    }

    auto payload_dump = document_->animation_payload(animation_id_);
    if (!payload_dump.has_value()) {
        if (!tags_.empty()) {
            tags_.clear();
            refresh_recommendations();
            rebuild_buttons();
            mark_layout_dirty();
        }
        payload_signature_.clear();
        return;
    }

    if (*payload_dump == payload_signature_) {
        return;
    }
    payload_signature_ = *payload_dump;

    std::set<std::string> loaded_tags;
    nlohmann::json payload = nlohmann::json::parse(*payload_dump, nullptr, false);
    if (payload.is_object() && payload.contains("tags")) {
        std::unordered_set<std::string> parsed;
        append_tags_from_json(payload["tags"], &parsed);
        loaded_tags.insert(parsed.begin(), parsed.end());
    }

    if (loaded_tags != tags_) {
        tags_ = std::move(loaded_tags);
        refresh_recommendations();
        rebuild_buttons();
        mark_layout_dirty();
    }
}

void AnimationTagsPanel::refresh_recommendation_pool() {
    recommendation_pool_ = build_recommendation_pool_from_manifest();
    recommendation_pool_version_ = tag_utils::tag_version();
    refresh_recommendations();
    rebuild_buttons();
    mark_layout_dirty();
}

void AnimationTagsPanel::refresh_recommendations() {
    recommended_tags_.clear();
    std::unordered_set<std::string> seen;

    auto include_if_match = [&](const std::string& value) {
        if (tags_.count(value) != 0) {
            return;
        }
        if (seen.count(value) != 0) {
            return;
        }
        if (!search_query_.empty() && value.find(search_query_) == std::string::npos) {
            return;
        }
        seen.insert(value);
        recommended_tags_.push_back(value);
    };

    for (const auto& value : recommendation_pool_) {
        include_if_match(value);
    }

    const std::string normalized_query = normalize_tag(search_input_);
    if (!normalized_query.empty() && tags_.count(normalized_query) == 0 && seen.count(normalized_query) == 0) {
        recommended_tags_.insert(recommended_tags_.begin(), normalized_query);
    }
}

void AnimationTagsPanel::rebuild_buttons() {
    tag_chips_.clear();
    rec_chips_.clear();

    for (const auto& value : tags_) {
        Chip chip;
        chip.value = value;
        chip.button = std::make_unique<DMButton>(value, &DMStyles::HeaderButton(), kChipMinWidth, DMButton::height());
        tag_chips_.push_back(std::move(chip));
    }

    for (const auto& value : recommended_tags_) {
        Chip chip;
        chip.value = value;
        chip.button = std::make_unique<DMButton>(value, &DMStyles::AccentButton(), kChipMinWidth, DMButton::height());
        rec_chips_.push_back(std::move(chip));
    }
}

void AnimationTagsPanel::persist_tags_to_document() {
    if (!document_ || animation_id_.empty()) {
        return;
    }

    auto payload = document_->animation_payload_json(animation_id_);
    if (!payload.has_value() || !payload->is_object()) {
        return;
    }

    const std::vector<std::string> sorted_tags = sorted_tags_vector(tags_);
    (*payload)["tags"] = sorted_tags;
    if (!document_->update_animation_payload(animation_id_, *payload)) {
        return;
    }

    if (status_callback_) {
        status_callback_("Updated animation tags.");
    }
    if (on_tags_changed_) {
        on_tags_changed_(sorted_tags);
    }
}

void AnimationTagsPanel::mark_layout_dirty() {
    layout_dirty_ = true;
}

void AnimationTagsPanel::ensure_layout() const {
    if (!layout_dirty_) {
        return;
    }
    const_cast<AnimationTagsPanel*>(this)->layout_for_width(bounds_.w, bounds_.x, bounds_.y, true);
    const_cast<AnimationTagsPanel*>(this)->layout_dirty_ = false;
}

int AnimationTagsPanel::layout_for_width(int width, int origin_x, int origin_y, bool apply) const {
    const int content_width = std::max(0, width);
    const int x = origin_x;
    int y = origin_y;

    auto* self = const_cast<AnimationTagsPanel*>(this);

    const int search_height = search_box_ ? search_box_->height_for_width(content_width) : DMTextBox::height();
    if (apply) {
        self->search_rect_ = SDL_Rect{x, y, content_width, search_height};
        if (search_box_) {
            search_box_->set_rect(self->search_rect_);
        }
    }
    y += search_height + kPanelGap;

    const int label_height = DMStyles::Label().font_size + 4;
    if (apply) {
        self->tags_label_rect_ = SDL_Rect{x, y, content_width, label_height};
    }
    y += label_height + kPanelGap;

    const int tags_start = y;
    y = layout_chip_grid(self->tag_chips_, content_width, x, y, apply);
    if (apply) {
        self->tags_content_rect_ = SDL_Rect{x, tags_start, content_width, std::max(0, y - tags_start)};
    }
    y += kPanelGap;

    if (apply) {
        self->rec_label_rect_ = SDL_Rect{x, y, content_width, label_height};
    }
    y += label_height + kPanelGap;

    const int rec_start = y;
    y = layout_chip_grid(self->rec_chips_, content_width, x, y, apply);
    if (apply) {
        self->rec_content_rect_ = SDL_Rect{x, rec_start, content_width, std::max(0, y - rec_start)};
    }

    y += kPanelPadding;
    return std::max(0, y - origin_y);
}

int AnimationTagsPanel::layout_chip_grid(std::vector<Chip>& chips,
                                         int width,
                                         int origin_x,
                                         int start_y,
                                         bool apply) const {
    if (chips.empty()) {
        return start_y + DMButton::height();
    }

    const int gap = DMSpacing::small_gap();
    const int available = std::max(1, width);
    const int max_columns = std::max(1, (available + gap) / (kChipMinWidth + gap));
    int columns = std::min<int>(max_columns, static_cast<int>(chips.size()));
    columns = std::max(1, columns);
    int chip_width = (available - (columns - 1) * gap) / columns;
    chip_width = std::clamp(chip_width, kChipMinWidth, kChipMaxWidth);
    if (chip_width * columns + (columns - 1) * gap > available) {
        columns = std::max(1, (available + gap) / (chip_width + gap));
    }
    const int row_width = columns * chip_width + (columns - 1) * gap;
    const int offset_x = origin_x + std::max(0, (available - row_width) / 2);

    int row = 0;
    int col = 0;
    for (auto& chip : chips) {
        const int x = offset_x + col * (chip_width + gap);
        const int y = start_y + row * (DMButton::height() + gap);
        if (apply && chip.button) {
            chip.button->set_rect(SDL_Rect{x, y, chip_width, DMButton::height()});
        }

        ++col;
        if (col >= columns) {
            col = 0;
            ++row;
        }
    }

    const int rows = row + (col > 0 ? 1 : 0);
    return start_y + rows * DMButton::height() + std::max(0, rows - 1) * gap;
}

std::string AnimationTagsPanel::normalize_tag(std::string_view raw) {
    return tag_utils::normalize(raw);
}

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
void AnimationTagsPanelTestAccess::set_query(AnimationTagsPanel& panel, const std::string& query) {
    panel.search_input_ = query;
    panel.search_query_ = to_lower_copy(query);
    if (panel.search_box_) {
        panel.search_box_->set_value(query);
    }
    panel.refresh_recommendations();
    panel.rebuild_buttons();
    panel.mark_layout_dirty();
}

void AnimationTagsPanelTestAccess::add_tag(AnimationTagsPanel& panel, const std::string& tag) {
    const std::string normalized = panel.normalize_tag(tag);
    if (normalized.empty()) {
        return;
    }
    if (panel.tags_.insert(normalized).second) {
        panel.persist_tags_to_document();
        panel.refresh_recommendations();
        panel.rebuild_buttons();
        panel.mark_layout_dirty();
    }
}

void AnimationTagsPanelTestAccess::remove_tag(AnimationTagsPanel& panel, const std::string& tag) {
    const std::string normalized = panel.normalize_tag(tag);
    if (normalized.empty()) {
        return;
    }
    if (panel.tags_.erase(normalized) > 0) {
        panel.persist_tags_to_document();
        panel.refresh_recommendations();
        panel.rebuild_buttons();
        panel.mark_layout_dirty();
    }
}

std::vector<std::string> AnimationTagsPanelTestAccess::tags(const AnimationTagsPanel& panel) {
    return sorted_tags_vector(panel.tags_);
}

const std::vector<std::string>& AnimationTagsPanelTestAccess::recommended_tags(const AnimationTagsPanel& panel) {
    return panel.recommended_tags_;
}

void AnimationTagsPanelTestAccess::refresh_pool(AnimationTagsPanel& panel) {
    panel.refresh_recommendation_pool();
}
#endif

}  // namespace animation_editor
