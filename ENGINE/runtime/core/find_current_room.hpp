#pragma once

#include <vector>
#include <utility>

class Room;
class Asset;
class WarpedScreenGrid;

class CurrentRoomFinder {

	public:
    CurrentRoomFinder(std::vector<Room*>& rooms, Asset*& player);
    Room* getCurrentRoom() const;
    Room* getNeighboringRoom(Room* current) const;
    void setRooms(std::vector<Room*>& rooms);
    void setPlayer(Asset*& player);
    void setCamera(WarpedScreenGrid* camera);

        private:
    std::vector<Room*>* rooms_;
    Asset**             player_;
    WarpedScreenGrid*   camera_ = nullptr;
    mutable Room*       last_room_ = nullptr;
};
