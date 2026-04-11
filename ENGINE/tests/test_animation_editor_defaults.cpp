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
                        bool diagonals_3d) {
    window.document_ = document;
    window.asset_root_path_ = asset_root;
    window.ensure_defaults_modal_widgets();
    window.defaults_base_frame_paths_ = base_frames;
    window.defaults_basic_movement_checkbox_->set_value(basic);
    window.defaults_diagonals_checkbox_->set_value(diagonals);
    window.defaults_elevation_checkbox_->set_value(elevation);
    window.defaults_3d_diagonals_checkbox_->set_value(diagonals_3d);
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
    REQUIRE(payload.contains("hit_boxes"));
    REQUIRE(payload.contains("attack_boxes"));
    CHECK(payload["anchor_points"].size() == static_cast<std::size_t>(frame_count));
    CHECK(payload["hit_boxes"].size() == static_cast<std::size_t>(frame_count));
    CHECK(payload["attack_boxes"].size() == static_cast<std::size_t>(frame_count));
    for (int i = 0; i < frame_count; ++i) {
        CHECK(payload["anchor_points"][i].is_array());
        CHECK(payload["hit_boxes"][i].is_array());
        CHECK(payload["attack_boxes"][i].is_array());
        CHECK(payload["anchor_points"][i].empty());
        CHECK(payload["hit_boxes"][i].empty());
        CHECK(payload["attack_boxes"][i].empty());
    }
}

}  // namespace

TEST_CASE("AnimationEditorWindow create defaults uses forward/backward depth naming") {
    const fs::path root = make_unique_temp_dir("basic");
    const fs::path frame0 = root / "base_0.png";
    const fs::path frame1 = root / "base_1.png";
    write_dummy_png(frame0);
    write_dummy_png(frame1);

    auto document = make_document(root);
    animation_editor::AnimationEditorWindow window;
    configure_defaults(window, document, root, {frame0, frame1}, 4, true, true, false, false);

    window.handle_create_defaults();

    const std::set<std::string> expected_ids = {
        "left", "right", "forward", "backward",
        "forward_left", "forward_right", "backward_left", "backward_right",
    };
    const auto ids = document->animation_ids();
    CHECK(ids.size() == expected_ids.size());
    for (const auto& id : expected_ids) {
        CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    }
    CHECK(std::find(ids.begin(), ids.end(), "up") == ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "down") == ids.end());

    nlohmann::json payload_storage;
    const nlohmann::json* forward = payload_for(document, "forward", payload_storage);
    REQUIRE(forward != nullptr);
    check_local_movement_payload(*forward, 2, 0, 0, -4);

    const nlohmann::json* backward = payload_for(document, "backward", payload_storage);
    REQUIRE(backward != nullptr);
    check_local_movement_payload(*backward, 2, 0, 0, 4);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("AnimationEditorWindow create defaults adds elevation and 3D diagonal payloads") {
    const fs::path root = make_unique_temp_dir("elevation_3d");
    const fs::path frame0 = root / "base_0.png";
    const fs::path frame1 = root / "base_1.png";
    write_dummy_png(frame0);
    write_dummy_png(frame1);

    auto document = make_document(root);
    animation_editor::AnimationEditorWindow window;
    configure_defaults(window, document, root, {frame0, frame1}, 4, false, false, true, true);

    window.handle_create_defaults();

    const std::set<std::string> expected_ids = {
        "up", "down",
        "up_forward_left", "up_forward_right", "up_backward_left", "up_backward_right",
        "down_forward_left", "down_forward_right", "down_backward_left", "down_backward_right",
    };
    const auto ids = document->animation_ids();
    CHECK(ids.size() == expected_ids.size());
    for (const auto& id : expected_ids) {
        CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    }

    nlohmann::json payload_storage;
    const nlohmann::json* up = payload_for(document, "up", payload_storage);
    REQUIRE(up != nullptr);
    REQUIRE(up->contains("source"));
    CHECK((*up)["source"]["kind"] == "folder");
    check_local_movement_payload(*up, 2, 0, 4, 0);

    const nlohmann::json* down = payload_for(document, "down", payload_storage);
    REQUIRE(down != nullptr);
    REQUIRE(down->contains("source"));
    CHECK((*down)["source"]["kind"] == "folder");
    check_local_movement_payload(*down, 2, 0, -4, 0);

    const nlohmann::json* up_forward_right = payload_for(document, "up_forward_right", payload_storage);
    REQUIRE(up_forward_right != nullptr);
    REQUIRE(up_forward_right->contains("source"));
    CHECK((*up_forward_right)["source"]["kind"] == "animation");
    CHECK((*up_forward_right)["source"]["name"] == "up_forward_left");
    CHECK((*up_forward_right)["inherit_data"] == false);
    CHECK((*up_forward_right)["invert_frames_horizontal"] == true);
    CHECK((*up_forward_right)["invert_frames_vertical"] == false);
    REQUIRE(up_forward_right->contains("derived_modifiers"));
    CHECK((*up_forward_right)["derived_modifiers"]["reverse"] == false);
    CHECK_FALSE((*up_forward_right)["derived_modifiers"].contains("flipX"));
    CHECK_FALSE((*up_forward_right)["derived_modifiers"].contains("flipY"));
    check_local_movement_payload(*up_forward_right, 2, 4, 4, -4);

    const nlohmann::json* down_backward_left = payload_for(document, "down_backward_left", payload_storage);
    REQUIRE(down_backward_left != nullptr);
    REQUIRE(down_backward_left->contains("source"));
    CHECK((*down_backward_left)["source"]["kind"] == "animation");
    CHECK((*down_backward_left)["source"]["name"] == "down_forward_left");
    CHECK((*down_backward_left)["inherit_data"] == false);
    CHECK((*down_backward_left)["invert_frames_horizontal"] == false);
    CHECK((*down_backward_left)["invert_frames_vertical"] == false);
    check_local_movement_payload(*down_backward_left, 2, -4, -4, 4);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("AnimationEditorWindow create defaults emits one movement node for single-frame defaults") {
    const fs::path root = make_unique_temp_dir("single_frame");
    const fs::path frame0 = root / "base_0.png";
    write_dummy_png(frame0);

    auto document = make_document(root);
    animation_editor::AnimationEditorWindow window;
    configure_defaults(window, document, root, {frame0}, 9, true, false, false, false);

    window.handle_create_defaults();

    nlohmann::json payload_storage;
    const nlohmann::json* left = payload_for(document, "left", payload_storage);
    REQUIRE(left != nullptr);
    check_local_movement_payload(*left, 1, -9, 0, 0);

    const nlohmann::json* forward = payload_for(document, "forward", payload_storage);
    REQUIRE(forward != nullptr);
    check_local_movement_payload(*forward, 1, 0, 0, -9);

    std::error_code ec;
    fs::remove_all(root, ec);
}
