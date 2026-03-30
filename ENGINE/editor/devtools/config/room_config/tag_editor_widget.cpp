#include "tag_editor_widget.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"

#include "dm_styles.hpp"
#include "tag_library.hpp"
#include "tag_utils.hpp"
#include "core/manifest/manifest_loader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace {
constexpr int kChipWidth = 132;
constexpr int kRecommendChipWidth = 148;
constexpr size_t kRecommendationPreviewCount = 5;
constexpr size_t kMaxRecommendations = std::numeric_limits<size_t>::max();
constexpr size_t kAssetRecommendationLimit = 25;
constexpr int kAssetDropMinHeight = 34;
constexpr float kDragStartThresholdPx = 6.0f;
constexpr double kAssetSimilarityWeight = 0.55;
constexpr double kAssetPopularityWeight = 0.45;
constexpr size_t kSimilarAssetCount = 3;

struct TagDatasetEntry {
    std::vector<std::string> tags;
    std::vector<std::string> anti_tags;
};

struct AssetTagProfile {
    std::string asset_name;
    std::unordered_set<std::string> tags;
    std::unordered_set<std::string> anti_tags;
};

struct ManifestTagCorpus {
    std::vector<AssetTagProfile> profiles;
    std::vector<std::string> unique_tags;
    std::unordered_map<std::string, int> popularity;
    int max_popularity = 0;
};

struct SimilarAssetScore {
    const AssetTagProfile* profile = nullptr;
    double similarity = 0.0;
};

struct TagStats {
    int tag_count = 0;
    int anti_count = 0;
    int co_with_tags = 0;
    int co_with_anti = 0;
    int cross_hits = 0;
};

std::string to_lower(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

void append_strings(const nlohmann::json& node, std::unordered_set<std::string>& dest) {
    if (node.is_array()) {
        for (const auto& entry : node) {
            if (!entry.is_string()) continue;
            auto norm = tag_utils::normalize(entry.get<std::string>());
            if (!norm.empty()) dest.insert(std::move(norm));
        }
    } else if (node.is_string()) {
        auto norm = tag_utils::normalize(node.get<std::string>());
        if (!norm.empty()) dest.insert(std::move(norm));
    }
}

void extract_tag_section(const nlohmann::json& node, std::unordered_set<std::string>& tags, std::unordered_set<std::string>& anti) {
    if (node.is_array() || node.is_string()) {
        append_strings(node, tags);
        return;
    }
    if (!node.is_object()) return;
    if (node.contains("include")) append_strings(node["include"], tags);
    if (node.contains("tags")) append_strings(node["tags"], tags);
    if (node.contains("exclude")) append_strings(node["exclude"], anti);
    if (node.contains("anti_tags")) append_strings(node["anti_tags"], anti);
}

void extract_anti_section(const nlohmann::json& node, std::unordered_set<std::string>& anti) {
    if (node.is_array() || node.is_string()) {
        append_strings(node, anti);
        return;
    }
    if (!node.is_object()) return;
    if (node.contains("include")) append_strings(node["include"], anti);
    if (node.contains("exclude")) append_strings(node["exclude"], anti);
    if (node.contains("tags")) append_strings(node["tags"], anti);
    if (node.contains("anti_tags")) append_strings(node["anti_tags"], anti);
}

double jaccard_upper_bound(size_t left_size, size_t right_size) {
    if (left_size == 0 || right_size == 0) {
        return 0.0;
    }
    return static_cast<double>(std::min(left_size, right_size)) /
           static_cast<double>(std::max(left_size, right_size));
}

double jaccard_similarity(const std::set<std::string>& left, const std::unordered_set<std::string>& right) {
    if (left.empty() || right.empty()) {
        return 0.0;
    }

    size_t shared = 0;
    for (const auto& value : left) {
        if (right.count(value) != 0) {
            ++shared;
        }
    }
    if (shared == 0) {
        return 0.0;
    }

    const size_t union_count = left.size() + right.size() - shared;
    if (union_count == 0) {
        return 0.0;
    }
    return static_cast<double>(shared) / static_cast<double>(union_count);
}

double combined_similarity_upper_bound(const std::set<std::string>& current_tags,
                                       const std::set<std::string>& current_anti_tags,
                                       const AssetTagProfile& candidate) {
    const double pos_upper = jaccard_upper_bound(current_tags.size(), candidate.tags.size());
    const double neg_upper = jaccard_upper_bound(current_anti_tags.size(), candidate.anti_tags.size());
    return 0.5 * (pos_upper + neg_upper);
}

double combined_similarity_score(const std::set<std::string>& current_tags,
                                 const std::set<std::string>& current_anti_tags,
                                 const AssetTagProfile& candidate) {
    const double pos_similarity = jaccard_similarity(current_tags, candidate.tags);
    const double neg_similarity = jaccard_similarity(current_anti_tags, candidate.anti_tags);
    return 0.5 * (pos_similarity + neg_similarity);
}

void add_top_similar_asset(std::vector<SimilarAssetScore>& top_scores,
                           const AssetTagProfile* profile,
                           double similarity) {
    if (!profile) {
        return;
    }

    if (top_scores.size() < kSimilarAssetCount) {
        top_scores.push_back(SimilarAssetScore{profile, similarity});
    } else if (similarity > top_scores.back().similarity) {
        top_scores.back() = SimilarAssetScore{profile, similarity};
    } else {
        return;
    }

    std::sort(top_scores.begin(), top_scores.end(), [](const SimilarAssetScore& a, const SimilarAssetScore& b) {
        if (a.similarity == b.similarity) {
            const std::string left_name = a.profile ? a.profile->asset_name : std::string{};
            const std::string right_name = b.profile ? b.profile->asset_name : std::string{};
            return left_name < right_name;
        }
        return a.similarity > b.similarity;
    });
}

std::vector<SimilarAssetScore> collect_top_similar_assets(const ManifestTagCorpus& corpus,
                                                          const std::set<std::string>& current_tags,
                                                          const std::set<std::string>& current_anti_tags,
                                                          const std::string& subject_asset_name,
                                                          bool use_pruning) {
    std::vector<SimilarAssetScore> top_similar_assets;
    top_similar_assets.reserve(kSimilarAssetCount);

    for (const auto& profile : corpus.profiles) {
        if (!subject_asset_name.empty() && profile.asset_name == subject_asset_name) {
            continue;
        }

        if (use_pruning && top_similar_assets.size() >= kSimilarAssetCount) {
            const double upper_bound = combined_similarity_upper_bound(current_tags, current_anti_tags, profile);
            if (upper_bound <= top_similar_assets.back().similarity) {
                continue;
            }
        }

        const double similarity = combined_similarity_score(current_tags, current_anti_tags, profile);
        add_top_similar_asset(top_similar_assets, &profile, similarity);
    }

    return top_similar_assets;
}

ManifestTagCorpus build_manifest_tag_corpus() {
    ManifestTagCorpus corpus;

    manifest::ManifestData data;
    try {
        data = manifest::load_manifest();
    } catch (...) {
        return corpus;
    }

    if (!data.assets.is_object()) {
        return corpus;
    }

    for (auto it = data.assets.begin(); it != data.assets.end(); ++it) {
        const std::string asset_name = it.key();
        const nlohmann::json& payload = it.value();
        if (!payload.is_object()) {
            continue;
        }

        AssetTagProfile profile;
        profile.asset_name = asset_name;
        if (payload.contains("tags")) {
            extract_tag_section(payload["tags"], profile.tags, profile.anti_tags);
        }
        if (payload.contains("anti_tags")) {
            extract_anti_section(payload["anti_tags"], profile.anti_tags);
        }

        std::unordered_set<std::string> merged = profile.tags;
        merged.insert(profile.anti_tags.begin(), profile.anti_tags.end());
        for (const auto& tag : merged) {
            int& popularity = corpus.popularity[tag];
            popularity += 1;
            corpus.max_popularity = std::max(corpus.max_popularity, popularity);
        }

        if (!merged.empty()) {
            corpus.profiles.push_back(std::move(profile));
        }
    }

    corpus.unique_tags.reserve(corpus.popularity.size());
    for (const auto& [tag, _] : corpus.popularity) {
        corpus.unique_tags.push_back(tag);
    }
    std::sort(corpus.unique_tags.begin(), corpus.unique_tags.end());
    return corpus;
}

const ManifestTagCorpus& manifest_tag_corpus() {
    static ManifestTagCorpus cached;
    static bool loaded = false;
    static std::uint64_t loaded_version = 0;

    const std::uint64_t current_version = tag_utils::tag_version();
    if (loaded && loaded_version == current_version) {
        return cached;
    }

    cached = build_manifest_tag_corpus();
    loaded = true;
    loaded_version = current_version;
    return cached;
}

const std::vector<TagDatasetEntry>& tag_dataset() {
    static std::vector<TagDatasetEntry> dataset;
    static bool loaded = false;
    static std::uint64_t loaded_version = 0;

    std::uint64_t current_version = tag_utils::tag_version();
    if (loaded && loaded_version == current_version) {
        return dataset;
    }

    dataset.clear();
    loaded = true;
    loaded_version = current_version;

    const auto& corpus = manifest_tag_corpus();
    dataset.reserve(corpus.profiles.size());
    for (const auto& profile : corpus.profiles) {
        TagDatasetEntry entry;
        entry.tags.assign(profile.tags.begin(), profile.tags.end());
        entry.anti_tags.assign(profile.anti_tags.begin(), profile.anti_tags.end());
        std::sort(entry.tags.begin(), entry.tags.end());
        std::sort(entry.anti_tags.begin(), entry.anti_tags.end());
        dataset.push_back(std::move(entry));
    }
    return dataset;
}

bool contains_any(const std::set<std::string>& haystack, const std::vector<std::string>& needles) {
    for (const auto& value : needles) {
        if (haystack.count(value)) return true;
    }
    return false;
}

bool contains_any(const std::set<std::string>& haystack, const std::set<std::string>& needles) {
    for (const auto& value : needles) {
        if (haystack.count(value)) return true;
    }
    return false;
}

std::unique_ptr<DMButton> make_button(const std::string& text, const DMButtonStyle& style, int width) {
    return std::make_unique<DMButton>(text, &style, width, DMButton::height());
}

const DMButtonStyle& recommendation_yellow_style() {
    static const DMButtonStyle style{
        {dm::FONT_PATH, 16, dm::rgba(36, 36, 36, 255)},
        dm::rgba(250, 204, 21, 235),
        dm::rgba(253, 224, 71, 242),
        dm::rgba(202, 138, 4, 235),
        dm::rgba(161, 98, 7, 255),
        dm::rgba(36, 36, 36, 255)
    };
    return style;
}

const DMButtonStyle& recommendation_orange_style() {
    static const DMButtonStyle style{
        {dm::FONT_PATH, 16, dm::rgba(36, 36, 36, 255)},
        dm::rgba(251, 146, 60, 235),
        dm::rgba(253, 186, 116, 242),
        dm::rgba(234, 88, 12, 235),
        dm::rgba(194, 65, 12, 255),
        dm::rgba(36, 36, 36, 255)
    };
    return style;
}

}

