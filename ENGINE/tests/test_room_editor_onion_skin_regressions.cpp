#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path repo_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    INFO("Failed to open source file: " << path.string());
    REQUIRE(input.good());
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

void require_missing_token(const std::string& content, const char* token, const char* context) {
    INFO("Unexpected token in " << context << ": " << token);
    CHECK(content.find(token) == std::string::npos);
}

}  // namespace

TEST_CASE("Room editor panels do not expose onion-skin controls") {
    const auto root = repo_root();
    const std::string anchor_h =
        read_text_file(root / "ENGINE" / "editor" / "devtools" / "room_anchor_tools_panel.hpp");
    const std::string anchor_cpp =
        read_text_file(root / "ENGINE" / "editor" / "devtools" / "room_anchor_tools_panel.cpp");
    const std::string box_h =
        read_text_file(root / "ENGINE" / "editor" / "devtools" / "room_box_tools_panel.hpp");
    const std::string box_cpp =
        read_text_file(root / "ENGINE" / "editor" / "devtools" / "room_box_tools_panel.cpp");

    require_missing_token(anchor_h, "onion", "room_anchor_tools_panel.hpp");
    require_missing_token(anchor_cpp, "onion", "room_anchor_tools_panel.cpp");
    require_missing_token(box_h, "onion", "room_box_tools_panel.hpp");
    require_missing_token(box_cpp, "onion", "room_box_tools_panel.cpp");
    require_missing_token(anchor_cpp, "DMCheckbox(\"Show onion skin", "room_anchor_tools_panel.cpp");
    require_missing_token(box_cpp, "DMCheckbox(\"Show onion skin", "room_box_tools_panel.cpp");
}

TEST_CASE("Room editor overlay paths do not emit onion overlays in any mode") {
    const auto root = repo_root();
    const std::string editor_h = read_text_file(root / "ENGINE" / "editor" / "devtools" / "room_editor.hpp");
    const std::string editor_cpp = read_text_file(root / "ENGINE" / "editor" / "devtools" / "room_editor.cpp");

    require_missing_token(editor_h, "onion", "room_editor.hpp");
    require_missing_token(editor_cpp, "onion", "room_editor.cpp");
    require_missing_token(editor_cpp, "adjacent_frame_for_editor", "room_editor.cpp");
    require_missing_token(editor_cpp, "collect_anchor_overlay_handles_for_frame", "room_editor.cpp");
    require_missing_token(editor_cpp, "build_hitbox_overlay_volumes_for_frame", "room_editor.cpp");
    require_missing_token(editor_cpp, "build_attack_box_overlay_volumes_for_frame", "room_editor.cpp");
}

TEST_CASE("Anchor drag snap logic gates snapping by grid toggle") {
    const auto root = repo_root();
    const std::string editor_cpp = read_text_file(root / "ENGINE" / "editor" / "devtools" / "room_editor.cpp");
    CHECK(editor_cpp.find("if (snap_to_grid_enabled_)") != std::string::npos);
}

TEST_CASE("Anchor drag snap logic excludes dragged anchor as candidate") {
    const auto root = repo_root();
    const std::string editor_cpp = read_text_file(root / "ENGINE" / "editor" / "devtools" / "room_editor.cpp");
    CHECK(editor_cpp.find("if (candidate.name == anchor_name)") != std::string::npos);
}

TEST_CASE("Anchor drag snap logic uses fixed screen threshold") {
    const auto root = repo_root();
    const std::string editor_cpp = read_text_file(root / "ENGINE" / "editor" / "devtools" / "room_editor.cpp");
    CHECK(editor_cpp.find("kAnchorPointSnapRadiusPx") != std::string::npos);
}
