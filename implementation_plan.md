# Implementation Plan

[Overview]
Simplify loading screen to show only the main menu background image.

The loading screen currently uses random images from the LOADING CONTENT directory and displays tarot cards with scaling/rotation effects. This implementation will completely simplify the `showLoadingScreen()` method in `ENGINE/ui/main_menu.cpp` to show only the main menu background image, removing all tarot card display logic and any scaling/rotation effects.

[Types]
No type system changes required.

This implementation does not introduce any new data structures or modify existing type definitions. All changes are behavioral modifications to existing functions.

[Files]
Modify loading screen implementation in main menu files.

- **File to modify**: `ENGINE/ui/main_menu.cpp`
  - Modify `showLoadingScreen()` method to use `cached_bg_tex_` instead of loading separate background
  - Remove scaling/rotation effects from tarot card rendering
  - Ensure background rendering uses same method as main menu

[Functions]
Completely rewrite showLoadingScreen function to show only background.

- **Function to modify**: `MainMenu::showLoadingScreen()`
  - Current behavior: Loads background from loading content directory, displays tarot card with scaling/rotation, shows loading text and messages
  - New behavior: Shows only the main menu background image with loading text
  - Specific changes:
    * Replace background loading logic with direct use of `cached_bg_tex_`
    * Remove all tarot card display logic completely
    * Remove all scaling/rotation effects
    * Simplify to show only background + loading text
    * Remove message display logic

[Classes]
No class structure modifications needed.

The implementation requires no changes to class definitions, inheritance, or relationships. Only behavioral changes to existing methods.

[Dependencies]
No new dependencies required.

This implementation uses existing SDL and filesystem dependencies already present in the codebase. No additional libraries or packages needed.

[Testing]
Manual visual testing required.

- Verify loading screen displays main menu background correctly
- Confirm tarot card appears without scaling/rotation effects
- Ensure loading text and messages are still visible
- Test with different screen resolutions

[Implementation Order]
Sequential steps to simplify loading screen.

1. Backup existing `showLoadingScreen()` method implementation
2. Completely rewrite method to use only main menu background
3. Remove all tarot card and message display logic
4. Remove all scaling/rotation effects
5. Test the simplified loading screen
