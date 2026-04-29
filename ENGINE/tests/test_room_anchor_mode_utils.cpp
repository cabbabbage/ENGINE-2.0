#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

#include <nlohmann/json.hpp>

#include "devtools/room_anchor_mode_utils.hpp"
#include "room_editor_payload_contract_test_helper.hpp"

namespace {

using devmode::room_anchor_mode::default_anchor_position_for_frame;
using devmode::room_anchor_mode::anchor_mutable_in_mode;
using devmode::room_anchor_mode::anchor_visible_in_mode;
using devmode::room_anchor_mode::delete_anchor_in_mode;
using devmode::room_anchor_mode::find_anchor_in_mode;
using devmode::room_anchor_mode::make_default_anchor_for_frame;
using devmode::room_anchor_mode::make_unique_anchor_name;
using devmode::room_anchor_mode::next_default_anchor_name;
using devmode::room_anchor_mode::AnchorPointOwner;
using devmode::room_anchor_mode::normalize_anchor_points_payload;
using devmode::room_anchor_mode::rename_anchor_in_mode;
using devmode::room_anchor_mode::serialize_anchor_frame;
using devmode::room_anchor_mode::write_anchor_frame_to_payload;
using devmode::room_anchor_mode::wrap_index;

}  // namespace

TEST_CASE("Anchor mode wrap_index wraps positive and negative indices") {
    CHECK(wrap_index(0, 4) == 0);
    CHECK(wrap_index(3, 4) == 3);
    CHECK(wrap_index(4, 4) == 0);
    CHECK(wrap_index(5, 4) == 1);
    CHECK(wrap_index(-1, 4) == 3);
    CHECK(wrap_index(-5, 4) == 3);
}

TEST_CASE("Anchor mode unique naming trims and appends numeric suffixes") {
    const std::vector<std::string> existing{"anchor", "anchor_2", "hand"};

    CHECK(make_unique_anchor_name("  hand  ", existing) == "hand_2");
    CHECK(make_unique_anchor_name("  ", existing) == "anchor_3");
    CHECK(make_unique_anchor_name("new_anchor", existing) == "new_anchor");
    CHECK(make_unique_anchor_name("hand", existing, "hand") == "hand");
    CHECK(next_default_anchor_name(existing, AnchorPointOwner::NonLight) == "norm_anhor_point_1");
    CHECK(next_default_anchor_name(existing, AnchorPointOwner::Light) == "lightanchor_point_1");
}

TEST_CASE("Anchor mode default anchor position is center-bottom with depth zero") {
    const SDL_Point pos = default_anchor_position_for_frame(10, 8);
    CHECK(pos.x == 5);
    CHECK(pos.y == 7);

    const auto anchor = make_default_anchor_for_frame("anchor_1", 10, 8);
    CHECK(anchor.name == "anchor_1");
    CHECK(anchor.texture_x == 5);
    CHECK(anchor.texture_y == 7);
    CHECK(anchor.depth_offset == doctest::Approx(0.0f));
    CHECK(anchor.flip_horizontal);
    CHECK(anchor.flip_vertical);
    CHECK(anchor.rotation_degrees == doctest::Approx(0.0f));
    CHECK(anchor.hidden == false);
    CHECK(anchor.resolve_x == true);
    CHECK(anchor.scaling_method == AnchorScalingMethod::Parent);
}

TEST_CASE("Anchor mode payload write normalizes frame count and writes current frame only") {
    nlohmann::json payload = nlohmann::json::object({
        {"anchor_points", nlohmann::json::array({
            nlohmann::json::array({nlohmann::json::object({
                {"name", "keep_a"},
                {"texture_x", 1},
                {"texture_y", 2},
                {"depth_offset", 3},
            })}),
            nlohmann::json::array({nlohmann::json::object({
                {"name", "replace_me"},
                {"texture_x", 4},
                {"texture_y", 5},
                {"depth_offset", 6},
            })}),
        })}
    });

    const std::vector<DisplacedAssetAnchorPoint> replacement{
        DisplacedAssetAnchorPoint{"edited", 10, 11, 12.5f}
    };

    REQUIRE(write_anchor_frame_to_payload(payload, 4, 1, replacement));

    REQUIRE(payload.contains("anchor_points"));
    REQUIRE(payload["anchor_points"].is_array());
    REQUIRE(payload["anchor_points"].size() == 4);

    CHECK(payload["anchor_points"][0][0]["name"] == "keep_a");
    CHECK(payload["anchor_points"][1][0]["name"] == "edited");
    CHECK(payload["anchor_points"][1][0]["texture_x"] == 10);
    CHECK(payload["anchor_points"][1][0]["texture_y"] == 11);
    CHECK(payload["anchor_points"][1][0]["depth_offset"].get<float>() == doctest::Approx(12.5f));
    CHECK(payload["anchor_points"][1][0]["flip_horizontal"] == true);
    CHECK(payload["anchor_points"][1][0]["flip_vertical"] == true);
    CHECK(payload["anchor_points"][1][0]["rotation_degrees"].get<float>() == doctest::Approx(0.0f));
    CHECK(payload["anchor_points"][1][0]["hidden"] == false);
    CHECK(payload["anchor_points"][1][0]["resolve_x"] == true);
    CHECK(payload["anchor_points"][1][0]["scaling_method"] == "parent");
    CHECK(payload["anchor_points"][2].is_array());
    CHECK(payload["anchor_points"][3].is_array());
}

