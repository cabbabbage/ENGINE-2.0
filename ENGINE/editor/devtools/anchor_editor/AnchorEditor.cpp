#include "AnchorEditor.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

#include "assets/asset/Asset.hpp"
#include "devtools/asset_editor/animation_editor_window/PreviewProvider.hpp"
#include "core/manifest/manifest_loader.hpp"

namespace devmode::anchor_editor {

AnchorEditor::~AnchorEditor() {
    end();
}

void AnchorEditor::begin(const frame_editors::FrameEditorContext& context) {
    end();
    context_ = context;
    wants_close_ = false;
    rebuild_triggered_ = false;
    editor_finished_.store(false);
    launch_python_editor();
}

void AnchorEditor::end() {}

bool AnchorEditor::handle_event(const SDL_Event&) {
    return false;
}

void AnchorEditor::update(const Input&, float) {
    if (!editor_finished_.load() || rebuild_triggered_) {
        return;
    }

    rebuild_triggered_ = true;
    if (context_.target) {
        context_.target->rebuild_animation_runtime();
    }
    if (context_.preview) {
        context_.preview->invalidate(context_.animation_id);
    }
    if (context_.on_end) {
        context_.on_end();
    }
    wants_close_ = true;
}

void AnchorEditor::render_world(SDL_Renderer*) const {}

void AnchorEditor::render_overlays(SDL_Renderer*) const {}

void AnchorEditor::persist_pending_changes() {}

std::string AnchorEditor::quote_arg(const std::string& value) {
    std::string escaped = "\"";
    for (char ch : value) {
        if (ch == '\\' || ch == '\"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    escaped.push_back('\"');
    return escaped;
}

void AnchorEditor::launch_python_editor() {
    const std::string animation_id = context_.animation_id;
    const std::string manifest_path = manifest::manifest_path();
    const std::string asset_name =
        (context_.target && context_.target->info) ? context_.target->info->name : std::string{};

    std::thread([this, manifest_path, asset_name, animation_id]() {
        namespace fs = std::filesystem;

        fs::path script_path = fs::current_path() / "scripts" / "anchor_point_editor.py";
        if (!fs::exists(script_path)) {
            std::cerr << "[AnchorEditor] Missing Python anchor editor at " << script_path << "\n";
            editor_finished_.store(true);
            return;
        }

        std::string command = "python " + quote_arg(script_path.string()) +
                              " --manifest " + quote_arg(manifest_path) +
                              " --asset " + quote_arg(asset_name) +
                              " --animation " + quote_arg(animation_id);

        const int rc = std::system(command.c_str());
        if (rc != 0) {
            std::cerr << "[AnchorEditor] anchor_point_editor.py exited with code " << rc << "\n";
        }
        editor_finished_.store(true);
    }).detach();
}

}  // namespace devmode::anchor_editor
