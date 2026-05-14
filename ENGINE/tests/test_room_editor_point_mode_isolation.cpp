#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "assets/asset/animation.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "core/AssetsManager.hpp"
#include "core/runtime_world_context.hpp"
#include "devtools/room_anchor_mode_utils.hpp"
#include "devtools/room_anchor_tools_panel.hpp"
#include "devtools/room_box_payload_utils.hpp"
#include "devtools/room_box_tools_panel.hpp"
#include "devtools/dev_controls.hpp"
#include "devtools/room_editor.hpp"
#include "devtools/room_floor_box_tools_panel.hpp"
#include "devtools/room_movement_payload.hpp"
#include "gameplay/map_generation/room.hpp"
#include "gameplay/world/world_grid.hpp"
#include "room_editor_payload_contract_test_helper.hpp"
#include "utils/area.hpp"
#include "utils/map_grid_settings.hpp"

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

std::unique_ptr<Asset> make_boundary_proxy_test_asset(const std::string& asset_name,
                                                      const std::string& spawn_id,
                                                      int world_x,
                                                      int world_z) {
    auto info = std::make_shared<AssetInfo>(asset_name);
    info->type = "object";
    info->original_canvas_width = 64;
    info->original_canvas_height = 64;
    Area spawn_area("boundary_proxy_test_area", 0);
    return std::make_unique<Asset>(info,
                                   spawn_area,
                                   SDL_Point{world_x, world_z},
                                   0,
                                   spawn_id,
                                   std::string{},
                                   0);
}

std::unique_ptr<Room> make_nav_test_room(const std::string& name) {
    std::vector<SDL_Point> points{
        SDL_Point{0, 0},
        SDL_Point{100, 0},
        SDL_Point{100, 100},
        SDL_Point{0, 100},
    };
    Area area(name, points, 0);
    nlohmann::json data = nlohmann::json::object({
        {"name", name},
        {"geometry", "Square"},
        {"width", 100},
        {"height", 100},
        {"spawn_groups", nlohmann::json::array()},
    });
    auto room = std::make_unique<Room>(
        Room::Point{0, 0},
        "room",
        name,
        nullptr,
        "test_map",
        nullptr,
        &area,
        nullptr,
        MapGridSettings::defaults(),
        1000.0,
        "rooms_data",
        nullptr,
        nullptr,
        std::string{},
        Room::ManifestWriter{},
        false);
    room->assets_data() = std::move(data);
    return room;
}

struct BoundaryProxyFixture {
    explicit BoundaryProxyFixture(const std::string& spawn_id)
        : library(false),
          world_context(std::make_shared<RuntimeWorldContext>()) {
        nlohmann::json map_info = nlohmann::json::object({
            {"map_boundary_data",
             nlohmann::json::object({
                 {"spawn_groups",
                  nlohmann::json::array({
                      nlohmann::json::object({
                          {"spawn_id", spawn_id},
                          {"display_name", "Boundary Spawn"}
                      })
                  })}
             })}
        });
        boundary_asset = grid.create_asset_at_point(make_boundary_proxy_test_asset("boundary_proxy_asset",
                                                                                    spawn_id,
                                                                                    0,
                                                                                    80));
        REQUIRE(boundary_asset != nullptr);
        boundary_asset->finalize_setup();
        assets = std::make_unique<Assets>(library,
                                          nullptr,
                                          world_context,
                                          1280,
                                          720,
                                          0,
                                          0,
                                          0,
                                          nullptr,
                                          "boundary_proxy_test",
                                          map_info,
                                          std::string{},
                                          std::move(grid));
    }

