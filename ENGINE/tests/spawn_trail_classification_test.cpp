#include <cassert>
#include <string>

#include "gameplay/spawn/trail_classification.hpp"

int main() {
    using dynamic_spawn::is_trail_area_label;

    assert(is_trail_area_label("trail"));
    assert(is_trail_area_label("MainPath"));
    assert(is_trail_area_label("Scenic TRAIL segment"));

    assert(!is_trail_area_label("room"));
    assert(!is_trail_area_label("waterfall"));
    assert(!is_trail_area_label(std::string{}));
}
