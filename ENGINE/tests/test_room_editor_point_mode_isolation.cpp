#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "assets/asset/animation.hpp"
#include "assets/asset/asset_info.hpp"
#include "devtools/room_anchor_mode_utils.hpp"
#include "devtools/room_box_payload_utils.hpp"
#include "devtools/room_editor.hpp"
#include "devtools/room_movement_payload.hpp"

namespace {

Animation make_single_frame_animation() {
    Animation animation{};
    auto& path = animation.movement_path(0);
    path.clear();
    AnimationFrame frame{};
    frame.rebuild_anchor_lookup();
    path.push_back(frame);
    return animation;
}

AssetInfo::OvalAnchorMapping make_basic_oval_mapping(const std::string& asset_name) {
    AssetInfo::OvalAnchorMapping mapping{};
    mapping.name = "oval_1";
    mapping.asset_name = asset_name;
    mapping.width_radius_x = 32.0f;
    mapping.height_radius_z = 16.0f;
    AssetInfo::OvalAnchorPoint point{};
    point.angle_degrees = 0.0f;
    point.texture_x = 8;
    point.texture_y = 6;
    point.depth_offset = 0.0f;
    point.rotation_degrees = 0.0f;
    point.hidden = false;
    point.resolve_x = true;
    point.scaling_method = AnchorScalingMethod::Parent;
    mapping.points.push_back(point);
    return mapping;
}

}  // namespace

TEST_CASE("Anchor and light mode mutations operate on shared point ownership") {
    std::vector<DisplacedAssetAnchorPoint> points{
        DisplacedAssetAnchorPoint{"shared", 1, 2, 0.0f},
        DisplacedAssetAnchorPoint{"shared", 3, 4, 0.0f},
        DisplacedAssetAnchorPoint{"anchor_only", 5, 6, 0.0f},
        DisplacedAssetAnchorPoint{"light_only", 7, 8, 0.0f},
    };
    points[1].has_light_data = true;
    points[3].has_light_data = true;

    const auto is_reserved = [](const std::string&) {
        return false;
    };

    REQUIRE(devmode::room_anchor_mode::rename_anchor_in_mode(
        points,
        "shared",
        "light_shared",
        devmode::room_anchor_mode::AnchorPointOwner::Light,
        is_reserved));

    CHECK(points[0].name == "light_shared");
    CHECK(points[1].name == "light_shared");

    REQUIRE(devmode::room_anchor_mode::delete_anchor_in_mode(
        points,
        "light_shared",
        devmode::room_anchor_mode::AnchorPointOwner::NonLight,
        is_reserved));

    CHECK(devmode::room_anchor_mode::find_anchor_in_mode(
              points,
              "light_shared",
              devmode::room_anchor_mode::AnchorPointOwner::NonLight,
              is_reserved) == nullptr);
    CHECK(devmode::room_anchor_mode::find_anchor_in_mode(
              points,
              "light_shared",
              devmode::room_anchor_mode::AnchorPointOwner::Light,
              is_reserved) == nullptr);
}

TEST_CASE("Hitbox and attack box payload writers keep unrelated editor keys untouched") {
    nlohmann::json payload = nlohmann::json::object({
        {"anchor_points", nlohmann::json::array({nlohmann::json::array()})},
        {"movement", nlohmann::json::array({nlohmann::json::array({0, 0, 0})})},
        {"movement_total", nlohmann::json::object({{"dx", 0}, {"dy", 0}, {"dz", 0.0}})},
        {"oval_anchor_mappings", nlohmann::json::array()},
        {"hit_boxes", nlohmann::json::array({nlohmann::json::array()})},
        {"attack_boxes", nlohmann::json::array({nlohmann::json::array()})},
    });

    const nlohmann::json anchor_points_before = payload["anchor_points"];
    const nlohmann::json movement_before = payload["movement"];
    const nlohmann::json movement_total_before = payload["movement_total"];
    const nlohmann::json oval_before = payload["oval_anchor_mappings"];

    const std::vector<std::string> names;
    const std::vector<animation_update::FrameHitBox> hit_boxes{
        devmode::room_box_payload::make_default_hit_box(names, 64, 64)};
    REQUIRE(devmode::room_box_payload::write_hit_box_frame_to_payload(payload, 1, 0, hit_boxes));

    CHECK(payload["anchor_points"] == anchor_points_before);
    CHECK(payload["movement"] == movement_before);
    CHECK(payload["movement_total"] == movement_total_before);
    CHECK(payload["oval_anchor_mappings"] == oval_before);

    const std::vector<animation_update::FrameAttackBox> attack_boxes{
        devmode::room_box_payload::make_default_attack_box(names, 64, 64)};
    REQUIRE(devmode::room_box_payload::write_attack_box_frame_to_payload(payload, 1, 0, attack_boxes));

    CHECK(payload["anchor_points"] == anchor_points_before);
    CHECK(payload["movement"] == movement_before);
    CHECK(payload["movement_total"] == movement_total_before);
    CHECK(payload["oval_anchor_mappings"] == oval_before);
}

