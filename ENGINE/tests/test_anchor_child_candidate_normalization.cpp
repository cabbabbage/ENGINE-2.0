#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <random>
#include <string>

#include <nlohmann/json.hpp>

#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_library.hpp"
#include "gameplay/spawn/runtime_candidates.hpp"

namespace {

AssetInfo::AnchorChildPointCandidate make_raw_entry(const std::string& anchor_name,
                                                    nlohmann::json payload) {
    AssetInfo::AnchorChildPointCandidate entry{};
    entry.anchor_point_name = anchor_name;
    entry.candidates = std::move(payload);
    return entry;
}

std::optional<double> find_chance(const nlohmann::json& normalized_entry,
                                  const std::string& candidate_name) {
    if (!normalized_entry.is_object()) {
        return std::nullopt;
    }
    const auto it = normalized_entry.find("candidates");
    if (it == normalized_entry.end() || !it->is_array()) {
        return std::nullopt;
    }
    for (const auto& candidate : *it) {
        if (!candidate.is_object()) {
            continue;
        }
        if (candidate.value("name", std::string{}) != candidate_name) {
            continue;
        }
        const auto chance_it = candidate.find("chance");
        if (chance_it == candidate.end()) {
            return std::nullopt;
        }
        if (chance_it->is_number_float()) {
            return chance_it->get<double>();
        }
        if (chance_it->is_number_integer()) {
            return static_cast<double>(chance_it->get<std::int64_t>());
        }
        if (chance_it->is_number_unsigned()) {
            return static_cast<double>(chance_it->get<std::uint64_t>());
        }
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("AssetInfo normalizes legacy anchor child candidates to canonical chance arrays") {
    AssetInfo info("anchor_norm_asset");
    info.anchor_point_child_candidates = {
        make_raw_entry(
            "hat",
            nlohmann::json::object({
                {"legacy_scalar", 50},
                {"legacy_weight_string", nlohmann::json::object({{"weight", "25"}})},
                {"legacy_missing_weight", nlohmann::json::object()},
                {"legacy_tag", nlohmann::json::object({{"name", "hat"}, {"tag", true}})}
            }))
    };

    const nlohmann::json normalized = info.anchor_point_child_candidate_candidates("hat");
    REQUIRE(normalized.is_object());
    REQUIRE(normalized.contains("candidates"));
    REQUIRE(normalized["candidates"].is_array());

    const auto scalar = find_chance(normalized, "legacy_scalar");
    const auto weighted = find_chance(normalized, "legacy_weight_string");
    const auto missing = find_chance(normalized, "legacy_missing_weight");
    const auto tagged = find_chance(normalized, "#hat");

    REQUIRE(scalar.has_value());
    REQUIRE(weighted.has_value());
    REQUIRE(missing.has_value());
    REQUIRE(tagged.has_value());
    CHECK(*scalar == doctest::Approx(50.0));
    CHECK(*weighted == doctest::Approx(25.0));
    CHECK(*missing == doctest::Approx(100.0));
    CHECK(*tagged == doctest::Approx(100.0));
}

TEST_CASE("AssetInfo upsert canonicalizes legacy payload shape for anchor child candidates") {
    AssetInfo info("anchor_upsert_asset");
    const bool changed = info.upsert_anchor_point_child_candidate(
        "hat",
        nlohmann::json::object({
            {"legacy_scalar", 50},
            {"legacy_missing_weight", nlohmann::json::object()}
        }));
    CHECK(changed);

    const nlohmann::json payload = info.anchor_point_child_candidates_payload();
    REQUIRE(payload.is_array());
    REQUIRE(payload.size() == 1);
    REQUIRE(payload[0].is_object());
    REQUIRE(payload[0].contains("candidates"));
    REQUIRE(payload[0]["candidates"].is_object());
    REQUIRE(payload[0]["candidates"].contains("candidates"));
    REQUIRE(payload[0]["candidates"]["candidates"].is_array());

    const nlohmann::json normalized = payload[0]["candidates"];
    const auto scalar = find_chance(normalized, "legacy_scalar");
    const auto missing = find_chance(normalized, "legacy_missing_weight");
    REQUIRE(scalar.has_value());
    REQUIRE(missing.has_value());
    CHECK(*scalar == doctest::Approx(50.0));
    CHECK(*missing == doctest::Approx(100.0));
}

TEST_CASE("AssetInfo upsert with empty object preserves existing anchor child candidates") {
    AssetInfo info("anchor_upsert_preserve_asset");
    REQUIRE(info.upsert_anchor_point_child_candidate(
        "hat",
        nlohmann::json::object({
            {"legacy_scalar", 50}
        })));

    const bool changed = info.upsert_anchor_point_child_candidate("hat", nlohmann::json::object());
    CHECK_FALSE(changed);

    const nlohmann::json normalized = info.anchor_point_child_candidate_candidates("hat");
    const auto scalar = find_chance(normalized, "legacy_scalar");
    REQUIRE(scalar.has_value());
    CHECK(*scalar == doctest::Approx(50.0));
}

TEST_CASE("Normalized anchor tag candidates resolve through RuntimeCandidates tag selection") {
    AssetInfo info("anchor_tag_asset");
    info.anchor_point_child_candidates = {
        make_raw_entry(
            "hat",
            nlohmann::json::object({
                {"#tree", nlohmann::json::object()}
            }))
    };

    const nlohmann::json normalized = info.anchor_point_child_candidate_candidates("hat");
    REQUIRE(normalized.is_object());
    REQUIRE(normalized.contains("candidates"));
    REQUIRE(normalized["candidates"].is_array());

    const vibble::spawn::RuntimeCandidates runtime_candidates =
        vibble::spawn::RuntimeCandidates::from_json(normalized["candidates"]);
    REQUIRE_FALSE(runtime_candidates.empty());

    AssetLibrary library(false);
    library.add_asset("oak", nlohmann::json::object({{"tags", nlohmann::json::array({"tree"})}}));

    const auto& catalog_assets = library.all();
    vibble::spawn::RuntimeCandidates::AssetCatalogView catalog{&catalog_assets, false};
    std::mt19937 rng(7);
    const auto pick = runtime_candidates.pick_random(rng, catalog, vibble::spawn::ZeroWeightPolicy::UniformFallback);
    REQUIRE(pick.has_value());
    CHECK_FALSE(pick->is_null);
    CHECK(pick->resolved_asset_name == "oak");
}