    AssetLibrary library;
    std::shared_ptr<RuntimeWorldContext> world_context;
    world::WorldGrid grid;
    Asset* boundary_asset = nullptr;
    std::unique_ptr<Assets> assets;
};
void check_unchanged_key_bytes(const nlohmann::json& before,
                               const nlohmann::json& after,
                               const std::vector<std::string>& changed_keys) {
    const auto snapshot_before = room_editor_test_payload_contract::snapshot_key_bytes(before);
    const auto snapshot_after = room_editor_test_payload_contract::snapshot_key_bytes(after);
    const auto keys = room_editor_test_payload_contract::unchanged_keys_excluding(before, changed_keys);
    for (const auto& key : keys) {
        INFO("key=" << key);
        auto before_it = snapshot_before.find(key);
        auto after_it = snapshot_after.find(key);
        REQUIRE(before_it != snapshot_before.end());
        REQUIRE(after_it != snapshot_after.end());
        CHECK(before_it->second == after_it->second);
    }
}

template <typename TPanel>
void dispatch_left_click(TPanel& panel, int x, int y) {
    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = x;
    down.button.y = y;
    panel.handle_event(down);

    SDL_Event up{};
    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
    up.button.button = SDL_BUTTON_LEFT;
    up.button.x = x;
    up.button.y = y;
    panel.handle_event(up);
}

}  // namespace

TEST_CASE("Anchor and light mode mutations are isolated by ownership") {
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

    CHECK(points[0].name == "shared");
    CHECK(points[1].name == "light_shared");

    REQUIRE(devmode::room_anchor_mode::delete_anchor_in_mode(
        points,
        "shared",
        devmode::room_anchor_mode::AnchorPointOwner::NonLight,
        is_reserved));

    CHECK(devmode::room_anchor_mode::find_anchor_in_mode(
              points,
              "shared",
              devmode::room_anchor_mode::AnchorPointOwner::NonLight,
              is_reserved) == nullptr);
    CHECK(devmode::room_anchor_mode::find_anchor_in_mode(
              points,
              "light_shared",
              devmode::room_anchor_mode::AnchorPointOwner::Light,
              is_reserved) != nullptr);
}

TEST_CASE("Hitbox and attack box payload writers keep unrelated editor keys untouched") {
    nlohmann::json payload = room_editor_test_payload_contract::make_full_mixed_animation_payload();
    const nlohmann::json payload_before = payload;

    const std::vector<std::string> names;
    const std::vector<animation_update::FrameHitBox> hit_boxes{
        devmode::room_box_payload::make_default_hit_box(names, 64, 64)};
    REQUIRE(devmode::room_box_payload::write_hit_box_frame_to_payload(payload, 1, 0, hit_boxes));

    check_unchanged_key_bytes(payload_before, payload, {"hit_boxes", "box_schema_version"});

    const std::vector<animation_update::FrameAttackBox> attack_boxes{
        devmode::room_box_payload::make_default_attack_box(names, 64, 64)};
    const nlohmann::json before_attack_write = payload;
    REQUIRE(devmode::room_box_payload::write_attack_box_frame_to_payload(payload, 1, 0, attack_boxes));

    check_unchanged_key_bytes(before_attack_write, payload, {"attack_boxes", "box_schema_version"});
}

TEST_CASE("Movement payload updates keep anchor and box editor keys unchanged") {
    nlohmann::json payload = room_editor_test_payload_contract::make_full_mixed_animation_payload();
    const nlohmann::json before = payload;

    std::vector<devmode::room_movement_payload::MovementFrame> frames(2);
    frames[1].dx = 3.0f;
    frames[1].dy = 4.0f;
    frames[1].dz = 5.0f;

    const nlohmann::json updated = devmode::room_movement_payload::build_payload_from_frames(frames, payload);

    check_unchanged_key_bytes(before, updated, {"movement", "movement_total"});
    CHECK(updated["movement"].is_array());
    CHECK(updated["movement_total"].is_object());
}

