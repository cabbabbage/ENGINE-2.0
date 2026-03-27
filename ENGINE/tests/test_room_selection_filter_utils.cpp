#include <doctest/doctest.h>

#include "devtools/room_selection_filter_utils.hpp"

namespace {

using devmode::room_selection_filter::SelectionFilter;
using devmode::room_selection_filter::SelectionTraits;
using devmode::room_selection_filter::SpawnOwnership;
using devmode::room_selection_filter::effective_filter;
using devmode::room_selection_filter::matches_filter;

}  // namespace

TEST_CASE("Room selection filter keeps explicit mode while shift is held") {
    CHECK(effective_filter(SelectionFilter::Normal, true) == SelectionFilter::Normal);
    CHECK(effective_filter(SelectionFilter::Boundary, true) == SelectionFilter::Boundary);
    CHECK(effective_filter(SelectionFilter::Normal, false) == SelectionFilter::Normal);
}

TEST_CASE("Room selection filter gates map-wide and boundary domains by mode") {
    SelectionTraits map_asset{};
    map_asset.ownership = SpawnOwnership::MapAssets;

    SelectionTraits boundary_asset{};
    boundary_asset.ownership = SpawnOwnership::MapBoundary;

    CHECK(matches_filter(SelectionFilter::MapWide, false, map_asset));
    CHECK_FALSE(matches_filter(SelectionFilter::Normal, false, map_asset));
    CHECK_FALSE(matches_filter(SelectionFilter::Boundary, false, map_asset));

    CHECK(matches_filter(SelectionFilter::Boundary, false, boundary_asset));
    CHECK_FALSE(matches_filter(SelectionFilter::Normal, false, boundary_asset));
    CHECK_FALSE(matches_filter(SelectionFilter::MapWide, false, boundary_asset));
}

TEST_CASE("Room selection filter keeps anchored/tiled assets out of normal mode") {
    SelectionTraits anchored{};
    anchored.is_anchored = true;
    anchored.ownership = SpawnOwnership::Room;

    SelectionTraits tiled{};
    tiled.is_tiled = true;
    tiled.ownership = SpawnOwnership::Room;

    CHECK(matches_filter(SelectionFilter::Anchored, false, anchored));
    CHECK_FALSE(matches_filter(SelectionFilter::Normal, false, anchored));

    CHECK(matches_filter(SelectionFilter::Tiled, false, tiled));
    CHECK_FALSE(matches_filter(SelectionFilter::Normal, false, tiled));
}
