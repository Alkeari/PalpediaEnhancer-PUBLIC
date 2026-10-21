#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include <Input/KeyDef.hpp>
#include <Unreal/Common.hpp>

#include <PalpediaQuery.hpp>

namespace PalpediaEnhancer
{
    // Defined in PalpediaFilter.hpp. Declared opaquely here so a settings field can carry a sort
    // mode without this header pulling the whole filter (and the Unreal headers under it) in.
    enum class SortMode : uint8_t;

    // A key chord as written in Settings.ini: "Ctrl+N", "F7", "Shift+Alt+U". Empty = unbound.
    struct KeyChord
    {
        RC::Input::Key Key{};
        bool Ctrl{};
        bool Shift{};
        bool Alt{};
        bool Bound{};
    };

    // Everything a player can change without rebuilding. Read from Assets/Settings.ini at load
    // and again whenever the file changes on disk; every field has a default so a missing or
    // broken file never disables anything, it only logs.
    struct Settings
    {
        KeyChord ToggleUnread{RC::Input::Key::N, true, false, false, true};
        KeyChord ClearAll{};
        KeyChord MarkAllSeen{};
        bool VerboseLogging{};
        // Keep the chosen filters, search targets, sort and committed query across Palpedia opens in one session.
        bool RememberState{true};
        // Draw the Mark All Seen button beside the Stats tab. Off means the button is never
        // built; the hotkey above still works, since it does not go through the button.
        bool ShowMarkAllSeenButton{true};
        // Look up the newest published version once per game start and say so only when it is newer.
        bool CheckForUpdates{true};

        // The state a fresh Palpedia opens in, and what the Clear All hotkey returns it to.
        // SortMode{} is SortMode::None, the game's own order (the enum is opaque here).
        SortMode DefaultSort{};
        bool DefaultSortDescending{};
        bool DefaultUnreadOnly{};
        uint8_t DefaultSearchTargets{TargetBit(SearchTarget::Name)};

        // How often the file is checked for an edit, in seconds; 0 stops the watch entirely and
        // makes this a load-time-only file again.
        int LiveReloadSeconds{2};

        static auto Load(const std::filesystem::path& file) -> Settings;
        // One line naming every value in force, for the load and reload log entries.
        static auto Summary(const Settings& settings) -> RC::StringType;
        static auto ParseChord(RC::StringType text) -> std::optional<KeyChord>;
        // Both return nothing for text that names no valid choice, so the caller keeps its
        // default and logs one line rather than acting on a guess.
        static auto ParseSortMode(RC::StringType text) -> std::optional<SortMode>;
        static auto ParseSearchTargets(RC::StringType text) -> std::optional<uint8_t>;
    };

    // What one pass over the file found. The watcher needs more than the values: a file it could
    // not open, or one caught halfway through somebody else's write, must be refused outright
    // rather than applied as "every setting back to its default". Not nested in Settings: it
    // holds one by value.
    struct SettingsReading
    {
        Settings Values{};
        bool Readable{};
        int Recognized{}; // known key names seen, whether or not their value parsed
    };

    auto ReadSettings(const std::filesystem::path& file) -> SettingsReading;

    // The installed Assets folder next to dlls/: textures and Settings.ini live there.
    auto AssetsDirectory() -> std::filesystem::path;

    // Shared log gate. Chatty per-open diagnostics go through Verbose(); they only reach
    // UE4SS.log when Settings.VerboseLogging is on. Errors and one-time notices stay unconditional.
    auto SetVerboseLogging(bool enabled) -> void;
    auto VerboseLogging() -> bool;
    auto Verbose(const RC::StringType& message) -> void;

    // The settings this session loaded, published once at startup so UI code can read a player
    // preference without the whole struct being threaded through every widget builder.
    auto SetActiveSettings(const Settings& settings) -> void;
    auto ActiveSettings() -> const Settings&;
} // namespace PalpediaEnhancer