TEST_CASE("Anchor payload writer keeps unrelated mixed payload keys byte-equal") {
    nlohmann::json payload = room_editor_test_payload_contract::make_full_mixed_animation_payload();
    const nlohmann::json before = payload;

    const std::vector<DisplacedAssetAnchorPoint> replacement{
        DisplacedAssetAnchorPoint{"edited", 10, 11, 12.5f}
    };
    REQUIRE(devmode::room_anchor_mode::write_anchor_frame_to_payload(payload, 1, 0, replacement));

    check_unchanged_key_bytes(before, payload, {"anchor_points"});
}

TEST_CASE("Writer precondition failures do not partially mutate mixed payload") {
    const std::vector<std::string> names;
    const std::vector<animation_update::FrameHitBox> hit_boxes{
        devmode::room_box_payload::make_default_hit_box(names, 64, 64)};
    const std::vector<animation_update::FrameAttackBox> attack_boxes{
        devmode::room_box_payload::make_default_attack_box(names, 64, 64)};
    const std::vector<DisplacedAssetAnchorPoint> anchors{
        DisplacedAssetAnchorPoint{"edited", 2, 3, 4.0f}
    };

    SUBCASE("anchor writer rejects invalid frame bounds atomically") {
        nlohmann::json payload = room_editor_test_payload_contract::make_full_mixed_animation_payload();
        const std::string before = payload.dump();
        CHECK_FALSE(devmode::room_anchor_mode::write_anchor_frame_to_payload(payload, 0, 0, anchors));
        CHECK(payload.dump() == before);
        CHECK_FALSE(devmode::room_anchor_mode::write_anchor_frame_to_payload(payload, 1, 4, anchors));
        CHECK(payload.dump() == before);
    }

    SUBCASE("hit writer rejects invalid frame bounds atomically") {
        nlohmann::json payload = room_editor_test_payload_contract::make_full_mixed_animation_payload();
        const std::string before = payload.dump();
        CHECK_FALSE(devmode::room_box_payload::write_hit_box_frame_to_payload(payload, 0, 0, hit_boxes));
        CHECK(payload.dump() == before);
        CHECK_FALSE(devmode::room_box_payload::write_hit_box_frame_to_payload(payload, 1, 3, hit_boxes));
        CHECK(payload.dump() == before);
    }

    SUBCASE("attack writer rejects invalid frame bounds atomically") {
        nlohmann::json payload = room_editor_test_payload_contract::make_full_mixed_animation_payload();
        const std::string before = payload.dump();
        CHECK_FALSE(devmode::room_box_payload::write_attack_box_frame_to_payload(payload, 0, 0, attack_boxes));
        CHECK(payload.dump() == before);
        CHECK_FALSE(devmode::room_box_payload::write_attack_box_frame_to_payload(payload, 1, 3, attack_boxes));
        CHECK(payload.dump() == before);
    }
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
TEST_CASE("RoomEditor candidate source validation blocks cross-domain contexts") {
    RoomEditor editor(nullptr, 1280, 720);

    RoomEditorTestAccess::set_editor_mode(editor, RoomEditorTestAccess::mode_anchor());
    CHECK(RoomEditorTestAccess::validate_anchor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_anchor_non_light()));
    CHECK_FALSE(RoomEditorTestAccess::validate_anchor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_anchor_light()));
    CHECK_FALSE(RoomEditorTestAccess::validate_anchor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_oval_center()));
    CHECK_FALSE(RoomEditorTestAccess::validate_anchor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_floor_box()));

    RoomEditorTestAccess::set_editor_mode(editor, RoomEditorTestAccess::mode_light());
    CHECK(RoomEditorTestAccess::validate_anchor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_anchor_light()));
    CHECK_FALSE(RoomEditorTestAccess::validate_anchor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_anchor_non_light()));

    RoomEditorTestAccess::set_editor_mode(editor, RoomEditorTestAccess::mode_floor_box());
    CHECK(RoomEditorTestAccess::validate_floor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_floor_box()));
    CHECK_FALSE(RoomEditorTestAccess::validate_floor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_anchor_light()));
}

