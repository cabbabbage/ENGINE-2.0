#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <cmath>
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

Asset* find_owner_child_named(const Asset* owner, const std::string& child_name) {
    if (!owner) {
        return nullptr;
    }
    for (Asset* child : owner->children()) {
        if (!child || !child->info) {
            continue;
        }
        if (child->info->name == child_name) {
            return child;
        }
    }
    return nullptr;
}

AssetInfo::AnchorChildPointCandidate make_anchor_child_candidate_entry(const std::string& anchor_name,
                                                                       nlohmann::json candidates_array) {
    AssetInfo::AnchorChildPointCandidate entry{};
    entry.anchor_point_name = anchor_name;
    entry.candidates = nlohmann::json::object();
    entry.candidates["candidates"] = std::move(candidates_array);
    return entry;
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
    // Updates are queued; child transform should not change until flush.
    CHECK(spawned->world_x() != owner->world_x() + moved.offset_x);
    CHECK(spawned->world_y() != owner->world_y() + moved.offset_y);
    CHECK(spawned->world_z() != owner->world_z() + moved.offset_z);
    CHECK(anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().flush_pending_updates());
    CHECK_FALSE(anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().flush_pending_updates());

    CHECK(spawned->world_x() == owner->world_x() + moved.offset_x);
    CHECK(spawned->world_y() == owner->world_y() + moved.offset_y);
    CHECK(spawned->world_z() == owner->world_z() + moved.offset_z);

    const double anchor_world_depth =
        static_cast<double>(owner->world_z() + moved.offset_z) + moved.world_depth_offset;
    const double expected_bias = render_depth::bias_for_quantized_depth(anchor_world_depth, spawned->world_z());
    CHECK(spawned->render_depth_bias() == doctest::Approx(expected_bias));

    CHECK(anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().is_child_bound(spawned));
}

TEST_CASE("AnchorBoundAssetHelper batches repeated owner anchor dirties and flushes final state once") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 50, 100, 150, 0));
    REQUIRE(owner != nullptr);

    test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"eyes", 4, 5, 6, 0, 1, 0.0f, true});

    TestCustomController controller(owner);
    Input input;
    controller.schedule_child_creation("vibble_eyes");
    controller.update(input);

    ChildAsset* child = controller.child();
    REQUIRE(child != nullptr);
    child->bind("eyes");
    Asset* spawned = child->get_asset();
    REQUIRE(spawned != nullptr);

    const int initial_child_x = spawned->world_x();
    const int initial_child_y = spawned->world_y();
    const int initial_child_z = spawned->world_z();

    for (int step = 0; step < 8; ++step) {
        owner->move_to_world_position(60 + step, 120, 170, 0);
        test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"eyes", 4, 5, 6, 0, 1, 0.0f, true});
        anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().notify_anchor_changed(owner, "eyes");
    }

    // No flush yet: child should stay at the previous synchronized point.
    CHECK(spawned->world_x() == initial_child_x);
    CHECK(spawned->world_y() == initial_child_y);
    CHECK(spawned->world_z() == initial_child_z);

    CHECK(anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().flush_pending_updates());
    CHECK_FALSE(anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().flush_pending_updates());

    CHECK(spawned->world_x() == owner->world_x() + 4);
    CHECK(spawned->world_y() == owner->world_y() + 5);
    CHECK(spawned->world_z() == owner->world_z() + 6);
    CHECK(spawned->world_x() == 71);
}

TEST_CASE("Queued anchor flush keeps X-axis child placement stable across sequential moves") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 10, 20, 30, 0));
    REQUIRE(owner != nullptr);

    test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"eyes", 3, 2, 1, 0, 0, 0.0f, true});

    TestCustomController controller(owner);
    Input input;
    controller.schedule_child_creation("vibble_eyes");
    controller.update(input);

    ChildAsset* child = controller.child();
    REQUIRE(child != nullptr);
    child->bind("eyes");
    Asset* spawned = child->get_asset();
    REQUIRE(spawned != nullptr);

    int previous_child_x = spawned->world_x();
    for (int step = 0; step < 10; ++step) {
        owner->move_to_world_position(11 + step, 20, 30, 0);
        test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"eyes", 3, 2, 1, 0, 0, 0.0f, true});
        anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().notify_anchor_changed(owner, "eyes");
        CHECK(anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().flush_pending_updates());
        CHECK_FALSE(anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().flush_pending_updates());

        CHECK(spawned->world_x() == owner->world_x() + 3);
        CHECK(spawned->world_y() == owner->world_y() + 2);
        CHECK(spawned->world_z() == owner->world_z() + 1);
        CHECK(spawned->grid_resolution == 0);
        CHECK(spawned->world_x() == previous_child_x + 1);
        previous_child_x = spawned->world_x();
    }
}

