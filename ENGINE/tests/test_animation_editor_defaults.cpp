#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationEditorWindow.hpp"

namespace {

namespace fs = std::filesystem;

fs::path make_unique_temp_dir(const std::string& suffix) {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("engine_animation_defaults_" + suffix + "_" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(dir, ec);
    ec.clear();
    fs::create_directories(dir, ec);
    return dir;
}

void write_dummy_png(const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    out << "PNG";
}

std::shared_ptr<animation_editor::AnimationDocument> make_document(const fs::path& asset_root) {
    auto document = std::make_shared<animation_editor::AnimationDocument>();
    document->load_from_manifest(nlohmann::json::object(), asset_root, nullptr);
    return document;
}

void configure_defaults(animation_editor::AnimationEditorWindow& window,
                        const std::shared_ptr<animation_editor::AnimationDocument>& document,
                        const fs::path& asset_root,
                        const std::vector<fs::path>& base_frames,
                        int distance_per_frame,
                        bool basic,
                        bool diagonals,
                        bool elevation,
                        bool diagonals_3d,
                        bool base_faces_right = false) {
    window.document_ = document;
    window.asset_root_path_ = asset_root;
    window.ensure_defaults_modal_widgets();
    window.defaults_base_frame_paths_ = base_frames;
    window.defaults_basic_movement_checkbox_->set_value(basic);
    window.defaults_diagonals_checkbox_->set_value(diagonals);
    window.defaults_elevation_checkbox_->set_value(elevation);
    window.defaults_3d_diagonals_checkbox_->set_value(diagonals_3d);
    window.defaults_base_faces_right_checkbox_->set_value(base_faces_right);
    window.defaults_distance_box_->set_value(std::to_string(distance_per_frame));
}

const nlohmann::json* payload_for(const std::shared_ptr<animation_editor::AnimationDocument>& document,
                                  const std::string& id,
                                  nlohmann::json& storage) {
    auto payload = document->animation_payload_json(id);
    if (!payload.has_value() || !payload->is_object()) {
        return nullptr;
    }
    storage = std::move(*payload);
    return &storage;
}

void check_local_movement_payload(const nlohmann::json& payload,
                                  int frame_count,
                                  int total_dx,
                                  int total_dy,
                                  int total_dz) {
    auto distribute_component = [frame_count](int total) {
        std::vector<int> values(static_cast<std::size_t>(frame_count), 0);
        const int base = total / frame_count;
        int remainder = total % frame_count;
        for (int i = 0; i < frame_count; ++i) {
            values[static_cast<std::size_t>(i)] = base;
            if (remainder > 0) {
                ++values[static_cast<std::size_t>(i)];
                --remainder;
            } else if (remainder < 0) {
                --values[static_cast<std::size_t>(i)];
                ++remainder;
            }
        }
        return values;
    };
    const std::vector<int> frame_dx = distribute_component(total_dx);
    const std::vector<int> frame_dy = distribute_component(total_dy);
    const std::vector<int> frame_dz = distribute_component(total_dz);

    REQUIRE(payload.contains("movement"));
    REQUIRE(payload["movement"].is_array());
    REQUIRE(payload["movement"].size() == static_cast<std::size_t>(frame_count));
    for (int i = 0; i < frame_count; ++i) {
        CHECK(payload["movement"][i] ==
              nlohmann::json::array({frame_dx[static_cast<std::size_t>(i)],
                                     frame_dy[static_cast<std::size_t>(i)],
                                     frame_dz[static_cast<std::size_t>(i)]}));
    }

    REQUIRE(payload.contains("movement_total"));
    CHECK(payload["movement_total"]["dx"] == total_dx);
    CHECK(payload["movement_total"]["dy"] == total_dy);
    CHECK(payload["movement_total"]["dz"] == total_dz);
    REQUIRE(payload.contains("invert_x"));
    REQUIRE(payload.contains("invert_y"));
    REQUIRE(payload.contains("invert_z"));
    CHECK(payload["invert_x"] == false);
    CHECK(payload["invert_y"] == false);
    CHECK(payload["invert_z"] == false);

    REQUIRE(payload.contains("anchor_points"));
    CHECK_FALSE(payload.contains("hit_boxes"));
    CHECK_FALSE(payload.contains("attack_boxes"));
    CHECK(payload["anchor_points"].size() == static_cast<std::size_t>(frame_count));
    for (int i = 0; i < frame_count; ++i) {
        CHECK(payload["anchor_points"][i].is_array());
        CHECK(payload["anchor_points"][i].empty());
    }
}

void check_tags(const nlohmann::json& payload, std::initializer_list<const char*> tags) {
    REQUIRE(payload.contains("tags"));
    REQUIRE(payload["tags"].is_array());
    for (const char* tag : tags) {
        const bool found = std::any_of(payload["tags"].begin(), payload["tags"].end(), [tag](const nlohmann::json& entry) {
            return entry.is_string() && entry.get<std::string>() == tag;
        });
        CHECK(found);
    }
}

void check_inherited_payload(const nlohmann::json& payload,
                             const std::string& source_id,
                             bool invert_x,
                             bool invert_y,
                             bool invert_z,
                             bool flip_h,
                             bool flip_v) {
    REQUIRE(payload.contains("source"));
    CHECK(payload["source"]["kind"] == "animation");
    CHECK(payload["source"]["name"] == source_id);
    CHECK(payload["inherit_data"] == true);
    CHECK(payload["invert_x"] == invert_x);
    CHECK(payload["invert_y"] == invert_y);
    CHECK(payload["invert_z"] == invert_z);
    CHECK(payload["invert_frames_horizontal"] == flip_h);
    CHECK(payload["invert_frames_vertical"] == flip_v);
    CHECK_FALSE(payload.contains("movement"));
    CHECK_FALSE(payload.contains("movement_total"));
    CHECK_FALSE(payload.contains("anchor_points"));
    CHECK_FALSE(payload.contains("hit_boxes"));
    CHECK_FALSE(payload.contains("attack_boxes"));
}

}  // namespace

TEST_CASE("AnimationEditorWindow create defaults minimizes basic and diagonal frame sources") {
    const fs::path root = make_unique_temp_dir("basic");
    const fs::path frame0 = root / "base_0.png";
    const fs::path frame1 = root / "base_1.png";
    write_dummy_png(frame0);
    write_dummy_png(frame1);

    auto document = make_document(root);
    animation_editor::AnimationEditorWindow window;
    configure_defaults(window, document, root, {frame0, frame1}, 4, true, true, false, false, false);

    window.handle_create_defaults();

    const std::set<std::string> expected_ids = {
        "left", "right", "up", "down", "forward", "backward",
        "forward_left", "forward_right", "backward_left", "backward_right",
    };
    const auto ids = document->animation_ids();
    for (const auto& id : expected_ids) {
        CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    }
    nlohmann::json payload_storage;
    const nlohmann::json* left = payload_for(document, "left", payload_storage);
    REQUIRE(left != nullptr);
    REQUIRE(left->contains("source"));
    CHECK((*left)["source"]["kind"] == "folder");
    check_local_movement_payload(*left, 2, -4, 0, 0);
    check_tags(*left, {"movement", "directional", "left", "world_x_negative"});

    const nlohmann::json* right = payload_for(document, "right", payload_storage);
    REQUIRE(right != nullptr);
    check_inherited_payload(*right, "left", true, false, false, true, false);
    check_tags(*right, {"movement", "directional", "right", "world_x_positive"});

    const nlohmann::json* up = payload_for(document, "up", payload_storage);
    REQUIRE(up != nullptr);
    REQUIRE(up->contains("source"));
    CHECK((*up)["source"]["kind"] == "folder");
    check_local_movement_payload(*up, 2, 0, 0, -4);
    check_tags(*up, {"movement", "directional", "up", "forward", "world_z_negative"});

    const nlohmann::json* forward = payload_for(document, "forward", payload_storage);
    REQUIRE(forward != nullptr);
    check_inherited_payload(*forward, "up", false, false, false, false, false);

    const nlohmann::json* down = payload_for(document, "down", payload_storage);
    REQUIRE(down != nullptr);
    check_inherited_payload(*down, "up", false, false, true, false, true);
    check_tags(*down, {"movement", "directional", "down", "backward", "world_z_positive"});

    const nlohmann::json* backward = payload_for(document, "backward", payload_storage);
    REQUIRE(backward != nullptr);
    check_inherited_payload(*backward, "up", false, false, true, false, true);

    const nlohmann::json* forward_left = payload_for(document, "forward_left", payload_storage);
    REQUIRE(forward_left != nullptr);
    REQUIRE((*forward_left)["source"]["kind"] == "folder");
    check_local_movement_payload(*forward_left, 2, -4, 0, -4);

    const nlohmann::json* backward_right = payload_for(document, "backward_right", payload_storage);
    REQUIRE(backward_right != nullptr);
    check_inherited_payload(*backward_right, "forward_left", true, false, true, true, true);

    CHECK(fs::exists(root / "left" / "0.png"));
    CHECK(fs::exists(root / "up" / "0.png"));
    CHECK(fs::exists(root / "forward_left" / "0.png"));
    CHECK_FALSE(fs::exists(root / "right" / "0.png"));
    CHECK_FALSE(fs::exists(root / "down" / "0.png"));
    CHECK_FALSE(fs::exists(root / "backward_right" / "0.png"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("AnimationEditorWindow create defaults adds elevation and 3D diagonal inheritance") {
    const fs::path root = make_unique_temp_dir("elevation_3d");
    const fs::path frame0 = root / "base_0.png";
    const fs::path frame1 = root / "base_1.png";
    write_dummy_png(frame0);
    write_dummy_png(frame1);

    auto document = make_document(root);
    animation_editor::AnimationEditorWindow window;
    configure_defaults(window, document, root, {frame0, frame1}, 4, false, false, true, true, false);

    window.handle_create_defaults();

    const std::set<std::string> expected_ids = {
        "elevation_up", "elevation_down",
        "up_forward_left", "up_forward_right", "up_backward_left", "up_backward_right",
        "down_forward_left", "down_forward_right", "down_backward_left", "down_backward_right",
    };
    const auto ids = document->animation_ids();
    for (const auto& id : expected_ids) {
        CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    }

    nlohmann::json payload_storage;
    const nlohmann::json* elevation_up = payload_for(document, "elevation_up", payload_storage);
    REQUIRE(elevation_up != nullptr);
    REQUIRE(elevation_up->contains("source"));
    CHECK((*elevation_up)["source"]["kind"] == "folder");
    check_local_movement_payload(*elevation_up, 2, 0, 4, 0);
    check_tags(*elevation_up, {"movement", "directional", "elevation", "up", "world_y_positive"});

    const nlohmann::json* elevation_down = payload_for(document, "elevation_down", payload_storage);
    REQUIRE(elevation_down != nullptr);
    check_inherited_payload(*elevation_down, "elevation_up", false, true, false, false, true);
    check_tags(*elevation_down, {"movement", "directional", "elevation", "down", "world_y_negative"});

    const nlohmann::json* up_forward_right = payload_for(document, "up_forward_right", payload_storage);
    REQUIRE(up_forward_right != nullptr);
    check_inherited_payload(*up_forward_right, "up_forward_left", true, false, false, true, false);
    REQUIRE(up_forward_right->contains("derived_modifiers"));
    CHECK((*up_forward_right)["derived_modifiers"]["reverse"] == false);
    CHECK_FALSE((*up_forward_right)["derived_modifiers"].contains("flipX"));
    CHECK_FALSE((*up_forward_right)["derived_modifiers"].contains("flipY"));
    check_tags(*up_forward_right, {"movement", "directional", "up", "forward", "right", "world_x_positive", "world_y_positive", "world_z_negative"});

    const nlohmann::json* down_backward_left = payload_for(document, "down_backward_left", payload_storage);
    REQUIRE(down_backward_left != nullptr);
    check_inherited_payload(*down_backward_left, "up_forward_left", false, true, true, false, true);
    check_tags(*down_backward_left, {"movement", "directional", "down", "backward", "left", "world_x_negative", "world_y_negative", "world_z_positive"});

    CHECK(fs::exists(root / "elevation_up" / "0.png"));
    CHECK(fs::exists(root / "up_forward_left" / "0.png"));
    CHECK_FALSE(fs::exists(root / "elevation_down" / "0.png"));
    CHECK_FALSE(fs::exists(root / "down_backward_left" / "0.png"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("AnimationEditorWindow create defaults emits one movement node for single-frame defaults") {
    const fs::path root = make_unique_temp_dir("single_frame");
    const fs::path frame0 = root / "base_0.png";
    write_dummy_png(frame0);

    auto document = make_document(root);
    animation_editor::AnimationEditorWindow window;
    configure_defaults(window, document, root, {frame0}, 9, true, false, false, false, true);

    window.handle_create_defaults();

    nlohmann::json payload_storage;
    const nlohmann::json* right = payload_for(document, "right", payload_storage);
    REQUIRE(right != nullptr);
    check_local_movement_payload(*right, 1, 9, 0, 0);

    const nlohmann::json* left = payload_for(document, "left", payload_storage);
    REQUIRE(left != nullptr);
    check_inherited_payload(*left, "right", true, false, false, true, false);

    const nlohmann::json* up = payload_for(document, "up", payload_storage);
    REQUIRE(up != nullptr);
    check_local_movement_payload(*up, 1, 0, 0, -9);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("AnimationEditorWindow create defaults requests base PNGs when none are selected") {
    const fs::path root = make_unique_temp_dir("picker_required");
    const fs::path frame0 = root / "base_0.png";
    const fs::path frame1 = root / "base_1.png";
    write_dummy_png(frame0);
    write_dummy_png(frame1);

    auto document = make_document(root);
    animation_editor::AnimationEditorWindow window;
    configure_defaults(window, document, root, {}, 6, true, false, false, false, true);

    bool picker_called = false;
    window.png_sequence_picker_override_ = [&]() {
        picker_called = true;
        return std::vector<fs::path>{frame1, frame0};
    };

    window.handle_create_defaults();

    CHECK(picker_called);
    CHECK(window.defaults_base_frame_paths_.size() == 2);
    nlohmann::json payload_storage;
    const nlohmann::json* right = payload_for(document, "right", payload_storage);
    REQUIRE(right != nullptr);
    check_local_movement_payload(*right, 2, 6, 0, 0);
    CHECK(fs::exists(root / "right" / "0.png"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("AnimationEditorWindow delete animation uses injectable SDL confirmation result") {
    const fs::path root = make_unique_temp_dir("delete_confirm");
    auto document = make_document(root);
    animation_editor::AnimationEditorWindow window;
    configure_defaults(window, document, root, {}, 4, true, false, false, false, false);

    document->create_animation("walk");
    auto ids_before = document->animation_ids();
    REQUIRE(std::find(ids_before.begin(), ids_before.end(), "walk") != ids_before.end());

    window.choice_prompt_override_ = [](const std::string&, const std::string&, const std::vector<int>&) {
        return std::optional<int>{0};
    };
    window.delete_animation_with_confirmation("walk");
    auto ids_after_cancel = document->animation_ids();
    CHECK(std::find(ids_after_cancel.begin(), ids_after_cancel.end(), "walk") != ids_after_cancel.end());

    window.choice_prompt_override_ = [](const std::string&, const std::string&, const std::vector<int>&) {
        return std::optional<int>{1};
    };
    window.delete_animation_with_confirmation("walk");
    auto ids_after_confirm = document->animation_ids();
    CHECK(std::find(ids_after_confirm.begin(), ids_after_confirm.end(), "walk") == ids_after_confirm.end());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("AnimationEditorWindow create defaults button opens modal on mouse down") {
    animation_editor::AnimationEditorWindow window;
    window.set_visible(true, false);
    window.set_bounds(SDL_Rect{0, 0, 900, 700});
    REQUIRE(window.create_defaults_button_ != nullptr);
    const SDL_Rect button_rect = window.create_defaults_button_->rect();
    REQUIRE(button_rect.w > 0);
    REQUIRE(button_rect.h > 0);
    window.defaults_modal_visible_ = false;

    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = button_rect.x + 4;
    down.button.y = button_rect.y + 4;

    const bool handled = window.handle_header_event(down);
    CHECK(handled);
    CHECK(window.defaults_modal_visible_);
}

TEST_CASE("AnimationEditorWindow handle_event opens defaults modal before nested handlers") {
    animation_editor::AnimationEditorWindow window;
    window.set_visible(true, false);
    window.set_bounds(SDL_Rect{0, 0, 900, 700});
    REQUIRE(window.create_defaults_button_ != nullptr);
    const SDL_Rect button_rect = window.create_defaults_button_->rect();
    REQUIRE(button_rect.w > 0);
    REQUIRE(button_rect.h > 0);
    window.defaults_modal_visible_ = false;

    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = button_rect.x + 4;
    down.button.y = button_rect.y + 4;

    const bool handled = window.handle_event(down);
    CHECK(handled);
    CHECK(window.defaults_modal_visible_);
}

TEST_CASE("AnimationEditorWindow defaults modal closes on outside click and can reopen") {
    animation_editor::AnimationEditorWindow window;
    window.set_visible(true, false);
    window.set_bounds(SDL_Rect{0, 0, 900, 700});
    REQUIRE(window.create_defaults_button_ != nullptr);
    const SDL_Rect button_rect = window.create_defaults_button_->rect();
    REQUIRE(button_rect.w > 0);
    REQUIRE(button_rect.h > 0);

    SDL_Event open_down{};
    open_down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    open_down.button.button = SDL_BUTTON_LEFT;
    open_down.button.x = button_rect.x + 4;
    open_down.button.y = button_rect.y + 4;
    CHECK(window.handle_event(open_down));
    CHECK(window.defaults_modal_visible_);
    CHECK(window.defaults_modal_rect_.w > 0);
    CHECK(window.defaults_modal_rect_.h > 0);

    SDL_Event outside_down{};
    outside_down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    outside_down.button.button = SDL_BUTTON_LEFT;
    outside_down.button.x = window.defaults_modal_rect_.x - 10;
    outside_down.button.y = window.defaults_modal_rect_.y - 10;
    CHECK(window.handle_event(outside_down));
    CHECK_FALSE(window.defaults_modal_visible_);
    CHECK(window.defaults_modal_rect_.w == 0);
    CHECK(window.defaults_modal_rect_.h == 0);

    SDL_Event reopen_down{};
    reopen_down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    reopen_down.button.button = SDL_BUTTON_LEFT;
    reopen_down.button.x = button_rect.x + 4;
    reopen_down.button.y = button_rect.y + 4;
    CHECK(window.handle_event(reopen_down));
    CHECK(window.defaults_modal_visible_);
    CHECK(window.defaults_modal_rect_.w > 0);
    CHECK(window.defaults_modal_rect_.h > 0);
}

TEST_CASE("AnimationEditorWindow defaults modal guard prevents locked state on invalid bounds") {
    animation_editor::AnimationEditorWindow window;
    window.set_visible(true, false);
    window.set_bounds(SDL_Rect{0, 0, 280, 180});
    REQUIRE(window.create_defaults_button_ != nullptr);
    const SDL_Rect button_rect = window.create_defaults_button_->rect();
    REQUIRE(button_rect.w > 0);
    REQUIRE(button_rect.h > 0);

    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = button_rect.x + 4;
    down.button.y = button_rect.y + 4;

    CHECK(window.handle_event(down));
    CHECK_FALSE(window.defaults_modal_visible_);
    CHECK(window.defaults_modal_rect_.w == 0);
    CHECK(window.defaults_modal_rect_.h == 0);

    SDL_Event retry_down{};
    retry_down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    retry_down.button.button = SDL_BUTTON_LEFT;
    retry_down.button.x = button_rect.x + 6;
    retry_down.button.y = button_rect.y + 6;
    CHECK(window.handle_event(retry_down));
    CHECK_FALSE(window.defaults_modal_visible_);
}

TEST_CASE("AnimationEditorWindow add animation button activates on mouse down") {
    animation_editor::AnimationEditorWindow window;
    auto document = std::make_shared<animation_editor::AnimationDocument>();
    document->load_from_manifest(nlohmann::json::object(), fs::temp_directory_path(), nullptr);
    window.document_ = document;
    window.set_visible(true, false);
    window.set_bounds(SDL_Rect{0, 0, 900, 700});

    REQUIRE(window.add_button_ != nullptr);
    const SDL_Rect button_rect = window.add_button_->rect();
    REQUIRE(button_rect.w > 0);
    REQUIRE(button_rect.h > 0);

    window.text_prompt_override_ = [](const std::string&, const std::string&, const std::string&) {
        return std::optional<std::string>{"jump"};
    };

    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = button_rect.x + 4;
    down.button.y = button_rect.y + 4;

    CHECK(window.handle_event(down));
    const auto ids = document->animation_ids();
    CHECK(std::find(ids.begin(), ids.end(), "jump") != ids.end());
}

TEST_CASE("AnimationEditorWindow controller button activates on mouse up only") {
    animation_editor::AnimationEditorWindow window;
    window.set_visible(true, false);
    window.set_bounds(SDL_Rect{0, 0, 900, 700});
    REQUIRE(window.controller_button_ != nullptr);
    const SDL_Rect button_rect = window.controller_button_->rect();
    REQUIRE(button_rect.w > 0);
    REQUIRE(button_rect.h > 0);

    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = button_rect.x + 4;
    down.button.y = button_rect.y + 4;
    CHECK(window.handle_event(down));
    CHECK_FALSE(window.defaults_modal_visible_);

    SDL_Event up{};
    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
    up.button.button = SDL_BUTTON_LEFT;
    up.button.x = button_rect.x + 4;
    up.button.y = button_rect.y + 4;
    CHECK(window.handle_event(up));
}

TEST_CASE("AnimationEditorWindow header button press is blocked while defaults modal is active") {
    animation_editor::AnimationEditorWindow window;
    auto document = std::make_shared<animation_editor::AnimationDocument>();
    document->load_from_manifest(nlohmann::json::object(), fs::temp_directory_path(), nullptr);
    window.document_ = document;
    window.set_visible(true, false);
    window.set_bounds(SDL_Rect{0, 0, 900, 700});
    REQUIRE(window.add_button_ != nullptr);
    const SDL_Rect add_rect = window.add_button_->rect();

    window.defaults_modal_visible_ = true;
    window.defaults_modal_rect_ = SDL_Rect{200, 120, 400, 280};
    window.text_prompt_override_ = [](const std::string&, const std::string&, const std::string&) {
        return std::optional<std::string>{"blocked"};
    };

    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = add_rect.x + 4;
    down.button.y = add_rect.y + 4;
    CHECK(window.handle_event(down));
    CHECK(window.defaults_modal_visible_);
    const auto ids = document->animation_ids();
    CHECK(std::find(ids.begin(), ids.end(), "blocked") == ids.end());
}

TEST_CASE("AnimationEditorWindow header button press is blocked while list context menu is open") {
    animation_editor::AnimationEditorWindow window;
    auto document = std::make_shared<animation_editor::AnimationDocument>();
    document->load_from_manifest(nlohmann::json::object(), fs::temp_directory_path(), nullptr);
    window.document_ = document;
    window.set_visible(true, false);
    window.set_bounds(SDL_Rect{0, 0, 900, 700});
    REQUIRE(window.add_button_ != nullptr);
    REQUIRE(window.list_context_menu_ != nullptr);

    const SDL_Rect add_rect = window.add_button_->rect();
    window.text_prompt_override_ = [](const std::string&, const std::string&, const std::string&) {
        return std::optional<std::string>{"menu_blocked"};
    };
    window.list_context_menu_->open(window.bounds_, SDL_Point{add_rect.x + 2, add_rect.y + 2},
                                    {{.label = "Dummy", .callback = []() {}}});
    REQUIRE(window.list_context_menu_->is_open());

    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = add_rect.x + 4;
    down.button.y = add_rect.y + 4;
    CHECK(window.handle_event(down));
    CHECK(window.list_context_menu_->is_open());
    const auto ids = document->animation_ids();
    CHECK(std::find(ids.begin(), ids.end(), "menu_blocked") == ids.end());
}

TEST_CASE("AnimationEditorWindow header recovers from invalid defaults modal state") {
    animation_editor::AnimationEditorWindow window;
    auto document = std::make_shared<animation_editor::AnimationDocument>();
    document->load_from_manifest(nlohmann::json::object(), fs::temp_directory_path(), nullptr);
    window.document_ = document;
    window.set_visible(true, false);
    window.set_bounds(SDL_Rect{0, 0, 900, 700});

    REQUIRE(window.add_button_ != nullptr);
    const SDL_Rect button_rect = window.add_button_->rect();
    REQUIRE(button_rect.w > 0);
    REQUIRE(button_rect.h > 0);

    window.defaults_modal_visible_ = true;
    window.defaults_modal_rect_ = SDL_Rect{0, 0, 0, 0};
    window.text_prompt_override_ = [](const std::string&, const std::string&, const std::string&) {
        return std::optional<std::string>{"recovered"};
    };

    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = button_rect.x + 4;
    down.button.y = button_rect.y + 4;

    CHECK(window.handle_event(down));
    CHECK_FALSE(window.defaults_modal_visible_);
    const auto ids = document->animation_ids();
    CHECK(std::find(ids.begin(), ids.end(), "recovered") != ids.end());
}
