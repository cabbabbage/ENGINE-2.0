#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "animation/controllers/custom_controllers/vibble_controller.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/asset_info.hpp"
#include "utils/input.hpp"

namespace {

constexpr float kPi = 3.14159265358979323846f;

float degrees_to_radians(float degrees) {
    return degrees * (kPi / 180.0f);
}

AssetInfo::OvalAnchorPoint make_oval_point(float angle_degrees,
                                           int texture_x,
                                           int texture_y,
                                           float depth_offset,
                                           float rotation_degrees,
                                           bool hidden,
                                           bool resolve_x,
                                           bool flip_horizontal,
                                           bool flip_vertical,
                                           AnchorScalingMethod scaling_method) {
    AssetInfo::OvalAnchorPoint point{};
    point.angle_degrees = angle_degrees;
    point.texture_x = texture_x;
    point.texture_y = texture_y;
    point.depth_offset = depth_offset;
    point.rotation_degrees = rotation_degrees;
    point.hidden = hidden;
    point.resolve_x = resolve_x;
    point.flip_horizontal = flip_horizontal;
    point.flip_vertical = flip_vertical;
    point.scaling_method = scaling_method;
    return point;
}

std::shared_ptr<AssetInfo> make_info_with_single_frame_anchor(const std::string& asset_name,
                                                              const DisplacedAssetAnchorPoint& anchor) {
    auto info = std::make_shared<AssetInfo>(asset_name);
    Animation animation{};
    auto& path = animation.movement_path(0);
    path.clear();

    AnimationFrame frame{};
    frame.anchor_points.push_back(anchor);
    frame.rebuild_anchor_lookup();
    path.push_back(frame);

    info->animations.clear();
    info->animations["default"] = std::move(animation);
    info->start_animation = "default";
    return info;
}

bool has_candidate_named(const nlohmann::json& normalized_entry, const std::string& name) {
    if (!normalized_entry.is_object()) {
        return false;
    }
    const auto it = normalized_entry.find("candidates");
    if (it == normalized_entry.end() || !it->is_array()) {
        return false;
    }
    for (const auto& candidate : *it) {
        if (!candidate.is_object()) {
            continue;
        }
        if (candidate.value("name", std::string{}) == name) {
            return true;
        }
    }
    return false;
}

void set_animation_with_empty_anchor_frames(AssetInfo& info,
                                            const std::string& animation_name,
                                            int frame_count) {
    const int clamped_count = std::max(1, frame_count);
    Animation animation{};
    auto& path = animation.movement_path(0);
    path.clear();
    path.reserve(static_cast<std::size_t>(clamped_count));
    for (int i = 0; i < clamped_count; ++i) {
        AnimationFrame frame{};
        frame.frame_index = i;
        frame.rebuild_anchor_lookup();
        path.push_back(frame);
    }
    info.animations.clear();
    info.animations[animation_name] = std::move(animation);
    info.start_animation = animation_name;

    nlohmann::json animation_payload = nlohmann::json::object();
    animation_payload["source"] = nlohmann::json::object({
        {"kind", "folder"},
        {"path", animation_name},
    });
    animation_payload["anchor_points"] = nlohmann::json::array();
    for (int i = 0; i < clamped_count; ++i) {
        animation_payload["anchor_points"].push_back(nlohmann::json::array());
    }
    REQUIRE(info.update_animation_properties(animation_name, animation_payload));
}

bool frame_has_anchor(const AnimationFrame& frame, const std::string& anchor_name) {
    return frame.find_anchor(anchor_name) != nullptr;
}

bool frame_anchor_payload_has_name(const nlohmann::json& frame_payload, const std::string& anchor_name) {
    if (!frame_payload.is_array()) {
        return false;
    }
    for (const auto& entry : frame_payload) {
        if (!entry.is_object()) {
            continue;
        }
        if (entry.value("name", std::string{}) == anchor_name) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("AssetInfo oval mappings normalize defaults and round-trip") {
    AssetInfo info("oval_asset");

    AssetInfo::OvalAnchorMapping mapping{};
    mapping.name = "eyes";
    mapping.width_radius_x = -10.0f;
    mapping.height_radius_z = 0.0f;
    mapping.points.push_back(make_oval_point(-45.0f,
                                             100,
                                             25,
                                             std::numeric_limits<float>::infinity(),
                                             std::numeric_limits<float>::quiet_NaN(),
                                             false,
                                             true,
                                             true,
                                             true,
                                             AnchorScalingMethod::Parent));
    REQUIRE(info.upsert_oval_anchor_mapping(mapping));
    REQUIRE(info.oval_anchor_mappings.size() == 1);

    const AssetInfo::OvalAnchorMapping* stored_eyes = info.find_oval_anchor_mapping("eyes", false);
    REQUIRE(stored_eyes != nullptr);
    CHECK(stored_eyes->width_radius_x == doctest::Approx(48.0f));
    CHECK(stored_eyes->height_radius_z == doctest::Approx(24.0f));
    REQUIRE(!stored_eyes->points.empty());
    CHECK(stored_eyes->points.front().angle_degrees == doctest::Approx(315.0f));
    CHECK(stored_eyes->points.front().texture_x == 34);
    CHECK(stored_eyes->points.front().texture_y == -17);
    CHECK(stored_eyes->points.front().depth_offset == doctest::Approx(0.0f).epsilon(1e-4f));
    CHECK(stored_eyes->points.front().rotation_degrees == doctest::Approx(0.0f));
    CHECK(stored_eyes->asset_name == "oval_asset");

    AssetInfo::OvalAnchorMapping auto_points{};
    auto_points.name = "hat";
    auto_points.width_radius_x = 60.0f;
    auto_points.height_radius_z = 30.0f;
    REQUIRE(info.upsert_oval_anchor_mapping(auto_points));
    const AssetInfo::OvalAnchorMapping* stored_hat = info.find_oval_anchor_mapping("hat", false);
    REQUIRE(stored_hat != nullptr);
    REQUIRE(stored_hat->points.size() == 8);
    CHECK(stored_hat->points[0].angle_degrees == doctest::Approx(0.0f));
    CHECK(stored_hat->points[1].angle_degrees == doctest::Approx(45.0f));

    const nlohmann::json payload = info.oval_anchor_mappings_payload();
    REQUIRE(payload.is_array());
    CHECK(payload.size() == 2);

    AssetInfo copy("oval_asset_copy");
    copy.set_oval_anchor_mappings_payload(payload);
    CHECK(copy.oval_anchor_mappings_payload() == payload);
}

TEST_CASE("AssetInfo oval mappings drop duplicate point angles and keep valid ordering") {
    AssetInfo info("oval_dedupe_asset");
    AssetInfo::OvalAnchorMapping mapping{};
    mapping.name = "hat";
    mapping.asset_name = "vibble_hat";
    mapping.width_radius_x = 20.0f;
    mapping.height_radius_z = 10.0f;
    mapping.points = {
        make_oval_point(0.0f, 0, 0, 0.0f, 0.0f, false, true, true, true, AnchorScalingMethod::Parent),
        make_oval_point(360.0f, 0, 0, 1.0f, 5.0f, false, true, true, true, AnchorScalingMethod::Parent),
        make_oval_point(45.0f, 0, 0, 2.0f, 10.0f, false, true, true, true, AnchorScalingMethod::Parent),
    };

    REQUIRE(info.upsert_oval_anchor_mapping(mapping));
    const AssetInfo::OvalAnchorMapping* stored = info.find_oval_anchor_mapping("hat", false);
    REQUIRE(stored != nullptr);
    REQUIRE(stored->points.size() == 2);
    CHECK(stored->points[0].angle_degrees == doctest::Approx(0.0f));
    CHECK(stored->points[1].angle_degrees == doctest::Approx(45.0f));
}

TEST_CASE("AssetInfo oval rename appends legacy alias and renames anchor candidate entries") {
    AssetInfo info("oval_rename_asset");
    set_animation_with_empty_anchor_frames(info, "default", 2);

    AssetInfo::OvalAnchorMapping mapping{};
    mapping.name = "eyes";
    mapping.width_radius_x = 40.0f;
    mapping.height_radius_z = 20.0f;
    mapping.points = {
        make_oval_point(0.0f, 0, 0, 0.0f, 0.0f, false, true, true, true, AnchorScalingMethod::Parent),
        make_oval_point(180.0f, 0, 0, 4.0f, 45.0f, true, false, false, false, AnchorScalingMethod::Real3DPoint),
    };
    REQUIRE(info.upsert_oval_anchor_mapping(mapping));
    {
        auto before_it = info.animations.find("default");
        REQUIRE(before_it != info.animations.end());
        for (std::size_t frame_index = 0; frame_index < before_it->second.frame_count(); ++frame_index) {
            const AnimationFrame* frame = before_it->second.primary_frame_at(frame_index);
            REQUIRE(frame != nullptr);
            CHECK(frame_has_anchor(*frame, "eyes_oval_center"));
        }
    }

    REQUIRE(info.upsert_anchor_point_child_candidate(
        "eyes",
        nlohmann::json::object({
            {"vibble_eyes", 100}
        })));

    REQUIRE(info.rename_oval_anchor_mapping("eyes", "eyes_front", true));
    const AssetInfo::OvalAnchorMapping* renamed = info.find_oval_anchor_mapping("eyes_front", true);
    REQUIRE(renamed != nullptr);
    CHECK(std::find(renamed->legacy_names.begin(), renamed->legacy_names.end(), "eyes") != renamed->legacy_names.end());

    const nlohmann::json moved_candidates = info.anchor_point_child_candidate_candidates("eyes_front");
    CHECK(has_candidate_named(moved_candidates, "vibble_eyes"));

    auto anim_it = info.animations.find("default");
    REQUIRE(anim_it != info.animations.end());
    REQUIRE(anim_it->second.has_frames());
    for (std::size_t frame_index = 0; frame_index < anim_it->second.frame_count(); ++frame_index) {
        const AnimationFrame* frame = anim_it->second.primary_frame_at(frame_index);
        REQUIRE(frame != nullptr);
        CHECK(frame_has_anchor(*frame, "eyes_front_oval_center"));
        CHECK_FALSE(frame_has_anchor(*frame, "eyes_oval_center"));
    }
}

TEST_CASE("AssetInfo oval delete removes center anchors for canonical and legacy names") {
    AssetInfo info("oval_delete_asset");
    set_animation_with_empty_anchor_frames(info, "default", 3);

    AssetInfo::OvalAnchorMapping mapping{};
    mapping.name = "eyes_front";
    mapping.asset_name = "oval_delete_asset";
    mapping.legacy_names = {"eyes"};
    mapping.width_radius_x = 40.0f;
    mapping.height_radius_z = 20.0f;
    mapping.points = {
        make_oval_point(0.0f, 0, 0, 0.0f, 0.0f, false, true, true, true, AnchorScalingMethod::Parent),
        make_oval_point(180.0f, 0, 0, 0.0f, 0.0f, false, true, true, true, AnchorScalingMethod::Parent),
    };
    REQUIRE(info.upsert_oval_anchor_mapping(mapping));

    auto before_it = info.animations.find("default");
    REQUIRE(before_it != info.animations.end());
    for (std::size_t frame_index = 0; frame_index < before_it->second.frame_count(); ++frame_index) {
        const AnimationFrame* frame = before_it->second.primary_frame_at(frame_index);
        REQUIRE(frame != nullptr);
        CHECK(frame_has_anchor(*frame, "eyes_front_oval_center"));
        CHECK(frame_has_anchor(*frame, "eyes_oval_center"));
    }

    REQUIRE(info.remove_oval_anchor_mapping("eyes_front"));
    CHECK(info.find_oval_anchor_mapping("eyes_front", true) == nullptr);

    auto after_it = info.animations.find("default");
    REQUIRE(after_it != info.animations.end());
    for (std::size_t frame_index = 0; frame_index < after_it->second.frame_count(); ++frame_index) {
        const AnimationFrame* frame = after_it->second.primary_frame_at(frame_index);
        REQUIRE(frame != nullptr);
        CHECK_FALSE(frame_has_anchor(*frame, "eyes_front_oval_center"));
        CHECK_FALSE(frame_has_anchor(*frame, "eyes_oval_center"));
    }

    const nlohmann::json payload = info.animation_payload("default");
    REQUIRE(payload.is_object());
    const auto anchor_points_it = payload.find("anchor_points");
    REQUIRE(anchor_points_it != payload.end());
    REQUIRE(anchor_points_it->is_array());
    for (const auto& frame_payload : *anchor_points_it) {
        CHECK_FALSE(frame_anchor_payload_has_name(frame_payload, "eyes_front_oval_center"));
        CHECK_FALSE(frame_anchor_payload_has_name(frame_payload, "eyes_oval_center"));
    }
}

TEST_CASE("Vibble assets seed default oval mappings") {
    AssetInfo vibble("vibble");
    REQUIRE(vibble.oval_anchor_mappings.size() >= 5);

    for (const std::string& required_name : {"eyes", "hat", "mouth", "neck", "weapon"}) {
        const AssetInfo::OvalAnchorMapping* mapping = vibble.find_oval_anchor_mapping(required_name, false);
        REQUIRE(mapping != nullptr);
        CHECK(mapping->asset_name == "vibble");
        CHECK(mapping->points.size() == 8);
    }
}

TEST_CASE("Oval runtime interpolation blends numeric fields and uses nearest discrete fields") {
    auto info = std::make_shared<AssetInfo>("oval_runtime_asset");
    info->oval_anchor_mappings.clear();

    AssetInfo::OvalAnchorMapping mapping{};
    mapping.name = "eyes";
    mapping.asset_name = "oval_runtime_asset";
    mapping.width_radius_x = 50.0f;
    mapping.height_radius_z = 20.0f;
    mapping.points = {
        make_oval_point(0.0f, 0, 0, 0.0f, 0.0f, false, true, true, true, AnchorScalingMethod::Parent),
        make_oval_point(90.0f, 20, 10, 10.0f, 90.0f, true, false, false, false, AnchorScalingMethod::Real3DPoint),
    };
    REQUIRE(info->upsert_oval_anchor_mapping(mapping));

    Area spawn_area("oval_runtime_area", 0);
    Asset asset(info, spawn_area, SDL_Point{100, 100}, 0);

    REQUIRE(asset.set_directional_heading_radians(degrees_to_radians(45.0f)));
    auto mid = asset.anchor_state("eyes");
    REQUIRE(mid.has_value());
    CHECK(mid->is_active());
    CHECK(mid->depth_offset == doctest::Approx(5.0f).epsilon(1e-4));
    CHECK(mid->rotation_degrees == doctest::Approx(45.0f).epsilon(1e-4));
    CHECK(mid->hidden == false);
    CHECK(mid->resolve_x == true);
    CHECK(mid->scaling_method == AnchorScalingMethod::Parent);

    REQUIRE(asset.set_directional_heading_radians(degrees_to_radians(80.0f)));
    auto near_ninety = asset.anchor_state("eyes");
    REQUIRE(near_ninety.has_value());
    CHECK(near_ninety->depth_offset == doctest::Approx(8.88889f).epsilon(1e-4));
    CHECK(near_ninety->rotation_degrees == doctest::Approx(80.0f).epsilon(1e-4));
    CHECK(near_ninety->hidden == true);
    CHECK(near_ninety->resolve_x == false);
    CHECK(near_ninety->scaling_method == AnchorScalingMethod::Real3DPoint);
}

TEST_CASE("Oval runtime resolves anchors on the XZ plane and applies depth_offset vertically") {
    auto info = std::make_shared<AssetInfo>("oval_xz_runtime_asset");
    info->oval_anchor_mappings.clear();

    AssetInfo::OvalAnchorMapping mapping{};
    mapping.name = "weapon";
    mapping.asset_name = "vibble_weapon";
    mapping.width_radius_x = 40.0f;
    mapping.height_radius_z = 20.0f;
    mapping.points = {
        make_oval_point(0.0f, 0, 0, 1.0f, 0.0f, false, true, true, true, AnchorScalingMethod::Parent),
        make_oval_point(90.0f, 0, 0, 7.0f, 0.0f, false, true, true, true, AnchorScalingMethod::Parent),
    };
    REQUIRE(info->upsert_oval_anchor_mapping(mapping));

    Area spawn_area("oval_xz_runtime_area", 0);
    Asset asset(info, spawn_area, SDL_Point{100, 200}, 0);
    asset.move_to_world_position(100, 40, 200, 0);

    REQUIRE(asset.set_directional_heading_radians(degrees_to_radians(90.0f)));
    const auto resolved = asset.anchor_state("weapon");
    REQUIRE(resolved.has_value());
    CHECK(resolved->is_active());
    CHECK(resolved->world_exact_pos_2d.x == doctest::Approx(100.0f).epsilon(1e-4f));
    CHECK(resolved->world_exact_pos_2d.y == doctest::Approx(47.0f).epsilon(1e-4f));
    CHECK(resolved->world_exact_z == doctest::Approx(220.0f).epsilon(1e-4f));
    CHECK(resolved->flat_world_exact_pos_2d.x == doctest::Approx(100.0f).epsilon(1e-4f));
    CHECK(resolved->flat_world_exact_pos_2d.y == doctest::Approx(40.0f).epsilon(1e-4f));
    CHECK(resolved->flat_world_exact_z == doctest::Approx(220.0f).epsilon(1e-4f));
    CHECK(resolved->depth_offset == doctest::Approx(7.0f).epsilon(1e-4f));
}

TEST_CASE("AssetInfo migrates legacy oval depth payloads to XZ texture_y offsets") {
    AssetInfo info("oval_legacy_migration_asset");
    const nlohmann::json legacy_payload = nlohmann::json::array({
        nlohmann::json{
            {"name", "eyes"},
            {"asset_name", "vibble_eyes"},
            {"width_radius_x", 30.0},
            {"height_radius_z", 20.0},
            {"points", nlohmann::json::array({
                nlohmann::json{{"angle_degrees", 0.0}, {"texture_y", 0}, {"depth_offset", 0.0}},
                nlohmann::json{{"angle_degrees", 90.0}, {"texture_y", 0}, {"depth_offset", 20.0}},
                nlohmann::json{{"angle_degrees", 180.0}, {"texture_y", 0}, {"depth_offset", 0.0}},
                nlohmann::json{{"angle_degrees", 270.0}, {"texture_y", 0}, {"depth_offset", -20.0}},
            })}
        }
    });

    info.set_oval_anchor_mappings_payload(legacy_payload);
    const AssetInfo::OvalAnchorMapping* mapping = info.find_oval_anchor_mapping("eyes", false);
    REQUIRE(mapping != nullptr);
    REQUIRE(mapping->points.size() == 4);
    CHECK(mapping->points[0].texture_y == 0);
    CHECK(mapping->points[1].texture_y == 20);
    CHECK(mapping->points[2].texture_y == 0);
    CHECK(mapping->points[3].texture_y == -20);
    CHECK(mapping->points[0].depth_offset == doctest::Approx(0.0f));
    CHECK(mapping->points[1].depth_offset == doctest::Approx(0.0f));
    CHECK(mapping->points[2].depth_offset == doctest::Approx(0.0f));
    CHECK(mapping->points[3].depth_offset == doctest::Approx(0.0f));
}

TEST_CASE("Oval runtime uses per-frame center anchor and keeps offset non-zero") {
    auto info = std::make_shared<AssetInfo>("oval_center_runtime_asset");
    info->oval_anchor_mappings.clear();

    Animation animation{};
    auto& path = animation.movement_path(0);
    path.clear();

    AnimationFrame frame0{};
    frame0.frame_index = 0;
    frame0.anchor_points.push_back(DisplacedAssetAnchorPoint{
        "eyes_oval_center",
        0,
        0,
        5.0f,
    });
    frame0.rebuild_anchor_lookup();
    path.push_back(frame0);

    AnimationFrame frame1{};
    frame1.frame_index = 1;
    frame1.anchor_points.push_back(DisplacedAssetAnchorPoint{
        "eyes_oval_center",
        0,
        0,
        15.0f,
    });
    frame1.rebuild_anchor_lookup();
    path.push_back(frame1);

    info->animations.clear();
    info->animations["default"] = std::move(animation);
    info->start_animation = "default";

    AssetInfo::OvalAnchorMapping mapping{};
    mapping.name = "eyes";
    mapping.asset_name = "oval_center_runtime_asset";
    mapping.width_radius_x = 20.0f;
    mapping.height_radius_z = 10.0f;
    mapping.points = {
        make_oval_point(0.0f, 0, 0, 0.0f, 0.0f, false, true, true, true, AnchorScalingMethod::Parent),
        make_oval_point(180.0f, 0, 0, 0.0f, 0.0f, false, true, true, true, AnchorScalingMethod::Parent),
    };
    REQUIRE(info->upsert_oval_anchor_mapping(mapping));

    Area spawn_area("oval_center_runtime_area", 0);
    Asset asset(info, spawn_area, SDL_Point{0, 0}, 0);
    REQUIRE(asset.set_directional_heading_radians(0.0f));

    auto resolved_frame0 = asset.anchor_state("eyes",
                                              anchor_points::GridMaterialization::None,
                                              Asset::AnchorResolveMode::ForceRecompute);
    REQUIRE(resolved_frame0.has_value());
    CHECK(resolved_frame0->world_exact_pos_2d.x == doctest::Approx(20.0f).epsilon(1e-4f));
    const float frame0_z = resolved_frame0->world_exact_z;

    AnimationFrame* runtime_frame1 = info->animations["default"].primary_frame_at(1);
    REQUIRE(runtime_frame1 != nullptr);
    asset.current_frame = runtime_frame1;

    auto resolved_frame1 = asset.anchor_state("eyes",
                                              anchor_points::GridMaterialization::None,
                                              Asset::AnchorResolveMode::ForceRecompute);
    REQUIRE(resolved_frame1.has_value());
    CHECK(resolved_frame1->world_exact_pos_2d.x == doctest::Approx(20.0f).epsilon(1e-4f));
    CHECK(resolved_frame1->world_exact_z > frame0_z);
}

TEST_CASE("Oval runtime interpolation wraps across 360-degree seam") {
    auto info = std::make_shared<AssetInfo>("oval_wrap_asset");
    info->oval_anchor_mappings.clear();

    AssetInfo::OvalAnchorMapping mapping{};
    mapping.name = "weapon";
    mapping.asset_name = "oval_wrap_asset";
    mapping.width_radius_x = 40.0f;
    mapping.height_radius_z = 20.0f;
    mapping.points = {
        make_oval_point(315.0f, 0, 0, -10.0f, -45.0f, true, false, true, true, AnchorScalingMethod::Parent),
        make_oval_point(45.0f, 0, 0, 10.0f, 45.0f, false, true, false, false, AnchorScalingMethod::Real3DFloorPoint),
    };
    REQUIRE(info->upsert_oval_anchor_mapping(mapping));

    Area spawn_area("oval_wrap_area", 0);
    Asset asset(info, spawn_area, SDL_Point{25, 25}, 0);
    REQUIRE(asset.set_directional_heading_radians(0.0f));

    auto resolved = asset.anchor_state("weapon");
    REQUIRE(resolved.has_value());
    CHECK(resolved->depth_offset == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(resolved->rotation_degrees == doctest::Approx(0.0f).epsilon(1e-4));
    // At exact midpoint, discrete values snap to the previous point by design.
    CHECK(resolved->hidden == true);
    CHECK(resolved->resolve_x == false);
    CHECK(resolved->scaling_method == AnchorScalingMethod::Parent);
}

TEST_CASE("Oval runtime alias lookup resolves legacy names") {
    auto info = std::make_shared<AssetInfo>("oval_alias_asset");
    info->oval_anchor_mappings.clear();

    AssetInfo::OvalAnchorMapping mapping{};
    mapping.name = "eyes_front";
    mapping.asset_name = "oval_alias_asset";
    mapping.legacy_names = {"eyes"};
    mapping.points = {
        make_oval_point(0.0f, 0, 0, 2.0f, 10.0f, false, true, true, true, AnchorScalingMethod::Parent),
        make_oval_point(180.0f, 0, 0, 6.0f, 30.0f, false, true, true, true, AnchorScalingMethod::Parent),
    };
    REQUIRE(info->upsert_oval_anchor_mapping(mapping));

    Area spawn_area("oval_alias_area", 0);
    Asset asset(info, spawn_area, SDL_Point{10, 10}, 0);
    REQUIRE(asset.set_directional_heading_radians(0.0f));

    const auto canonical = asset.anchor_state("eyes_front");
    const auto alias = asset.anchor_state("eyes");
    REQUIRE(canonical.has_value());
    REQUIRE(alias.has_value());
    CHECK(canonical->is_active());
    CHECK(alias->is_active());
    CHECK(alias->depth_offset == doctest::Approx(canonical->depth_offset).epsilon(1e-4));
    CHECK(alias->rotation_degrees == doctest::Approx(canonical->rotation_degrees).epsilon(1e-4));
}

TEST_CASE("Runtime falls back to frame anchors when oval mapping is absent") {
    DisplacedAssetAnchorPoint frame_anchor{};
    frame_anchor.name = "eyes";
    frame_anchor.texture_x = 3;
    frame_anchor.texture_y = 4;
    frame_anchor.depth_offset = 7.5f;
    frame_anchor.rotation_degrees = 12.0f;
    frame_anchor.hidden = true;
    frame_anchor.resolve_x = false;
    frame_anchor.scaling_method = AnchorScalingMethod::Real3DPoint;

    auto info = make_info_with_single_frame_anchor("fallback_asset", frame_anchor);
    info->oval_anchor_mappings.clear();

    Area spawn_area("fallback_area", 0);
    Asset asset(info, spawn_area, SDL_Point{50, 75}, 0);

    const auto resolved = asset.anchor_state("eyes");
    REQUIRE(resolved.has_value());
    CHECK(resolved->is_active());
    CHECK(resolved->depth_offset == doctest::Approx(7.5f));
    CHECK(resolved->rotation_degrees == doctest::Approx(12.0f));
    CHECK(resolved->hidden == true);
    CHECK(resolved->resolve_x == false);
    CHECK(resolved->scaling_method == AnchorScalingMethod::Real3DPoint);
}

TEST_CASE("vibble_controller updates heading from mouse world vector") {
    auto info = std::make_shared<AssetInfo>("vibble");
    Area spawn_area("vibble_controller_area", 0);
    Asset player(info, spawn_area, SDL_Point{100, 100}, 0);
    player.move_to_world_position(100, 100, 0, 0);

    vibble_controller controller(&player);
    Input input{};
    input.set_screen_to_world_mapper([](SDL_Point screen) {
        return SDL_Point{screen.x, screen.y};
    });

    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = 130;
    motion.motion.y = 100;
    input.handleEvent(motion);
    input.update();
    controller.update(input);

    REQUIRE(player.has_directional_heading_radians());
    CHECK(player.directional_heading_radians() == doctest::Approx(0.0f).epsilon(1e-4));

    motion.motion.x = 100;
    motion.motion.y = 130;
    input.handleEvent(motion);
    input.update();
    controller.update(input);
    CHECK(player.directional_heading_radians() == doctest::Approx(degrees_to_radians(90.0f)).epsilon(1e-4));
}
