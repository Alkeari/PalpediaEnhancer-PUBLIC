# Source modules

What each file under `Source/PalpediaEnhancer/` holds and the behavior contracts behind it.

- `dllmain.cpp`: lifecycle, engine-tick callback, symbol probe, self-disable, and the live-settings watch. `on_update()` is the mod's only off-thread work and compares `Settings.ini`'s last-write time and size every `LiveReloadSeconds` (default 2, `0` off), parses only after that stamp holds still 60 ms, and drops the result when a re-stat taken after the read no longer matches, so a file that is unopenable, torn, or names no known key is refused and the last good settings stay in force. The parsed struct crosses to `GameThreadTick`, which applies every consequence. UE4SS cannot REMOVE a keybind, so a rebind registers the new chord and leaves the old registration in place and every callback asks the live chord table what its chord means now, which is what makes a rebind take and `None` genuinely unbind.
- `PalpediaUi.cpp`: panels, buttons, search state machine, label fitting. `ApplyMarkSeenSetting` shows, collapses or builds the Mark All Seen button on a screen that is already open, while the `Default*` values describe how the Palpedia OPENS and so land at the next open or on Clear All, never onto a list the player is reading.
- `PalpediaFilter.cpp`: species cache, drops and ranch data, rich-text expansion.
- `PalpediaQuery.cpp`: search query language (wildcards, brace alternation, AND/OR/NOT), engine-independent with its own standalone test suite.
- `PalpediaUpdate.cpp`, `PalpediaUpdateFetch.cpp`: the update check, started once per game start from `dllmain.cpp` on its own thread while `CheckForUpdates` is on: reads the newest version from `https://alkeari.com/updates/palpedia-enhancer.json` (WinHTTP, nothing sent but the request) and compares it with the installed version; the comparison is engine-independent with its own standalone test suite (`tests\PalpediaUpdate\run-tests.ps1`). A newer version is shown once, translated, as a line in the game's own on-screen log (`PalpediaNotice.cpp`, through `PalLogManager::AddLog`) as soon as a player is in a world, and logged to `UE4SS.log`.
- `PalpediaLocalization.cpp`: language detection, `Assets/lang` loading.
- `PalpediaSettings.cpp`: `Assets/Settings.ini`, asset directory resolved relative to the dll. Player preferences only, never an internal identifier, asset path or panel geometry; every value validated, an unreadable one logged once and left at its default. `ReadSettings` reports whether the file opened and how many known keys it held, so the watcher can refuse a torn read. `DefaultSort` omits `SortMode::Number`, the one order the Sort panel cannot show or leave. `SetActiveSettings`/`ActiveSettings` publish the loaded struct so the UI reads a preference without it being threaded through every builder.
- `PalpediaText.cpp`: FText as raw 0x18 bytes converted through KismetTextLibrary, which is why nothing here holds UE4SS's own `FText`.

## Settings.ini contents

Hotkeys ToggleUnreadKey, ClearAllKey, MarkAllSeenKey; ShowMarkAllSeenButton, CheckForUpdates, RememberState, VerboseLogging, LiveReloadSeconds; the opening state DefaultSort, DefaultSortDescending, DefaultUnreadOnly, DefaultSearchTargets.

## UI construction

Every control is a vanilla `WBP_CommonButton_Activation` with a 9-slice `button.png` frame (Slate Box brush) and a game-font label centered and shrunk to fit by `CenterLabels`. Panels come from `BuildPanel(PanelSpec)` with sections (Ownership, Met Status, Pal Type, Work Suitability); the backdrop is `panel_gradient.png` stretched, `panel_pattern.png` tiled and a `panel.png` frame.

Search targets: Name, Drops (what a Pal drops when caught or defeated, read by reflection), Ranch (items named in the partner-skill sentence containing `<mapObjectName id=|MonsterFarm|/>`), Desc (free text). Search is Unicode-aware (`CharLowerBuffW`) and per-species cached.
