#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include <nlohmann/json.hpp>

#include "animation/controllers/shared/custom_asset_controller.hpp"
#include "animation/controllers/shared/child_asset.hpp"
#include "animation/controllers/shared/anchor_bound_asset_helper.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "stubs/asset/child_asset_runtime_test_support.hpp"
#include "utils/input.hpp"

#include "rendering/render/render_depth_policy.hpp"

namespace {

using test_child_asset_runtime::AnchorSpec;

struct AssetsScope {
    AssetsScope()
        : assets(test_child_asset_runtime::create_assets_stub()) {}

    ~AssetsScope() {
        test_child_asset_runtime::destroy_assets_stub(assets);
    }

    Assets* assets = nullptr;
};

class TestCustomController : public CustomAssetController {
public:
    explicit TestCustomController(Asset* self)
        : CustomAssetController(self) {}

    void schedule_child_creation(std::string asset_name) {
        pending_child_name_ = std::move(asset_name);
    }

    ChildAsset* child() {
        return child_ ? &(*child_) : nullptr;
    }

protected:
    void on_update(const Input&) override {
        if (!child_ && !pending_child_name_.empty()) {
            Asset* owner = self_ptr();
            Assets* owner_assets = assets();
            if (owner && owner_assets) {
                child_.emplace(*owner, std::move(pending_child_name_));
            }
            pending_child_name_.clear();
        }
        if (child_) {
            child_->update();
        }
    }

    void on_process_pending_attacks(Asset& self) override {
        (void)self;
    }

private:
    std::optional<ChildAsset> child_;
    std::string pending_child_name_;
};

class RetryingChildController : public CustomAssetController {
public:
    explicit RetryingChildController(Asset* self)
        : CustomAssetController(self) {}

    ChildAsset* child() {
        return child_ ? &(*child_) : nullptr;
    }

protected:
    void on_update(const Input&) override {
        Asset* owner = self_ptr();
        Assets* owner_assets = assets();
        if (!owner || !owner_assets) {
            return;
        }

        if (child_ && child_->get_asset()) {
            child_->update();
            return;
        }

        child_.reset();
        child_.emplace(*owner, "vibble_eyes");
        if (!child_->get_asset()) {
            child_.reset();
            return;
        }

        child_->bind("eyes");
        child_->update();
    }

    void on_process_pending_attacks(Asset& self) override {
        (void)self;
    }

private:
    std::optional<ChildAsset> child_;
};

bool contains_spawn_candidate_named(const nlohmann::json& node, const std::string& name) {
    if (node.is_object()) {
        const auto candidates_it = node.find("candidates");
        if (candidates_it != node.end() && candidates_it->is_array()) {
            for (const auto& candidate : *candidates_it) {
                if (candidate.is_object() && candidate.value("name", std::string{}) == name) {
                    return true;
                }
            }
        }
        for (const auto& [key, value] : node.items()) {
            (void)key;
            if (contains_spawn_candidate_named(value, name)) {
                return true;
            }
        }
        return false;
    }

    if (node.is_array()) {
        for (const auto& item : node) {
            if (contains_spawn_candidate_named(item, name)) {
                return true;
            }
        }
    }
    return false;
}

std::filesystem::path repo_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

} // namespace

TEST_CASE("ChildAsset constructs with explicit owner and assets") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 10, 15, 20, 0));
    REQUIRE(owner != nullptr);

    ChildAsset child(*owner, "vibble_eyes");
    REQUIRE(child.get_asset() != nullptr);
    CHECK(child.is_hidden());
    CHECK_FALSE(child.is_bound());
    CHECK(test_child_asset_runtime::asset_count(*assets_scope.assets) == 2);
}

TEST_CASE("AnchorBoundAssetHelper keeps child updated when owner anchor moves") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 10, 20, 30, 0));
    REQUIRE(owner != nullptr);

    test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"eyes", 4, 5, 6, 0, 2, 0.0f, true});

    TestCustomController controller(owner);
    Input input;
    controller.schedule_child_creation("vibble_eyes");
    controller.update(input);

    ChildAsset* child = controller.child();
    REQUIRE(child != nullptr);
    child->bind("eyes");
    Asset* spawned = child->get_asset();
    REQUIRE(spawned != nullptr);

    AnchorSpec moved{"eyes", 7, 8, 9, 0, 3, -0.25f, true};
    test_child_asset_runtime::set_anchor(*owner, moved);

    anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().notify_anchor_changed(owner, "eyes");

    CHECK(spawned->world_x() == owner->world_x() + moved.offset_x);
    CHECK(spawned->world_y() == owner->world_y() + moved.offset_y);
    CHECK(spawned->world_z() == owner->world_z() + moved.offset_z);

    const double anchor_world_depth =
        static_cast<double>(owner->world_z() + moved.offset_z) + moved.world_depth_offset;
    const double expected_bias = render_depth::bias_for_quantized_depth(anchor_world_depth, spawned->world_z());
    CHECK(spawned->render_depth_bias() == doctest::Approx(expected_bias));

    CHECK(anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().is_child_bound(spawned));
}