TagEditorWidget::TagEditorWidget(Mode mode)
    : mode_(mode) {}

TagEditorWidget::~TagEditorWidget() = default;

void TagEditorWidget::set_mode(Mode mode) {
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    clear_asset_session_state();
    reset_toggle_state();
    refresh_recommendations();
    rebuild_buttons();
    mark_dirty();
}

void TagEditorWidget::set_tags(const std::vector<std::string>& tags,
                               const std::vector<std::string>& anti_tags) {
    tags_.clear();
    anti_tags_.clear();
    clear_search();
    clear_asset_session_state();
    for (const auto& t : tags) {
        auto norm = normalize(t);
        if (!norm.empty()) tags_.insert(std::move(norm));
    }
    for (const auto& t : anti_tags) {
        auto norm = normalize(t);
        if (norm.empty()) continue;
        if (tags_.count(norm)) continue;
        anti_tags_.insert(std::move(norm));
    }
    show_browse_tags_ = false;
    refresh_recommendations();
    rebuild_buttons();
    reset_toggle_state();
    clear_drag_state();
    mark_dirty();
}

void TagEditorWidget::set_subject_asset_name(const std::string& asset_name) {
    if (subject_asset_name_ == asset_name) {
        return;
    }
    subject_asset_name_ = asset_name;
    if (mode_ == Mode::AssetInfoOverhaul) {
        refresh_recommendations();
        rebuild_buttons();
        mark_dirty();
    }
}

std::vector<std::string> TagEditorWidget::tags() const {
    return std::vector<std::string>(tags_.begin(), tags_.end());
}

std::vector<std::string> TagEditorWidget::anti_tags() const {
    return std::vector<std::string>(anti_tags_.begin(), anti_tags_.end());
}

void TagEditorWidget::set_on_changed(std::function<void(const std::vector<std::string>&,
                                                        const std::vector<std::string>&)> cb) {
    on_changed_ = std::move(cb);
}

void TagEditorWidget::set_rect(const SDL_Rect& r) {
    rect_ = r;
    mark_dirty();
}

int TagEditorWidget::height_for_width(int w) const {
    auto self = const_cast<TagEditorWidget*>(this);
    int width = std::max(40, w);
    return self->layout(width, 0, 0, false);
}

bool TagEditorWidget::handle_event(const SDL_Event& e) {
    if (mode_ == Mode::AssetInfoOverhaul) {
        return handle_event_asset_overhaul(e);
    }
    layout_if_needed();
    bool used = false;
    for (const auto& chip : tag_chips_) {
        handle_chip_click(chip, e, [this](const std::string& value) { remove_tag(value); }, used);
    }
    for (const auto& chip : anti_chips_) {
        handle_chip_click(chip, e, [this](const std::string& value) { remove_anti_tag(value); }, used);
    }
    for (const auto& chip : rec_tag_chips_) {
        handle_chip_click(chip, e, [this](const std::string& value) { add_tag(value); }, used);
    }
    for (const auto& chip : rec_anti_chips_) {
        handle_chip_click(chip, e, [this](const std::string& value) { add_anti_tag(value); }, used);
    }
    if (show_more_tags_btn_ && show_more_tags_btn_->rect().w > 0) {
        if (show_more_tags_btn_->handle_event(e)) {
            used = true;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                show_all_tag_recs_ = !show_all_tag_recs_;
                update_toggle_labels();
                mark_dirty();
            }
        }
    }
    if (show_more_anti_btn_ && show_more_anti_btn_->rect().w > 0) {
        if (show_more_anti_btn_->handle_event(e)) {
            used = true;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                show_all_anti_recs_ = !show_all_anti_recs_;
                update_toggle_labels();
                mark_dirty();
            }
        }
    }
    if (tag_search_box_ && tag_search_box_->rect().w > 0) {
        bool search_used = tag_search_box_->handle_event(e);
        if (search_used) {
            used = true;
            search_input_ = tag_search_box_->value();
            std::string lowered = to_lower(search_input_);
            if (lowered != search_query_) {
                search_query_ = std::move(lowered);
                update_search_filter();
                mark_dirty();
            }
        } else if (tag_search_box_->is_editing() && e.type == SDL_EVENT_KEY_DOWN &&
                   (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)) {
            used = true;
            add_search_text_as_tag();
        } else if (event_targets_rect(e, tag_search_box_->rect())) {
            used = true;
        }
    }
    if (add_tag_btn_ && add_tag_btn_->rect().w > 0) {
        if (add_tag_btn_->handle_event(e)) {
            used = true;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                add_search_text_as_tag();
            }
        }
    }
    if (add_as_anti_widget_ && add_as_anti_widget_->rect().w > 0) {
        if (add_as_anti_widget_->handle_event(e)) {
            used = true;
        }
    }
    if (browse_tags_widget_ && browse_tags_widget_->rect().w > 0) {
        if (browse_tags_widget_->handle_event(e)) {
            used = true;
        }
    }
    return used;
}