TEST_CASE("RoomEditor room nav selection updates current room and visible room config") {
    auto spawn_room = make_nav_test_room("spawn_room");
    auto clicked_room = make_nav_test_room("clicked_room");

    RoomEditor editor(nullptr, 1280, 720);
    editor.set_current_room(spawn_room.get());
    editor.open_room_config();
    REQUIRE(RoomEditorTestAccess::current_room(editor) == spawn_room.get());
    CHECK(RoomEditorTestAccess::room_config_header_text(editor) == "Room: spawn_room");

    CHECK(RoomEditorTestAccess::select_current_room_from_nav(editor, clicked_room.get()));

    CHECK(RoomEditorTestAccess::current_room(editor) == clicked_room.get());
    CHECK(RoomEditorTestAccess::room_config_header_text(editor) == "Room: clicked_room");
}

TEST_CASE("DevControls preserves dev-selected room when runtime sync reports player room") {
    auto player_room = make_nav_test_room("baseball");
    auto clicked_room = make_nav_test_room("clicked_room");

    DevControls controls(nullptr, 1280, 720);
    controls.set_current_room(player_room.get(), true);
    controls.set_enabled(true);

    controls.set_current_room(clicked_room.get(), true);
    controls.set_current_room(player_room.get(), false);

    CHECK(controls.resolve_current_room(player_room.get()) == clicked_room.get());
}

TEST_CASE("RoomEditor oval candidate source requires explicit center or point selection") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_editor_mode(editor, RoomEditorTestAccess::mode_oval());

    RoomEditorTestAccess::set_oval_candidate_selection(editor, true, -1);
    CHECK(RoomEditorTestAccess::validate_anchor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_oval_center()));
    CHECK_FALSE(RoomEditorTestAccess::validate_anchor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_oval_point()));

    RoomEditorTestAccess::set_oval_candidate_selection(editor, false, 0);
    CHECK(RoomEditorTestAccess::validate_anchor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_oval_point()));
    CHECK_FALSE(RoomEditorTestAccess::validate_anchor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_oval_center()));

    RoomEditorTestAccess::set_oval_candidate_selection(editor, false, -1);
    CHECK_FALSE(RoomEditorTestAccess::validate_anchor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_oval_point()));
    CHECK_FALSE(RoomEditorTestAccess::validate_anchor_candidate_source(
        editor, RoomEditorTestAccess::candidate_source_oval_center()));
}

TEST_CASE("RoomEditor delete confirmation cancel path produces zero mutation") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_delete_confirm_callback_for_tests(editor, 0);

    int apply_calls = 0;
    const bool result = RoomEditorTestAccess::execute_delete_confirmation_flow(
        editor,
        RoomEditorTestAccess::mode_floor_box(),
        true,
        true,
        1,
        apply_calls);
    CHECK_FALSE(result);
    CHECK(apply_calls == 0);
}

TEST_CASE("RoomEditor delete confirmation confirm path mutates once when scope is valid") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_delete_confirm_callback_for_tests(editor, 1);

    int apply_calls = 0;
    const bool result = RoomEditorTestAccess::execute_delete_confirmation_flow(
        editor,
        RoomEditorTestAccess::mode_anchor(),
        true,
        true,
        1,
        apply_calls);
    CHECK(result);
    CHECK(apply_calls == 1);
}

TEST_CASE("RoomEditor delete confirmation confirm path succeeds in light mode") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_delete_confirm_callback_for_tests(editor, 1);

    int apply_calls = 0;
    const bool result = RoomEditorTestAccess::execute_delete_confirmation_flow(
        editor,
        RoomEditorTestAccess::mode_light(),
        true,
        true,
        1,
        apply_calls);
    CHECK(result);
    CHECK(apply_calls == 1);
}

