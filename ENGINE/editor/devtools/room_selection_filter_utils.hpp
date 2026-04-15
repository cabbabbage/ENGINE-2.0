#pragma once

namespace devmode::room_selection_filter {

enum class SelectionFilter {
    All,
    Normal,
    Tiled,
    Boundary,
    Anchored,
};

enum class SpawnOwnership {
    Room,
    MapBoundary,
    Other,
};

struct SelectionTraits {
    bool is_tiled = false;
    bool is_anchored = false;
    SpawnOwnership ownership = SpawnOwnership::Other;
};

SelectionFilter effective_filter(SelectionFilter filter, bool shift_down);
bool matches_filter(SelectionFilter filter, bool shift_down, const SelectionTraits& traits);

}  // namespace devmode::room_selection_filter