TEST_CASE("Bound child movement marks render package dirty on every horizontal step") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 40, 20, 30, 0));
    REQUIRE(owner != nullptr);

    test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"eyes", 3, 2, 1, 0, 0, 0.0f, true});

    TestCustomController controller(owner);
    Input input;
    controller.schedule_child_creation("vibble_eyes");
    controller.update(input);

    ChildAsset* child = controller.child();
    REQUIRE(child != nullptr);
    child->bind("eyes");
    Asset* spawned = child->get_asset();
    REQUIRE(spawned != nullptr);

    spawned->clear_composite_dirty();
    spawned->clear_mesh_dirty();
    CHECK_FALSE(spawned->is_composite_dirty());
    CHECK_FALSE(spawned->is_mesh_dirty());

    for (int step = 0; step < 8; ++step) {
        owner->move_to_world_position(41 + step, 20, 30, 0);
        test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"eyes", 3, 2, 1, 0, 0, 0.0f, true});
        anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().notify_anchor_changed(owner, "eyes");
        CHECK(anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().flush_pending_updates());
        CHECK_FALSE(anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().flush_pending_updates());

        CHECK(spawned->world_x() == owner->world_x() + 3);
        CHECK(spawned->world_y() == owner->world_y() + 2);
        CHECK(spawned->world_z() == owner->world_z() + 1);
        CHECK(spawned->is_composite_dirty());
        CHECK(spawned->is_mesh_dirty());

        spawned->clear_composite_dirty();
        spawned->clear_mesh_dirty();
        CHECK_FALSE(spawned->is_composite_dirty());
        CHECK_FALSE(spawned->is_mesh_dirty());
    }
}

TEST_CASE("Child anchor residual stays stable across horizontal movement with exact subpixel offsets") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 10, 20, 30, 0));
    REQUIRE(owner != nullptr);

    AnchorSpec initial_anchor{};
    initial_anchor.name = "eyes";
    initial_anchor.offset_x = 3;
    initial_anchor.offset_y = 2;
    initial_anchor.offset_z = 1;
    initial_anchor.depth_offset = 0;
    initial_anchor.resolution_layer = 0;
    initial_anchor.world_depth_offset = 0.0f;
    initial_anchor.exists = true;
    initial_anchor.exact_offset_x = 3.25f;
    initial_anchor.exact_offset_y = 2.0f;
    initial_anchor.exact_offset_z = 1.0f;
    test_child_asset_runtime::set_anchor(*owner, initial_anchor);

    TestCustomController controller(owner);
    Input input;
    controller.schedule_child_creation("vibble_eyes");
    controller.update(input);

    ChildAsset* child = controller.child();
    REQUIRE(child != nullptr);
    child->bind("eyes");
    Asset* spawned = child->get_asset();
    REQUIRE(spawned != nullptr);

    for (int step = 0; step < 12; ++step) {
        owner->move_to_world_position(10 + step, 20, 30, 0);
        test_child_asset_runtime::set_anchor(*owner, initial_anchor);
        anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().notify_anchor_changed(owner, "eyes");
        CHECK(anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().flush_pending_updates());

        CHECK(spawned->world_x() == static_cast<int>(std::lround(static_cast<float>(owner->world_x()) + 3.25f)));
        CHECK(spawned->world_y() == owner->world_y() + 2);
        CHECK(spawned->world_z() == owner->world_z() + 1);
        CHECK(spawned->grid_resolution == 0);
        CHECK(spawned->render_anchor_offset_x() == doctest::Approx(0.25f).epsilon(1e-6));
        CHECK(spawned->render_anchor_offset_y() == doctest::Approx(0.0f).epsilon(1e-6));
        CHECK(spawned->render_anchor_offset_z() == doctest::Approx(0.0f).epsilon(1e-6));
    }
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

    AnchorSpec hidden_anchor{};
    hidden_anchor.name = "eyes";
    hidden_anchor.offset_x = 10;
    hidden_anchor.offset_y = 20;
    hidden_anchor.offset_z = 7;
    hidden_anchor.depth_offset = 0;
    hidden_anchor.resolution_layer = 4;
    hidden_anchor.world_depth_offset = 0.5f;
    hidden_anchor.exists = true;
    hidden_anchor.hidden = true;
    hidden_anchor.resolve_x = true;
    test_child_asset_runtime::set_anchor(*owner, hidden_anchor);
    child->update();
    CHECK(child->is_hidden());
    CHECK(spawned->is_anchor_hidden());

    hidden_anchor.hidden = false;
    test_child_asset_runtime::set_anchor(*owner, hidden_anchor);
    child->update();
    CHECK_FALSE(child->is_hidden());

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