void TagEditorWidget::render(SDL_Renderer* r) const {
    if (mode_ == Mode::AssetInfoOverhaul) {
        render_asset_overhaul(r);
        return;
    }
    if (!r) return;
    const_cast<TagEditorWidget*>(this)->layout_if_needed();

    draw_label(r, "Tags", tags_label_rect_);
    draw_label(r, "Anti Tags", anti_label_rect_);
    if (rec_tags_label_rect_.w > 0) draw_label(r, "Tag Recommendations", rec_tags_label_rect_);
    if (rec_anti_label_rect_.w > 0) draw_label(r, "Anti Tag Recommendations", rec_anti_label_rect_);

    for (const auto& chip : tag_chips_) {
        if (chip.button) chip.button->render(r);
    }
    for (const auto& chip : anti_chips_) {
        if (chip.button) chip.button->render(r);
    }
    for (const auto& chip : rec_tag_chips_) {
        if (chip.button) chip.button->render(r);
    }
    for (const auto& chip : rec_anti_chips_) {
        if (chip.button) chip.button->render(r);
    }
    if (tag_search_box_ && tag_search_box_->rect().w > 0) {
        tag_search_box_->render(r);
    }
    if (add_tag_btn_ && add_tag_btn_->rect().w > 0) {
        add_tag_btn_->render(r);
    }
    if (show_more_tags_btn_ && show_more_tags_btn_->rect().w > 0) {
        show_more_tags_btn_->render(r);
    }
    if (show_more_anti_btn_ && show_more_anti_btn_->rect().w > 0) {
        show_more_anti_btn_->render(r);
    }
    if (add_as_anti_widget_ && add_as_anti_widget_->rect().w > 0) {
        add_as_anti_widget_->render(r);
    }
    if (browse_tags_btn_ && browse_tags_btn_->rect().w > 0) {
        browse_tags_btn_->render(r);
    }
}

void TagEditorWidget::rebuild_buttons() {
    tag_chips_.clear();
    anti_chips_.clear();
    rec_tag_chips_.clear();
    rec_anti_chips_.clear();

    const auto& tag_style = DMStyles::CreateButton();
    const auto& anti_style = DMStyles::DeleteButton();
    const auto& rec_style = mode_ == Mode::AssetInfoOverhaul
        ? recommendation_yellow_style()
        : DMStyles::ListButton();

    for (const auto& value : tags_) {
        Chip chip;
        chip.value = value;
        chip.kind = ChipListKind::Tags;
        chip.button = make_button(value, tag_style, kChipWidth);
        tag_chips_.push_back(std::move(chip));
    }

    for (const auto& value : anti_tags_) {
        Chip chip;
        chip.value = value;
        chip.kind = ChipListKind::AntiTags;
        chip.button = make_button(value, anti_style, kChipWidth);
        anti_chips_.push_back(std::move(chip));
    }

    if (mode_ == Mode::AssetInfoOverhaul && show_search_virtual_chip_ && !search_virtual_value_.empty()) {
        Chip chip;
        chip.value = search_virtual_value_;
        chip.kind = ChipListKind::SearchInputVirtual;
        chip.button = make_button(search_virtual_value_, recommendation_orange_style(), kRecommendChipWidth);
        rec_tag_chips_.push_back(std::move(chip));
    }

    for (const auto& value : recommended_tags_) {
        Chip chip;
        chip.value = value;
        chip.kind = ChipListKind::Recommended;
        const std::string label = mode_ == Mode::AssetInfoOverhaul ? value : "+ " + value;
        chip.button = make_button(label, rec_style, kRecommendChipWidth);
        rec_tag_chips_.push_back(std::move(chip));
    }

    for (const auto& value : recommended_anti_) {
        Chip chip;
        chip.value = value;
        chip.kind = ChipListKind::Recommended;
        chip.button = make_button("- " + value, rec_style, kRecommendChipWidth);
        rec_anti_chips_.push_back(std::move(chip));
    }

    if (mode_ == Mode::Legacy) {
        update_search_filter();
    }
}

void TagEditorWidget::refresh_recommendations() {
    if (mode_ == Mode::AssetInfoOverhaul) {
        refresh_recommendations_asset_mode();
        return;
    }

    const auto& dataset = tag_dataset();
    std::unordered_map<std::string, TagStats> stats;

    for (const auto& entry : dataset) {
        bool shares_tag = contains_any(tags_, entry.tags);
        bool shares_anti = contains_any(anti_tags_, entry.anti_tags);
        bool shares_cross = false;
        if (!shares_cross) {
            shares_cross = contains_any(anti_tags_, entry.tags);
        }
        if (!shares_cross) {
            shares_cross = contains_any(tags_, entry.anti_tags);
        }

        for (const auto& value : entry.tags) {
            auto& st = stats[value];
            st.tag_count++;
            if (shares_tag) st.co_with_tags++;
            if (shares_anti) st.co_with_anti++;
            if (shares_cross) st.cross_hits++;
        }
        for (const auto& value : entry.anti_tags) {
            auto& st = stats[value];
            st.anti_count++;
            if (shares_tag) st.co_with_tags++;
            if (shares_anti) st.co_with_anti++;
            if (shares_cross) st.cross_hits++;
        }
    }

    for (const auto& value : tags_) {
        auto& st = stats[value];
        st.tag_count++;
        st.co_with_tags += 2;
    }
    for (const auto& value : anti_tags_) {
        auto& st = stats[value];
        st.anti_count++;
        st.co_with_anti += 2;
    }

    std::set<std::string> candidates(TagLibrary::instance().tags().begin(), TagLibrary::instance().tags().end());
    for (const auto& [value, _] : stats) {
        candidates.insert(value);
    }

    struct CandidateScore {
        std::string value;
        double tag_score = 0.0;
        double anti_score = 0.0;
        double tie_break = 0.0;
};

    std::vector<CandidateScore> scores;
    scores.reserve(candidates.size());
    for (const auto& value : candidates) {
        const auto it = stats.find(value);
        TagStats st;
        if (it != stats.end()) st = it->second;

        CandidateScore cs;
        cs.value = value;
        cs.tag_score = st.tag_count + 0.5 * st.anti_count + 2.0 * st.co_with_tags + 1.2 * st.co_with_anti + 0.5 * st.cross_hits;
        cs.anti_score = st.anti_count + 0.5 * st.tag_count + 2.0 * st.co_with_anti + 1.2 * st.co_with_tags + 0.5 * st.cross_hits;
        cs.tie_break = st.tag_count + st.anti_count + st.co_with_tags + st.co_with_anti + st.cross_hits;
        scores.push_back(std::move(cs));
    }

    auto make_list = [&](auto selector) {
        std::vector<std::string> output;
        std::vector<std::string> zero_scores;
        auto sorted = scores;
        std::sort(sorted.begin(), sorted.end(), [&](const CandidateScore& a, const CandidateScore& b) {
            double sa = selector(a);
            double sb = selector(b);
            if (sa == sb) {
                if (a.tie_break == b.tie_break) return a.value < b.value;
                return a.tie_break > b.tie_break;
            }
            return sa > sb;
        });
        for (const auto& cand : sorted) {
            if (tags_.count(cand.value) || anti_tags_.count(cand.value)) continue;
            double score = selector(cand);
            if (score > 0.0) {
                output.push_back(cand.value);
            } else {
                zero_scores.push_back(cand.value);
            }
            if (output.size() >= kMaxRecommendations) break;
        }
        if (output.size() < kMaxRecommendations) {
            std::sort(zero_scores.begin(), zero_scores.end());
            for (const auto& value : zero_scores) {
                if (tags_.count(value) || anti_tags_.count(value)) continue;
                output.push_back(value);
                if (output.size() >= kMaxRecommendations) break;
            }
        }
        return output;
};

    recommendation_context_scores_.clear();
    recommendation_popularity_.clear();
    for (const auto& cand : scores) {
        const int popularity = static_cast<int>(cand.tie_break);
        recommendation_context_scores_[cand.value] = cand.tag_score;
        recommendation_popularity_[cand.value] = popularity;
    }

    recommended_tags_ = make_list([](const CandidateScore& c) { return c.tag_score; });
    recommended_anti_ = make_list([](const CandidateScore& c) { return c.anti_score; });
}

