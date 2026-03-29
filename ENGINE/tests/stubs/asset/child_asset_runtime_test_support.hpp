#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

class Asset;
class Assets;

namespace test_child_asset_runtime {

struct AnchorSpec {
    std::string name;
    int offset_x = 0;
    int offset_y = 0;
    int offset_z = 0;
    int depth_offset = 0;
    std::optional<int> resolution_layer{};
    float world_depth_offset = 0.0f;
    bool exists = true;
    std::optional<float> exact_offset_x{};
    std::optional<float> exact_offset_y{};
    std::optional<float> exact_offset_z{};
    bool flip_horizontal = false;
    bool flip_vertical = false;
    float rotation_degrees = 0.0f;
};

Assets* create_assets_stub();
void destroy_assets_stub(Assets* assets);

std::unique_ptr<Asset> make_test_asset(const std::string& name,
                                       int world_x = 0,
                                       int world_y = 0,
                                       int world_z = 0,
                                       int resolution_layer = 0);
Asset* attach_owned_asset(Assets* assets, std::unique_ptr<Asset> asset);

void set_spawn_failures(Assets& assets, const std::string& name, int failures);
void set_anchor(Asset& asset, const AnchorSpec& spec);
void clear_anchors(Asset& asset);

std::size_t asset_count(const Assets& assets);
bool active_assets_dirty(const Assets& assets);
void clear_active_assets_dirty(Assets& assets);

} // namespace test_child_asset_runtime