TEST_CASE("ChildAsset anchor binding applies flat-point perspective override and clears on invalid paths") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 20, 30, 40, 1));
    REQUIRE(owner != nullptr);

    AnchorSpec anchor_spec{};
    anchor_spec.name = "eyes";
    anchor_spec.offset_x = 5;
    anchor_spec.offset_y = 6;
    anchor_spec.offset_z = 7;
    anchor_spec.resolution_layer = 2;
    anchor_spec.exists = true;
    anchor_spec.flat_perspective_scale = 2.4f;
    anchor_spec.flat_perspective_valid = true;
    test_child_asset_runtime::set_anchor(*owner, anchor_spec);

    TestCustomController controller(owner);
    Input input;
    controller.schedule_child_creation("vibble_eyes");
    controller.update(input);

    ChildAsset* child = controller.child();
    REQUIRE(child != nullptr);
    child->bind("eyes");
    Asset* spawned = child->get_asset();
    REQUIRE(spawned != nullptr);

    Asset::PerspectiveSample sample = spawned->runtime_perspective_sample();
    CHECK(spawned->has_anchor_perspective_override());
    CHECK(sample.source == Asset::PerspectiveSource::AnchorBindingOverride);
    CHECK(sample.scale == doctest::Approx(2.4f));

    anchor_spec.hidden = true;
    test_child_asset_runtime::set_anchor(*owner, anchor_spec);
    child->update();
    sample = spawned->runtime_perspective_sample();
    CHECK(child->is_hidden());
    CHECK_FALSE(spawned->has_anchor_perspective_override());
    CHECK(sample.source != Asset::PerspectiveSource::AnchorBindingOverride);

    anchor_spec.hidden = false;
    test_child_asset_runtime::set_anchor(*owner, anchor_spec);
    child->update();
    sample = spawned->runtime_perspective_sample();
    CHECK_FALSE(child->is_hidden());
    CHECK(spawned->has_anchor_perspective_override());
    CHECK(sample.source == Asset::PerspectiveSource::AnchorBindingOverride);

    test_child_asset_runtime::clear_anchors(*owner);
    child->update();
    sample = spawned->runtime_perspective_sample();
    CHECK(child->is_hidden());
    CHECK_FALSE(spawned->has_anchor_perspective_override());
    CHECK(sample.source != Asset::PerspectiveSource::AnchorBindingOverride);

    test_child_asset_runtime::set_anchor(*owner, anchor_spec);
    child->update();
    sample = spawned->runtime_perspective_sample();
    CHECK_FALSE(child->is_hidden());
    CHECK(spawned->has_anchor_perspective_override());
    CHECK(sample.source == Asset::PerspectiveSource::AnchorBindingOverride);

    child->unbind();
    sample = spawned->runtime_perspective_sample();
    CHECK_FALSE(spawned->has_anchor_perspective_override());
    CHECK(sample.source != Asset::PerspectiveSource::AnchorBindingOverride);
}

