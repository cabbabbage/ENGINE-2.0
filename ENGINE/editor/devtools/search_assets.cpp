#include "search_assets.hpp"

#include "asset_list_view.hpp"
#include "DockableCollapsible.hpp"
#include "DockManager.hpp"
#include "dm_styles.hpp"
#include "tag_utils.hpp"
#include "utils/input.hpp"
#include "dev_mode_utils.hpp"
#include "assets/asset_library.hpp"
#include "assets/asset/asset_info.hpp"
#include "core/AssetsManager.hpp"
#include "devtools/core/manifest_store.hpp"

#include <nlohmann/json.hpp>
#include <set>
#include <unordered_set>
#include <cctype>
#include <algorithm>
#include <vector>
#include <cmath>
#include <utility>
#include <string_view>

SearchAssets::SearchAssets(devmode::core::ManifestStore* manifest_store)
    : manifest_store_(manifest_store) {
    if (!manifest_store_) {
        owned_manifest_store_ = std::make_unique<devmode::core::ManifestStore>();
        manifest_store_ = owned_manifest_store_.get();
    }
    panel_ = std::make_unique<DockableCollapsible>("Search Assets", true, 64, 64);
    panel_->set_expanded(true);
    panel_->set_visible(false);
    panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
    panel_->set_close_button_enabled(true);
    panel_->set_scroll_enabled(true);
    panel_->set_row_gap(DMSpacing::small_gap());
    panel_->set_col_gap(16);
    panel_->set_floating_content_width(520);
    panel_->reset_scroll();

    list_view_.set_callbacks(AssetListView::Callbacks{
        [this](const AssetListView::Entry& entry) { this->activate_result(Result{entry.label, entry.value, entry.is_tag, entry.manifest_name, entry.tags, entry.recommended}); },
        nullptr,
        nullptr,
        nullptr
    });

    panel_->set_rows(list_view_.rows());
    last_known_position_ = panel_->position();
    pending_position_ = last_known_position_;
    has_pending_position_ = true;
    tag_data_version_ = tag_utils::tag_version();
}

SearchAssets::~SearchAssets() = default;

namespace {

DockManager::PanelInfo build_panel_info_for_panel(DockableCollapsible* panel,
                                                  int fallback_width,
                                                  int fallback_height,
                                                  bool force_layout) {
    DockManager::PanelInfo info;
    info.panel = panel;
    info.force_layout = force_layout;
    info.preferred_width = fallback_width;
    info.preferred_height = fallback_height;
    if (!panel) {
        return info;
    }
    SDL_Rect rect = panel->rect();
    if (rect.w > 0) {
        info.preferred_width = rect.w;
    }
    int resolved_height = rect.h > 0 ? rect.h : panel->height();
    if (resolved_height > 0) {
        info.preferred_height = resolved_height;
    }
    return info;
}

}  // namespace

void SearchAssets::apply_position(int x, int y) {
    if (!panel_) {
        panel_ = std::make_unique<DockableCollapsible>("Search Assets", true, x, y);
        panel_->set_expanded(true);
        panel_->set_visible(false);
        panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
        panel_->set_close_button_enabled(true);
        panel_->set_scroll_enabled(true);
        panel_->reset_scroll();
        panel_->set_row_gap(DMSpacing::small_gap());
        panel_->set_col_gap(16);
        panel_->set_cell_width(220);
        panel_->set_floating_content_width(520);
        panel_->set_rows(list_view_.rows());
    }
    if (embedded_) {
        panel_->set_rect(SDL_Rect{x, y, panel_->rect().w, panel_->rect().h});
        return;
    }
    panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
}

void SearchAssets::set_position(int x, int y) {
    if (embedded_) {
        set_anchor_position(x, y);
        return;
    }
    pending_position_ = SDL_Point{x, y};
    has_pending_position_ = true;
    has_custom_position_ = false;
    apply_position(x, y);
    ensure_visible_position();
    if (panel_) {
        last_known_position_ = panel_->position();
    }
}

void SearchAssets::set_anchor_position(int x, int y) {
    if (embedded_) {
        set_position(x, y);
        return;
    }
    pending_position_ = SDL_Point{x, y};
    has_pending_position_ = true;
    if (has_custom_position_) {
        return;
    }
    apply_position(x, y);
    ensure_visible_position();
    if (panel_) {
        last_known_position_ = panel_->position();
    }
}

