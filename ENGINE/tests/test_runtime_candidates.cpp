#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_library.hpp"
#include "gameplay/spawn/asset_spawn_planner.hpp"
#include "gameplay/spawn/runtime_candidates.hpp"

namespace {

std::shared_ptr<AssetInfo> make_asset(const std::string& name,
                                      const std::vector<std::string>& tags = {},
                                      const std::vector<std::string>& anti_tags = {}) {
    auto info = std::make_shared<AssetInfo>(name);
    info->set_tags(tags);
    info->set_anti_tags(anti_tags);
    return info;
}

vibble::spawn::RuntimeCandidates::AssetCatalogView make_catalog(
    const std::unordered_map<std::string, std::shared_ptr<AssetInfo>>& assets,
    bool case_insensitive = false) {
    return vibble::spawn::RuntimeCandidates::AssetCatalogView{&assets, case_insensitive};
}

}  // namespace

TEST_CASE("RuntimeCandidates parses chance and weight fields consistently") {
    const nlohmann::json raw = nlohmann::json::array({
        nlohmann::json::object({{"name", "A"}, {"weight", "10"}}),
        nlohmann::json::object({{"name", "B"}, {"chance", "2.5"}}),
        nlohmann::json::object({{"name", ""}, {"chance", -3}})
    });

    const auto candidates = vibble::spawn::RuntimeCandidates::from_json(raw);
    const auto& entries = candidates.entries();
    REQUIRE(entries.size() == 3);

    CHECK(entries[0].key == "A");
    CHECK(entries[0].weight == doctest::Approx(10.0));
    CHECK(entries[1].key == "B");
    CHECK(entries[1].weight == doctest::Approx(2.5));
    CHECK(entries[2].weight == doctest::Approx(0.0));
}

TEST_CASE("RuntimeCandidates resolves tags per pick and honors blocked anti-candidates") {
    std::unordered_map<std::string, std::shared_ptr<AssetInfo>> assets;
    assets.emplace("oak", make_asset("oak", {"tree"}));
    assets.emplace("pine", make_asset("pine", {"tree"}));

    const nlohmann::json raw = nlohmann::json::array({
        nlohmann::json::object({{"name", "#tree"}, {"chance", 100}}),
        nlohmann::json::object({{"name", "oak"}, {"chance", 0}})
    });

    const auto candidates = vibble::spawn::RuntimeCandidates::from_json(raw);
    auto catalog = make_catalog(assets);
    std::mt19937 rng(1234);

    for (int i = 0; i < 20; ++i) {
        const auto resolved =
            candidates.pick_random(rng, catalog, vibble::spawn::ZeroWeightPolicy::UniformFallback);
        REQUIRE(resolved.has_value());
        REQUIRE_FALSE(resolved->is_null);
        CHECK(resolved->resolved_asset_name != "oak");
        CHECK(resolved->resolved_asset_name == "pine");
    }
}

TEST_CASE("RuntimeCandidates zero-weight behavior respects policy") {
    std::unordered_map<std::string, std::shared_ptr<AssetInfo>> assets;
    assets.emplace("A", make_asset("A"));
    assets.emplace("B", make_asset("B"));

    const nlohmann::json raw = nlohmann::json::array({
        nlohmann::json::object({{"name", "A"}, {"chance", 0}}),
        nlohmann::json::object({{"name", "B"}, {"chance", 0}})
    });

    const auto candidates = vibble::spawn::RuntimeCandidates::from_json(raw);
    auto catalog = make_catalog(assets);
    std::mt19937 rng(7);

    const auto uniform_pick =
        candidates.pick_random(rng, catalog, vibble::spawn::ZeroWeightPolicy::UniformFallback);
    REQUIRE(uniform_pick.has_value());
    CHECK_FALSE(uniform_pick->is_null);

    const auto no_selection_pick =
        candidates.pick_hashed(42ULL, catalog, vibble::spawn::ZeroWeightPolicy::NoSelection);
    CHECK_FALSE(no_selection_pick.has_value());
}

