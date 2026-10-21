# Changelog

What changed in Palpedia Enhancer, newest first. Only changes you can actually notice are listed:
internal and build work is deliberately left out. This file is the source for the change notes
published on the Steam Workshop listing.

Entries begin on 2026-08-14, when the project was created.

## v1.12.0 - 2026-10-19

Added: once per game start the mod looks up its newest published version and tells you only when a newer one exists; CheckForUpdates in Settings.ini turns this off.

## v1.11.1 - 2026-09-07

Changed: the search box, search button and filter button Palworld v1.0.4 added to the Palpedia are hidden, and this mod's own search, sort and filter row takes their place.
Fixed: the control row is measured from the Palpedia list rather than a button the v1.0.4 update moved, so it lines up with the list instead of falling back to fixed coordinates.

## v1.11.0 - 2026-09-05

Added: an edit to Settings.ini applies within seconds while you play, with no restart.
Added: a LiveReloadSeconds setting choosing how often the file is checked, where 0 turns the checking off.
Changed: rebinding a hotkey, or setting it to None, takes effect immediately, and the Mark All Seen button appears or disappears on the screen already open.

## v1.10.0 - 2026-09-04

- Added: Settings.ini can set the sort order the Palpedia opens in, and whether that order is reversed.
- Added: Settings.ini can open the Palpedia already showing only the entries you have not opened yet.
- Added: Settings.ini can set which of Name, Drops, Ranch and Desc the search box looks in when the Palpedia opens.
- Added: MarkAllSeenKey, a hotkey that marks every unread entry as opened without reaching for the button.
- Added: ShowMarkAllSeenButton hides the Mark All Seen button for players who would rather use the hotkey or nothing at all.
- Changed: the Clear All hotkey now returns the Palpedia to your configured defaults instead of the vanilla ones.
- Changed: every setting is documented in Settings.ini with the values it accepts, so mod config managers can offer them as switches and lists.
- Fixed: a setting written in a form the mod cannot read now says so in UE4SS.log instead of falling back in silence.

## v1.9.0 - 2026-09-04

- Changed: The filter and sort panels, the buttons and the search field follow the Alkeari palette.
- Changed: Section headings are gold with a gray rule, replacing the previous light blue.
- Changed: The search box is a neutral dark field whose text turns gold while you are typing in it.
- Changed: Clear All is amber with black text instead of red.
- Changed: The panel, button and search-field artwork is neutral gray with no blue tint.

## v1.8.0 - 2026-09-02

- Added: a Mark All Seen button beside the Stats tab that marks every unread Palpedia entry as opened at once, clearing the new dots on the list and on the menu tab without opening each entry.
- Changed: the mod now loads under both the Steam Workshop UE4SS and the newer GitHub/Nexus UE4SS release (2026-08-28).

## v1.7.0 - 2026-08-30

- Changed: search now runs when you press Enter or close the search bar, like the game's own search fields, instead of re-filtering on every keystroke.
- Added: the committed search and the search bar's text persist across Palpedia opens, like the filters and search targets (respects RememberState).
- Fixed: the Habitat tab's map could not be dragged after using search.
- Fixed: pressing Enter in the search bar wiped the search results.
- Fixed: clicking anything outside the search bar while typing was ignored; the first click now lands, and clicking away applies the search.
- Fixed: the search-target toggles, clear button, and search button show hover effects and respond to a single click while typing, without applying a half-typed search.

## v1.6.0 - 2026-08-27

- Added: search now supports `*`/`?` wildcards (usable across multi-word phrases with no special syntax, e.g. `applied * handbook i`), `{a,b,c}` alternation, `word|word`, `-word` exclusion, and `(...)` grouping, on top of the existing `name:`/`drops:`/`ranch:`/`desc:` prefixes (now scoped per word or group instead of the whole query).

## v1.5.2 - 2026-08-22

- Added: A README.txt inside the mod folder with install steps, settings and where to report bugs, for players who install the zip by hand.

## v1.5.1 - 2026-08-22

- Changed: The Filter window starts below the game's tab bar and compass instead of overlapping them.

## v1.5.0 - 2026-08-22

- Changed: The Filter window is organized into titled sections (Ownership, Met Status, Pal Type,
  Work Suitability) with New and Clear All on the title row; the Sort window into Sort By and Order.
- Changed: Both windows sit on a gradient backdrop with a subtle pattern instead of a flat black plate.
- Changed: Long captions shrink to fit their tile instead of spilling past it.

## v1.4.1 - 2026-08-22

- Changed: Clear All in the Filter window is a solid red tile so it cannot be mistaken for a filter.

## v1.4.0 - 2026-08-22

- Added: The whole UI follows the game's language, with translations for all 17 languages Palworld ships. Search is case-insensitive in every script.

## v1.3.5 - 2026-08-22

- Changed: Clear All in the Filter window is tinted red so it stands apart from the filters around it.

## v1.3.4 - 2026-08-22 (first public release)

- Added: Show only Palpedia entries you have not opened yet, so the new marker can be cleared without
  scrolling the whole list.
- Added: Filter the Palpedia by element and work suitability.
- Added: Filter by ownership: never caught, caught, or caught five or more times.
- Added: Search by Pal name, drop item, ranch produce, or description text - any combination at once via the Name/Drops/Ranch/Desc toggles, or a `name:` / `drops:` / `ranch:` / `desc:` prefix to pick one. Ranch lists only Pals that produce the item when assigned to a ranch.
- Added: Sort by name, element, work type, discovery state, or ownership; Default keeps the game's
  own Palpedia numbering.
- Added: Sort with unread entries first, and ascending or descending order.
- Changed: Every button and window uses the same framed look as the Sort and Filter tiles, and
  captions use the game's own font.
- Added: `Settings.ini` with rebindable hotkeys (toggle new-only, clear all), a RememberState
  switch, and verbose logging for bug reports.
- Changed: Element and work-suitability captions follow the game's language.
- Changed: If a game update breaks something the mod relies on, it turns itself off with a clear
  log message instead of crashing.