void TagEditorWidget::mark_dirty() {
    layout_dirty_ = true;
}

void TagEditorWidget::layout_if_needed() const {
    if (!layout_dirty_) return;
    auto self = const_cast<TagEditorWidget*>(this);
    int width = std::max(40, rect_.w);
    self->layout(width, rect_.x, rect_.y, true);
    self->layout_dirty_ = false;
}

int TagEditorWidget::layout(int width, int origin_x, int origin_y, bool apply) {
    if (mode_ == Mode::AssetInfoOverhaul) {
        return layout_asset_overhaul(width, origin_x, origin_y, apply);
    }
    const int pad = DMSpacing::small_gap();
    const int label_gap = DMSpacing::label_gap();
    const int section_gap = DMSpacing::item_gap();
    int y = origin_y + pad;
    int label_h = label_height();

    if (apply) {
        tags_label_rect_ = SDL_Rect{ origin_x, y, width, label_h };
    }
    y += label_h + label_gap;
    y = layout_grid(tag_chips_, width, origin_x, y, apply);
    y += section_gap;

    if (apply) {
        anti_label_rect_ = SDL_Rect{ origin_x, y, width, label_h };
    }
    y += label_h + label_gap;
    y = layout_grid(anti_chips_, width, origin_x, y, apply);
    y += section_gap;

    bool has_tag_recs = !rec_tag_chips_.empty();
    bool has_anti_recs = !rec_anti_chips_.empty();

    if (has_tag_recs) {
        if (apply) {
            rec_tags_label_rect_ = SDL_Rect{ origin_x, y, width, label_h };
        }
        y += label_h + label_gap;
    } else if (apply) {
        rec_tags_label_rect_ = SDL_Rect{0,0,0,0};
    }

    if (!tag_search_box_) {
        tag_search_box_ = std::make_unique<DMTextBox>("", search_input_);
    }
    if (!add_tag_btn_) {
        add_tag_btn_ = std::make_unique<DMButton>("+", &DMStyles::CreateButton(), 36, DMTextBox::height());
    }
    if (!add_as_anti_checkbox_) {
        add_as_anti_checkbox_ = std::make_unique<DMCheckbox>("As Anti", false);
        add_as_anti_widget_ = std::make_unique<CheckboxWidget>(add_as_anti_checkbox_.get());
    }
    if (!browse_tags_btn_) {
        browse_tags_btn_ = std::make_unique<DMButton>("Browse", &DMStyles::WarnButton(), 80, DMTextBox::height());
        browse_tags_widget_ = std::make_unique<ButtonWidget>(browse_tags_btn_.get(), [this]() {
            show_browse_tags_ = !show_browse_tags_;
            update_browse_mode();
            mark_dirty();
        });
    }
    tag_search_box_->set_value(search_input_);

    int controls_y = y;
    int button_gap = DMSpacing::small_gap();
    int desired_button = std::min(48, std::max(28, width / 5 + 20));
    int button_width = std::min(width, desired_button);
    int min_search = 60;
    int search_width = width - button_width - button_gap;
    if (search_width < min_search) {
        int deficit = min_search - search_width;
        button_width = std::max(24, button_width - deficit);
        if (button_width > width) button_width = width;
        search_width = width - button_width - button_gap;
    }
    if (search_width < 0) {
        search_width = 0;
        button_width = width;
    }
    int search_height = 0;
    if (search_width > 0) {
        search_height = std::max(tag_search_box_->height_for_width(search_width), DMTextBox::height());
    }
    int button_height = DMButton::height();
    int button_offset = 0;
    if (search_width > 0 && search_height > DMTextBox::height()) {
        button_offset = (search_height - DMTextBox::height()) / 2;
    }
    if (apply) {
        if (search_width > 0) {
            tag_search_box_->set_rect(SDL_Rect{ origin_x, controls_y, search_width, search_height });
        } else {
            tag_search_box_->set_rect(SDL_Rect{0,0,0,0});
        }
        int add_x = origin_x + (search_width > 0 ? search_width + button_gap : 0);
        int final_button_width = std::min(width, std::max(24, button_width));
        int button_y = controls_y + button_offset;
        add_tag_btn_->set_rect(SDL_Rect{ add_x, button_y, final_button_width, button_height });
    }

    int checkbox_y = controls_y + search_height + DMSpacing::small_gap();
    int checkbox_spacing = DMSpacing::small_gap();

    if (add_as_anti_checkbox_) {
        int checkbox_width = add_as_anti_checkbox_->preferred_width();
        if (checkbox_width > 0) {
            add_as_anti_checkbox_->set_rect(SDL_Rect{ origin_x, checkbox_y, checkbox_width, DMCheckbox::height() });
            if (add_as_anti_widget_) {
                add_as_anti_widget_->set_rect(add_as_anti_checkbox_->rect());
            }
        }
    }

    int browse_x = origin_x;
    if (add_as_anti_checkbox_ && add_as_anti_checkbox_->rect().w > 0) {
        browse_x = origin_x + add_as_anti_checkbox_->rect().w + checkbox_spacing;
    }

    if (browse_tags_btn_) {
        int browse_width = std::min(browse_tags_btn_->preferred_width(), width - (browse_x - origin_x));
        browse_width = std::max(browse_width, 60);
        if (apply && browse_width > 0) {
            browse_tags_btn_->set_rect(SDL_Rect{ browse_x, checkbox_y, browse_width, DMButton::height() });
        }
    }

    int controls_height = (checkbox_y - controls_y) + DMButton::height();
    int total_height = controls_y + controls_height;
    y = total_height + DMSpacing::item_gap();

    if (has_tag_recs) {
        size_t matches = filtered_tag_order_.empty() && search_query_.empty() ? rec_tag_chips_.size() : filtered_tag_order_.size();
        size_t visible_tags = show_all_tag_recs_ ? matches : std::min(kRecommendationPreviewCount, matches);
        const auto* display_order = filtered_tag_order_.empty() && search_query_.empty() ? nullptr : &filtered_tag_order_;
        y = layout_grid(rec_tag_chips_, width, origin_x, y, apply, visible_tags, display_order);

        bool show_tag_toggle = matches > visible_tags || show_all_tag_recs_;
        int toggle_gap = DMSpacing::small_gap();
        if (show_tag_toggle) {
            if (!show_more_tags_btn_) {
                show_more_tags_btn_ = make_button("Show More", DMStyles::WarnButton(), kRecommendChipWidth);
            }
            if (apply) {
                update_toggle_labels();
                int button_w = std::max(80, std::min(width, kRecommendChipWidth));
                show_more_tags_btn_->set_rect(SDL_Rect{ origin_x, y + toggle_gap, button_w, DMButton::height() });
            }
            y += toggle_gap + DMButton::height();
        } else if (apply) {
            if (show_more_tags_btn_) show_more_tags_btn_->set_rect(SDL_Rect{0,0,0,0});
        }
        y += section_gap;
    } else if (apply) {
        if (show_more_tags_btn_) show_more_tags_btn_->set_rect(SDL_Rect{0,0,0,0});
    }

    if (has_anti_recs) {
        if (apply) {
            rec_anti_label_rect_ = SDL_Rect{ origin_x, y, width, label_h };
        }
        y += label_h + label_gap;
        size_t visible_anti = show_all_anti_recs_ ? rec_anti_chips_.size() : std::min(kRecommendationPreviewCount, rec_anti_chips_.size());
        y = layout_grid(rec_anti_chips_, width, origin_x, y, apply, visible_anti);

        bool show_anti_toggle = show_all_anti_recs_ || rec_anti_chips_.size() > kRecommendationPreviewCount;
        if (show_anti_toggle) {
            if (!show_more_anti_btn_) {
                show_more_anti_btn_ = make_button("Show More", DMStyles::WarnButton(), kRecommendChipWidth);
            }
            int toggle_gap = DMSpacing::small_gap();
            if (apply) {
                update_toggle_labels();
                int button_w = std::max(80, std::min(width, kRecommendChipWidth));
                show_more_anti_btn_->set_rect(SDL_Rect{ origin_x, y + toggle_gap, button_w, DMButton::height() });
            }
            y += toggle_gap + DMButton::height();
        } else if (apply && show_more_anti_btn_) {
            show_more_anti_btn_->set_rect(SDL_Rect{0,0,0,0});
        }
        y += section_gap;
    } else if (apply) {
        rec_anti_label_rect_ = SDL_Rect{0,0,0,0};
        if (show_more_anti_btn_) show_more_anti_btn_->set_rect(SDL_Rect{0,0,0,0});
    }

    y += pad;
    return y - origin_y;
}