TEST_CASE("ChildAsset applies anchor flip and rotation overrides to spawned child") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 10, 15, 20, 0));
    REQUIRE(owner != nullptr);

    AnchorSpec anchor_spec{};
    anchor_spec.name = "eyes";
    anchor_spec.offset_x = 1;
    anchor_spec.offset_y = 2;
    anchor_spec.offset_z = 3;
    anchor_spec.flip_horizontal = true;
    anchor_spec.flip_vertical = false;
    anchor_spec.rotation_degrees = 22.5f;
    test_child_asset_runtime::set_anchor(*owner, anchor_spec);

    TestCustomController controller(owner);
    Input input;
    controller.schedule_child_creation("vibble_eyes");
    controller.update(input);

    ChildAsset* child = controller.child();
    REQUIRE(child != nullptr);
    child->bind("eyes");
    Asset* spawned = child->get_asset();
    REQUIRE(spawned != nullptr);

    CHECK(spawned->has_anchor_sprite_transform_override());
    CHECK((spawned->effective_render_flip() & SDL_FLIP_HORIZONTAL) != 0);
    CHECK((spawned->effective_render_flip() & SDL_FLIP_VERTICAL) == 0);
    CHECK(spawned->effective_render_angle() == doctest::Approx(22.5));

    anchor_spec.flip_horizontal = false;
    anchor_spec.flip_vertical = true;
    anchor_spec.rotation_degrees = -15.0f;
    test_child_asset_runtime::set_anchor(*owner, anchor_spec);
    child->update();

    CHECK((spawned->effective_render_flip() & SDL_FLIP_HORIZONTAL) == 0);
    CHECK((spawned->effective_render_flip() & SDL_FLIP_VERTICAL) != 0);
    CHECK(spawned->effective_render_angle() == doctest::Approx(-15.0));

    anchor_spec.exists = false;
    test_child_asset_runtime::set_anchor(*owner, anchor_spec);
    child->update();
    CHECK_FALSE(spawned->has_anchor_sprite_transform_override());
    CHECK(spawned->effective_render_flip() == SDL_FLIP_NONE);
    CHECK(spawned->effective_render_angle() == doctest::Approx(0.0));
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

TEST_CASE("CustomAssetController resolves anchor child candidates in constructor and binds child") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 32, 48, 64, 0));
    REQUIRE(owner != nullptr);
    REQUIRE(owner->info != nullptr);

    test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"hat", 5, 7, 3, 0, 0, 0.0f, true});
    assets_scope.assets->library().add_asset("vibble_hat", nlohmann::json::object());

    owner->info->anchor_point_child_candidates = {
        make_anchor_child_candidate_entry(
            "hat",
            nlohmann::json::array({nlohmann::json{
                {"name", "vibble_hat"},
                {"chance", 100}
            }}))
    };

    CustomAssetController controller(owner);
    Input input;
    controller.update(input);

    Asset* candidate_child = find_owner_child_named(owner, "vibble_hat");
    REQUIRE(candidate_child != nullptr);
    CHECK(candidate_child->world_x() == owner->world_x() + 5);
    CHECK(candidate_child->world_y() == owner->world_y() + 7);
    CHECK(candidate_child->world_z() == owner->world_z() + 3);
}

TEST_CASE("CustomAssetController skips null anchor child candidate resolution") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 10, 20, 30, 0));
    REQUIRE(owner != nullptr);
    REQUIRE(owner->info != nullptr);

    owner->info->anchor_point_child_candidates = {
        make_anchor_child_candidate_entry(
            "hat",
            nlohmann::json::array({nlohmann::json{
                {"name", "null"},
                {"chance", 100}
            }}))
    };

    CustomAssetController controller(owner);
    Input input;
    controller.update(input);

    CHECK(find_owner_child_named(owner, "vibble_hat") == nullptr);
}

