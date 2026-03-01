# Map Save Overhaul Plan

## Objectives
- Route all map manifest and asset persistence through a single SaveManager with explicit ordering and atomic writes.
- Keep ManifestStore as the low-level writer, invoked only by SaveManager batch save; no Dev Mode UI callback may hit disk directly.
- Move room, trail, boundary, fog, terrain, and map-asset edits to in-memory models with dirty tracking, saved only in batches on exit or explicit save.
- Preserve data fidelity by round-tripping unknown manifest JSON while introducing typed MapData ownership.
- Eliminate legacy enqueue/persist/flush and cache rebuild scanners; batch-on-exit becomes the only Dev Mode save path.

## Principles and Guardrails
- No disk I/O inside edit callbacks except the Python Frame Editor explicit exception; enforce via asserts/logging.
- Manifest writes are atomic: write to temp, fsync, then rename; treat partial failures as non-destructive.
- Cache is read/generate-only during load; cache writes occur only during batch save and only for dirty assets.
- Dirty propagation must bubble from room/trail/boundary/fog/terrain/asset edits up to MapData so the owning map entry is marked dirty.
- MapData must preserve and re-emit any unknown JSON sections to avoid data loss during migration.

## Work Plan

### 1) Inventory and Guardrails
- [~] Audit all call sites of enqueue_map_entry, persist_map_manifest_entry, manifest_store_->flush(), save_assets_json, persist_map_info_to_disk, save_bundle/cache save loops across editor/runtime; document per file/function. *(Runtime Room, AssetsManager, MainApp, AssetLibraryUI updated; editor UI still pending audit.)*
- [x] Add temporary logging/assert in ManifestStore and cache write paths to flag disk writes made outside SaveManager; gate Dev Mode edit callbacks against these paths. *(ManifestStore now tracks guarded writes, emits warnings, and exposes telemetry; cache paths still pending.)*
- [~] Add metrics counters for cache miss generation and attempted direct manifest writes to ease regression detection. *(Telemetry for unguarded/asset/map writes added; cache miss metrics still TODO.)*

### 2) SaveManager Architecture and Batch Pipeline
- [x] Finalize SaveManager interface under devtools/core: owns ordered list of ISaveable objects, tracks dirty state, and exposes register_saveable/unregister_saveable/mark_dirty/save_all. *(Ordered stages + guard-scoped saves implemented.)*
- [ ] Define batch save sequence: gather dirty MapData and assets, write manifest entries via ManifestStore to temp, fsync, rename to manifest.json, then write asset caches for items whose manifest save succeeded, finally clear dirty flags.
- [~] Keep ManifestStore unchanged as low-level writer but only invoked by SaveManager; remove any other callers. *(DevSaveCoordinator now uses ManifestStore directly with SaveManager guard; legacy callers remain.)*
- [ ] Expose a DevSaveCoordinator wrapper to orchestrate save on app exit/explicit save, delegating to SaveManager only.
- [x] Add loud logging for skipped or failed steps and include map_id/asset_id context for debugging. *(Guard violations now log with context.)*

### 3) MapData Model Introduction
- [ ] Create MapData (shared runtime/editor) with typed ownership of layers/settings, rooms metadata/order/layer refs, trails, map-wide assets, boundary data, fog settings, terrain/grid settings, dev map settings.
- [ ] Implement MapData::from_manifest_entry/to_manifest_entry adapters that round-trip unknown JSON by storing an extras blob merged back on save.
- [~] Add MapData dirty flag and APIs mark_dirty/is_dirty/save_self_to_manifest; ensure child objects signal MapData on mutation. *(Map-level dirty tracking added via DevControls; room/trail mark_dirty feeds SaveManager, but MapData type still absent.)*
- [~] Replace direct mutations of Assets::map_info_json or manifest blobs in MapModeUI, MapLayersController, DevControls, RoomEditor, trail/room configurators with MapData API calls. *(UI/editors now mutate in-memory JSON and mark dirty; no direct disk writes.)*
- [~] Register MapData with SaveManager so each dirty map entry is composed and written in the batch. *(SaveManager now owns map saveable via DevControls map_dirty flag.)*