TEST_CASE("Movement payload updates keep anchor and box editor keys unchanged") {
    nlohmann::json payload = nlohmann::json::object({
        {"anchor_points", nlohmann::json::array({nlohmann::json::array()})},
        {"hit_boxes", nlohmann::json::array({nlohmann::json::array()})},
        {"attack_boxes", nlohmann::json::array({nlohmann::json::array()})},
        {"oval_anchor_mappings", nlohmann::json::array()},
        {"movement", nlohmann::json::array({nlohmann::json::array({0, 0, 0})})},
        {"movement_total", nlohmann::json::object({{"dx", 0}, {"dy", 0}, {"dz", 0.0}})},
    });

    const nlohmann::json anchor_before = payload["anchor_points"];
    const nlohmann::json hit_before = payload["hit_boxes"];
    const nlohmann::json attack_before = payload["attack_boxes"];
    const nlohmann::json oval_before = payload["oval_anchor_mappings"];

    std::vector<devmode::room_movement_payload::MovementFrame> frames(2);
    frames[1].dx = 3.0f;
    frames[1].dy = 4.0f;
    frames[1].dz = 5.0f;

    const nlohmann::json updated = devmode::room_movement_payload::build_payload_from_frames(frames, payload);

    CHECK(updated["anchor_points"] == anchor_before);
    CHECK(updated["hit_boxes"] == hit_before);
    CHECK(updated["attack_boxes"] == attack_before);
    CHECK(updated["oval_anchor_mappings"] == oval_before);
    CHECK(updated["movement"].is_array());
    CHECK(updated["movement_total"].is_object());
}

TEST_CASE("Oval mapping updates do not rewrite animation payload keys from other editors") {
    AssetInfo info("isolation_asset");
    info.animations.clear();
    info.animations["default"] = make_single_frame_animation();
    info.start_animation = "default";

    nlohmann::json animation_payload = nlohmann::json::object({
        {"source", nlohmann::json::object({{"kind", "folder"}, {"path", "default"}})},
        {"anchor_points", nlohmann::json::array({nlohmann::json::array()})},
        {"hit_boxes", nlohmann::json::array({nlohmann::json::array()})},
        {"attack_boxes", nlohmann::json::array({nlohmann::json::array()})},
        {"movement", nlohmann::json::array({nlohmann::json::array({0, 0, 0})})},
        {"movement_total", nlohmann::json::object({{"dx", 0}, {"dy", 0}, {"dz", 0.0}})},
    });
    REQUIRE(info.update_animation_properties("default", animation_payload));
    const nlohmann::json before_animation_payload = info.animation_payload("default");

    REQUIRE(info.upsert_oval_anchor_mapping(make_basic_oval_mapping(info.name)));

    CHECK(info.animation_payload("default") == before_animation_payload);
}

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)
TEST_CASE("RoomEditor queues re-entrant subview requests while transition is in progress") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_subview_change_in_progress(editor, true);

    RoomEditorTestAccess::request_subview(editor, RoomEditorTestAccess::subview_anchor(), true);

    CHECK(RoomEditorTestAccess::has_pending_subview_request(editor));
    CHECK(RoomEditorTestAccess::pending_subview(editor) == RoomEditorTestAccess::subview_anchor());
    CHECK(RoomEditorTestAccess::pending_subview_animate(editor));
}

