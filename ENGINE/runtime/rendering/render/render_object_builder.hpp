#pragma once

class Asset;
struct RenderObject;

namespace render_build {

bool build_direct_asset_render_object(Asset* asset, RenderObject& out_object);

}