TEST_CASE("CustomAssetController candidate resolution is deterministic for same anchor and position") {
    AssetsScope assets_scope;
    assets_scope.assets->library().add_asset("vibble_hat", nlohmann::json::object());
    assets_scope.assets->library().add_asset("vibble_mouth", nlohmann::json::object());

    auto make_owner = [&](int x, int y, int z) {
        Asset* owner = test_child_asset_runtime::attach_owned_asset(
            assets_scope.assets,
            test_child_asset_runtime::make_test_asset("vibble", x, y, z, 0));
        REQUIRE(owner != nullptr);
        REQUIRE(owner->info != nullptr);
        test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"hat", 2, 2, 2, 0, 0, 0.0f, true});
        owner->info->anchor_point_child_candidates = {
            make_anchor_child_candidate_entry(
                "hat",
                nlohmann::json::array({
                    nlohmann::json{{"name", "vibble_hat"}, {"chance", 50}},
                    nlohmann::json{{"name", "vibble_mouth"}, {"chance", 50}}
                }))
        };
        return owner;
    };

    Asset* owner_a = make_owner(40, 50, 60);
    Asset* owner_b = make_owner(40, 50, 60);

    CustomAssetController controller_a(owner_a);
    CustomAssetController controller_b(owner_b);
    Input input;
    controller_a.update(input);
    controller_b.update(input);

    Asset* child_a_hat = find_owner_child_named(owner_a, "vibble_hat");
    Asset* child_a_mouth = find_owner_child_named(owner_a, "vibble_mouth");
    Asset* child_b_hat = find_owner_child_named(owner_b, "vibble_hat");
    Asset* child_b_mouth = find_owner_child_named(owner_b, "vibble_mouth");

    const bool a_selected_hat = child_a_hat != nullptr;
    const bool b_selected_hat = child_b_hat != nullptr;
    const bool a_selected_mouth = child_a_mouth != nullptr;
    const bool b_selected_mouth = child_b_mouth != nullptr;
    CHECK((a_selected_hat && b_selected_hat) || (a_selected_mouth && b_selected_mouth));
}

TEST_CASE("CustomAssetController anchor candidate retries stop after 60 failed updates") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 10, 15, 20, 0));
    REQUIRE(owner != nullptr);
    REQUIRE(owner->info != nullptr);

    test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"hat", 1, 1, 1, 0, 0, 0.0f, true});
    assets_scope.assets->library().add_asset("vibble_hat", nlohmann::json::object());
    owner->info->anchor_point_child_candidates = {
        make_anchor_child_candidate_entry(
            "hat",
            nlohmann::json::array({nlohmann::json{
                {"name", "vibble_hat"},
                {"chance", 100}
            }}))
    };

    test_child_asset_runtime::set_spawn_failures(*assets_scope.assets, "vibble_hat", 10'000);

    CustomAssetController controller(owner);
    Input input;
    for (int idx = 0; idx < 75; ++idx) {
        controller.update(input);
    }
    CHECK(find_owner_child_named(owner, "vibble_hat") == nullptr);

    test_child_asset_runtime::set_spawn_failures(*assets_scope.assets, "vibble_hat", 0);
    for (int idx = 0; idx < 10; ++idx) {
        controller.update(input);
    }
    CHECK(find_owner_child_named(owner, "vibble_hat") == nullptr);
}

TEST_CASE("CustomAssetController anchor candidate recovers when failures stay below retry cap") {
    AssetsScope assets_scope;
    Asset* owner = test_child_asset_runtime::attach_owned_asset(
        assets_scope.assets,
        test_child_asset_runtime::make_test_asset("vibble", 22, 33, 44, 0));
    REQUIRE(owner != nullptr);
    REQUIRE(owner->info != nullptr);

    test_child_asset_runtime::set_anchor(*owner, AnchorSpec{"hat", 3, 4, 5, 0, 0, 0.0f, true});
    assets_scope.assets->library().add_asset("vibble_hat", nlohmann::json::object());
    owner->info->anchor_point_child_candidates = {
        make_anchor_child_candidate_entry(
            "hat",
            nlohmann::json::array({nlohmann::json{
                {"name", "vibble_hat"},
                {"chance", 100}
            }}))
    };

    test_child_asset_runtime::set_spawn_failures(*assets_scope.assets, "vibble_hat", 3);

    CustomAssetController controller(owner);
    Input input;
    for (int idx = 0; idx < 10; ++idx) {
        controller.update(input);
    }

    Asset* candidate_child = find_owner_child_named(owner, "vibble_hat");
    REQUIRE(candidate_child != nullptr);
    CHECK(candidate_child->world_x() == owner->world_x() + 3);
    CHECK(candidate_child->world_y() == owner->world_y() + 4);
    CHECK(candidate_child->world_z() == owner->world_z() + 5);
}

TEST_CASE("Manifest no longer authors vibble eyes as a spawn-group child asset") {
    const std::filesystem::path manifest_path = repo_root() / "manifest.json";
    std::ifstream input(manifest_path);
    REQUIRE(input.is_open());

    nlohmann::json manifest = nlohmann::json::parse(input, nullptr, true, true);
    CHECK_FALSE(contains_spawn_candidate_named(manifest, "vibble_eyes"));
    CHECK(manifest["assets"].contains("vibble_eyes"));
}