void SearchAssets::set_screen_dimensions(int width, int height) {
    if (width > 0) {
        screen_w_ = width;
    }
    if (height > 0) {
        screen_h_ = height;
    }
    if (embedded_) {
        if (panel_) {
            panel_->set_work_area(SDL_Rect{0, 0, embedded_rect_.w > 0 ? embedded_rect_.w : screen_w_,
                                           embedded_rect_.h > 0 ? embedded_rect_.h : screen_h_});
            if (embedded_rect_.w > 0 || embedded_rect_.h > 0) {
                SDL_Rect rect = embedded_rect_;
                if (rect.w <= 0) rect.w = panel_->rect().w;
                if (rect.h <= 0) rect.h = panel_->rect().h;
                panel_->set_rect(rect);
            }
        }
        return;
    }
    if (panel_) {
        panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
        ensure_visible_position();
        last_known_position_ = panel_->position();
        if (!has_custom_position_) {
            pending_position_ = last_known_position_;
            has_pending_position_ = true;
        }
    }
}

void SearchAssets::layout_with_parent(const DockManager::SlidingParentInfo& parent) {
    if (embedded_) {
        return;
    }
    has_custom_position_ = false;
    ensure_visible_position(&parent);
}

void SearchAssets::set_floating_stack_key(std::string key) {
    floating_stack_key_ = std::move(key);
}

void SearchAssets::set_embedded_mode(bool embedded) {
    embedded_ = embedded;
    if (!panel_) {
        return;
    }
    panel_->set_floatable(!embedded_);
    panel_->set_show_header(!embedded_);
    panel_->set_close_button_enabled(!embedded_);
    if (embedded_) {
        panel_->set_scroll_enabled(true);
        panel_->set_work_area(SDL_Rect{0, 0, embedded_rect_.w, embedded_rect_.h});
    } else {
        panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
    }
}

void SearchAssets::set_embedded_rect(const SDL_Rect& rect) {
    embedded_rect_ = rect;
    if (!panel_) {
        return;
    }
    if (!embedded_) {
        apply_position(rect.x, rect.y);
        return;
    }
    SDL_Rect applied = rect;
    if (applied.w <= 0) {
        applied.w = panel_->rect().w > 0 ? panel_->rect().w : 260;
    }
    if (applied.h <= 0) {
        applied.h = panel_->rect().h > 0 ? panel_->rect().h : 0;
    }
    panel_->set_cell_width(std::max(120, applied.w - 20));
    if (applied.h > 0) {
        panel_->set_visible_height(applied.h);
        panel_->set_available_height_override(applied.h);
    }
    panel_->set_work_area(SDL_Rect{0, 0, applied.w, applied.h});
    panel_->set_rect(applied);
    Input dummy;
    panel_->update(dummy, applied.w, applied.h);
}

SDL_Rect SearchAssets::rect() const {
    if (!panel_) {
        return SDL_Rect{0, 0, 0, 0};
    }
    return panel_->rect();
}

std::string SearchAssets::to_lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void SearchAssets::open(Callback cb) {
    cb_ = std::move(cb);
    load_assets();  // Always reload assets when opening to ensure we have the latest from manifest store
    if (embedded_) {
        if (panel_) {
            panel_->set_visible(true);
            panel_->set_expanded(true);
            panel_->reset_scroll();
            panel_->force_pointer_ready();
            SDL_Rect applied = embedded_rect_;
            if (applied.w <= 0) applied.w = panel_->rect().w;
            if (applied.h <= 0) applied.h = panel_->rect().h;
            panel_->set_rect(applied);
            Input dummy;
            panel_->update(dummy, applied.w, applied.h);
        }
        list_view_.set_query("");
        list_view_.refresh_tiles();
        panel_->set_rows(list_view_.rows());
        return;
    }
    SDL_Point target = last_known_position_;
    if (has_pending_position_ && !has_custom_position_) {
        target = pending_position_;
    }
    apply_position(target.x, target.y);
    ensure_visible_position();
    if (!floating_stack_key_.empty()) {
        DockManager::instance().open_floating(
            "Search Assets",
            panel_.get(),
            [this]() { this->close(); },
            floating_stack_key_);
    }
    panel_->set_visible(true);
    panel_->set_expanded(true);
    panel_->reset_scroll();
    Input dummy;
    panel_->update(dummy, screen_w_, screen_h_);
    ensure_visible_position();
    last_known_position_ = panel_->position();
    if (!has_custom_position_) {
        pending_position_ = last_known_position_;
        has_pending_position_ = true;
    }
    list_view_.set_query("");
    filter_assets();
    if (auto* box = list_view_.search_box()) {
        box->start_editing();
    }
}