TEST_CASE("RoomEditor drains queued subview request after transition completes") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_subview_change_in_progress(editor, true);
    RoomEditorTestAccess::request_subview(editor, RoomEditorTestAccess::subview_asset_info(), false);
    REQUIRE(RoomEditorTestAccess::has_pending_subview_request(editor));

    RoomEditorTestAccess::set_subview_change_in_progress(editor, false);
    RoomEditorTestAccess::drain_pending_subview_request(editor);

    CHECK_FALSE(RoomEditorTestAccess::has_pending_subview_request(editor));
    CHECK(RoomEditorTestAccess::active_subview(editor) == RoomEditorTestAccess::subview_asset_info());
}

TEST_CASE("RoomEditor ignores animation-editor closed callback during subview transition") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_active_subview(editor, RoomEditorTestAccess::subview_animation_editor());
    RoomEditorTestAccess::set_subview_change_in_progress(editor, true);

    RoomEditorTestAccess::invoke_on_animation_editor_closed(editor);

    CHECK(RoomEditorTestAccess::active_subview(editor) == RoomEditorTestAccess::subview_animation_editor());
    CHECK_FALSE(RoomEditorTestAccess::has_pending_subview_request(editor));
}

TEST_CASE("RoomEditor defers animation-editor closed fallback transition to update pass") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_active_subview(editor, RoomEditorTestAccess::subview_animation_editor());

    RoomEditorTestAccess::invoke_on_animation_editor_closed(editor);

    CHECK(RoomEditorTestAccess::active_subview(editor) == RoomEditorTestAccess::subview_animation_editor());
    CHECK(RoomEditorTestAccess::has_pending_animation_editor_close_subview(editor));
    CHECK(RoomEditorTestAccess::pending_animation_editor_close_subview(editor) ==
          RoomEditorTestAccess::subview_asset_info());

    RoomEditorTestAccess::process_pending_animation_editor_close(editor);

    CHECK_FALSE(RoomEditorTestAccess::has_pending_animation_editor_close_subview(editor));
    CHECK(RoomEditorTestAccess::active_subview(editor) == RoomEditorTestAccess::subview_animation_editor());
}

TEST_CASE("RoomEditor opens spawn-group panel only on double left-click for same asset") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::reset_click_tracking(editor);

    int asset_a = 1;
    int asset_b = 2;

    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, &asset_a, true, 1000));
    CHECK(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, &asset_a, true, 1200));

    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, &asset_a, true, 1605));
    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, &asset_b, true, 1700));
}

TEST_CASE("RoomEditor click tracking ignores non-spawn assets and resets on null identity") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::reset_click_tracking(editor);

    int asset_a = 1;

    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, &asset_a, false, 1000));
    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, &asset_a, true, 1100));
    CHECK(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, &asset_a, true, 1250));

    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, nullptr, true, 1300));
    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, &asset_a, true, 1350));
}

TEST_CASE("RoomEditor selection sync with snap enabled does not trigger spawn-group snapping") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::reset_snap_spawn_group_to_resolution_call_count(editor);
    RoomEditorTestAccess::set_shared_footer_present(editor, true);
    RoomEditorTestAccess::set_snap_to_grid_enabled(editor, true);

    RoomEditorTestAccess::update_grid_resolution_for_selection(
        editor,
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x1234)));

    CHECK(RoomEditorTestAccess::snap_spawn_group_to_resolution_call_count(editor) == 0);
}

TEST_CASE("RoomEditor suppresses release action when pressed selection becomes invalid") {
    RoomEditor editor(nullptr, 1280, 720);
    int asset_a = 1;
    std::string spawn_id;

    RoomEditorTestAccess::set_active_asset_identities(editor, {&asset_a});
    RoomEditorTestAccess::set_mouse_press_state(editor, &asset_a, "spawn_a", true, false);

    RoomEditorTestAccess::set_active_asset_identities(editor, {});
    CHECK_FALSE(RoomEditorTestAccess::consume_pressed_asset_release(editor, 1000, spawn_id));
    CHECK(spawn_id.empty());
}

TEST_CASE("RoomEditor release behavior keeps double-click spawn open semantics for valid pressed asset") {
    RoomEditor editor(nullptr, 1280, 720);
    int asset_a = 1;
    std::string spawn_id;

    RoomEditorTestAccess::reset_click_tracking(editor);
    RoomEditorTestAccess::set_active_asset_identities(editor, {&asset_a});
    RoomEditorTestAccess::set_mouse_press_state(editor, &asset_a, "spawn_a", true, false);

    CHECK_FALSE(RoomEditorTestAccess::consume_pressed_asset_release(editor, 1000, spawn_id));
    CHECK(spawn_id.empty());

    CHECK(RoomEditorTestAccess::consume_pressed_asset_release(editor, 1200, spawn_id));
    CHECK(spawn_id == "spawn_a");
}

