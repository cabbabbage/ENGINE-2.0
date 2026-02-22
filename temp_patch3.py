from pathlib import Path
path = Path('ENGINE/runtime/rendering/render/dynamic_boundary_system.cpp')
text = path.read_text()
replacements = [
    ('if (region_entry.region_kind != world::GridPoint::RegionKind::Boundary || region_entry.blocked)', 'if (region_entry.region_kind != RegionKind::Boundary || region_entry.blocked)'),
    ('gp->region_kind = region_entry.region_kind;', 'gp->region_kind = to_world_region_kind(region_entry.region_kind);'),
    ('entry.region_kind = world::GridPoint::RegionKind::Boundary;', 'entry.region_kind = RegionKind::Boundary;'),
    ('const auto match_area = [&](const Area* area, world::GridPoint::RegionKind kind, const Room* room) -> bool {', 'const auto match_area = [&](const Area* area, RegionKind kind, const Room* room) -> bool {'),
    ('if (match_area(room->room_area.get(), world::GridPoint::RegionKind::Room, room)) {', 'if (match_area(room->room_area.get(), RegionKind::Room, room)) {'),
    ('if (match_area(named.area.get(), world::GridPoint::RegionKind::Trail, room)) {', 'if (match_area(named.area.get(), RegionKind::Trail, room)) {'),
]
for old, new in replacements:
    if old not in text:
        raise SystemExit(f'Missing pattern: {old}')
    text = text.replace(old, new, 1)
marker = '}
}
DynamicBoundarySystem::DynamicBoundarySystem() = default;'
helper = '''
world::GridPoint::RegionKind to_world_region_kind(DynamicBoundarySystem::RegionKind kind) {
    switch (kind) {
    case DynamicBoundarySystem::RegionKind::Room:
        return world::GridPoint::RegionKind::Room;
    case DynamicBoundarySystem::RegionKind::Trail:
        return world::GridPoint::RegionKind::Trail;
    default:
        return world::GridPoint::RegionKind::Boundary;
    }
}
'''
if marker not in text:
    raise SystemExit('Marker not found for helper insertion')
text = text.replace(marker, '}
' + helper + marker.split('}
}
')[2], 1)
path.write_text(text)