TEST_CASE("Anchor mode mixed payload contract preserves untouched keys and fails atomically on invalid bounds") {
    nlohmann::json payload = room_editor_test_payload_contract::make_full_mixed_animation_payload();
    const nlohmann::json before = payload;

    const std::vector<DisplacedAssetAnchorPoint> replacement{
        DisplacedAssetAnchorPoint{"edited", 10, 11, 12.5f}
    };
    REQUIRE(write_anchor_frame_to_payload(payload, 1, 0, replacement));

    const auto keys = room_editor_test_payload_contract::unchanged_keys_excluding(before, {"anchor_points"});
    const auto before_snapshot = room_editor_test_payload_contract::snapshot_key_bytes(before);
    const auto after_snapshot = room_editor_test_payload_contract::snapshot_key_bytes(payload);
    for (const auto& key : keys) {
        INFO("key=" << key);
        REQUIRE(before_snapshot.find(key) != before_snapshot.end());
        REQUIRE(after_snapshot.find(key) != after_snapshot.end());
        CHECK(before_snapshot.at(key) == after_snapshot.at(key));
    }

    const std::string stable_after_success = payload.dump();
    CHECK_FALSE(write_anchor_frame_to_payload(payload, 0, 0, replacement));
    CHECK(payload.dump() == stable_after_success);
    CHECK_FALSE(write_anchor_frame_to_payload(payload, 1, 3, replacement));
    CHECK(payload.dump() == stable_after_success);
}

TEST_CASE("Anchor mode serialization preserves scaling method tokens") {
    std::vector<DisplacedAssetAnchorPoint> anchors;

    DisplacedAssetAnchorPoint parent_anchor{"a_parent", 1, 2, 0.0f};
    parent_anchor.scaling_method = AnchorScalingMethod::Parent;
    anchors.push_back(parent_anchor);

    DisplacedAssetAnchorPoint real_3d_anchor{"a_real_3d", 3, 4, 1.0f};
    real_3d_anchor.scaling_method = AnchorScalingMethod::Real3DPoint;
    anchors.push_back(real_3d_anchor);

    DisplacedAssetAnchorPoint relative_2d_anchor{"a_relative_2d", 5, 6, 2.0f};
    relative_2d_anchor.scaling_method = AnchorScalingMethod::Relative2DAnchorPoint;
    anchors.push_back(relative_2d_anchor);

    DisplacedAssetAnchorPoint floor_anchor{"a_floor", 7, 8, 3.0f};
    floor_anchor.scaling_method = AnchorScalingMethod::Real3DFloorPoint;
    anchors.push_back(floor_anchor);

    const nlohmann::json frame_json = serialize_anchor_frame(anchors);
    REQUIRE(frame_json.is_array());
    REQUIRE(frame_json.size() == 4);

    CHECK(frame_json[0]["scaling_method"] == "parent");
    CHECK(frame_json[1]["scaling_method"] == "real_3d_point");
    CHECK(frame_json[2]["scaling_method"] == "relative_2d_anchor_point");
    CHECK(frame_json[3]["scaling_method"] == "real_3d_floor_point");
}

TEST_CASE("Anchor scaling method parser falls back to parent for invalid tokens") {
    CHECK(anchor_points::anchor_scaling_method_from_token("parent") == AnchorScalingMethod::Parent);
    CHECK(anchor_points::anchor_scaling_method_from_token("real_3d_point") == AnchorScalingMethod::Real3DPoint);
    CHECK(anchor_points::anchor_scaling_method_from_token("relative_2d_anchor_point") ==
          AnchorScalingMethod::Relative2DAnchorPoint);
    CHECK(anchor_points::anchor_scaling_method_from_token("real_3d_floor_point") ==
          AnchorScalingMethod::Real3DFloorPoint);
    CHECK(anchor_points::anchor_scaling_method_from_token("not_a_method") == AnchorScalingMethod::Parent);
}