int TagEditorWidget::layout_asset_overhaul(int width, int origin_x, int origin_y, bool apply) {
    const int pad = DMSpacing::small_gap();
    const int label_gap = DMSpacing::label_gap();
    const int section_gap = DMSpacing::item_gap();
    const int label_h = label_height();
    int y = origin_y + pad;

    if (!tag_search_box_) {
        tag_search_box_ = std::make_unique<DMTextBox>("", search_input_);
    }
    tag_search_box_->set_value(search_input_);

    if (apply) {
        rec_anti_label_rect_ = SDL_Rect{0,0,0,0};
        if (show_more_tags_btn_) show_more_tags_btn_->set_rect(SDL_Rect{0,0,0,0});
        if (show_more_anti_btn_) show_more_anti_btn_->set_rect(SDL_Rect{0,0,0,0});
        if (add_tag_btn_) add_tag_btn_->set_rect(SDL_Rect{0,0,0,0});
        if (add_as_anti_checkbox_) add_as_anti_checkbox_->set_rect(SDL_Rect{0,0,0,0});
        if (add_as_anti_widget_) add_as_anti_widget_->set_rect(SDL_Rect{0,0,0,0});
        if (browse_tags_btn_) browse_tags_btn_->set_rect(SDL_Rect{0,0,0,0});
        if (browse_tags_widget_) browse_tags_widget_->set_rect(SDL_Rect{0,0,0,0});
        tags_drop_rect_ = SDL_Rect{0,0,0,0};
        anti_drop_rect_ = SDL_Rect{0,0,0,0};
        rec_drop_rect_ = SDL_Rect{0,0,0,0};
    }

    const int tags_label_y = y;
    if (apply) {
        tags_label_rect_ = SDL_Rect{origin_x, y, width, label_h};
    }
    y += label_h + label_gap;
    const int tags_chip_start = y;
    y = layout_grid(tag_chips_, width, origin_x, y, apply, kMaxRecommendations, nullptr);
    const int tags_bottom = std::max(y, tags_chip_start + kAssetDropMinHeight);
    if (apply) {
        tags_drop_rect_ = SDL_Rect{origin_x, tags_label_y, width, std::max(1, tags_bottom - tags_label_y)};
    }
    y = tags_bottom + section_gap;

    const int anti_label_y = y;
    if (apply) {
        anti_label_rect_ = SDL_Rect{origin_x, y, width, label_h};
    }
    y += label_h + label_gap;
    const int anti_chip_start = y;
    y = layout_grid(anti_chips_, width, origin_x, y, apply, kMaxRecommendations, nullptr);
    const int anti_bottom = std::max(y, anti_chip_start + kAssetDropMinHeight);
    if (apply) {
        anti_drop_rect_ = SDL_Rect{origin_x, anti_label_y, width, std::max(1, anti_bottom - anti_label_y)};
    }
    y = anti_bottom + section_gap;

    const int rec_label_y = y;
    if (apply) {
        rec_tags_label_rect_ = SDL_Rect{origin_x, y, width, label_h};
    }
    y += label_h + label_gap;
    const int rec_chip_start = y;
    y = layout_grid(rec_tag_chips_, width, origin_x, y, apply, kMaxRecommendations, nullptr);
    const int rec_bottom = std::max(y, rec_chip_start + kAssetDropMinHeight);
    if (apply) {
        rec_drop_rect_ = SDL_Rect{origin_x, rec_label_y, width, std::max(1, rec_bottom - rec_label_y)};
    }
    y = rec_bottom + section_gap;

    const int search_height = std::max(tag_search_box_->height_for_width(width), DMTextBox::height());
    if (apply) {
        tag_search_box_->set_rect(SDL_Rect{origin_x, y, width, search_height});
    }
    y += search_height + pad;
    return y - origin_y;
}

int TagEditorWidget::layout_grid(std::vector<Chip>& chips, int width, int origin_x, int start_y, bool apply,
                                size_t visible_count, const std::vector<size_t>* display_order) {
    size_t available = display_order ? display_order->size() : chips.size();
    size_t count = std::min(visible_count, available);
    if (apply) {
        for (auto& chip : chips) {
            if (chip.button) chip.button->set_rect(SDL_Rect{0,0,0,0});
        }
    }
    if (count == 0) {
        return start_y;
    }
    const int gap = DMSpacing::small_gap();
    int chip_width = &chips == &rec_tag_chips_ || &chips == &rec_anti_chips_ ? kRecommendChipWidth : kChipWidth;
    chip_width = std::min(chip_width, width);
    chip_width = std::max(chip_width, 80);
    int columns = std::max(1, (width + gap) / (chip_width + gap));
    int chip_height = DMButton::height();

    for (size_t i = 0; i < count; ++i) {
        size_t idx = display_order ? (*display_order)[i] : i;
        if (idx >= chips.size()) continue;
        int row = static_cast<int>(i / columns);
        int col = static_cast<int>(i % columns);
        int x = origin_x + col * (chip_width + gap);
        int y = start_y + row * (chip_height + gap);
        if (apply && chips[idx].button) {
            chips[idx].button->set_rect(SDL_Rect{ x, y, chip_width, chip_height });
        }
    }

    int rows = static_cast<int>((count + columns - 1) / columns);
    if (rows <= 0) return start_y;
    int total_height = rows * chip_height + (rows - 1) * gap;
    return start_y + total_height;
}

int TagEditorWidget::label_height() {
    static int cached = 0;
    if (cached > 0) return cached;
    const DMLabelStyle& style = DMStyles::Label();
    TTF_Font* font = style.open_font();
    if (!font) {
        cached = style.font_size;
        return cached;
    }
    int w = 0;
    int h = 0;
    ttf_util::GetStringSize(font, "Tags", &w, &h);
    TTF_CloseFont(font);
    cached = h;
    return cached;
}

void TagEditorWidget::draw_label(SDL_Renderer* r, const std::string& text, const SDL_Rect& rect) const {
    if (rect.w <= 0 && rect.h <= 0) return;
    const DMLabelStyle& style = DMStyles::Label();
    TTF_Font* font = style.open_font();
    if (!font) return;
    SDL_Surface* surf = ttf_util::RenderTextBlended(font, text.c_str(), style.color);
    if (surf) {
        SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
        if (tex) {
            SDL_Rect dst{ rect.x, rect.y, surf->w, surf->h };
            sdl_render::Texture(r, tex, nullptr, &dst);
            SDL_DestroyTexture(tex);
        }
        SDL_DestroySurface(surf);
    }
    TTF_CloseFont(font);
}