TEST_CASE("RoomEditor delete confirmation confirm path succeeds for hitbox and attack modes") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_delete_confirm_callback_for_tests(editor, 1);

    int apply_calls = 0;
    const bool hitbox_result = RoomEditorTestAccess::execute_delete_confirmation_flow(
        editor,
        RoomEditorTestAccess::mode_hitbox(),
        true,
        true,
        1,
        apply_calls);
    CHECK(hitbox_result);
    CHECK(apply_calls == 1);

    apply_calls = 0;
    const bool attack_result = RoomEditorTestAccess::execute_delete_confirmation_flow(
        editor,
        RoomEditorTestAccess::mode_attack_box(),
        true,
        true,
        1,
        apply_calls);
    CHECK(attack_result);
    CHECK(apply_calls == 1);
}

TEST_CASE("RoomEditor delete confirmation blocks zero affected scope") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_delete_confirm_callback_for_tests(editor, 1);

    int apply_calls = 0;
    const bool result = RoomEditorTestAccess::execute_delete_confirmation_flow(
        editor,
        RoomEditorTestAccess::mode_anchor(),
        true,
        true,
        0,
        apply_calls);
    CHECK_FALSE(result);
    CHECK(apply_calls == 0);
}

TEST_CASE("RoomEditor delete confirmation blocks stale selection at confirmation time") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_delete_confirm_callback_for_tests(editor, 1);

    int apply_calls = 0;
    const bool result = RoomEditorTestAccess::execute_delete_confirmation_flow(
        editor,
        RoomEditorTestAccess::mode_hitbox(),
        true,
        false,
        1,
        apply_calls);
    CHECK_FALSE(result);
    CHECK(apply_calls == 0);
}

TEST_CASE("RoomEditor delete confirmation remains valid when transient UI selection drifts") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_delete_confirm_callback_for_tests(editor, 1);

    int apply_calls = 0;
    const bool result = RoomEditorTestAccess::execute_delete_confirmation_with_transient_ui_drift(
        editor,
        RoomEditorTestAccess::mode_hitbox(),
        true,
        1,
        apply_calls);
    CHECK(result);
    CHECK(apply_calls == 1);
}

TEST_CASE("RoomEditor delete confirmation blocks when snapshot target no longer exists") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_delete_confirm_callback_for_tests(editor, 1);

    int apply_calls = 0;
    const bool result = RoomEditorTestAccess::execute_delete_confirmation_with_transient_ui_drift(
        editor,
        RoomEditorTestAccess::mode_hitbox(),
        false,
        1,
        apply_calls);
    CHECK_FALSE(result);
    CHECK(apply_calls == 0);
}

TEST_CASE("RoomEditor delete confirmation dont-ask-again is session scoped per mode") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_delete_confirm_callback_for_tests(editor, 2);

    int apply_calls = 0;
    const bool first_result = RoomEditorTestAccess::execute_delete_confirmation_flow(
        editor,
        RoomEditorTestAccess::mode_attack_box(),
        true,
        true,
        1,
        apply_calls);
    CHECK(first_result);
    CHECK(apply_calls == 1);
    CHECK(RoomEditorTestAccess::delete_confirmation_disabled_for_mode(
        editor, RoomEditorTestAccess::mode_attack_box()));

    RoomEditorTestAccess::set_delete_confirm_callback_for_tests(editor, 0);
    apply_calls = 0;
    const bool second_result = RoomEditorTestAccess::execute_delete_confirmation_flow(
        editor,
        RoomEditorTestAccess::mode_attack_box(),
        true,
        true,
        1,
        apply_calls);
    CHECK(second_result);
    CHECK(apply_calls == 1);
}

TEST_CASE("RoomEditor delete persistence remains configured for immediate flush") {
    CHECK(RoomEditorTestAccess::delete_persist_priority_for_tests() ==
          static_cast<int>(devmode::core::DevSaveCoordinator::Priority::Immediate));
    CHECK(RoomEditorTestAccess::delete_persist_flush_now_for_tests());
}