TEST_CASE("Anchor mode payload normalization enforces exact frame count") {
    nlohmann::json payload = nlohmann::json::object({
        {"anchor_points", nlohmann::json::array({
            nlohmann::json::array(),
            nlohmann::json::array(),
            nlohmann::json::array(),
            nlohmann::json::array(),
            nlohmann::json::array(),
        })}
    });

    normalize_anchor_points_payload(payload, 3);
    REQUIRE(payload.contains("anchor_points"));
    REQUIRE(payload["anchor_points"].is_array());
    CHECK(payload["anchor_points"].size() == 3);
}

TEST_CASE("Anchor mode ownership isolates light and non-light anchors") {
    std::vector<DisplacedAssetAnchorPoint> anchors{
        DisplacedAssetAnchorPoint{"shared", 1, 2, 0.0f},
        DisplacedAssetAnchorPoint{"shared", 3, 4, 0.0f},
        DisplacedAssetAnchorPoint{"anchor_only", 5, 6, 0.0f},
        DisplacedAssetAnchorPoint{"light_only", 7, 8, 0.0f},
        DisplacedAssetAnchorPoint{"shared_oval_center", 9, 10, 0.0f},
    };
    anchors[1].has_light_data = true;
    anchors[3].has_light_data = true;

    const auto is_reserved_name = [](const std::string& name) {
        return name == "shared_oval_center";
    };

    CHECK(anchor_visible_in_mode(anchors[0],
                                 devmode::room_anchor_mode::AnchorPointOwner::NonLight,
                                 is_reserved_name));
    CHECK_FALSE(anchor_visible_in_mode(anchors[0],
                                       devmode::room_anchor_mode::AnchorPointOwner::Light,
                                       is_reserved_name));
    CHECK(anchor_visible_in_mode(anchors[1],
                                 devmode::room_anchor_mode::AnchorPointOwner::Light,
                                 is_reserved_name));
    CHECK_FALSE(anchor_visible_in_mode(anchors[1],
                                       devmode::room_anchor_mode::AnchorPointOwner::NonLight,
                                       is_reserved_name));
    CHECK(!anchor_mutable_in_mode(anchors[4],
                                  devmode::room_anchor_mode::AnchorPointOwner::NonLight,
                                  is_reserved_name));
    CHECK(!anchor_mutable_in_mode(anchors[4],
                                  devmode::room_anchor_mode::AnchorPointOwner::Light,
                                  is_reserved_name));
}

TEST_CASE("Anchor mode rename and delete respect ownership split for same names") {
    std::vector<DisplacedAssetAnchorPoint> anchors{
        DisplacedAssetAnchorPoint{"shared", 1, 2, 0.0f},
        DisplacedAssetAnchorPoint{"shared", 3, 4, 0.0f},
        DisplacedAssetAnchorPoint{"shared", 7, 8, 0.0f},
        DisplacedAssetAnchorPoint{"other", 5, 6, 0.0f},
    };
    anchors[1].has_light_data = true;
    anchors[2].has_light_data = true;

    const auto is_reserved_name = [](const std::string&) {
        return false;
    };

    REQUIRE(rename_anchor_in_mode(anchors,
                                  "shared",
                                  "anchor_renamed",
                                  devmode::room_anchor_mode::AnchorPointOwner::NonLight,
                                  is_reserved_name));

    CHECK(anchors[0].name == "anchor_renamed");
    CHECK(anchors[1].name == "shared");
    CHECK(anchors[2].name == "shared");
    CHECK(anchors[3].name == "other");

    REQUIRE(delete_anchor_in_mode(anchors,
                                  "shared",
                                  devmode::room_anchor_mode::AnchorPointOwner::Light,
                                  is_reserved_name));

    CHECK(anchors.size() == 2);
    CHECK(anchors[0].name == "anchor_renamed");
    CHECK(!anchors[0].has_light_data);
    CHECK(anchors[1].name == "other");
    CHECK(find_anchor_in_mode(anchors,
                              "other",
                              devmode::room_anchor_mode::AnchorPointOwner::NonLight,
                              is_reserved_name) != nullptr);
    CHECK(find_anchor_in_mode(anchors,
                              "anchor_renamed",
                              devmode::room_anchor_mode::AnchorPointOwner::NonLight,
                              is_reserved_name) != nullptr);
    const auto remaining_light_it = std::find_if(
        anchors.begin(), anchors.end(), [](const DisplacedAssetAnchorPoint& anchor) {
            return anchor.name == "shared" && anchor.has_light_data;
        });
    CHECK(remaining_light_it == anchors.end());
}