TEST_CASE("ChildAsset controller retry recreates child after one failed spawn attempt") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 10, 15, 20, 0));
    REQUIRE(owner != nullptr);

    test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"eyes", 1, 2, 3, 0, 0, 0.0f, true});
    test_child_asset_runtime::set_spawn_failures(*assets_scope.assets, "vibble_eyes", 1);

    RetryingChildController controller(owner);
    Input input;

    controller.update(input);
    CHECK(controller.child() == nullptr);
    CHECK(test_child_asset_runtime::asset_count(*assets_scope.assets) == 1);

    controller.update(input);
    ChildAsset* child = controller.child();
    REQUIRE(child != nullptr);
    REQUIRE(child->get_asset() != nullptr);
    CHECK_FALSE(child->is_hidden());
    CHECK(child->is_bound());

    Asset* spawned = child->get_asset();
    REQUIRE(spawned != nullptr);
    CHECK(spawned->world_x() == 11);
    CHECK(spawned->world_y() == 17);
    CHECK(spawned->world_z() == 23);
    CHECK(test_child_asset_runtime::asset_count(*assets_scope.assets) == 2);
}

TEST_CASE("ChildAsset bind and update follow the owner anchor and recover from missing anchors") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 100, 50, 200, 2));
    REQUIRE(owner != nullptr);

    test_child_asset_runtime::set_anchor(*owner, AnchorSpec{
        "eyes", 10, 20, 7, 0, 4, 0.5f, true
    });

    TestCustomController controller(owner);
    Input input;
    controller.schedule_child_creation("vibble_eyes");
    controller.update(input);

    ChildAsset* child = controller.child();
    REQUIRE(child != nullptr);
    REQUIRE(child->get_asset() != nullptr);

    child->bind("eyes");
    CHECK(child->is_bound());
    CHECK_FALSE(child->is_hidden());

    Asset* spawned = child->get_asset();
    REQUIRE(spawned != nullptr);
    CHECK(spawned->world_x() == 110);
    CHECK(spawned->world_y() == 70);
    CHECK(spawned->world_z() == 207);
    CHECK(spawned->grid_resolution == 4);
    CHECK(spawned->render_depth_bias() == doctest::Approx(-0.5));

    test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"eyes", 10, 20, 7, 0, 4, 0.5f, false});
    child->update();
    CHECK(child->is_hidden());
    CHECK(spawned->is_anchor_hidden());

    owner->move_to_world_position(50, 60, 70, 1);
    test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"eyes", 12, 14, 10, 0, 2, 0.0f, true});
    child->update();
    CHECK_FALSE(child->is_hidden());
    CHECK(spawned->world_x() == 62);
    CHECK(spawned->world_y() == 74);
    CHECK(spawned->world_z() == 80);
}

TEST_CASE("ChildAsset lifecycle controls visibility, one-shot placement, unbind, and deletion") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 10, 15, 20, 0));
    REQUIRE(owner != nullptr);

    test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"eyes", 4, 5, 6, 0, 2, 0.0f, true});

    TestCustomController controller(owner);
    Input input;
    controller.schedule_child_creation("vibble_eyes");
    controller.update(input);

    ChildAsset* child = controller.child();
    REQUIRE(child != nullptr);
    REQUIRE(test_child_asset_runtime::asset_count(*assets_scope.assets) == 2);
    Asset* spawned = child->get_asset();
    REQUIRE(spawned != nullptr);
    CHECK(child->is_hidden());

    child->bind("eyes");
    CHECK(child->is_bound());
    CHECK(spawned->world_x() == 14);
    CHECK(spawned->world_y() == 20);
    CHECK(spawned->world_z() == 26);
    CHECK_FALSE(child->is_hidden());

    child->hide();
    CHECK(child->is_hidden());

    child->unhide();
    CHECK_FALSE(spawned->is_hidden());
    CHECK_FALSE(spawned->is_anchor_hidden());

    owner->move_to_world_position(30, 40, 50, 1);
    child->update();
    CHECK(spawned->world_x() == 34);
    CHECK(spawned->world_y() == 45);
    CHECK(spawned->world_z() == 56);

    child->unbind();
    CHECK_FALSE(child->is_bound());
    const int unbound_x = spawned->world_x();
    const int unbound_y = spawned->world_y();
    const int unbound_z = spawned->world_z();
    owner->move_to_world_position(100, 200, 300, 0);
    child->update();
    CHECK(spawned->world_x() == unbound_x);
    CHECK(spawned->world_y() == unbound_y);
    CHECK(spawned->world_z() == unbound_z);
    CHECK_FALSE(spawned->is_hidden());

    child->set_grid_point("eyes");
    CHECK(spawned->world_x() == 104);
    CHECK(spawned->world_y() == 205);
    CHECK(spawned->world_z() == 306);
    CHECK_FALSE(child->is_hidden());

    child->hide();
    CHECK(child->is_hidden());
    child->unhide();
    CHECK_FALSE(child->is_hidden());

    child->destroy();
    CHECK(assets_scope.assets->find_asset_by_stable_id("vibble_eyes") == nullptr);
    CHECK(test_child_asset_runtime::asset_count(*assets_scope.assets) == 1);
    child->destroy();
    CHECK(test_child_asset_runtime::asset_count(*assets_scope.assets) == 1);
}

TEST_CASE("Manifest no longer authors vibble eyes as a spawn-group child asset") {
    const std::filesystem::path manifest_path = repo_root() / "manifest.json";
    std::ifstream input(manifest_path);
    REQUIRE(input.is_open());

    nlohmann::json manifest = nlohmann::json::parse(input, nullptr, true, true);
    CHECK_FALSE(contains_spawn_candidate_named(manifest, "vibble_eyes"));
    CHECK(manifest["assets"].contains("vibble_eyes"));
}