TEST_CASE("RoomEditor delete shortcut routes to stack-mode delete path in stack editor mode") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::reset_delete_shortcut_route_counters(editor);
    RoomEditorTestAccess::set_editor_mode(editor, RoomEditorTestAccess::mode_hitbox());

    RoomEditorTestAccess::invoke_delete_shortcut(editor, true, false);

    CHECK(RoomEditorTestAccess::delete_shortcut_stack_dispatch_count(editor) == 1);
    CHECK(RoomEditorTestAccess::delete_shortcut_asset_delete_count(editor) == 0);
}

TEST_CASE("RoomEditor delete shortcut routes to asset delete path in normal mode") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::reset_delete_shortcut_route_counters(editor);
    RoomEditorTestAccess::set_editor_mode(editor, RoomEditorTestAccess::mode_normal());

    RoomEditorTestAccess::invoke_delete_shortcut(editor, true, false);

    CHECK(RoomEditorTestAccess::delete_shortcut_stack_dispatch_count(editor) == 0);
    CHECK(RoomEditorTestAccess::delete_shortcut_asset_delete_count(editor) == 1);
}

TEST_CASE("RoomBoxToolsPanel delete button is selection-gated and stale click safe") {
    RoomBoxToolsPanel panel(RoomBoxToolsPanel::Kind::HitBox);
    panel.set_visible(true);
    panel.set_screen_dimensions(1280, 720);
    panel.set_system_enabled(true);
    panel.set_box_names({"Box A", "Box B"});

    CHECK_FALSE(RoomBoxToolsPanelTestAccess::delete_button_visible(panel));

    panel.set_selection(0, 0);
    REQUIRE(RoomBoxToolsPanelTestAccess::delete_button_visible(panel));
    const SDL_Rect stale_delete_rect = RoomBoxToolsPanelTestAccess::delete_button_rect(panel);

    int delete_calls = 0;
    panel.set_on_delete([&delete_calls]() { ++delete_calls; });
    dispatch_left_click(panel,
                        stale_delete_rect.x + stale_delete_rect.w / 2,
                        stale_delete_rect.y + stale_delete_rect.h / 2);
    CHECK(delete_calls == 1);

    panel.clear_selection();
    CHECK_FALSE(RoomBoxToolsPanelTestAccess::delete_button_visible(panel));
    dispatch_left_click(panel,
                        stale_delete_rect.x + stale_delete_rect.w / 2,
                        stale_delete_rect.y + stale_delete_rect.h / 2);
    CHECK(delete_calls == 1);
}

TEST_CASE("RoomFloorBoxToolsPanel delete button is selection-gated and stale click safe") {
    RoomFloorBoxToolsPanel panel;
    panel.set_visible(true);
    panel.set_screen_dimensions(1280, 720);
    panel.set_system_enabled(true);
    panel.set_floor_box_names({"Floor A"});

    CHECK_FALSE(RoomFloorBoxToolsPanelTestAccess::delete_button_visible(panel));

    panel.set_selection(0);
    REQUIRE(RoomFloorBoxToolsPanelTestAccess::delete_button_visible(panel));
    const SDL_Rect stale_delete_rect = RoomFloorBoxToolsPanelTestAccess::delete_button_rect(panel);

    int delete_calls = 0;
    panel.set_on_delete([&delete_calls]() { ++delete_calls; });
    dispatch_left_click(panel,
                        stale_delete_rect.x + stale_delete_rect.w / 2,
                        stale_delete_rect.y + stale_delete_rect.h / 2);
    CHECK(delete_calls == 1);

    panel.clear_selection();
    CHECK_FALSE(RoomFloorBoxToolsPanelTestAccess::delete_button_visible(panel));
    dispatch_left_click(panel,
                        stale_delete_rect.x + stale_delete_rect.w / 2,
                        stale_delete_rect.y + stale_delete_rect.h / 2);
    CHECK(delete_calls == 1);
}

