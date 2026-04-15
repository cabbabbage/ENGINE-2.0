#include "devtools/room_selection_filter_utils.hpp"

namespace devmode::room_selection_filter {

SelectionFilter effective_filter(SelectionFilter filter, bool shift_down) {
    (void)shift_down;
    return filter;
}

bool matches_filter(SelectionFilter filter, bool shift_down, const SelectionTraits& traits) {
    const SelectionFilter effective = effective_filter(filter, shift_down);

    const bool is_boundary_domain = traits.ownership == SpawnOwnership::MapBoundary;

    if (effective != SelectionFilter::Anchored &&
        effective != SelectionFilter::All &&
        traits.is_anchored) {
        return false;
    }

    switch (effective) {
        case SelectionFilter::All:
            return true;
        case SelectionFilter::Normal:
            return !traits.is_tiled &&
                   !traits.is_anchored &&
                   !is_boundary_domain;
        case SelectionFilter::Tiled:
            return traits.is_tiled &&
                   !is_boundary_domain;
        case SelectionFilter::Boundary:
            return is_boundary_domain;
        case SelectionFilter::Anchored:
            return traits.is_anchored &&
                   !is_boundary_domain;
        default:
            return false;
    }
}

}  // namespace devmode::room_selection_filter
