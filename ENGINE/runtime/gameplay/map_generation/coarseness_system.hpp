#pragma once

#include <vector>

class Room;

namespace vibble::mapgen::coarseness {

void apply_coarseness_expansion(std::vector<Room*>& rooms);

}