void TagEditorWidget::handle_chip_click(const Chip& chip, const SDL_Event& e,
                                        const std::function<void(const std::string&)>& on_click,
                                        bool& used) {
    if (!chip.button) return;
    if (chip.button->handle_event(e)) {
        used = true;
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            on_click(chip.value);
        }
    }
}

void TagEditorWidget::add_tag(const std::string& value) {
    auto norm = normalize(value);
    if (norm.empty()) return;
    if (mode_ == Mode::AssetInfoOverhaul) {
        session_recommended_.erase(norm);
        hidden_recommended_.erase(norm);
    }
    bool changed = false;
    if (anti_tags_.erase(norm) > 0) changed = true;
    if (tags_.insert(norm).second) changed = true;
    if (changed) {
        refresh_recommendations();
        rebuild_buttons();
        mark_dirty();
        notify_changed();
    }
}

void TagEditorWidget::add_anti_tag(const std::string& value) {
    auto norm = normalize(value);
    if (norm.empty()) return;
    if (mode_ == Mode::AssetInfoOverhaul) {
        session_recommended_.erase(norm);
        hidden_recommended_.erase(norm);
    }
    bool changed = false;
    if (tags_.erase(norm) > 0) changed = true;
    if (anti_tags_.insert(norm).second) changed = true;
    if (changed) {
        refresh_recommendations();
        rebuild_buttons();
        mark_dirty();
        notify_changed();
    }
}

void TagEditorWidget::remove_tag(const std::string& value) {
    auto norm = normalize(value);
    if (tags_.erase(norm) > 0) {
        refresh_recommendations();
        rebuild_buttons();
        mark_dirty();
        notify_changed();
    }
}

void TagEditorWidget::remove_anti_tag(const std::string& value) {
    auto norm = normalize(value);
    if (anti_tags_.erase(norm) > 0) {
        refresh_recommendations();
        rebuild_buttons();
        mark_dirty();
        notify_changed();
    }
}

std::string TagEditorWidget::normalize(const std::string& value) {
    return tag_utils::normalize(value);
}

void TagEditorWidget::notify_changed() {
    if (!on_changed_) return;
    on_changed_(tags(), anti_tags());
}

void TagEditorWidget::reset_toggle_state() {
    show_all_tag_recs_ = false;
    show_all_anti_recs_ = false;
    update_toggle_labels();
    if (show_more_tags_btn_) show_more_tags_btn_->set_rect(SDL_Rect{0,0,0,0});
    if (show_more_anti_btn_) show_more_anti_btn_->set_rect(SDL_Rect{0,0,0,0});
    if (tag_search_box_) tag_search_box_->set_rect(SDL_Rect{0,0,0,0});
    if (add_tag_btn_) add_tag_btn_->set_rect(SDL_Rect{0,0,0,0});
}

void TagEditorWidget::update_toggle_labels() {
    if (show_more_tags_btn_) {
        show_more_tags_btn_->set_text(show_all_tag_recs_ ? "Show Less" : "Show More");
    }
    if (show_more_anti_btn_) {
        show_more_anti_btn_->set_text(show_all_anti_recs_ ? "Show Less" : "Show More");
    }
}

void TagEditorWidget::update_search_filter() {
    if (mode_ == Mode::AssetInfoOverhaul) {
        rebuild_recommended_from_search();
        rebuild_buttons();
        return;
    }
    filtered_tag_order_.clear();
    if (rec_tag_chips_.empty()) return;
    if (search_query_.empty()) {
        filtered_tag_order_.reserve(rec_tag_chips_.size());
        for (size_t i = 0; i < rec_tag_chips_.size(); ++i) {
            filtered_tag_order_.push_back(i);
        }
        return;
    }
    for (size_t i = 0; i < rec_tag_chips_.size(); ++i) {
        std::string lowered = to_lower(rec_tag_chips_[i].value);
        if (lowered.find(search_query_) != std::string::npos) {
            filtered_tag_order_.push_back(i);
        }
    }
}

void TagEditorWidget::clear_search() {
    search_input_.clear();
    search_query_.clear();
    if (tag_search_box_) tag_search_box_->set_value("");
    update_search_filter();
}

void TagEditorWidget::add_search_text_as_tag() {
    if (mode_ == Mode::AssetInfoOverhaul) {
        return;
    }
    auto normalized = normalize(search_input_);
    if (normalized.empty()) return;
    bool add_as_anti = add_as_anti_checkbox_ && add_as_anti_checkbox_->value();
    if (add_as_anti) {
        if (anti_tags_.count(normalized)) return;
        add_anti_tag(normalized);
    } else {
        if (tags_.count(normalized)) return;
        add_tag(normalized);
    }
    clear_search();
}

void TagEditorWidget::update_browse_mode() {
    if (mode_ == Mode::AssetInfoOverhaul) {
        return;
    }
    if (show_browse_tags_) {

        std::vector<std::string> all_tags = TagLibrary::instance().tags();
        recommended_tags_.clear();
        recommended_anti_.clear();
        for (const auto& tag : all_tags) {
            if (tags_.count(tag)) continue;
            recommended_tags_.push_back(tag);
        }
        for (const auto& tag : all_tags) {
            if (anti_tags_.count(tag)) continue;
            recommended_anti_.push_back(tag);
        }
    } else {

        refresh_recommendations();
    }
    rebuild_buttons();
    mark_dirty();
}

bool TagEditorWidget::starts_with_casefold(const std::string& value, const std::string& prefix) {
    if (prefix.size() > value.size()) return false;
    return std::equal(prefix.begin(), prefix.end(), value.begin());
}

void TagEditorWidget::refresh_recommendations_asset_mode() {
    recommended_anti_.clear();
    recommendation_context_scores_.clear();
    recommendation_popularity_.clear();

    const auto& corpus = manifest_tag_corpus();
    if (corpus.unique_tags.empty()) {
        rebuild_recommended_from_search();
        return;
    }

    std::vector<SimilarAssetScore> top_similar_assets =
        collect_top_similar_assets(corpus, tags_, anti_tags_, subject_asset_name_, true);

    std::unordered_map<std::string, double> similarity_likelihood;
    double max_similarity_likelihood = 0.0;
    for (const auto& similar_asset : top_similar_assets) {
        if (!similar_asset.profile || similar_asset.similarity <= 0.0) {
            continue;
        }
        for (const auto& tag : similar_asset.profile->tags) {
            double& score = similarity_likelihood[tag];
            score += similar_asset.similarity;
            if (score > max_similarity_likelihood) {
                max_similarity_likelihood = score;
            }
        }
    }

    const double inv_max_popularity = corpus.max_popularity > 0
        ? 1.0 / static_cast<double>(corpus.max_popularity)
        : 0.0;
    const double inv_max_similarity = max_similarity_likelihood > 0.0
        ? 1.0 / max_similarity_likelihood
        : 0.0;

    std::unordered_set<std::string> candidate_pool;
    for (const auto& profile : corpus.profiles) {
        if (!subject_asset_name_.empty() && profile.asset_name == subject_asset_name_) {
            continue;
        }
        candidate_pool.insert(profile.tags.begin(), profile.tags.end());
        candidate_pool.insert(profile.anti_tags.begin(), profile.anti_tags.end());
    }

    for (const auto& candidate : corpus.unique_tags) {
        if (!subject_asset_name_.empty() && candidate_pool.count(candidate) == 0) {
            continue;
        }
        if (tags_.count(candidate) || anti_tags_.count(candidate)) {
            continue;
        }

        const auto pop_it = corpus.popularity.find(candidate);
        const int popularity = pop_it != corpus.popularity.end() ? pop_it->second : 0;
        const auto similar_it = similarity_likelihood.find(candidate);
        const double similarity = similar_it != similarity_likelihood.end() ? similar_it->second : 0.0;

        const double normalized_popularity = static_cast<double>(popularity) * inv_max_popularity;
        const double normalized_similarity = similarity * inv_max_similarity;
        const double score = kAssetSimilarityWeight * normalized_similarity +
                             kAssetPopularityWeight * normalized_popularity;

        recommendation_context_scores_[candidate] = score;
        recommendation_popularity_[candidate] = popularity;
    }

    rebuild_recommended_from_search();
}

