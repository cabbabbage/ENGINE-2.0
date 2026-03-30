#include <doctest/doctest.h>

#include <vector>

#include <nlohmann/json.hpp>

#include "devtools/room_anchor_mode_utils.hpp"

namespace {

using devmode::room_anchor_mode::default_anchor_position_for_frame;
using devmode::room_anchor_mode::make_default_anchor_for_frame;
using devmode::room_anchor_mode::make_unique_anchor_name;
using devmode::room_anchor_mode::next_default_anchor_name;
using devmode::room_anchor_mode::normalize_anchor_points_payload;
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
    CHECK(next_default_anchor_name(existing) == "anchor_1");
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
    CHECK(payload["anchor_points"][2].is_array());
    CHECK(payload["anchor_points"][3].is_array());
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
