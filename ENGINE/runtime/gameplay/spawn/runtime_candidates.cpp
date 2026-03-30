#include "runtime_candidates.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>

#include "spawn_group_codec.hpp"
#include "assets/asset/asset_info.hpp"

namespace vibble::spawn {

namespace {

constexpr std::uint64_t kHashMixer = 0x9e3779b97f4a7c15ULL;

}

std::shared_ptr<AssetInfo> RuntimeCandidates::AssetCatalogView::find_info(
    const std::string& name,
    std::string* resolved_name) const {
    if (resolved_name) {
        resolved_name->clear();
    }
    if (!assets || name.empty()) {
        return nullptr;
    }

    auto exact = assets->find(name);
    if (exact != assets->end() && exact->second) {
        if (resolved_name) {
            *resolved_name = exact->first;
        }
        return exact->second;
    }

    if (!case_insensitive_lookup) {
        return nullptr;
    }

    const std::string wanted_lower = RuntimeCandidates::to_lower_copy(name);
    for (const auto& [asset_name, info] : *assets) {
        if (!info) {
            continue;
        }
        if (RuntimeCandidates::to_lower_copy(asset_name) == wanted_lower) {
            if (resolved_name) {
                *resolved_name = asset_name;
            }
            return info;
        }
        if (!info->name.empty() && RuntimeCandidates::to_lower_copy(info->name) == wanted_lower) {
            if (resolved_name) {
                *resolved_name = asset_name;
            }
            return info;
        }
    }

    return nullptr;
}

std::string RuntimeCandidates::trim_copy(const std::string& value) {
    auto is_not_space = [](unsigned char ch) { return !std::isspace(ch); };
    auto begin = std::find_if(value.begin(), value.end(), is_not_space);
    auto end = std::find_if(value.rbegin(), value.rend(), is_not_space).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

std::string RuntimeCandidates::sanitize_key(std::string value) {
    value = trim_copy(value);
    if (!value.empty() && value.front() == '#') {
        value.erase(0, 1);
    }
    return value;
}

std::string RuntimeCandidates::to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

RuntimeCandidates RuntimeCandidates::from_json(const nlohmann::json& candidates_json) {
    RuntimeCandidates result;
    if (!candidates_json.is_array()) {
        return result;
    }

    result.entries_.reserve(candidates_json.size());

    for (const auto& candidate_json : candidates_json) {
        CandidateEntry entry{};
        entry.weight = 0.0;
        entry.kind = CandidateKind::Asset;
        entry.is_null = candidate_json.is_null();

        std::string name;
        std::string label;
        bool use_tag = false;
        std::string tag_value;

        auto detect_tag_from_name = [&](const std::string& value) {
            if (!value.empty() && value.front() == '#') {
                use_tag = true;
                tag_value = value.substr(1);
            }
        };

        if (candidate_json.is_object()) {
            if (candidate_json.contains("name") && candidate_json["name"].is_string()) {
                name = candidate_json["name"].get<std::string>();
            } else if (candidate_json.contains("asset_name") && candidate_json["asset_name"].is_string()) {
                name = candidate_json["asset_name"].get<std::string>();
            }
            detect_tag_from_name(name);

            if (candidate_json.contains("display_name") && candidate_json["display_name"].is_string()) {
                label = candidate_json["display_name"].get<std::string>();
            } else if (candidate_json.contains("label") && candidate_json["label"].is_string()) {
                label = candidate_json["label"].get<std::string>();
            }

            if (candidate_json.contains("tag")) {
                const auto& tag_json = candidate_json["tag"];
                if (tag_json.is_boolean() && tag_json.get<bool>()) {
                    use_tag = true;
                    if (tag_value.empty() && !name.empty()) {
                        tag_value = name.front() == '#' ? name.substr(1) : name;
                    }
                } else if (tag_json.is_string()) {
                    use_tag = true;
                    tag_value = tag_json.get<std::string>();
                }
            }
            if (candidate_json.contains("tag_name") && candidate_json["tag_name"].is_string()) {
                use_tag = true;
                tag_value = candidate_json["tag_name"].get<std::string>();
            }

            entry.weight = spawn_group_codec::read_candidate_chance(candidate_json, 0.0);
        } else if (candidate_json.is_string()) {
            name = candidate_json.get<std::string>();
            detect_tag_from_name(name);
            label = name;
        } else if (candidate_json.is_number()) {
            entry.weight = candidate_json.get<double>();
        }

        entry.raw_identifier = trim_copy(name);
        const std::string lowered_name = to_lower_copy(sanitize_key(name));
        if (lowered_name == "null") {
            entry.is_null = true;
        }

        if (use_tag) {
            const std::string key = sanitize_key(tag_value.empty() ? name : tag_value);
            entry.key = key;
            if (!key.empty()) {
                entry.kind = CandidateKind::Tag;
            } else {
                entry.kind = CandidateKind::Asset;
                entry.is_null = true;
            }
        } else {
            entry.key = sanitize_key(name);
        }

        if (!std::isfinite(entry.weight) || entry.weight < 0.0) {
            entry.weight = 0.0;
        }

        std::string fallback_identifier = entry.raw_identifier;
        if (fallback_identifier.empty() && entry.kind == CandidateKind::Tag && !entry.key.empty()) {
            fallback_identifier = "#" + entry.key;
        }
        entry.display_name = !label.empty() ? label
                                            : (!entry.key.empty() ? entry.key : fallback_identifier);
        if (entry.display_name.empty() && entry.is_null) {
            entry.display_name = "null";
        }

        if (entry.weight <= 0.0) {
            if (entry.kind == CandidateKind::Tag && !entry.key.empty()) {
                result.blocked_tags_.insert(entry.key);
            } else if (!entry.is_null && !entry.key.empty()) {
                result.blocked_assets_.insert(entry.key);
            }
        } else if (entry.kind == CandidateKind::Tag && !entry.key.empty()) {
            result.positive_tags_.insert(entry.key);
        }

        result.entries_.push_back(std::move(entry));
    }

    return result;
}

std::vector<RuntimeCandidates::WeightedEntry> RuntimeCandidates::build_weighted_entries(
    const std::unordered_set<int>* excluded_entry_indices) const {
    std::vector<WeightedEntry> weighted;
    weighted.reserve(entries_.size());
    for (int idx = 0; idx < static_cast<int>(entries_.size()); ++idx) {
        if (excluded_entry_indices && excluded_entry_indices->count(idx) > 0) {
            continue;
        }
        double weight = entries_[static_cast<size_t>(idx)].weight;
        if (!std::isfinite(weight) || weight < 0.0) {
            weight = 0.0;
        }
        weighted.push_back(WeightedEntry{idx, weight});
    }
    return weighted;
}

std::uint64_t RuntimeCandidates::xorshift64(std::uint64_t value) {
    value ^= (value << 13);
    value ^= (value >> 7);
    value ^= (value << 17);
    return value;
}

double RuntimeCandidates::hash_unit_interval(std::uint64_t value) {
    const std::uint64_t mixed = xorshift64(value ^ kHashMixer);
    return static_cast<double>(mixed) / static_cast<double>(std::numeric_limits<std::uint64_t>::max());
}

std::optional<ResolvedCandidate> RuntimeCandidates::resolve_asset_entry(
    int entry_index,
    const CandidateEntry& entry,
    const AssetCatalogView& catalog) const {
    ResolvedCandidate resolved{};
    resolved.entry_index = entry_index;
    resolved.entry_identifier = entry.raw_identifier;
    resolved.display_name = entry.display_name;
    resolved.weight = entry.weight;
    resolved.is_null = true;

    if (entry.is_null || entry.key.empty()) {
        return resolved;
    }

    std::string resolved_name;
    std::shared_ptr<AssetInfo> info = catalog.find_info(entry.key, &resolved_name);
    if (!info || resolved_name.empty()) {
        return resolved;
    }

    resolved.resolved_asset_name = resolved_name;
    resolved.info = std::move(info);
    resolved.is_null = false;
    return resolved;
}

void RuntimeCandidates::reset_tag_cache_if_needed(const AssetCatalogView& catalog) const {
    const void* ptr = static_cast<const void*>(catalog.assets);
    if (cached_assets_ptr_ == ptr) {
        return;
    }
    cached_assets_ptr_ = ptr;
    tag_matches_cache_.clear();
}

const std::vector<std::pair<std::string, std::shared_ptr<AssetInfo>>>&
RuntimeCandidates::resolve_tag_matches(const std::string& tag, const AssetCatalogView& catalog) const {
    static const std::vector<std::pair<std::string, std::shared_ptr<AssetInfo>>> kEmpty;
    if (!catalog.assets || tag.empty()) {
        return kEmpty;
    }

    reset_tag_cache_if_needed(catalog);
    auto cache_it = tag_matches_cache_.find(tag);
    if (cache_it != tag_matches_cache_.end()) {
        return cache_it->second;
    }

    auto& matches = tag_matches_cache_[tag];
    for (const auto& [asset_name, info] : *catalog.assets) {
        if (!info) {
            continue;
        }
        if (!info->has_tag(tag)) {
            continue;
        }
        if (blocked_assets_.count(asset_name) > 0) {
            continue;
        }

        bool blocked_by_other_tag = false;
        for (const auto& blocked_tag : blocked_tags_) {
            if (blocked_tag.empty() || blocked_tag == tag) {
                continue;
            }
            if (info->has_tag(blocked_tag)) {
                blocked_by_other_tag = true;
                break;
            }
        }
        if (blocked_by_other_tag) {
            continue;
        }

        bool blocked_by_anti_tag = false;
        for (const auto& anti_tag : info->anti_tags) {
            if (anti_tag == tag) {
                continue;
            }
            if (positive_tags_.count(anti_tag) > 0) {
                blocked_by_anti_tag = true;
                break;
            }
        }
        if (blocked_by_anti_tag) {
            continue;
        }

        matches.push_back(std::make_pair(asset_name, info));
    }
    return matches;
}

std::optional<ResolvedCandidate> RuntimeCandidates::resolve_entry_random(
    int entry_index,
    std::mt19937& rng,
    const AssetCatalogView& catalog) const {
    if (entry_index < 0 || entry_index >= static_cast<int>(entries_.size())) {
        return std::nullopt;
    }
    const CandidateEntry& entry = entries_[static_cast<size_t>(entry_index)];
    if (entry.kind == CandidateKind::Asset) {
        return resolve_asset_entry(entry_index, entry, catalog);
    }

    ResolvedCandidate resolved{};
    resolved.entry_index = entry_index;
    resolved.entry_identifier = entry.raw_identifier;
    resolved.display_name = entry.display_name;
    resolved.weight = entry.weight;
    resolved.is_null = true;

    if (entry.is_null || entry.key.empty()) {
        return resolved;
    }

    const auto& matches = resolve_tag_matches(entry.key, catalog);
    if (matches.empty()) {
        return resolved;
    }

    std::uniform_int_distribution<size_t> dist(0, matches.size() - 1);
    const auto& match = matches[dist(rng)];
    resolved.resolved_asset_name = match.first;
    resolved.info = match.second;
    resolved.is_null = !(resolved.info && !resolved.resolved_asset_name.empty());
    return resolved;
}

std::optional<ResolvedCandidate> RuntimeCandidates::resolve_entry_hashed(
    int entry_index,
    std::uint64_t hash,
    const AssetCatalogView& catalog) const {
    if (entry_index < 0 || entry_index >= static_cast<int>(entries_.size())) {
        return std::nullopt;
    }
    const CandidateEntry& entry = entries_[static_cast<size_t>(entry_index)];
    if (entry.kind == CandidateKind::Asset) {
        return resolve_asset_entry(entry_index, entry, catalog);
    }

    ResolvedCandidate resolved{};
    resolved.entry_index = entry_index;
    resolved.entry_identifier = entry.raw_identifier;
    resolved.display_name = entry.display_name;
    resolved.weight = entry.weight;
    resolved.is_null = true;

    if (entry.is_null || entry.key.empty()) {
        return resolved;
    }

    const auto& matches = resolve_tag_matches(entry.key, catalog);
    if (matches.empty()) {
        return resolved;
    }

    const std::uint64_t mixed = xorshift64(hash ^ (static_cast<std::uint64_t>(entry_index) + kHashMixer));
    const size_t index = static_cast<size_t>(mixed % static_cast<std::uint64_t>(matches.size()));
    const auto& match = matches[index];
    resolved.resolved_asset_name = match.first;
    resolved.info = match.second;
    resolved.is_null = !(resolved.info && !resolved.resolved_asset_name.empty());
    return resolved;
}

std::optional<ResolvedCandidate> RuntimeCandidates::pick_random(
    std::mt19937& rng,
    const AssetCatalogView& catalog,
    ZeroWeightPolicy zero_weight_policy) const {
    return pick_random_excluding(rng, catalog, std::unordered_set<int>{}, zero_weight_policy);
}

std::optional<ResolvedCandidate> RuntimeCandidates::pick_random_excluding(
    std::mt19937& rng,
    const AssetCatalogView& catalog,
    const std::unordered_set<int>& excluded_entry_indices,
    ZeroWeightPolicy zero_weight_policy) const {
    auto weighted = build_weighted_entries(&excluded_entry_indices);
    if (weighted.empty()) {
        return std::nullopt;
    }

    double positive_total = 0.0;
    std::vector<double> weights;
    weights.reserve(weighted.size());
    for (const auto& item : weighted) {
        const double weight = item.weight > 0.0 ? item.weight : 0.0;
        if (weight > 0.0) {
            positive_total += weight;
        }
        weights.push_back(weight);
    }

    if (positive_total <= 0.0) {
        if (zero_weight_policy == ZeroWeightPolicy::NoSelection) {
            return std::nullopt;
        }
        std::fill(weights.begin(), weights.end(), 1.0);
    }

    std::discrete_distribution<size_t> dist(weights.begin(), weights.end());
    const size_t selected_weighted_index = dist(rng);
    if (selected_weighted_index >= weighted.size()) {
        return std::nullopt;
    }

    return resolve_entry_random(weighted[selected_weighted_index].entry_index, rng, catalog);
}

std::optional<ResolvedCandidate> RuntimeCandidates::pick_hashed(
    std::uint64_t hash,
    const AssetCatalogView& catalog,
    ZeroWeightPolicy zero_weight_policy) const {
    auto weighted = build_weighted_entries(nullptr);
    if (weighted.empty()) {
        return std::nullopt;
    }

    double positive_total = 0.0;
    for (const auto& item : weighted) {
        if (item.weight > 0.0) {
            positive_total += item.weight;
        }
    }

    if (positive_total <= 0.0) {
        if (zero_weight_policy == ZeroWeightPolicy::NoSelection) {
            return std::nullopt;
        }
        const std::uint64_t mixed = xorshift64(hash ^ kHashMixer);
        const size_t idx = static_cast<size_t>(mixed % static_cast<std::uint64_t>(weighted.size()));
        return resolve_entry_hashed(weighted[idx].entry_index, mixed, catalog);
    }

    const double roll = hash_unit_interval(hash) * positive_total;
    double cumulative = 0.0;
    int fallback_entry_index = -1;
    for (const auto& item : weighted) {
        const double weight = item.weight > 0.0 ? item.weight : 0.0;
        if (weight <= 0.0) {
            continue;
        }
        fallback_entry_index = item.entry_index;
        cumulative += weight;
        if (roll < cumulative) {
            return resolve_entry_hashed(item.entry_index, hash, catalog);
        }
    }

    if (fallback_entry_index >= 0) {
        return resolve_entry_hashed(fallback_entry_index, hash, catalog);
    }
    return std::nullopt;
}

void RuntimeCandidates::append_positive_asset_names(
    std::unordered_set<std::string>& out,
    const AssetCatalogView& catalog) const {
    for (int idx = 0; idx < static_cast<int>(entries_.size()); ++idx) {
        const CandidateEntry& entry = entries_[static_cast<size_t>(idx)];
        if (entry.weight <= 0.0 || entry.is_null) {
            continue;
        }
        if (entry.kind == CandidateKind::Asset) {
            std::string resolved_name;
            std::shared_ptr<AssetInfo> info = catalog.find_info(entry.key, &resolved_name);
            if (info && !resolved_name.empty()) {
                out.insert(resolved_name);
            }
            continue;
        }

        const auto& matches = resolve_tag_matches(entry.key, catalog);
        for (const auto& match : matches) {
            if (!match.first.empty()) {
                out.insert(match.first);
            }
        }
    }
}

}  // namespace vibble::spawn