void SearchAssets::close() {
    if (panel_) {
        if (embedded_) {
            panel_->set_visible(false);
        } else {
            last_known_position_ = panel_->position();
            if (!has_custom_position_) {
                pending_position_ = last_known_position_;
                has_pending_position_ = true;
            }
            panel_->set_visible(false);
        }
    }
    cb_ = nullptr;
    if (auto* box = list_view_.search_box()) {
        box->stop_editing();
    }
}

bool SearchAssets::visible() const {
    return panel_ && panel_->is_visible();
}

void SearchAssets::set_extra_results_provider(ExtraResultsProvider provider) {
    extra_results_provider_ = std::move(provider);
    if (panel_ && panel_->is_visible()) {
        filter_assets();
    }
}

void SearchAssets::set_asset_filter(AssetFilter filter) {
    asset_filter_ = std::move(filter);
    load_assets();
    if (panel_ && panel_->is_visible()) {
        filter_assets();
    }
}

void SearchAssets::set_assets(Assets* assets) {
    if (assets == assets_) {
        return;
    }
    assets_ = assets;
    list_view_.set_assets(assets_);
    all_.clear();
    results_.clear();
    tag_data_version_ = 0;
    load_assets();
    if (panel_ && panel_->is_visible()) {
        filter_assets();
    }
}

void SearchAssets::load_assets() {
    all_.clear();

    if (assets_) {
        const auto& lib_assets = assets_->library().all();
        all_.reserve(lib_assets.size());
        for (const auto& [lib_name, info] : lib_assets) {
            if (!info) {
                continue;
            }
            Asset asset;
            asset.name = info->name.empty() ? lib_name : info->name;
            asset.manifest_name = lib_name;
            asset.tags = info->tags;
            asset.payload = nullptr;
            all_.push_back(std::move(asset));
        }
        return;
    }

    if (!manifest_store_) {
        return;
    }
    auto manifest_assets = manifest_store_->assets();
    for (const auto& asset_view : manifest_assets) {
        if (!asset_view) {
            continue;
        }
        if (asset_filter_ && (!asset_view.data || !asset_filter_(*asset_view.data))) {
            continue;
        }
        Asset asset;
        const nlohmann::json& data = *asset_view.data;
        asset.name = data.value("asset_name", asset_view.name);
        if (asset.name.empty()) {
            asset.name = asset_view.name;
        }
        asset.manifest_name = asset_view.name;
        if (data.contains("tags") && data["tags"].is_array()) {
            for (const auto& tag : data["tags"]) {
                if (tag.is_string()) {
                    std::string tag_value = tag.get<std::string>();
                    asset.tags.push_back(tag_value);
                }
            }
        }
        asset.payload = asset_view.data;
        all_.push_back(std::move(asset));
    }
}

void SearchAssets::filter_assets() {
    if (!panel_ || !panel_->is_visible()) return;
    auto current_version = tag_utils::tag_version();
    if (current_version != tag_data_version_) {
        load_assets();
        tag_data_version_ = current_version;
    }

    std::string q = list_view_.query();
    results_.clear();
    std::unordered_set<std::string> seen_labels;
    std::set<std::string> tagset;
    std::vector<AssetListView::Entry> entries;

    auto push_result = [&](Result res) {
        if (!seen_labels.insert(res.label).second) {
            return;
        }
        results_.push_back(res);
        AssetListView::Entry entry;
        entry.label = res.label;
        entry.value = res.value;
        entry.is_tag = res.is_tag;
        entry.manifest_name = res.manifest_name;
        entry.tags = res.tags;
        entry.recommended = res.recommended;
        entries.push_back(std::move(entry));
    };

    for (const auto& a : all_) {
        AssetListView::Entry entry;
        entry.label = a.name;
        entry.value = !a.manifest_name.empty() ? a.manifest_name : a.name;
        entry.manifest_name = a.manifest_name;
        entry.tags = a.tags;
        if (!AssetListView::matches_query(entry, q)) {
            continue;
        }
        Result res;
        res.label = entry.label;
        res.value = entry.value;
        res.is_tag = false;
        res.manifest_name = entry.manifest_name;
        res.tags = entry.tags;
        push_result(std::move(res));

        for (const auto& t : a.tags) {
            if (AssetListView::matches_query(AssetListView::Entry{std::string("#") + t, t, true, "", {t}}, q)) {
                tagset.insert(t);
            }
        }
    }

    if (extra_results_provider_) {
        try {
            auto extras = extra_results_provider_();
            for (auto& extra : extras) {
                if (extra.label.empty() || extra.value.empty()) {
                    continue;
                }
                AssetListView::Entry entry;
                entry.label = extra.label;
                entry.value = extra.value;
                entry.is_tag = extra.is_tag;
                entry.manifest_name = extra.manifest_name;
                entry.tags = extra.tags;
                entry.recommended = extra.recommended;
                if (!AssetListView::matches_query(entry, q)) {
                    continue;
                }
                push_result(extra);
            }
        } catch (...) {
        }
    }

    for (const auto& t : tagset) {
        Result res;
        res.label = std::string("#") + t;
        res.value = t;
        res.is_tag = true;
        res.tags = {t};
        push_result(std::move(res));
    }

    list_view_.set_entries(std::move(entries));
    list_view_.refresh_tiles();
    panel_->set_rows(list_view_.rows());

    Input dummy;
    if (embedded_) {
        int w = embedded_rect_.w > 0 ? embedded_rect_.w : screen_w_;
        int h = embedded_rect_.h > 0 ? embedded_rect_.h : screen_h_;
        panel_->update(dummy, w, h);
    } else {
        panel_->update(dummy, screen_w_, screen_h_);
    }
}