TEST_CASE("RuntimeCandidates hashed picks are deterministic and support fractional weights") {
    std::unordered_map<std::string, std::shared_ptr<AssetInfo>> assets;
    assets.emplace("A", make_asset("A"));
    assets.emplace("B", make_asset("B"));

    const nlohmann::json raw = nlohmann::json::array({
        nlohmann::json::object({{"name", "A"}, {"chance", 0.25}}),
        nlohmann::json::object({{"name", "B"}, {"chance", 0.75}})
    });

    const auto candidates = vibble::spawn::RuntimeCandidates::from_json(raw);
    auto catalog = make_catalog(assets);

    const auto one = candidates.pick_hashed(123456ULL, catalog, vibble::spawn::ZeroWeightPolicy::NoSelection);
    const auto two = candidates.pick_hashed(123456ULL, catalog, vibble::spawn::ZeroWeightPolicy::NoSelection);
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    CHECK(one->entry_index == two->entry_index);
    CHECK(one->resolved_asset_name == two->resolved_asset_name);

    int count_a = 0;
    int count_b = 0;
    for (std::uint64_t h = 0; h < 500; ++h) {
        const auto pick =
            candidates.pick_hashed(h, catalog, vibble::spawn::ZeroWeightPolicy::NoSelection);
        REQUIRE(pick.has_value());
        REQUIRE_FALSE(pick->is_null);
        if (pick->resolved_asset_name == "A") {
            ++count_a;
        } else if (pick->resolved_asset_name == "B") {
            ++count_b;
        }
    }
    CHECK(count_a > 0);
    CHECK(count_b > 0);
    CHECK(count_b > count_a);
}

TEST_CASE("RuntimeCandidates retry-aware exclusion removes failed entries from selection") {
    std::unordered_map<std::string, std::shared_ptr<AssetInfo>> assets;
    assets.emplace("A", make_asset("A"));
    assets.emplace("B", make_asset("B"));

    const nlohmann::json raw = nlohmann::json::array({
        nlohmann::json::object({{"name", "A"}, {"chance", 1}}),
        nlohmann::json::object({{"name", "B"}, {"chance", 1}})
    });

    const auto candidates = vibble::spawn::RuntimeCandidates::from_json(raw);
    auto catalog = make_catalog(assets);
    std::mt19937 rng(9);

    std::unordered_set<int> exclude_a{0};
    for (int i = 0; i < 10; ++i) {
        const auto pick = candidates.pick_random_excluding(
            rng, catalog, exclude_a, vibble::spawn::ZeroWeightPolicy::UniformFallback);
        REQUIRE(pick.has_value());
        CHECK(pick->entry_index == 1);
        CHECK(pick->resolved_asset_name == "B");
    }

    std::unordered_set<int> exclude_all{0, 1};
    const auto none = candidates.pick_random_excluding(
        rng, catalog, exclude_all, vibble::spawn::ZeroWeightPolicy::UniformFallback);
    CHECK_FALSE(none.has_value());
}

TEST_CASE("AssetSpawnPlanner integrates RuntimeCandidates and resolves tag candidates at runtime pick") {
    AssetLibrary library(false);
    library.add_asset("oak", nlohmann::json::object({{"tags", nlohmann::json::array({"tree"})}}));
    library.add_asset("pine", nlohmann::json::object({{"tags", nlohmann::json::array({"tree"})}}));

    nlohmann::json source = nlohmann::json::object({
        {"spawn_groups", nlohmann::json::array({
            nlohmann::json::object({
                {"spawn_id", "spn-test"},
                {"display_name", "trees"},
                {"position", "Random"},
                {"min_number", 1},
                {"max_number", 1},
                {"candidates", nlohmann::json::array({
                    nlohmann::json::object({{"name", "#tree"}, {"chance", 100}})
                })}
            })
        })}
    });

    Area area("planner_test", 0);
    AssetSpawnPlanner planner(std::vector<nlohmann::json>{source}, area, library);
    const auto& queue = planner.get_spawn_queue();
    REQUIRE(queue.size() == 1);
    REQUIRE(queue[0].has_candidates());

    const auto& asset_map = library.all();
    vibble::spawn::RuntimeCandidates::AssetCatalogView catalog{&asset_map, false};
    std::mt19937 rng(5);
    const auto pick = queue[0].select_candidate(rng, catalog);
    REQUIRE(pick.has_value());
    REQUIRE_FALSE(pick->is_null);
    CHECK((pick->resolved_asset_name == "oak" || pick->resolved_asset_name == "pine"));
}
