#pragma once

#include <vector>
#include <memory>
#include <unordered_set>
#include <SDL3/SDL.h>

class Assets;
class Asset;
class Room;

class InitializeAssets {

	public:
    static void initialize(Assets& assets, int screen_width, int screen_height, int screen_center_x, int screen_center_z, int map_radius);

        private:
    static void find_player(Assets& assets);
};
