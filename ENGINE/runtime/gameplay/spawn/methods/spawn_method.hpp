#pragma once

struct SpawnInfo;
class Area;
class SpawnContext;

// Single entry point for all spawn positioning strategies.
class SpawnMethod {
public:
    void spawn(const SpawnInfo& item, const Area* area, SpawnContext& ctx) const;
};
