# Implementation Plan for Immediate Frame Editor Saving

[Overview]
Ensure frame data changes are immediately persisted to JSON files when edited in any frame editor mode.

This implementation adds a dedicated immediate save mechanism to the AnimationDocument class that bypasses the dirty flag system, ensuring that critical frame data changes made in frame editors (Movement, SyncChildren, AsyncChildren, AttackGeometry, HitGeometry) are immediately written to disk without waiting for explicit save actions or editor session termination.

[Types]
Add immediate save method signatures and error handling types.

1. **AnimationDocument Class Extensions**:
   - `bool save_animation_payload_immediately(const std::string& animation_id, const nlohmann::json& payload)` - New method that updates payload and immediately persists to disk
   - `bool persist_current_state()` - Helper method that saves the current document state to disk immediately

2. **ManifestTransaction Class Extensions**:
   - `set_immediate_persist(bool immediate)` - Method to flag transactions that require immediate persistence
   - `bool immediate_persist_ = false` - New member variable to track immediate persist requirement

3. **Error Handling Types**:
   - `enum class SaveResult { Success, Failure, NoChanges }` - Return status for save operations
   - `struct SaveErrorInfo { std::string message; std::string animation_id; }` - Error information structure

[Files]
Modify core document and transaction classes, then update all frame editors to use immediate saving.

**New Files to Create**:
- None required - all changes will be to existing files

**Files to Modify**:
- `ENGINE/editor/devtools/asset_editor/animation_editor_window/AnimationDocument.hpp` - Add immediate save method declarations
- `ENGINE/editor/devtools/asset_editor/animation_editor_window/AnimationDocument.cpp` - Implement immediate save methods
- `ENGINE/editor/devtools/frame_editors/shared/ManifestTransaction.hpp` - Add immediate persist support
- `ENGINE/editor/devtools/frame_editors/MovementFrameEditor.cpp` - Use immediate save for movement frame changes
- `ENGINE/editor/devtools/frame_editors/SyncChildrenFrameEditor.cpp` - Use immediate save for sync child timeline changes
- `ENGINE/editor/devtools/frame_editors/AsyncChildrenFrameEditor.cpp` - Use immediate save for async child timeline changes
- `ENGINE/editor/devtools/frame_editors/AttackGeoFrameEditor.cpp` - Use immediate save for attack geometry changes
- `ENGINE/editor/devtools/frame_editors/HitGeoFrameEditor.cpp` - Use immediate save for hit geometry changes

**Configuration Files**:
- None - no configuration changes needed

[Functions]
Add immediate save functionality and modify frame editors to use it.

**New Functions**:
- `AnimationDocument::save_animation_payload_immediately()` - Updates payload and immediately persists to disk
- `AnimationDocument::persist_current_state()` - Internal helper to write current state to disk
- `ManifestTransaction::set_immediate_persist()` - Flags transaction for immediate persistence

**Modified Functions**:
- `MovementFrameEditor::persist_changes()` - Modify to use immediate save instead of regular commit
- `SyncChildrenFrameEditor::ensure_manifest_transaction()` - Configure transaction for immediate persist
- `AsyncChildrenFrameEditor::persist_changes()` - Use immediate save for async child changes
- `AttackGeoFrameEditor::persist_changes()` - Use immediate save for attack geometry changes
- `HitGeoFrameEditor::persist_changes()` - Use immediate save for hit geometry changes
- `ManifestTransaction::commit()` - Add logic to call immediate save when flagged

**Removed Functions**:
- None - all existing functionality preserved

[Classes]
Extend existing classes with immediate save capabilities.

**Modified Classes**:
- `AnimationDocument` - Add immediate save methods and error handling
- `ManifestTransaction` - Add immediate persist flag and logic
- `MovementFrameEditor` - Use immediate save for frame changes
- `SyncChildrenFrameEditor` - Use immediate save for child timeline changes
- `AsyncChildrenFrameEditor` - Use immediate save for async child changes
- `AttackGeoFrameEditor` - Use immediate save for attack geometry changes
- `HitGeoFrameEditor` - Use immediate save for hit geometry changes

**New Classes**:
- None required

[Dependencies]
No new external dependencies required.

All implementation uses existing nlohmann::json, SDL, and filesystem dependencies already present in the codebase. No additional libraries or packages needed.

[Testing]
Comprehensive testing strategy for immediate save functionality.

**Test Requirements**:
- Verify movement frame changes save immediately to JSON
- Confirm sync child timeline changes persist immediately
- Test async child timeline immediate saving
- Validate attack geometry changes save immediately
- Ensure hit geometry changes persist immediately
- Test error handling for failed saves
- Verify existing functionality remains unchanged

**Test Files to Update**:
- `TESTS/test_frame_editor_async_payload.cpp` - Add immediate save tests
- Create new test file for immediate save functionality

[Implementation Order]
Sequential implementation steps to ensure clean integration.

1. Add immediate save methods to AnimationDocument class
2. Extend ManifestTransaction with immediate persist support
3. Update MovementFrameEditor to use immediate saving
4. Update SyncChildrenFrameEditor to use immediate saving
5. Update AsyncChildrenFrameEditor to use immediate saving
6. Update AttackGeoFrameEditor to use immediate saving
7. Update HitGeoFrameEditor to use immediate saving
8. Add comprehensive error handling
9. Create and run tests to verify functionality
10. Perform manual testing of all frame editor modes
