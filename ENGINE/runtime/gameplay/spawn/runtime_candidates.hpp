#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

class AssetInfo;

namespace vibble::spawn {

enum class CandidateKind {
    Asset = 0,
    Tag = 1,
};

enum class ZeroWeightPolicy {
    UniformFallback = 0,
    NoSelection = 1,
};

struct CandidateEntry {
    std::string raw_identifier;
    std::string key;
    std::string display_name;
    CandidateKind kind = CandidateKind::Asset;
    double weight = 0.0;
    bool is_null = false;
};

struct ResolvedCandidate {
    int entry_index = -1;
    std::string entry_identifier;
    std::string display_name;
    std::string resolved_asset_name;
    std::shared_ptr<AssetInfo> info;
    double weight = 0.0;
    bool is_null = true;
};

class RuntimeCandidates {
public:
    struct AssetCatalogView {
        const std::unordered_map<std::string, std::shared_ptr<AssetInfo>>* assets = nullptr;
        bool case_insensitive_lookup = false;

        AssetCatalogView(
            const std::unordered_map<std::string, std::shared_ptr<AssetInfo>>* assets_ptr,
            bool case_insensitive_lookup_value)
            : assets(assets_ptr), case_insensitive_lookup(case_insensitive_lookup_value) {}

        std::shared_ptr<AssetInfo> find_info(const std::string& name,
                                             std::string* resolved_name = nullptr) const;

    private:
        void ensure_alias_index() const;

        mutable const void* alias_index_assets_ptr_ = nullptr;
        mutable std::optional<std::unordered_map<std::string, std::pair<std::string, std::shared_ptr<AssetInfo>>>>
            alias_index_;
    };

    static RuntimeCandidates from_json(const nlohmann::json& candidates_json);

    bool empty() const { return entries_.empty(); }
    const std::vector<CandidateEntry>& entries() const { return entries_; }

    std::optional<ResolvedCandidate> pick_random(std::mt19937& rng,
                                                 const AssetCatalogView& catalog,
                                                 ZeroWeightPolicy zero_weight_policy) const;

    std::optional<ResolvedCandidate> pick_hashed(std::uint64_t hash,
                                                 const AssetCatalogView& catalog,
                                                 ZeroWeightPolicy zero_weight_policy) const;

    std::optional<ResolvedCandidate> pick_random_excluding(
        std::mt19937& rng,
        const AssetCatalogView& catalog,
        const std::unordered_set<int>& excluded_entry_indices,
        ZeroWeightPolicy zero_weight_policy) const;

    void append_positive_asset_names(std::unordered_set<std::string>& out,
                                     const AssetCatalogView& catalog) const;

private:
    std::vector<CandidateEntry> entries_;
    std::unordered_set<std::string> blocked_tags_;
    std::unordered_set<std::string> blocked_assets_;
    std::unordered_set<std::string> positive_tags_;

    mutable const void* cached_assets_ptr_ = nullptr;
    mutable std::unordered_map<std::string,
                               std::vector<std::pair<std::string, std::shared_ptr<AssetInfo>>>>
        tag_matches_cache_;

    static std::string sanitize_key(std::string value);

    struct WeightedEntry {
        int entry_index = -1;
        double weight = 0.0;
    };

    std::vector<WeightedEntry> build_weighted_entries(
        const std::unordered_set<int>* excluded_entry_indices) const;

    static std::uint64_t xorshift64(std::uint64_t value);
    static double hash_unit_interval(std::uint64_t value);

    std::optional<ResolvedCandidate> resolve_entry_random(int entry_index,
                                                          std::mt19937& rng,
                                                          const AssetCatalogView& catalog) const;
    std::optional<ResolvedCandidate> resolve_entry_hashed(int entry_index,
                                                          std::uint64_t hash,
                                                          const AssetCatalogView& catalog) const;
    std::optional<ResolvedCandidate> resolve_asset_entry(int entry_index,
                                                         const CandidateEntry& entry,
                                                         const AssetCatalogView& catalog) const;

    const std::vector<std::pair<std::string, std::shared_ptr<AssetInfo>>>& resolve_tag_matches(
        const std::string& tag,
        const AssetCatalogView& catalog) const;

    void reset_tag_cache_if_needed(const AssetCatalogView& catalog) const;
};

}  // namespace vibble::spawn
