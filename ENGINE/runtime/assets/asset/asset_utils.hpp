#pragma once

#include "Asset.hpp"

inline void set_camera_recursive(Asset* asset, WarpedScreenGrid* v) {
        if (!asset) return;
        asset->set_camera(v);
}

inline void set_assets_owner_recursive(Asset* asset, Assets* owner) {
        if (!asset) return;
        asset->set_assets(owner);
}
