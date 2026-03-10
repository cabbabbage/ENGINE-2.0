#pragma once

#include <vector>
#include <SDL3/SDL.h>

class Asset;

class Range {
 public:

  static bool is_in_range(const Asset* a, const Asset* b, int radius);
  static bool is_in_range(const Asset* a, const SDL_Point& b, int radius);
  static bool is_in_range(const SDL_Point& a, const Asset* b, int radius);
  static bool is_in_range(const SDL_Point& a, const SDL_Point& b, int radius);

  static double get_distance(const Asset* a, const Asset* b);
  static double get_distance(const Asset* a, const SDL_Point& b);
  static double get_distance(const SDL_Point& a, const Asset* b);
  static double get_distance(const SDL_Point& a, const SDL_Point& b);

  static long long distance_sq(const Asset* a, const Asset* b);
  static long long distance_sq(const Asset* a, const SDL_Point& b);
  static long long distance_sq(const SDL_Point& a, const Asset* b);
  static long long distance_sq(const SDL_Point& a, const SDL_Point& b);

  static void get_in_range(const SDL_Point& center, int radius, const std::vector<Asset*>& candidates, std::vector<Asset*>& out);

 private:

  static bool plane_coords(const Asset* a, double& right, double& depth);
  static bool plane_coords(const SDL_Point& p, double& right, double& depth);

  static bool in_range_plane(double left_right, double left_depth, double right_right, double right_depth, int radius);
  static double distance_plane(double left_right, double left_depth, double right_right, double right_depth);
};
