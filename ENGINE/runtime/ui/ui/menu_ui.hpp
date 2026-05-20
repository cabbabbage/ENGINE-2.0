#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>

#include "app/main.hpp"
#include "styles.hpp"
#include "button.hpp"

class MenuUI : public MainApp {

    public:
    enum class MenuAction {
    NONE = 0,
    EXIT,
    SETTINGS,
    QUIT
};
    MenuUI(EngineRenderer* renderer, int screen_w, int screen_h, MapDescriptor map, LoadingScreen* loading_screen = nullptr, AssetLibrary* asset_library = nullptr, SDL_Window* window = nullptr);
    ~MenuUI();
    void init();
    bool wants_return_to_main_menu() const;

	private:
    void game_loop();
    void toggleMenu();
    void openMenu();
    void closeMenu();
    void exitDevModeForPause();
    void handle_event(const SDL_Event& e);
    void render();
    MenuAction consumeAction();
    void rebuildButtons();
    SDL_Point measureText(const LabelStyle& style, const std::string& s) const;
    void blitText(SDL_Renderer* r, const LabelStyle& style, const std::string& s, int x, int y, bool shadow, SDL_Color override_col) const;
    void blitTextCentered(SDL_Renderer* r, const LabelStyle& style, const std::string& s, const SDL_Rect& rect, bool shadow, SDL_Color override_col) const;
    SDL_Texture* loadTexture(const std::string& abs_utf8_path);
    SDL_Texture* loadTexture(const std::filesystem::path& p);
    std::filesystem::path firstImageIn(const std::filesystem::path& folder) const;
    SDL_Rect coverDst(SDL_Texture* tex) const;
    SDL_Rect fitCenter(SDL_Texture* tex, int max_w, int max_h, int cx, int cy) const;
    std::string pickRandomLine(const std::filesystem::path& csv_path) const;
    void drawVignette(Uint8 alpha) const;
    bool run_exit_save_sequence(const std::string& reason);
    void doExit();
    void doSettings();
    void doQuit();
    void doToggleDevMode();

	private:
    struct MenuButton {
    Button     button;
    MenuAction action = MenuAction::NONE;
};
    struct MenuPacketStructuralKey {
    std::uint64_t layout_version = 0;
    std::uint64_t theme_version = 0;
    std::uint32_t viewport_bucket = 0;
    std::uint32_t feature_flags = 0;
    std::uint32_t material_variant_id = 0;

    bool operator==(const MenuPacketStructuralKey& other) const;
};
    struct MenuDrawCommand {
    enum class Kind {
        BackgroundFill,
        Vignette,
        TitleText,
        Button
    };
    Kind kind = Kind::BackgroundFill;
    std::size_t button_index = 0;
    SDL_Rect rect{0, 0, 0, 0};
    std::string text{};
    Uint8 alpha = 0;
};
    struct MenuStructuralPacket {
    MenuPacketStructuralKey key{};
    std::vector<MenuDrawCommand> commands{};
    std::uint64_t creation_frame = 0;
    std::uint64_t last_used_frame = 0;
    std::size_t estimated_memory_cost = 0;
    bool valid = true;
};
    struct MenuPacketCacheStats {
    std::uint64_t cache_hit_count = 0;
    std::uint64_t cache_miss_count = 0;
    std::uint64_t structural_rebuild_count = 0;
    std::uint64_t reused_packet_count = 0;
    double structural_build_ms_total = 0.0;
    double reuse_patch_ms_total = 0.0;
};
    void invalidate_packet_cache();
    void invalidate_packet_cache_entry(const MenuPacketStructuralKey& key);
    MenuPacketStructuralKey compute_packet_structural_key() const;
    std::vector<MenuDrawCommand> build_structural_commands() const;
    std::size_t estimate_command_memory_cost(const std::vector<MenuDrawCommand>& commands) const;
    void trim_packet_cache_if_needed();
    void emit_packet_cache_metrics() const;

    bool menu_active_ = false;
    MenuAction last_action_ = MenuAction::NONE;
    bool return_to_main_menu_ = false;
    SDL_Texture* background_tex_ = nullptr;
    std::vector<MenuButton> buttons_;
    std::unordered_map<std::size_t, MenuStructuralPacket> packet_cache_{};
    std::vector<MenuDrawCommand> dynamic_packet_commands_{};
    std::uint64_t packet_layout_version_ = 0;
    std::uint64_t packet_theme_version_ = 1;
    std::uint64_t packet_frame_index_ = 0;
    std::size_t packet_cache_memory_footprint_ = 0;
    std::size_t packet_cache_max_entries_ = 8;
    std::size_t packet_cache_max_memory_bytes_ = 256 * 1024;
    MenuPacketCacheStats packet_cache_stats_{};
};