TEST_CASE("RoomAnchorToolsPanel delete button is selection-gated and stale click safe") {
    RoomAnchorToolsPanel panel;
    panel.set_visible(true);
    panel.set_screen_dimensions(1280, 720);
    panel.set_anchor_names({"Anchor A"});

    CHECK_FALSE(RoomAnchorToolsPanelTestAccess::delete_button_visible(panel));

    panel.set_selected_anchor("Anchor A");
    REQUIRE(RoomAnchorToolsPanelTestAccess::delete_button_visible(panel));
    const SDL_Rect stale_delete_rect = RoomAnchorToolsPanelTestAccess::delete_button_rect(panel);

    int delete_calls = 0;
    panel.set_on_delete([&delete_calls]() { ++delete_calls; });
    dispatch_left_click(panel,
                        stale_delete_rect.x + stale_delete_rect.w / 2,
                        stale_delete_rect.y + stale_delete_rect.h / 2);
    CHECK(delete_calls == 1);

    panel.set_selected_anchor("");
    CHECK_FALSE(RoomAnchorToolsPanelTestAccess::delete_button_visible(panel));
    dispatch_left_click(panel,
                        stale_delete_rect.x + stale_delete_rect.w / 2,
                        stale_delete_rect.y + stale_delete_rect.h / 2);
    CHECK(delete_calls == 1);
}

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

TEST_CASE("RoomEditor opens spawn-group panel only on double left-click for same spawn group") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::reset_click_tracking(editor);

    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, "spawn_a", true, 1000));
    CHECK(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, "spawn_a", true, 1200));

    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, "spawn_a", true, 1605));
    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, "spawn_b", true, 1700));
}

TEST_CASE("RoomEditor click tracking ignores non-spawn groups and resets on empty spawn id") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::reset_click_tracking(editor);

    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, "spawn_a", false, 1000));
    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, "spawn_a", true, 1100));
    CHECK(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, "spawn_a", true, 1250));

    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, "", true, 1300));
    CHECK_FALSE(RoomEditorTestAccess::should_open_spawn_group_panel_for_click(
        editor, "spawn_a", true, 1350));
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

TEST_CASE("RoomEditor explicit overlay snap resolution is source of truth") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_shared_footer_present(editor, true);

    RoomEditorTestAccess::set_overlay_snap_resolution(editor, 6);
    CHECK(RoomEditorTestAccess::current_grid_resolution(editor) == 6);

    RoomEditorTestAccess::update_grid_resolution_for_selection(
        editor,
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x5678)));
    CHECK(RoomEditorTestAccess::current_grid_resolution(editor) == 6);
}

TEST_CASE("RoomEditor overlay resolution resnap respects snap toggle") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::reset_snap_spawn_group_to_resolution_call_count(editor);

    RoomEditorTestAccess::set_snap_to_grid_enabled(editor, false);
    RoomEditorTestAccess::resnap_spawn_groups_to_overlay_resolution(editor, 5);
    CHECK(RoomEditorTestAccess::snap_spawn_group_to_resolution_call_count(editor) == 0);

    RoomEditorTestAccess::set_snap_to_grid_enabled(editor, true);
    RoomEditorTestAccess::resnap_spawn_groups_to_overlay_resolution(editor, 5);
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

TEST_CASE("RoomEditor spawn-group deferred queue coalesces repeated edits for same spawn id") {
    RoomEditor editor(nullptr, 1280, 720);

    RoomEditorTestAccess::enqueue_spawn_group_work(editor, "spawn_a", true, true, true, true);
    RoomEditorTestAccess::enqueue_spawn_group_work(editor, "spawn_a", true, false, false, false);
    RoomEditorTestAccess::enqueue_spawn_group_work(editor, "spawn_a", false, true, false, true);

    CHECK(RoomEditorTestAccess::pending_spawn_group_work_size(editor) == 1);

    RoomEditorTestAccess::process_pending_spawn_group_work(editor);
    CHECK(RoomEditorTestAccess::pending_spawn_group_work_size(editor) == 0);
}

