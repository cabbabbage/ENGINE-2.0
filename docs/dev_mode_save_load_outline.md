# Dev Mode save/load structure (assets, rooms, maps)

## Schema upgrade note

- Manifest JSON now enforces version 2 and each map blob must carry `schema_version`: older files that omit `schema_version` or still hard-code the deprecated `dy` field will be rejected to keep the runtime/editor coordinate system unambiguous (height is always Y, planar offsets use `dz`).
- Because of the strict validation and coordinate rename, there is no automated migration. You must re-save the affected maps/assets through Dev Mode (open them in the editor and save) so that new entries emit `schema_version = 1` plus the `dz` offsets. Treat this as a one-time “recreate content” requirement whenever you pull this change.

## 1) Asset edits (Asset Info / animation editor)

- **Load (authoring data):**
  - Asset metadata is loaded from `manifest.json` via `manifest::load_manifest()` and cached by `ManifestStore`.
  - `ManifestStore` treats the `assets` object in `manifest.json` as the editable asset source.
- **Save:**
  - Dev Mode enqueues asset updates through `DevSaveCoordinator::enqueue_manifest_asset(...)`.
  - The coordinator applies edits through `ManifestStore::begin_asset_edit(...)->commit()`, which updates `manifest_cache_["assets"][name]` and submits a write to `manifest.json` via `DevJsonStore`.
  - `manifest_store.flush()` (or coordinator flush) forces pending debounced writes to disk.
- **Source of truth:**
  - **Authoritative:** `manifest.json -> assets`.
  - **Generated/cache sidecar:** `PrimaryAssetCache::save_current(...)` is also triggered after asset saves (bundle rebuild). This is cache/output for runtime performance, not the canonical authoring source.
- **Runtime vs disk:**
  - Runtime works on in-memory asset/editor state + `ManifestStore` cache.
  - Disk persistence is batched/debounced by `DevJsonStore` (temp-file + rename write path).

## 2) Room editor (room asset placement/config)

- **Load:**
  - Room data is loaded from the map manifest blob (`map_info_json` / map entry), specifically `rooms_data`.
  - `Room::build_room_payload_for_save()` resolves payload from `map_info_root_` (in-memory map JSON) or fallback manifest map entry, then targets `payload[data_section_][room_name]`.
- **Save:**
  - Room editor uses `RoomEditor::enqueue_current_room_save(...)`.
  - Save path writes a full updated map payload through `Room::apply_room_payload_for_save(...)`.
  - With manifest store present, this calls `persist_map_manifest_entry(...)` => `ManifestStore::update_map_entry(map_id, payload)` => `manifest.json` (`maps[map_id]`).
- **Source of truth:**
  - **Authoritative:** `manifest.json -> maps[map_id] -> rooms_data` (room entries embedded in map entry).
  - No per-room authoring files are written in this flow.
- **Runtime vs disk:**
  - Runtime edits mutate room-local `assets_json` and in-memory map blob pointers.
  - Persisting room edits writes back the entire map entry payload to manifest (debounced/immediate depending on coordinator priority).

## 3) Map editor / Map Mode (layers, boundaries, map assets, fog/terrain, room list ops)

- **Load:**
  - Game startup loads `manifest.json`, reads `maps[map_id]` in `MainApp`, and passes it to `AssetLoader`/`Assets` as map manifest/map_info.
  - `AssetLoader::load_from_manifest(...)` hydrates/normalizes sections (`map_assets_data`, `map_boundary_data`, `rooms_data`, `trails_data`, etc.).
- **Save:**
  - `MapModeUI::save_map_info_to_disk(...)` enqueues/saves map entry using `enqueue_map_entry(...)` (or direct `persist_map_manifest_entry(...)`).
  - This updates `manifest.json -> maps[map_id]` with the current full map payload.
- **Source of truth:**
  - **Authoritative:** `manifest.json -> maps[map_id]` (single map blob including rooms/layers/assets/boundaries/trails/settings).
  - No separate map/room file set is authoritative in current flow.
- **Runtime vs disk:**
  - Runtime map editing happens on `Assets::map_info_json_` (shared to Dev Mode UI).
  - Saves push current in-memory map blob back to manifest via `ManifestStore`.

## 4) Cache / generated file effects

- `DevJsonStore` is a write-through/debounced JSON persistence layer with in-memory digest cache and pending-write queue; it can delay writes briefly but ultimately targets canonical files (e.g., `manifest.json`).
- Asset bundle cache rebuild (`PrimaryAssetCache::save_current`) occurs after asset edits and animation saves; this can affect runtime loaded bundles, but not the canonical manifest asset definitions.
- No separate room-per-file cache output is used as source of truth in these Dev Mode save paths.

## Practical authoritative model (recommended)

- **Assets:** `manifest.json/assets/*`
- **Rooms + map structure + map assets/boundary/trails/settings:** `manifest.json/maps/<map_id>`
- **Caches/generated outputs:** treat as derived artifacts; regenerate from manifest-backed authoring data when needed.
