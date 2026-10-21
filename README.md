# Palpedia Enhancer

Search, sort and filter the Palworld Palpedia, and clear every unread notification dot with one key instead of opening 150 entries by hand.

> **Client side. Works in singleplayer, in co-op, and on dedicated servers.**

---

## Availability

- [Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3788253343)
- [Nexus Mods](https://www.nexusmods.com/palworld/mods/5307)

## What It Does

- **Clear the new dots in one key** - `Ctrl+N` shows only the entries you have not opened, and Mark All Seen beside the Stats tab clears every dot at once.
- **Filter down to what you want** - by ownership, met status, element and work suitability. Choices inside one category widen the results, and each category narrows them.
- **Search four fields** - name, drops, ranch products and description, in any combination, with wildcards, alternation and exclusions.
- **Sort any way you read** - name, element, work type, discovery state, ownership, or unread first, ascending or descending. The default restores the game's own numbering.
- **Reset everything at once** - Clear All on the Filter window's title row, or a hotkey that also puts the sort, the search and the unread view back to your defaults.
- **Everything composes** - a search result is always inside the active filters, in the active sort order.
- **One set of controls, not two** - on Palworld 1.0.4 and later the game's own Palpedia search and filter buttons are hidden, and this mod's search, Sort and Filter take their place.
- **It looks like it belongs** - built from the game's own buttons and font, and localized into all seventeen languages Palworld ships.

### Search syntax

| Type this | To find |
|---|---|
| `flame` | anything matching "flame" in the fields you have turned on |
| `name:flame drops:oil` | one term per field, both at once |
| `applied * handbook i` | anything between "Applied" and "Handbook I", spaces included |
| `{fire,water}` | any one of the listed words |
| `wool|fur` | either term |
| `-egg` | anything without "egg" |

Search is case-insensitive in every language, and runs when you press Enter, click away, or close the search bar, the same as the game's own search fields.

---

## Requirements

- Palworld on Steam. The Microsoft Store and Game Pass builds are not supported.
- Keyboard and mouse. The search box and filters this mod adds have no controller support.

## Configuration

`Assets\Settings.ini` inside the mod folder. An update reinstalls it with defaults, so keep a copy of your edits. A value the mod cannot read is reported in `UE4SS.log` and that setting keeps its default.

| Setting | Default | What it does |
|---|---|---|
| `ToggleUnreadKey` | `Ctrl+N` | Show only new entries. A-Z, 0-9 or F1-F12, with optional `Ctrl+`, `Shift+` or `Alt+`. `None` unbinds. |
| `ClearAllKey` | `None` | Put every filter, sort and search target back to the defaults below. |
| `MarkAllSeenKey` | `None` | Mark every unread entry as opened, the same as the button. |
| `RememberState` | `true` | Keep filters, sort and search across Palpedia opens within one session. Nothing is written to disk. |
| `ShowMarkAllSeenButton` | `true` | Draw the Mark All Seen button. The hotkey works either way. |
| `DefaultSort` | `Default` | The order the list opens in: `Default`, `Name`, `Element`, `WorkType`, `Discovery`, `Ownership` or `UnreadFirst`. |
| `DefaultSortDescending` | `false` | Open with that order reversed. |
| `DefaultUnreadOnly` | `false` | Open already showing only unopened entries. |
| `DefaultSearchTargets` | `Name` | What the search box looks in: `Name`, `Drops`, `Ranch`, `Desc`, or several joined by commas. |
| `CheckForUpdates` | `true` | Once per game start, read the newest published version of this mod from `alkeari.com/updates/palpedia-enhancer.json` and tell you only when a newer one exists. Nothing is sent but that one request. |
| `VerboseLogging` | `false` | Per-open diagnostics in `UE4SS.log`. Turn this on before reporting a bug. |
| `LiveReloadSeconds` | `2` | How often the file is checked for your edits while you play, from 0 to 60 seconds. 0 stops the checking, and turning it back on takes effect the next time the game starts. |

Every setting is also on the Mod Configs tab of [Universal Config Manager](https://www.nexusmods.com/palworld/mods/5545) if you run it, so you can change them without leaving the game. It is entirely optional and this mod never requires it.

## Compatibility

- **Client side.** It changes only what your own game draws and sends nothing to the server, so it behaves the same in singleplayer, in co-op and on a dedicated server.
- **Host side: none.** Nobody else needs it installed, and it is never installed on a server.
- **Palworld 1.0.4 and 1.0.3.** Which version you run is worked out while the game is running, so one build serves both.
- If a Palworld update renames something the mod depends on, it turns itself off and writes `[PalpediaEnhancer] DISABLED: ...` to `UE4SS.log` instead of crashing. Wait for an update rather than reporting a crash.

## License

License terms are in the [Alkeari License Agreement](https://gist.github.com/Alkeari/2c6ec0cdf3dafee375b1a00b28ca190a).