TEST_CASE("RoomEditor ownership classification keeps room and boundary domains isolated") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_spawn_id_ownership_cache(editor, {"room_spawn"}, {"boundary_spawn"});

    CHECK(RoomEditorTestAccess::classify_spawn_group_ownership(editor, "room_spawn") ==
          static_cast<int>(devmode::room_selection_filter::SpawnOwnership::Room));
    CHECK(RoomEditorTestAccess::classify_spawn_group_ownership(editor, "boundary_spawn") ==
          static_cast<int>(devmode::room_selection_filter::SpawnOwnership::MapBoundary));
    CHECK(RoomEditorTestAccess::classify_spawn_group_ownership(editor, "other_spawn") ==
          static_cast<int>(devmode::room_selection_filter::SpawnOwnership::Other));
}

TEST_CASE("RoomEditor boundary spawn groups are recognized even when assets are not typed boundary") {
    constexpr const char* kBoundarySpawnId = "boundary_spawn";
    BoundaryProxyFixture fixture(kBoundarySpawnId);
    REQUIRE(fixture.boundary_asset != nullptr);

    RoomEditor editor(fixture.assets.get(), 1280, 720);

    CHECK(RoomEditorTestAccess::spawn_group_is_boundary(editor, kBoundarySpawnId));
    CHECK(RoomEditorTestAccess::classify_spawn_group_ownership(editor, kBoundarySpawnId) ==
          static_cast<int>(devmode::room_selection_filter::SpawnOwnership::MapBoundary));
}

TEST_CASE("RoomEditor spawn membership gating allows boundary ownership without room containment checks") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_spawn_id_ownership_cache(editor, {"room_spawn"}, {"boundary_spawn"});

    CHECK(RoomEditorTestAccess::spawn_membership_allows_room_selection(editor, "boundary_spawn", ""));
    CHECK_FALSE(RoomEditorTestAccess::spawn_membership_allows_room_selection(editor, "other_spawn", ""));
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

TEST_CASE("RoomEditor mode ownership policy isolates editor domains") {
    RoomEditor editor(nullptr, 1280, 720);
    const int hitbox = RoomEditorTestAccess::mode_hitbox();
    const int attack = RoomEditorTestAccess::mode_attack_box();
    const int movement = RoomEditorTestAccess::mode_movement();

    CHECK(RoomEditorTestAccess::mode_owns_hitbox_domain(editor, hitbox));
    CHECK_FALSE(RoomEditorTestAccess::mode_owns_hitbox_domain(editor, attack));
    CHECK(RoomEditorTestAccess::mode_owns_attack_domain(editor, attack));
    CHECK_FALSE(RoomEditorTestAccess::mode_owns_attack_domain(editor, hitbox));
    CHECK(RoomEditorTestAccess::mode_owns_movement_domain(editor, movement));
    CHECK_FALSE(RoomEditorTestAccess::mode_owns_oval_domain(editor, hitbox));
}

TEST_CASE("RoomEditor mode-switch domain checks prevent leakage between hitbox and attack") {
    RoomEditor editor(nullptr, 1280, 720);
    RoomEditorTestAccess::set_editor_mode(editor, RoomEditorTestAccess::mode_hitbox());
    CHECK(RoomEditorTestAccess::mode_owns_hitbox_domain(editor, RoomEditorTestAccess::mode_hitbox()));
    CHECK_FALSE(RoomEditorTestAccess::mode_owns_attack_domain(editor, RoomEditorTestAccess::mode_hitbox()));

    RoomEditorTestAccess::set_editor_mode(editor, RoomEditorTestAccess::mode_attack_box());
    CHECK(RoomEditorTestAccess::mode_owns_attack_domain(editor, RoomEditorTestAccess::mode_attack_box()));
    CHECK_FALSE(RoomEditorTestAccess::mode_owns_hitbox_domain(editor, RoomEditorTestAccess::mode_attack_box()));
}
#endif
