# Spawn Group Mutation Regression Scenarios

These scenarios validate batched spawn-group deletion behavior while editor interactions are active.

## 1) Rapid delete/recreate while dragging
1. Open Room Editor and select multiple assets from the same spawn group.
2. Begin dragging one selected asset continuously.
3. While drag is active, delete the spawn group and immediately recreate/regenerate it repeatedly.
4. Verify no crash/UAF occurs and drag state clears without stale pointers.

## 2) Hover/selection stress during map-wide spawn group edits
1. Open map asset spawn group panel.
2. Continuously move cursor across dense assets to force hover changes.
3. Repeatedly edit and apply spawn group settings that trigger regeneration/removal.
4. Verify hover/selection does not reference deleted assets and resumes after commit.

## 3) Save failure safety
1. Force a save failure (e.g. lock room/map manifest write target).
2. Delete a spawn group.
3. Verify deletion is canceled, no world mutation is applied, and user gets a save-failed notice.

## 4) Single refresh invariant
1. Select and hover assets in the target spawn group.
2. Delete the spawn group once.
3. Verify editor selection/hover sync refresh happens once after rebuild (no per-asset flicker).