void SearchAssets::activate_result(const Result& result) {
    std::string v = result.value;
    if (result.is_tag && (v.empty() || v.front() != '#')) {
        v.insert(v.begin(), '#');
    }
    if (cb_) cb_(v);
    close();
}

bool SearchAssets::handle_event(const SDL_Event& e) {
    if (!panel_ || !panel_->is_visible()) return false;
    SDL_Point before = panel_->position();
    bool used = panel_->handle_event(e);
    SDL_Point after = panel_->position();
    if (!embedded_) {
        if (after.x != before.x || after.y != before.y) {
            has_custom_position_ = true;
            last_known_position_ = after;
            ensure_visible_position();
        }
    }
    if (list_view_.update_query_from_widget()) {
        filter_assets();
    }
    return used;
}

void SearchAssets::update(const Input& input) {
    if (panel_ && panel_->is_visible()) {
        if (embedded_) {
            int w = embedded_rect_.w > 0 ? embedded_rect_.w : screen_w_;
            int h = embedded_rect_.h > 0 ? embedded_rect_.h : screen_h_;
            panel_->update(input, w, h);
        } else {
            panel_->update(input, screen_w_, screen_h_);
            last_known_position_ = panel_->position();
            if (!has_custom_position_) {
                pending_position_ = last_known_position_;
                has_pending_position_ = true;
            }
        }
        if (tag_utils::tag_version() != tag_data_version_) {
            filter_assets();
        }
    }
}

void SearchAssets::render(SDL_Renderer* r) const {
    if (panel_ && panel_->is_visible()) panel_->render(r);
}

bool SearchAssets::is_point_inside(int x, int y) const {
    if (!panel_ || !panel_->is_visible()) return false;
    return panel_->is_point_inside(x, y);
}

void SearchAssets::set_manifest_store(devmode::core::ManifestStore* manifest_store) {
    if (manifest_store == manifest_store_) {
        return;
    }
    manifest_store_ = manifest_store;
    if (!manifest_store_) {
        owned_manifest_store_ = std::make_unique<devmode::core::ManifestStore>();
        manifest_store_ = owned_manifest_store_.get();
    } else {
        owned_manifest_store_.reset();
    }
    all_.clear();
    results_.clear();
    tag_data_version_ = 0;
    load_assets();
}

void SearchAssets::set_query_for_testing(const std::string& value) {
    list_view_.set_query(value);
    filter_assets();
}

std::vector<std::pair<std::string, bool>> SearchAssets::results_for_testing() const {
    std::vector<std::pair<std::string, bool>> out;
    out.reserve(results_.size());
    for (const auto& res : results_) {
        out.emplace_back(res.value, res.is_tag);
    }
    return out;
}

DockManager::PanelInfo SearchAssets::build_panel_info(bool force_layout) const {
    constexpr int kFallbackWidth = 520;
    constexpr int kFallbackHeight = 400;
    return build_panel_info_for_panel(panel_.get(), kFallbackWidth, kFallbackHeight, force_layout);
}

void SearchAssets::ensure_visible_position(const DockManager::SlidingParentInfo* parent) {
    if (embedded_) {
        return;
    }
    if (!panel_) {
        return;
    }
    if (has_custom_position_) {
        return;
    }

    DockManager::PanelInfo info = build_panel_info(true);

    if (parent) {
        SDL_Point placement = DockManager::instance().positionFor(info, parent);
        panel_->set_position_from_layout_manager(placement.x, placement.y);
    } else {
        std::vector<DockManager::PanelInfo> panels;
        panels.push_back(info);
        DockManager::instance().layoutAll(panels);
    }

    last_known_position_ = panel_->position();
    pending_position_ = last_known_position_;
    has_pending_position_ = true;
}