### 4) Room and Trail Migration
- Add mark_dirty/is_dirty/save_self_to_manifest methods to Room (and trail instances if represented as Room); ensure save_self_to_manifest writes only into the provided manifest JSON object.
- Remove Room::push_payload side effects; editing mutates in-memory fields/JSON only, no immediate persistence.
- Convert room/trail persistence from push-on-change to object-owned save; all editors call mark_dirty instead of persistence functions.
- Replace save_assets_json call sites in RoomEditor and TrailEditorSuite with mark_dirty; register room/trail objects with SaveManager for ordered save.
- Ensure map-wide editor callbacks mutate runtime model and call mark_dirty without disk I/O; preserve live visual refresh (regen/invalidate) decoupled from persistence.
- [~] Partial: Room now exposes mark_dirty/is_dirty, save_assets_json is in-memory only, editors (RoomEditor/TrailEditorSuite/RoomConfigurator) now mark map dirty via callbacks; SaveManager batches map writes.

### 5) AssetInfo and Cache Handling
- Add dirty field plus mark_dirty, save_self_to_manifest, save_self_to_cache_if_dirty to AssetInfo; cache writes occur only after manifest save succeeds.
- Update asset-edit UI paths in asset_info_ui, map_assets_modals, dev_controls, map_mode_ui to mutate AssetInfo in memory and call mark_dirty.
- Refactor PrimaryAssetCache::load_or_build to avoid save_bundle on cache miss/stale hash; return generated data in memory and rely on SaveManager batch to persist.
- Enforce read/generate-only cache behavior during load; writes only during batch save via SaveManager.
- Remove blanket cache save loop in MainApp destructor; SaveManager handles dirty asset cache writes.
- Add logging/metrics for cache miss generation to aid debugging after removing scanners/comparison code.

### 6) Dev UI and Runtime Integration
- Replace manifest writes in MapModeUI fog/terrain/layer callbacks with dirty-mark callbacks on MapData/rooms/trails; keep visual refresh calls intact.
- In DevControls modals (map_assets_modal_, boundary_assets_modal_), replace persist_map_info_to_disk() with MapData::mark_dirty and in-memory mutation.
- [~] Remove direct calls to persist_map_manifest_entry/enqueue_map_entry from runtime/editor systems; route through SaveManager registration and dirty tracking. *(Runtime Room, AssetsManager, MainApp, DevSaveCoordinator now use ManifestStore with guards; editor UIs now mark dirty and batch via SaveManager.)*
- Ensure boundary/fog/terrain/map-asset edits mark both the local object and MapData dirty because MapData owns the embedded map entry.
- Adjust dev_save_coordinator and dev_save_coordinator.hpp to delegate to SaveManager and drop ad-hoc manifest_store_->flush usage.
- Keep Python Frame Editor immediate save as explicit exception; document and isolate to prevent unintended batch overwrite.

### 7) Exception Handling for Python Frame Editor
- Preserve immediate-write behavior for the Python Frame Editor, but guard batch save so it merges the latest on-disk manifest or uses a version stamp check to avoid overwriting newer frames.
- Add targeted tests to ensure Frame Editor changes persist immediately and survive subsequent batch saves without loss.
- Make the exception path explicit in SaveManager (e.g., whitelist) with loud logging when exercised.

### 8) Cleanup, Tests, and Validation
- Delete or hard-disable remaining calls to enqueue_map_entry, persist_map_manifest_entry, ad-hoc manifest_store_->flush, and any direct manifest/cache writes outside SaveManager; remove legacy cache rebuild scanners and manifest vs cache comparison code.
- Add assertions or compile-time errors to prevent introducing new direct manifest/cache writes in Dev Mode code paths.
- Implement automated tests: no disk writes during edit callbacks; batch save on quit persists edits to assets, rooms, trails, fog, terrain, boundary, and map settings; unknown JSON sections round-trip; dirty flags clear after successful batch.
- Add a manual validation checklist: edit various objects, confirm zero disk writes while editing, quit and relaunch to verify persistence across all map components, verify metrics/logging for cache miss generation.
- Update developer docs/readme for the new SaveManager workflow, MapData API, and prohibited direct persistence calls.
- Final hardening pass: ensure batch-on-exit is the only C++ Dev Mode saving model and that atomic manifest writes plus ordered cache saves are enforced.
