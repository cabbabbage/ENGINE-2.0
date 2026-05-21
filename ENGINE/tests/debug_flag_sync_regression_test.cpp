#include <cassert>

namespace {

struct RendererDebugFlags {
    bool movement = false;
    bool anchor = false;
    bool impass = false;
};

struct FakeAssetsManager;

struct FakeDevControls {
    explicit FakeDevControls(FakeAssetsManager* assets) : assets_(assets) {}

    void toggle_movement_from_ui(bool enabled);
    void toggle_anchor_from_ui(bool enabled);
    void toggle_impass_from_ui(bool enabled);
    void sync_from_assets();

    bool movement = false;
    bool anchor = false;
    bool impass = false;

    FakeAssetsManager* assets_ = nullptr;
};

struct FakeAssetsManager {
    void set_movement_debug_enabled(bool enabled, bool notify = true) {
        renderer_flags.movement = enabled;
        if (notify && dev_controls) dev_controls->sync_from_assets();
    }
    void set_anchor_debug_enabled(bool enabled, bool notify = true) {
        renderer_flags.anchor = enabled;
        if (notify && dev_controls) dev_controls->sync_from_assets();
    }
    void set_impass_debug_enabled(bool enabled, bool notify = true) {
        renderer_flags.impass = enabled;
        if (notify && dev_controls) dev_controls->sync_from_assets();
    }

    void handle_anchor_shortcut() { set_anchor_debug_enabled(!renderer_flags.anchor); }

    RendererDebugFlags renderer_flags{};
    FakeDevControls* dev_controls = nullptr;
};

void FakeDevControls::toggle_movement_from_ui(bool enabled) { assets_->set_movement_debug_enabled(enabled); }
void FakeDevControls::toggle_anchor_from_ui(bool enabled) { assets_->set_anchor_debug_enabled(enabled); }
void FakeDevControls::toggle_impass_from_ui(bool enabled) { assets_->set_impass_debug_enabled(enabled); }
void FakeDevControls::sync_from_assets() {
    movement = assets_->renderer_flags.movement;
    anchor = assets_->renderer_flags.anchor;
    impass = assets_->renderer_flags.impass;
}

}  // namespace

int main() {
    FakeAssetsManager assets;
    FakeDevControls controls(&assets);
    assets.dev_controls = &controls;

    controls.toggle_anchor_from_ui(true);
    controls.toggle_movement_from_ui(true);
    controls.toggle_impass_from_ui(true);
    assert(assets.renderer_flags.anchor && controls.anchor);
    assert(assets.renderer_flags.movement && controls.movement);
    assert(assets.renderer_flags.impass && controls.impass);

    assets.handle_anchor_shortcut();
    assert(!assets.renderer_flags.anchor);
    assert(!controls.anchor);

    controls.toggle_anchor_from_ui(true);
    assert(assets.renderer_flags.anchor == controls.anchor);
    assert(assets.renderer_flags.movement == controls.movement);
    assert(assets.renderer_flags.impass == controls.impass);
}