TEST_CASE("RoomEditor drag mode mapping normalizes spawn methods consistently") {
    const int exact_mode = RoomEditorTestAccess::drag_mode_for_spawn_method("Exact", false);
    const int free_mode = RoomEditorTestAccess::drag_mode_for_spawn_method("Random", false);

    CHECK(RoomEditorTestAccess::drag_mode_for_spawn_method("Exact Position", false) == exact_mode);
    CHECK(RoomEditorTestAccess::drag_mode_for_spawn_method("Percent", false) != free_mode);
    CHECK(RoomEditorTestAccess::drag_mode_for_spawn_method("Perimeter", false) !=
          RoomEditorTestAccess::drag_mode_for_spawn_method("Perimeter", true));
    CHECK(RoomEditorTestAccess::drag_mode_for_spawn_method("Edge", false) != free_mode);
    CHECK(RoomEditorTestAccess::drag_mode_for_spawn_method("Unknown Method", false) == free_mode);
}

TEST_CASE("RoomEditor spawn-group edits queue deferred work while callback scope is active") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::reset_respawn_spawn_group_call_count(editor);
    RoomEditorTestAccess::set_spawn_group_callback_in_progress(editor, true);
    REQUIRE(RoomEditorTestAccess::spawn_group_callback_in_progress(editor));

    RoomEditorTestAccess::enqueue_spawn_group_work(editor, "spawn_a", true, true, true, true);

    CHECK(RoomEditorTestAccess::pending_spawn_group_work_size(editor) == 1);
    CHECK(RoomEditorTestAccess::respawn_spawn_group_call_count(editor) == 0);
}

TEST_CASE("RoomEditor processes and clears deferred spawn-group queue work") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::enqueue_spawn_group_work(editor, "spawn_a", false, true, false, false);
    RoomEditorTestAccess::enqueue_spawn_group_work(editor, "spawn_b", false, true, false, false);
    REQUIRE(RoomEditorTestAccess::pending_spawn_group_work_size(editor) == 2);

    RoomEditorTestAccess::set_spawn_group_callback_in_progress(editor, false);
    RoomEditorTestAccess::process_pending_spawn_group_work(editor);

    CHECK(RoomEditorTestAccess::pending_spawn_group_work_size(editor) == 0);
}

TEST_CASE("RoomEditor extrusion drag resolution is side-locked and clamps to min depth one") {
    const int base_forward = 8;
    const int base_backward = 6;
    const float start_half_separation = 10.0f;

    CHECK(RoomEditorTestAccess::resolve_extrusion_drag_value(
              true, -5, start_half_separation, base_forward, base_backward) == 1);
    CHECK(RoomEditorTestAccess::resolve_extrusion_drag_value(
              true, 20, start_half_separation, base_forward, base_backward) == 16);

    CHECK(RoomEditorTestAccess::resolve_extrusion_drag_value(
              false, 5, start_half_separation, base_forward, base_backward) == 1);
    CHECK(RoomEditorTestAccess::resolve_extrusion_drag_value(
              false, -20, start_half_separation, base_forward, base_backward) == 12);
}

TEST_CASE("RoomEditor blocks camera interaction while hitbox extrusion handle is dragging") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_editor_mode(editor, RoomEditorTestAccess::mode_hitbox());
    RoomEditorTestAccess::set_hitbox_dragging_extrusion(editor, true);

    CHECK(RoomEditorTestAccess::editor_interaction_is_dragging(editor));
    CHECK(RoomEditorTestAccess::editor_interaction_camera_blocked(editor));
}

TEST_CASE("RoomEditor blocks camera interaction while attack-box extrusion handle is dragging") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_editor_mode(editor, RoomEditorTestAccess::mode_attack_box());
    RoomEditorTestAccess::set_attack_box_dragging_extrusion(editor, true);

    CHECK(RoomEditorTestAccess::editor_interaction_is_dragging(editor));
    CHECK(RoomEditorTestAccess::editor_interaction_camera_blocked(editor));
}
#endif
