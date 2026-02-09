#pragma once

#include <SDL3/SDL.h>
#include <memory>
#include <string>
#include <unordered_set>

#include <nlohmann/json.hpp>

struct MapDescriptor {
    std::string    id;
    nlohmann::json data;
};

class Assets;
class SceneRenderer;
class AssetLoader;
class Input;
class LoadingScreen;
class AssetLibrary;
class EngineRenderer;

class MainApp {

        public:
    MainApp(MapDescriptor map, EngineRenderer* renderer, int screen_w, int screen_h, LoadingScreen* loading_screen = nullptr, AssetLibrary* asset_library = nullptr);
    virtual ~MainApp();
    virtual void init();
    virtual void game_loop();
    virtual void setup();
    SDL_Renderer* raw_renderer() const;
	protected:
	protected:
    MapDescriptor map_descriptor_;
    std::string   map_path_;
    EngineRenderer* renderer_   = nullptr;
    int           screen_w_   = 0;
    int           screen_h_   = 0;
    std::unique_ptr<AssetLoader> loader_;
    Assets*        game_assets_      = nullptr;
    SceneRenderer* scene_            = nullptr;
    Input*         input_            = nullptr;
    SDL_Texture* overlay_texture_    = nullptr;
    bool dev_mode_ = false;
    LoadingScreen* loading_screen_   = nullptr;
    AssetLibrary*  asset_library_    = nullptr;
};

void run(SDL_Window* window, EngineRenderer& renderer, int screen_w, int screen_h, bool rebuild_cache);
