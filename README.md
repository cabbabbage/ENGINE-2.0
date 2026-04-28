# VIBBLE - מנוע משחק דו-ממדי (Departed Affairs and Co.)


## התקנה
### התחלה מהירה (Windows)
1. שכפל את המאגר.
2. הרץ `setup.bat` משורש הפרויקט אם ברצונך לבנות ולהדר.

הסקריפט מתקין כלי בנייה (Git, כלי הבנייה של MSVC, CMake, Ninja, vcpkg), מושך תלויות, מגדיר בניית RelWithDebInfo, מהדר ומפעיל את המנוע. נדרשים Windows 10/11 וחיבור לאינטרנט, ומומלצות הרשאות מנהל לצורך התקנת הכלים.


## סקירה כללית
- מנוע דו-ממדי מבוסס SDL3; התוכן נשמר בקבצים חיצוניים מונעי-JSON עבור מפות, נכסים, תאורה ואנימציות.
- טעינה מונעת-manifest שומרת את נתוני המשחק מחוץ לקובץ ההפעלה כדי ששינויי נכסים לא ידרשו הידור מחדש.
- מצב פיתוח מובנה בתוך המנוע לעריכת חדרים, תאורה, נכסים ונקודות זימון, עם כתיבה מיידית חזרה לקובצי התוכן.
- כלי הנכסים מייצרים מחדש מטמוני אנימציה ותאורה לפי בקשה מפורשת ותיקון קבצים חסרים.
- חוזה דגל ההפעלה של manifest הנכסים וסכמת floor-box: `ENGINE/runtime/core/manifest/ASSET_MANIFEST_SCHEMA.md`.

## הרצה
- מומלץ: להריץ `run.bat` כדי להגדיר, לבנות ולהפעיל את המנוע.
- כדי לבנות מאפס הרץ `compile_and_run.bat`; הוא מפעיל את `release\engine.exe` שנוצר בתהליך הבנייה.
- הרצות חוזרות משתמשות בבנייה שהוגדרה; הרץ שוב `compile_and_run.bat` אחרי משיכת שינויי תלויות.

## מצב פיתוח
- החלפה עם `Ctrl+D` או דרך תפריט ההשהיה (`Esc`), או הפעלה אוטומטית כשהמפה ללא שחקן.
- מפחית איכות רינדור לשיפור תגובתיות וחושף עורכים למפות, נכסים, תאורה ונקודות זימון. ההגדרות נשמרות ב-`dev_mode_settings.json`.

## בקרים מותאמים אישית
- הוסף בקרים חדשים תחת `ENGINE/runtime/animation/controllers/custom_controllers/` ורשום אותם ב-`ControllerFactory::create_by_key`.
- הבקרים מקושרים לפי שם נכס (לדוגמה, הנכס `spider` ממופה ל-`spider_controller`).

## בדיקות
- בניית בדיקות: `cmake --build --preset windows-vcpkg-release --target engine_tests`
- הרצת בדיקות: `ENGINE/engine_tests.exe`

## רישיון

רישיון MIT - ראו `LICENSE`.


# Custom Controllers And Helpers

## What a custom controller is
Custom controllers are this engine's equivalent of Unity scripts/C++ blueprints for asset behavior.
Each asset can map to a controller by asset name (for example `frog` -> `frog_controller`).

## Canonical files
- Runtime base + facade:
  - `ENGINE/runtime/animation/controllers/shared/custom_asset_controller.hpp`
  - `ENGINE/runtime/animation/controllers/shared/custom_controller_api.hpp`
- Built-in custom controllers:
  - `ENGINE/runtime/animation/controllers/custom_controllers/`
- Factory mapping:
  - `ENGINE/runtime/assets/asset/controller_factory.cpp`
- Dev-mode generator/open flow:
  - `ENGINE/editor/devtools/asset_editor/animation_editor_window/CustomControllerService.cpp`

## Authoring include and namespace
Use one include in custom controllers:

```cpp
#include "animation/controllers/shared/custom_controller_api.hpp"
```

Use the short API namespace:

```cpp
custom_controller_api::resolve_valid_player_target(ctx);
```

## Lifecycle hooks (author surface)
Custom controllers derive from `CustomAssetController` and can override:
- `on_init()`: constructor-time setup hook.
- `on_update(const Input&)`: per-frame behavior.
- `on_attack(const animation_update::Attack&)`: called for each pending incoming attack before processing.
- `on_hit(const animation_update::Attack&)`: called when damage was applied and asset survived.
- `on_death()`: called when pending attack processing results in death.
- `on_no_pending_attacks()`: called when there were no pending attacks that tick.
- `on_process_pending_attacks(Asset&)`: post-processing hook (keep terminology; do not duplicate with separate damage hook).
- `on_orphaned_hook(Asset&, Asset*)`: called when orphaned.
- `on_pre_delete_hook(Asset&)`: called before delete.

Default recommendation: call `CustomAssetController::<same_hook>(...)` first in overrides, then custom logic.

## Runtime context access
- Read-only frame context:
  - `game_context()`
- Access current asset and assets manager:
  - `self_ptr()`
  - `assets()`
- Controlled mutable runtime game context:
  - `mutable_runtime_game_context()`

Use mutable context intentionally for runtime gameplay state updates.

## Common helper cookbook
- Resolve valid player target:
  - `custom_controller_api::resolve_valid_player_target(game_context())`
- Contact attack dispatch:
  - `custom_controller_api::dispatch_contact_attack(game_context())`
- Reverse animation helpers:
  - `begin_reverse_current_animation_until_stop(...)`
  - `begin_reverse_current_animation_to_default(...)`
  - `stop_reverse_current_animation(...)`
- Attack processing defaults:
  - provided by `CustomAssetController` pending-attack pipeline.

## Factory linking behavior
- Controllers are linked by normalized asset name key with `_controller` suffix.
- New generated controllers are inserted into `controller_factory.cpp` include and registry markers.
- Unknown keys fall back to base `CustomAssetController`.

## Dev-mode behavior (create/open)
- **Add Controller**:
  - uses `CustomControllerService` only.
  - creates `.hpp/.cpp` scaffold under `ENGINE/runtime/animation/controllers/custom_controllers/`.
  - auto-registers in controller factory.
- **Open Controller**:
  - opens existing header/source files.
  - **does not mutate files** (no metadata/factory/manifest/source rewrite on open).

## Troubleshooting
- Controller button says Add but you expect Open:
  - verify both `.hpp` and `.cpp` exist in `custom_controllers/`.
- Controller not running in runtime:
  - verify factory include + registry entry exist for your key.
  - verify asset name normalizes to your controller key (`<asset>_controller`).
- Behavior not receiving attack callbacks:
  - ensure asset can receive attacks (hitbox enabled, active, alive).
