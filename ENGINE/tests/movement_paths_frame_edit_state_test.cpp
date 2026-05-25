#include <cstdlib>
#include <iostream>
#include <vector>

#include <nlohmann/json.hpp>

#include "devtools/frame_editors/shared/FrameEditState.hpp"

namespace {

using devmode::frame_editors::MovementFrame;
using devmode::frame_editors::build_payload_from_movement_paths;
using devmode::frame_editors::parse_movement_paths_from_payload;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << "\n";
        std::exit(1);
    }
}

}  // namespace

int main() {
    nlohmann::json legacy = {
        {"movement", nlohmann::json::array({
            nlohmann::json::array({1, 0, 2}),
            nlohmann::json::array({3, 0, 4}),
        })},
    };
    auto legacy_paths = parse_movement_paths_from_payload(legacy);
    require(legacy_paths.size() == 1, "legacy movement should load as one path");
    require(legacy_paths[0].size() == 2, "legacy movement path should preserve frame count");
    require(static_cast<int>(legacy_paths[0][1].dx) == 3, "legacy path frame dx mismatch");
    require(static_cast<int>(legacy_paths[0][1].dz) == 4, "legacy path frame dz mismatch");

    std::vector<std::vector<MovementFrame>> paths(2);
    paths[0].resize(2);
    paths[0][0].dx = 1.0f;
    paths[0][1].dz = 2.0f;
    paths[1].resize(2);
    paths[1][0].dx = -5.0f;
    paths[1][1].dy = 7.0f;

    nlohmann::json stored = build_payload_from_movement_paths(paths, legacy);
    require(stored.contains("movement_paths"), "canonical payload should contain movement_paths");
    require(!stored.contains("movement"), "canonical payload should not retain legacy movement");
    require(stored["movement_paths"].size() == 2, "canonical payload should preserve multiple paths");
    require(stored["movement_total"]["dx"].get<int>() == 1, "primary movement_total dx mismatch");
    require(stored["movement_total"]["dz"].get<double>() == 2.0, "primary movement_total dz mismatch");
    require(stored["movement_totals"].size() == 2, "movement_totals should track each path");
    require(stored["movement_totals"][1]["dx"].get<int>() == -5, "second path total dx mismatch");
    require(stored["movement_totals"][1]["dy"].get<int>() == 7, "second path total dy mismatch");

    auto round_tripped = parse_movement_paths_from_payload(stored);
    require(round_tripped.size() == 2, "round-trip should keep two paths");
    require(static_cast<int>(round_tripped[1][0].dx) == -5, "round-trip second path dx mismatch");
    require(static_cast<int>(round_tripped[1][1].dy) == 7, "round-trip second path dy mismatch");

    return 0;
}