void TagEditorWidget::rebuild_recommended_from_search() {
    recommended_tags_.clear();
    show_search_virtual_chip_ = false;
    search_virtual_value_.clear();

    struct RankedCandidate {
        std::string value;
        int match_tier = 3;  // 0=exact, 1=prefix, 2=contains, 3=non-match
        double non_text_score = 0.0;
        int popularity = 0;
    };

    auto classify_match_tier = [&](const std::string& value) {
        if (search_query_.empty()) {
            return 0;
        }
        if (value == search_query_) {
            return 0;
        }
        if (starts_with_casefold(value, search_query_)) {
            return 1;
        }
        if (value.find(search_query_) != std::string::npos) {
            return 2;
        }
        return 3;
    };

    std::vector<RankedCandidate> ranked;
    ranked.reserve(recommendation_context_scores_.size());
    for (const auto& [value, contextual_score] : recommendation_context_scores_) {
        if (value.empty()) continue;
        if (tags_.count(value) || anti_tags_.count(value)) continue;
        if (hidden_recommended_.count(value)) continue;

        const auto pop_it = recommendation_popularity_.find(value);
        const int popularity = pop_it != recommendation_popularity_.end() ? pop_it->second : 0;
        ranked.push_back(RankedCandidate{
            value,
            classify_match_tier(value),
            contextual_score,
            popularity
        });
    }

    std::sort(ranked.begin(), ranked.end(), [](const RankedCandidate& a, const RankedCandidate& b) {
        if (a.match_tier != b.match_tier) return a.match_tier < b.match_tier;
        if (a.non_text_score != b.non_text_score) return a.non_text_score > b.non_text_score;
        if (a.popularity != b.popularity) return a.popularity > b.popularity;
        return a.value < b.value;
    });

    for (const auto& item : ranked) {
        recommended_tags_.push_back(item.value);
        if (recommended_tags_.size() >= kAssetRecommendationLimit) {
            break;
        }
    }

    std::string normalized_search = normalize(search_input_);
    if (!normalized_search.empty() &&
        !tags_.count(normalized_search) &&
        !anti_tags_.count(normalized_search) &&
        !hidden_recommended_.count(normalized_search)) {
        show_search_virtual_chip_ = true;
        search_virtual_value_ = normalized_search;
        recommended_tags_.erase(
            std::remove(recommended_tags_.begin(), recommended_tags_.end(), normalized_search),
            recommended_tags_.end());
    }
}

bool TagEditorWidget::handle_event_asset_overhaul(const SDL_Event& e) {
    layout_if_needed();
    bool used = false;

    auto update_search_state = [&]() {
        std::string lowered = to_lower(search_input_);
        if (lowered != search_query_) {
            search_query_ = std::move(lowered);
            update_search_filter();
            mark_dirty();
        }
    };

    if (tag_search_box_ && tag_search_box_->rect().w > 0) {
        bool search_used = tag_search_box_->handle_event(e);
        if (search_used) {
            used = true;
            search_input_ = tag_search_box_->value();
            update_search_state();
        } else if (event_targets_rect(e, tag_search_box_->rect())) {
            used = true;
        }
    }

    auto pump_chip_widgets = [&](auto& chips) {
        for (auto& chip : chips) {
            if (chip.button) {
                chip.button->handle_event(e);
            }
        }
    };
    pump_chip_widgets(tag_chips_);
    pump_chip_widgets(anti_chips_);
    pump_chip_widgets(rec_tag_chips_);

    auto pointer_from_event = [&](SDL_Point& out) {
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            out.x = static_cast<int>(std::lround(e.motion.x));
            out.y = static_cast<int>(std::lround(e.motion.y));
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            out.x = static_cast<int>(std::lround(e.button.x));
            out.y = static_cast<int>(std::lround(e.button.y));
            return true;
        }
        return false;
    };

    SDL_Point pointer{0,0};
    const bool has_pointer = pointer_from_event(pointer);

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && has_pointer) {
        bool search_hit = tag_search_box_ && SDL_PointInRect(&pointer, &tag_search_box_->rect());
        if (!search_hit) {
            if (Chip* chip = hit_chip_at(pointer)) {
                clear_drag_state();
                drag_state_.pressed = true;
                drag_state_.start_point = pointer;
                drag_state_.pointer = pointer;
                drag_state_.value = chip->value;
                drag_state_.source_kind = chip->kind;
                drag_state_.hover_target = chip->kind;
                used = true;
            } else if (drop_target_at(pointer) != ChipListKind::None) {
                used = true;
            }
        }
    } else if (e.type == SDL_EVENT_MOUSE_MOTION && has_pointer && drag_state_.pressed) {
        drag_state_.pointer = pointer;
        if (!drag_state_.dragging) {
            const float dx = static_cast<float>(pointer.x - drag_state_.start_point.x);
            const float dy = static_cast<float>(pointer.y - drag_state_.start_point.y);
            if (std::sqrt(dx * dx + dy * dy) >= kDragStartThresholdPx) {
                drag_state_.dragging = true;
            }
        }
        if (drag_state_.dragging) {
            drag_state_.hover_target = drop_target_at(pointer);
            mark_dirty();
        }
        used = true;
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT && has_pointer) {
        if (drag_state_.pressed) {
            if (drag_state_.dragging) {
                move_chip_between_lists(drag_state_.value, drag_state_.source_kind, drop_target_at(pointer));
            } else if (e.button.clicks >= 2) {
                if (const Chip* chip = hit_chip_at(pointer)) {
                    if (chip->value == drag_state_.value && chip->kind == drag_state_.source_kind) {
                        handle_chip_double_click(*chip);
                    }
                }
            } else if (const Chip* chip = hit_chip_at(pointer)) {
                if (chip->value == drag_state_.value && chip->kind == drag_state_.source_kind) {
                    if (chip->kind == ChipListKind::Recommended || chip->kind == ChipListKind::SearchInputVirtual) {
                        move_chip_between_lists(chip->value, chip->kind, ChipListKind::Tags);
                    }
                }
            }
            clear_drag_state();
            used = true;
        } else if (drop_target_at(pointer) != ChipListKind::None) {
            used = true;
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT && has_pointer) {
        if (const Chip* chip = hit_chip_at(pointer)) {
            if (chip->kind == ChipListKind::Recommended || chip->kind == ChipListKind::SearchInputVirtual) {
                move_chip_between_lists(chip->value, chip->kind, ChipListKind::AntiTags);
                used = true;
            }
        }
    } else if (e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
        if (drag_state_.pressed) {
            clear_drag_state();
            mark_dirty();
            used = true;
        }
    }

    return used;
}

