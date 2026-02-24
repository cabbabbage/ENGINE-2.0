#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <memory>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <string>

#include "devtools/core/dev_save_coordinator.hpp"

class Input;
class Room;
class SDL_Renderer;

class RoomConfigurator;
class TrailEditorSuite {
public:
    TrailEditorSuite();
    ~TrailEditorSuite();

    void set_screen_dimensions(int width, int height);

    void open(Room* trail);
    void close();
    bool is_open() const;

    void update(const Input& input);
    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;
    bool contains_point(int x, int y) const;

    Room* active_trail() const { return active_trail_; }

    void set_on_open_area(std::function<void(const std::string&, const std::string&)> cb,
                          std::string stack_key = {});
    void set_save_coordinator(devmode::core::DevSaveCoordinator* coordinator) { save_coordinator_ = coordinator; }

private:
    void ensure_ui();
    void update_bounds();
    void delete_spawn_group(const std::string& id);
    void add_spawn_group();
    void reorder_spawn_group(const std::string& id, size_t new_index);
    nlohmann::json* find_spawn_entry(const std::string& id);
    bool enqueue_active_trail_save(devmode::core::DevSaveCoordinator::Priority priority);

    int screen_w_ = 0;
    int screen_h_ = 0;
    SDL_Rect config_bounds_{0, 0, 0, 0};

    Room* active_trail_ = nullptr;
    std::unique_ptr<RoomConfigurator> configurator_;
    std::function<void(const std::string&, const std::string&)> on_open_area_{};
    std::string open_area_stack_key_{};
    devmode::core::DevSaveCoordinator* save_coordinator_ = nullptr;
};