void TagEditorWidget::render_asset_overhaul(SDL_Renderer* r) const {
    if (!r) return;
    const_cast<TagEditorWidget*>(this)->layout_if_needed();

    draw_label(r, "Tags", tags_label_rect_);
    draw_label(r, "Anti Tags", anti_label_rect_);
    draw_label(r, "Recommended Tags", rec_tags_label_rect_);

    if (drag_state_.dragging) {
        SDL_Rect target_rect{0,0,0,0};
        switch (drag_state_.hover_target) {
            case ChipListKind::Tags: target_rect = tags_drop_rect_; break;
            case ChipListKind::AntiTags: target_rect = anti_drop_rect_; break;
            case ChipListKind::Recommended: target_rect = rec_drop_rect_; break;
            default: break;
        }
        if (target_rect.w > 0 && target_rect.h > 0) {
            SDL_Color highlight = DMStyles::HighlightColor();
            highlight.a = static_cast<Uint8>(std::clamp<int>(highlight.a / 2, 0, 255));
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r, highlight.r, highlight.g, highlight.b, highlight.a);
            sdl_render::FillRect(r, &target_rect);
        }
    }

    for (const auto& chip : tag_chips_) {
        if (chip.button) chip.button->render(r);
    }
    for (const auto& chip : anti_chips_) {
        if (chip.button) chip.button->render(r);
    }
    for (const auto& chip : rec_tag_chips_) {
        if (chip.button) chip.button->render(r);
    }
    if (tag_search_box_ && tag_search_box_->rect().w > 0) {
        tag_search_box_->render(r);
    }
}

TagEditorWidget::Chip* TagEditorWidget::hit_chip_at(const SDL_Point& p) {
    auto hit_in = [&](std::vector<Chip>& chips) -> Chip* {
        for (auto& chip : chips) {
            if (!chip.button) continue;
            const SDL_Rect& rect = chip.button->rect();
            if (rect.w <= 0 || rect.h <= 0) continue;
            if (SDL_PointInRect(&p, &rect)) {
                return &chip;
            }
        }
        return nullptr;
    };
    if (Chip* chip = hit_in(tag_chips_)) return chip;
    if (Chip* chip = hit_in(anti_chips_)) return chip;
    return hit_in(rec_tag_chips_);
}

const TagEditorWidget::Chip* TagEditorWidget::hit_chip_at(const SDL_Point& p) const {
    return const_cast<TagEditorWidget*>(this)->hit_chip_at(p);
}

TagEditorWidget::ChipListKind TagEditorWidget::drop_target_at(const SDL_Point& p) const {
    if (tags_drop_rect_.w > 0 && tags_drop_rect_.h > 0 && SDL_PointInRect(&p, &tags_drop_rect_)) {
        return ChipListKind::Tags;
    }
    if (anti_drop_rect_.w > 0 && anti_drop_rect_.h > 0 && SDL_PointInRect(&p, &anti_drop_rect_)) {
        return ChipListKind::AntiTags;
    }
    if (rec_drop_rect_.w > 0 && rec_drop_rect_.h > 0 && SDL_PointInRect(&p, &rec_drop_rect_)) {
        return ChipListKind::Recommended;
    }
    return ChipListKind::None;
}

void TagEditorWidget::clear_drag_state() {
    drag_state_ = DragState{};
}

bool TagEditorWidget::move_chip_between_lists(const std::string& value, ChipListKind, ChipListKind target) {
    std::string norm = normalize(value);
    if (norm.empty() || target == ChipListKind::None) {
        return false;
    }
    if (target == ChipListKind::SearchInputVirtual) {
        target = ChipListKind::Recommended;
    }

    switch (target) {
        case ChipListKind::Tags:
            session_recommended_.erase(norm);
            hidden_recommended_.erase(norm);
            add_tag(norm);
            return true;
        case ChipListKind::AntiTags:
            session_recommended_.erase(norm);
            hidden_recommended_.erase(norm);
            add_anti_tag(norm);
            return true;
        case ChipListKind::Recommended: {
            bool changed = false;
            if (tags_.erase(norm) > 0) changed = true;
            if (anti_tags_.erase(norm) > 0) changed = true;
            hidden_recommended_.erase(norm);
            session_recommended_.insert(norm);
            refresh_recommendations();
            rebuild_buttons();
            mark_dirty();
            if (changed) {
                notify_changed();
            }
            return true;
        }
        default:
            return false;
    }
}

bool TagEditorWidget::handle_chip_double_click(const Chip& chip) {
    switch (chip.kind) {
        case ChipListKind::Tags:
            remove_tag(chip.value);
            return true;
        case ChipListKind::AntiTags:
            remove_anti_tag(chip.value);
            return true;
        case ChipListKind::Recommended:
        case ChipListKind::SearchInputVirtual: {
            const std::string norm = normalize(chip.value);
            if (norm.empty()) return false;
            hidden_recommended_.insert(norm);
            session_recommended_.erase(norm);
            refresh_recommendations();
            rebuild_buttons();
            mark_dirty();
            return true;
        }
        default:
            return false;
    }
}

void TagEditorWidget::clear_asset_session_state() {
    hidden_recommended_.clear();
    session_recommended_.clear();
    show_search_virtual_chip_ = false;
    search_virtual_value_.clear();
    recommendation_context_scores_.clear();
    recommendation_popularity_.clear();
    clear_drag_state();
}

bool TagEditorWidget::event_targets_rect(const SDL_Event& e, const SDL_Rect& rect) {
    if (rect.w <= 0 || rect.h <= 0) return false;
    SDL_Point p{0,0};
    bool relevant = false;
    switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            p = SDL_Point{
                static_cast<int>(std::lround(e.button.x)),
                static_cast<int>(std::lround(e.button.y))};
            relevant = true;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            p = SDL_Point{
                static_cast<int>(std::lround(e.motion.x)),
                static_cast<int>(std::lround(e.motion.y))};
            relevant = true;
            break;
        default:
            break;
    }
    if (!relevant) return false;
    return SDL_PointInRect(&p, &rect) != 0;
}

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
void TagEditorWidgetTestAccess::set_query(TagEditorWidget& widget, const std::string& query) {
    widget.search_input_ = query;
    widget.search_query_ = to_lower(query);
    widget.update_search_filter();
}

const std::vector<std::string>& TagEditorWidgetTestAccess::recommended_tags(const TagEditorWidget& widget) {
    return widget.recommended_tags_;
}

bool TagEditorWidgetTestAccess::has_search_virtual_chip(const TagEditorWidget& widget) {
    return widget.show_search_virtual_chip_;
}

const std::string& TagEditorWidgetTestAccess::search_virtual_value(const TagEditorWidget& widget) {
    return widget.search_virtual_value_;
}

void TagEditorWidgetTestAccess::rebuild_recommendations(TagEditorWidget& widget) {
    widget.refresh_recommendations();
    widget.rebuild_buttons();
}

std::vector<std::string> TagEditorWidgetTestAccess::top_similar_asset_names(const TagEditorWidget& widget) {
    const auto& corpus = manifest_tag_corpus();
    const auto top_scores = collect_top_similar_assets(
        corpus,
        widget.tags_,
        widget.anti_tags_,
        widget.subject_asset_name_,
        true);
    std::vector<std::string> names;
    names.reserve(top_scores.size());
    for (const auto& entry : top_scores) {
        if (entry.profile) {
            names.push_back(entry.profile->asset_name);
        }
    }
    return names;
}

bool TagEditorWidgetTestAccess::pruning_matches_exact(const TagEditorWidget& widget) {
    const auto& corpus = manifest_tag_corpus();
    const auto with_pruning = collect_top_similar_assets(
        corpus,
        widget.tags_,
        widget.anti_tags_,
        widget.subject_asset_name_,
        true);
    const auto without_pruning = collect_top_similar_assets(
        corpus,
        widget.tags_,
        widget.anti_tags_,
        widget.subject_asset_name_,
        false);

    if (with_pruning.size() != without_pruning.size()) {
        return false;
    }
    for (size_t i = 0; i < with_pruning.size(); ++i) {
        const auto* left_profile = with_pruning[i].profile;
        const auto* right_profile = without_pruning[i].profile;
        const std::string left_name = left_profile ? left_profile->asset_name : std::string{};
        const std::string right_name = right_profile ? right_profile->asset_name : std::string{};
        if (left_name != right_name) {
            return false;
        }
        if (std::abs(with_pruning[i].similarity - without_pruning[i].similarity) > 1e-9) {
            return false;
        }
    }
    return true;
}
#endif

