#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

#include <Input/Handler.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <PalpediaFilter.hpp>
#include <PalpediaSettings.hpp>
#include <PalpediaUi.hpp>
#include <PalpediaLocalization.hpp>
#include <PalpediaNotice.hpp>
#include <PalpediaUpdate.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

// The Palworld Steam build id this release was tested on; quoted in the disable message so a
// player can tell at a glance whether a game patch is the reason.
#define PALPEDIA_TESTED_BUILD L"25094871"

class PalpediaEnhancerMod : public RC::CppUserModBase
{
public:
    PalpediaEnhancerMod() : CppUserModBase()
    {
        ModName = STR("PalpediaEnhancer");
        ModVersion = STR("1.12.0");
        ModDescription = STR("Search, sort and filter the Palpedia, including entries you have not opened yet.");
        ModAuthors = STR("Alkeari");

        m_settingsFile = PalpediaEnhancer::AssetsDirectory() / L"Settings.ini";
        m_settings = PalpediaEnhancer::Settings::Load(m_settingsFile);
        PalpediaEnhancer::SetVerboseLogging(m_settings.VerboseLogging);
        PalpediaEnhancer::SetActiveSettings(m_settings);
        m_liveReloadSeconds.store(m_settings.LiveReloadSeconds, std::memory_order_relaxed);
        // The file as it stands is what the mod just loaded, so the watcher starts from it and
        // only reacts to the next edit.
        ReadStamp(m_appliedStamp);
        // Pure data: the filter is a plain object here, with no widget and no engine call behind
        // it, so the configured opening state can be put in place before the first tick.
        ApplyDefaultPreferences();

        SyncKeyBindings();
        ReportChord(m_settings.ToggleUnread, STR("ToggleUnreadKey"));
        ReportChord(m_settings.ClearAll, STR("ClearAllKey"));
        ReportChord(m_settings.MarkAllSeen, STR("MarkAllSeenKey"));

        if (m_settings.CheckForUpdates)
        {
            StartUpdateCheck();
        }
    }

    // Once per game start, and only while CheckForUpdates is on. The request runs on its own
    // thread so a slow or absent network never holds up loading; the state it fills is shared
    // rather than owned by the mod, so a fetch still in flight outlives nothing it writes to.
    auto StartUpdateCheck() -> void
    {
        if (m_updateStarted)
        {
            return;
        }
        m_updateStarted = true;
        // ModVersion is plain ASCII digits and dots.
        std::string installed(ModVersion.begin(), ModVersion.end());
        std::thread([state = m_update, installed = std::move(installed)] {
            if (const auto feed = PalpediaEnhancer::Update::FetchFeed())
            {
                state->Newer = PalpediaEnhancer::Update::NewerThanInstalled(*feed, installed);
            }
            state->Done.store(true, std::memory_order_release);
        }).detach();
    }

    // Shows the newer version once, in the game's own on-screen log, as soon as a player is in a
    // world to see it; the title screen draws no log, so a line posted there would be lost. The
    // world check is retried every few seconds rather than every tick.
    auto AnnounceUpdate() -> void
    {
        if (m_updateAnnounced || !m_settings.CheckForUpdates || !m_update->Done.load(std::memory_order_acquire))
        {
            return;
        }
        if (!m_update->Newer)
        {
            m_updateAnnounced = true;
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastAnnounceTry < std::chrono::seconds(3))
        {
            return;
        }
        m_lastAnnounceTry = now;
        if (!PalpediaEnhancer::Notice::PlayerInWorld())
        {
            return;
        }
        m_updateAnnounced = true;
        const RC::StringType newer(m_update->Newer->begin(), m_update->Newer->end());
        RC::StringType line = PalpediaEnhancer::Loc::T(STR("update_available"));
        if (const auto at = line.find(STR("{0}")); at != RC::StringType::npos)
        {
            line.replace(at, 3, newer);
        }
        const bool shown = PalpediaEnhancer::Notice::PostGameLog(line);
        RC::Output::send<RC::LogLevel::Normal>(std::format(STR("[PalpediaEnhancer] Version {} is available (installed {}){}\n"),
                                                           newer,
                                                           ModVersion,
                                                           shown ? STR("") : STR("; the in-game log could not be reached")));
    }

    static auto ReportChord(const PalpediaEnhancer::KeyChord& chord, const RC::CharType* name) -> void
    {
        if (!chord.Bound)
        {
            RC::Output::send<RC::LogLevel::Normal>(std::format(STR("[PalpediaEnhancer] {} is unbound\n"), name));
        }
    }

    // Identifies one chord, so the same key with the same modifiers is only ever registered once.
    static auto ChordId(const PalpediaEnhancer::KeyChord& chord) -> uint32_t
    {
        return (static_cast<uint32_t>(chord.Key) << 3) | (chord.Ctrl ? 4u : 0u) | (chord.Shift ? 2u : 0u) | (chord.Alt ? 1u : 0u);
    }

    // Publishes the chords the input thread matches against, then makes sure every chord now in
    // use has a registration. UE4SS offers a mod no way to REMOVE a keybind, so a rebind leaves
    // the old registration in place; that is why nothing is decided at registration time. Every
    // callback asks this table what its chord means right now, so a key Settings.ini has stopped
    // naming does nothing the moment the file is reloaded, and None genuinely unbinds.
    auto SyncKeyBindings() -> void
    {
        {
            std::lock_guard<std::mutex> lock(m_bindMutex);
            m_liveToggleUnread = m_settings.ToggleUnread;
            m_liveClearAll = m_settings.ClearAll;
            m_liveMarkAllSeen = m_settings.MarkAllSeen;
        }
        EnsureChordRegistered(m_settings.ToggleUnread, STR("ToggleUnreadKey"));
        EnsureChordRegistered(m_settings.ClearAll, STR("ClearAllKey"));
        EnsureChordRegistered(m_settings.MarkAllSeen, STR("MarkAllSeenKey"));
    }

    auto EnsureChordRegistered(const PalpediaEnhancer::KeyChord& chord, const RC::CharType* name) -> void
    {
        if (!chord.Bound || !m_registeredChords.insert(ChordId(chord)).second)
        {
            return;
        }

        RC::Input::Handler::ModifierKeyArray modifiers{};
        size_t count = 0;
        if (chord.Ctrl)
        {
            modifiers[count++] = RC::Input::ModifierKey::CONTROL;
        }
        if (chord.Shift)
        {
            modifiers[count++] = RC::Input::ModifierKey::SHIFT;
        }
        if (chord.Alt)
        {
            modifiers[count++] = RC::Input::ModifierKey::ALT;
        }
        // Keybinds fire on UE4SS's input thread. Nothing in the callback touches the filter or a
        // widget; each only raises a flag that the game-thread tick picks up and applies.
        const RC::Input::EventCallbackCallable callback = [this, chord]() { OnChordPressed(chord); };
        try
        {
            if (count == 0)
            {
                register_keydown_event(chord.Key, callback);
            }
            else
            {
                register_keydown_event(chord.Key, modifiers, callback);
            }
        }
        catch (...)
        {
            m_registeredChords.erase(ChordId(chord));
            RC::Output::send<RC::LogLevel::Warning>(std::format(STR("[PalpediaEnhancer] Could not register {}\n"), name));
        }
    }

    // Input thread. One chord can drive more than one action, so every match is raised.
    auto OnChordPressed(const PalpediaEnhancer::KeyChord& fired) -> void
    {
        std::lock_guard<std::mutex> lock(m_bindMutex);
        const auto matches = [&fired](const PalpediaEnhancer::KeyChord& bound) {
            return bound.Bound && bound.Key == fired.Key && bound.Ctrl == fired.Ctrl && bound.Shift == fired.Shift && bound.Alt == fired.Alt;
        };
        if (matches(m_liveToggleUnread))
        {
            m_unreadTogglePending.store(true, std::memory_order_relaxed);
        }
        if (matches(m_liveClearAll))
        {
            m_clearAllPending.store(true, std::memory_order_relaxed);
        }
        if (matches(m_liveMarkAllSeen))
        {
            m_markAllSeenPending.store(true, std::memory_order_relaxed);
        }
    }

    // Every engine object this mod touches - widgets, slots, the scroll list, Blueprint functions
    // through ProcessEvent - belongs to the game thread. UE4SS calls a C++ mod's on_update() from
    // its own event-loop thread ("UE4SS-UpdateThread", UE4SSProgram::update), NOT the game
    // thread, so for every version before 1.1.13 all of this mod's widget construction and polling
    // ran concurrently with the game thread painting the very same Slate tree. That is the crash:
    // the engine's per-frame SlatePrepass walking a child this mod was mid-way through building
    // (vtable still on the abstract SWidget base -> "Pure virtual function being called") or a
    // children array being reallocated under it (read at 0xffffffffffffffff). Ten single-call-site
    // fixes could not stop it because it was never about which call was made, only which thread
    // made it. The engine-tick pre-callback is UE4SS's own game-thread entry point (the same one
    // Lua's ExecuteInGameThread uses), so the whole pass is moved there. See the design doc,
    // 2026-08-22.
    auto on_unreal_init() -> void override
    {
        try
        {
            RC::Unreal::Hook::RegisterEngineTickPreCallback(
                    [this](RC::Unreal::Hook::TCallbackIterationData<void>&, RC::Unreal::UEngine*, float, bool) { GameThreadTick(); },
                    {false, true, STR("PalpediaEnhancer"), STR("GameThreadTick")});
            RC::Output::send<RC::LogLevel::Normal>(STR("[PalpediaEnhancer] Running on the game thread via the engine tick hook\n"));
        }
        catch (const std::exception& error)
        {
            m_disabled = true;
            RC::StringType message{};
            for (const char* character = error.what(); character && *character; ++character)
            {
                message.push_back(static_cast<RC::StringType::value_type>(*character));
            }
            RC::Output::send<RC::LogLevel::Error>(
                    std::format(STR("[PalpediaEnhancer] Could not hook the engine tick; the mod is disabled rather than run off-thread: {}\n"), message));
        }
        catch (...)
        {
            m_disabled = true;
            RC::Output::send<RC::LogLevel::Error>(STR("[PalpediaEnhancer] Could not hook the engine tick; the mod is disabled rather than run off-thread\n"));
        }
    }

    // The ONLY thing this mod does off the game thread. See on_unreal_init: this runs on UE4SS's
    // update thread, where no engine object, widget or Blueprint call is safe. Reading a file on
    // disk is, and doing it here means a slow or contended disk costs the player no frame time.
    // What comes back is a plain struct of numbers and strings, handed to the game thread through
    // m_pendingSettings; every consequence of it is applied there.
    auto on_update() -> void override
    {
        try
        {
            PollSettingsFile();
        }
        catch (...)
        {
            // A file the mod cannot read is not worth a single frame of the player's game, let
            // alone the process. The settings already in force stay in force.
        }
    }

    // Unloading the mod while a search was open would otherwise leave the player in UI-only input
    // with nothing left running to put it back.
    ~PalpediaEnhancerMod() override
    {
        try
        {
            m_ui.ReleaseInputMode();
        }
        catch (...)
        {
        }
    }

    // Nothing this mod does is worth taking the game down for. This runs every engine tick, on the
    // game thread, and an exception escaping it aborts the process, so everything is contained
    // and reported once.
    auto GameThreadTick() -> void
    {
        if (m_disabled)
        {
            return;
        }

        try
        {
            if (!m_probed)
            {
                m_probed = true;
                if (!ProbeGameSymbols())
                {
                    m_disabled = true;
                    return;
                }
            }

            TryRegisterHooks();

            // Settings.ini changed on disk and the watcher parsed it. Everything a new value
            // touches - the keybind table, the log gate, the published settings, the Mark All
            // Seen button - is applied from here, on the game thread.
            if (m_settingsReloadPending.exchange(false, std::memory_order_acquire))
            {
                ApplyReloadedSettings();
            }

            AnnounceUpdate();

            if (m_unreadTogglePending.exchange(false, std::memory_order_relaxed))
            {
                m_filter.ToggleUnreadOnly();
                m_pendingRefresh = true;
            }
            if (m_clearAllPending.exchange(false, std::memory_order_relaxed))
            {
                ApplyDefaultPreferences();
                m_pendingRefresh = true;
                m_sortPending = true;
            }

            const auto poll = m_ui.Update(m_filter);

            bool needsRefresh = m_pendingRefresh;
            m_pendingRefresh = false;

            if (poll.ScreenRebuilt)
            {
                // A fresh screen means every row pointer we remembered is gone.
                m_filter.Reset();
                if (!m_settings.RememberState)
                {
                    ApplyDefaultPreferences();
                    m_ui.ForgetQuery();
                }
                needsRefresh = true;
            }

            for (const auto& action : poll.FilterActions)
            {
                switch (action.What)
                {
                case PalpediaEnhancer::FilterAction::Kind::Unread:
                    m_filter.ToggleUnreadOnly();
                    break;
                case PalpediaEnhancer::FilterAction::Kind::Discovery:
                    m_filter.ToggleDiscovery(static_cast<PalpediaEnhancer::Discovery>(action.Value));
                    break;
                case PalpediaEnhancer::FilterAction::Kind::Element:
                    m_filter.ToggleElement(action.Value);
                    break;
                case PalpediaEnhancer::FilterAction::Kind::WorkSuitability:
                    m_filter.ToggleWorkSuitability(action.Value);
                    break;
                case PalpediaEnhancer::FilterAction::Kind::Ownership:
                    m_filter.ToggleOwnership(static_cast<PalpediaEnhancer::Ownership>(action.Value));
                    break;
                case PalpediaEnhancer::FilterAction::Kind::SortMode:
                    m_filter.ToggleSortMode(static_cast<PalpediaEnhancer::SortMode>(action.Value));
                    m_sortPending = true;
                    break;
                case PalpediaEnhancer::FilterAction::Kind::SortDescending:
                    m_filter.SetSortDescending(!m_filter.SortDescending());
                    m_sortPending = true;
                    break;
                case PalpediaEnhancer::FilterAction::Kind::ClearAll:
                    m_filter.ClearAll();
                    break;
                case PalpediaEnhancer::FilterAction::Kind::SearchTarget:
                    m_filter.ToggleSearchTarget(static_cast<PalpediaEnhancer::SearchTarget>(action.Value));
                    break;
                case PalpediaEnhancer::FilterAction::Kind::ClearSearch:
                    m_filter.SetQuery({});
                    m_filter.ResetSearchTargets();
                    break;
                case PalpediaEnhancer::FilterAction::Kind::MarkAllSeen:
                    m_ui.MarkAllSeen(poll.Rows);
                    break;
                default:
                    continue;
                }
                needsRefresh = true;
            }

            // Consumed whether or not the Palpedia is open: a press with no list in front of the
            // player does nothing and is not queued for the next open.
            if (m_markAllSeenPending.exchange(false, std::memory_order_relaxed) && !poll.Rows.empty())
            {
                m_ui.MarkAllSeen(poll.Rows);
                needsRefresh = true;
            }

            if (poll.QueryChanged)
            {
                m_filter.SetQuery(poll.Query);
                needsRefresh = true;
            }

            // A rebuilt screen carries the sort state forward, so it is re-applied on open - and
            // that includes Default: the game's own row order is not Paldex order (a boss sits in
            // row 1), so even the unsorted state is put into 000, 001, 002... on every open.
            if (poll.ScreenRebuilt)
            {
                m_sortPending = true;
            }

            if (needsRefresh && !poll.Rows.empty())
            {
                m_filter.Refresh(poll.Rows);
            }

            if (m_sortPending && !poll.Rows.empty())
            {
                m_sortPending = false;
                // Default (SortMode::None) is applied exactly like every other mode: SortRowOrder
                // returns the rows in their natural Paldex order and the list is put back into it.
                // This used to be skipped for None, which is why "Default" did nothing after any
                // other sort had been applied - the skip existed because an identity reorder once
                // crashed, and that crash was the off-thread race fixed in v1.1.13, not the reorder.
                {
                    // A reorder failure must never take the whole mod down with it: that is what
                    // made the Close button die after picking a sort. Sort is isolated so every
                    // other control keeps working even when the engine rejects the reorder.
                    try
                    {
                        const auto sortedRows = m_filter.SortRowOrder(poll.Rows);
                        const auto rowId = [](RC::Unreal::UObject* row) {
                            if (!row)
                            {
                                return RC::StringType{STR("(null)")};
                            }
                            RC::Unreal::FName id{};
                            if (auto* value = row->GetValuePtrByPropertyNameInChain<RC::Unreal::FName>(STR("ChachedBaseCharacterID")))
                            {
                                id = *value;
                            }
                            return id.ToString();
                        };
                        RC::Output::send<RC::LogLevel::Normal>(
                                std::format(STR("[PalpediaEnhancer] Sort apply: mode={} rows={} first={} last={}\n"),
                                            static_cast<int>(m_filter.CurrentSortMode()),
                                            sortedRows.size(),
                                            sortedRows.empty() ? RC::StringType{STR("?")} : rowId(sortedRows.front()),
                                            sortedRows.empty() ? RC::StringType{STR("?")} : rowId(sortedRows.back())));
                        if (!m_ui.ReorderActiveList(sortedRows))
                        {
                            // Still settling from the previous reorder. Keep the request rather
                            // than drop it, so the second of two quick clicks is not silently lost.
                            m_sortPending = true;
                        }
                    }
                    catch (const std::exception& error)
                    {
                        RC::StringType message{};
                        for (const char* character = error.what(); character && *character; ++character)
                        {
                            message.push_back(static_cast<RC::StringType::value_type>(*character));
                        }
                        RC::Output::send<RC::LogLevel::Error>(std::format(STR("[PalpediaEnhancer] Sort apply failed: {}\n"), message));
                    }
                }
            }
        }
        catch (const std::exception& error)
        {
            m_disabled = true;
            RC::StringType message{};
            for (const char* character = error.what(); character && *character; ++character)
            {
                message.push_back(static_cast<RC::StringType::value_type>(*character));
            }
            RC::Output::send<RC::LogLevel::Error>(std::format(STR("[PalpediaEnhancer] Disabled after an error: {}\n"), message));
        }
        catch (...)
        {
            m_disabled = true;
            RC::Output::send<RC::LogLevel::Error>(STR("[PalpediaEnhancer] Disabled after an unknown error\n"));
        }
    }

private:
    // Last-write time plus size. Two reads that agree mean the file has not changed; they are
    // also what a still-being-written file keeps moving, which is the whole point of comparing
    // them rather than re-parsing on a timer.
    struct FileStamp
    {
        uint64_t Written{};
        uint64_t Size{};
        bool Present{};

        auto operator==(const FileStamp& other) const -> bool
        {
            return Present == other.Present && Written == other.Written && Size == other.Size;
        }
    };

    auto ReadStamp(FileStamp& stamp) const -> bool
    {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(m_settingsFile.c_str(), GetFileExInfoStandard, &info))
        {
            stamp = FileStamp{};
            return false;
        }
        stamp.Written = (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
        stamp.Size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        stamp.Present = true;
        return true;
    }

    // UE4SS's update thread, every few milliseconds. Costs one file-attribute read per
    // LiveReloadSeconds while nothing is happening, and re-parses only after the file has stopped
    // changing.
    auto PollSettingsFile() -> void
    {
        const int seconds = m_liveReloadSeconds.load(std::memory_order_relaxed);
        if (seconds <= 0)
        {
            return; // watching turned off in Settings.ini
        }

        const auto now = std::chrono::steady_clock::now();
        const bool settling = m_candidateStamp.Present;
        if (!settling && m_lastPollDone && now - m_lastPoll < std::chrono::seconds(seconds))
        {
            return;
        }
        m_lastPoll = now;
        m_lastPollDone = true;

        FileStamp stamp{};
        if (!ReadStamp(stamp))
        {
            // Gone or unreadable: nothing is applied and nothing is lost. Reported once, then
            // quietly retried, because an editor that writes through a temporary file makes the
            // original briefly absent.
            if (!m_watchWarned)
            {
                m_watchWarned = true;
                RC::Output::send<RC::LogLevel::Warning>(
                        std::format(STR("[PalpediaEnhancer] Cannot read {}; keeping the settings already in force\n"), m_settingsFile.wstring()));
            }
            m_candidateStamp = FileStamp{};
            return;
        }
        m_watchWarned = false;

        if (stamp == m_appliedStamp)
        {
            m_candidateStamp = FileStamp{};
            return;
        }

        // Changed. A file is not read the instant it differs: another program may be halfway
        // through writing it, and a half-written file parses into "every missing setting back to
        // its default", which is exactly the silent wipe this must never do. It has to hold still
        // first, checked on every update-thread pass rather than once per interval so a normal
        // save still applies within a few milliseconds of landing.
        if (!(stamp == m_candidateStamp))
        {
            m_candidateStamp = stamp;
            m_candidateSince = now;
            return;
        }
        if (now - m_candidateSince < std::chrono::milliseconds(60))
        {
            return;
        }

        const auto reading = PalpediaEnhancer::ReadSettings(m_settingsFile);

        // The file moved between the two stamps, so what came back is a snapshot of a write in
        // progress: dropped without a warning, since the new content settles and applies on a
        // later pass. This is the check that makes a torn read impossible to act on, rather than
        // merely unlikely.
        FileStamp after{};
        if (!ReadStamp(after) || !(after == stamp))
        {
            m_candidateStamp = FileStamp{};
            m_readRetries = 0;
            return;
        }

        // Settled but still not openable: another program is holding it, which is what a save in
        // progress looks like in an editor that keeps its handle. Retried for about a second
        // before the edit is given up on, rather than losing it to one unlucky moment.
        if (!reading.Readable && ++m_readRetries <= 20)
        {
            m_candidateSince = now;
            return;
        }

        // Whatever comes of this read, the file is not read again until it changes once more: a
        // file the mod refuses must not be re-parsed and re-refused on every poll for the rest of
        // the session.
        m_appliedStamp = stamp;
        m_candidateStamp = FileStamp{};
        m_readRetries = 0;

        if (!reading.Readable || reading.Recognized == 0)
        {
            RC::Output::send<RC::LogLevel::Warning>(std::format(
                    STR("[PalpediaEnhancer] Settings.ini changed but {}; keeping the settings already in force\n"),
                    reading.Readable ? STR("names no setting this mod knows") : STR("could not be opened")));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_settingsMutex);
            m_pendingSettings = reading.Values;
        }
        m_settingsReloadPending.store(true, std::memory_order_release);
    }

    // Game thread. The only place a reloaded value becomes the value the mod runs on.
    auto ApplyReloadedSettings() -> void
    {
        PalpediaEnhancer::Settings fresh{};
        {
            std::lock_guard<std::mutex> lock(m_settingsMutex);
            fresh = m_pendingSettings;
        }

        const bool markSeenChanged = fresh.ShowMarkAllSeenButton != m_settings.ShowMarkAllSeenButton;
        m_settings = fresh;
        if (fresh.CheckForUpdates)
        {
            StartUpdateCheck();
        }
        PalpediaEnhancer::SetVerboseLogging(fresh.VerboseLogging);
        PalpediaEnhancer::SetActiveSettings(fresh);
        m_liveReloadSeconds.store(fresh.LiveReloadSeconds, std::memory_order_relaxed);
        SyncKeyBindings();

        // The Default* values describe the state a fresh Palpedia opens in, so they are not
        // forced onto a list the player is looking at; the next open, and the Clear All hotkey,
        // both read the new values from m_settings.
        if (markSeenChanged)
        {
            m_ui.ApplyMarkSeenSetting(fresh.ShowMarkAllSeenButton);
        }

        RC::Output::send<RC::LogLevel::Normal>(
                std::format(STR("[PalpediaEnhancer] Settings.ini reloaded: {}\n"), PalpediaEnhancer::Settings::Summary(fresh)));
    }

    // Back to the state Settings.ini asks a fresh Palpedia to open in: no filters and no query,
    // then the configured sort, direction, unread-only and search targets (all vanilla unless the
    // player changed them). The UI's remembered box text is cleared by the caller through
    // m_ui.ForgetQuery(), which cannot be done here: the filter has no reach into widgets.
    auto ApplyDefaultPreferences() -> void
    {
        m_filter.ClearAll();
        m_filter.SetUnreadOnly(m_settings.DefaultUnreadOnly);
        m_filter.SetSortMode(m_settings.DefaultSort);
        m_filter.SetSortDescending(m_settings.DefaultSortDescending);
        m_filter.SetSearchTargets(m_settings.DefaultSearchTargets);
        m_filter.SetQuery({});
    }

    // Everything this mod resolves by name from the engine, checked once before the first real
    // tick. A game patch that renames or removes one of these must turn the mod off with a
    // readable reason, not let it limp on into whatever the missing piece used to guard.
    // Blueprint classes (the Palpedia screen, the button widget) are not checked here: they only
    // load when the player opens the Palpedia, and EnsureWidgets reports them on its own.
    auto ProbeGameSymbols() -> bool
    {
        struct Probe
        {
            const RC::CharType* Path;
            const RC::CharType* Function; // nullptr = the object itself is enough
        };
        static constexpr Probe probes[]{
                {STR("/Script/UMG.Default__WidgetBlueprintLibrary"), STR("Create")},
                {STR("/Script/UMG.Default__SlateBlueprintLibrary"), STR("LocalToAbsolute")},
                {STR("/Script/UMG.Default__SlateBlueprintLibrary"), STR("AbsoluteToLocal")},
                {STR("/Script/UMG.Default__SlateBlueprintLibrary"), STR("GetLocalSize")},
                {STR("/Script/Engine.Default__KismetRenderingLibrary"), STR("ImportFileAsTexture2D")},
                {STR("/Script/UMG.Default__CanvasPanel"), STR("AddChildToCanvas")},
                {STR("/Script/UMG.Default__Image"), STR("SetBrushFromTexture")},
                {STR("/Script/UMG.Default__TextBlock"), STR("SetFont")},
                {STR("/Script/UMG.Default__Widget"), STR("GetCachedGeometry")},
                {STR("/Script/CommonUI.Default__CommonButtonBase"), STR("HandleButtonClicked")},
                {STR("/Script/Pal.Default__PalUIUtility"), STR("GetPalElementTypeName")},
                {STR("/Script/Pal.Default__PalUIUtility"), STR("GetWorkSuitabilityName")},
                {STR("/Script/Pal.PalTextBlockBase"), nullptr},
                {STR("/Script/Engine.Default__KismetTextLibrary"), STR("Conv_StringToText")},
                {STR("/Script/Engine.Default__KismetTextLibrary"), STR("Conv_TextToString")},
        };

        for (const auto& probe : probes)
        {
            auto* object = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(nullptr, nullptr, probe.Path);
            const bool ok = object && (!probe.Function || object->GetFunctionByNameInChain(probe.Function));
            if (!ok)
            {
                RC::Output::send<RC::LogLevel::Error>(std::format(
                        STR("[PalpediaEnhancer] DISABLED: the game build no longer exposes {}{}{}. This mod was built against Palworld build {}; "
                            "wait for an update of the mod.\n"),
                        probe.Path,
                        probe.Function ? STR(":") : STR(""),
                        probe.Function ? probe.Function : STR(""),
                        PALPEDIA_TESTED_BUILD));
                return false;
            }
        }
        RC::Output::send<RC::LogLevel::Normal>(STR("[PalpediaEnhancer] Game symbol probe passed\n"));
        return true;
    }

    // Row setup is the only reliable signal that the Palpedia is building, since the screen class
    // has no Construct of its own.
    //
    // The function must be resolved by hand first. UE4SS's RegisterHook(name) overload passes the
    // result of StaticFindObject straight into a dereference with no null check, and it throws when
    // a function's native entry point is not populated yet.
    // Three distinct outcomes, and conflating the first two is what silently disabled the mod:
    // a Blueprint that has not loaded yet is not a failure, it is a "try again later". The Palpedia
    // Blueprint only loads when the player opens the Palpedia, which can be minutes after launch.
    enum class HookResult
    {
        NotLoadedYet,
        Failed,
        Hooked,
    };

    auto TryHook(const RC::StringType& path, RC::Unreal::UnrealScriptFunctionCallable postCallback) -> HookResult
    {
        return TryHook(path, [](RC::Unreal::UnrealScriptFunctionCallableContext&, void*) {}, postCallback);
    }

    auto TryHook(const RC::StringType& path,
                 RC::Unreal::UnrealScriptFunctionCallable preCallback,
                 RC::Unreal::UnrealScriptFunctionCallable postCallback,
                 int32_t* outParamsSize = nullptr) -> HookResult
    {
        auto* function = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(nullptr, nullptr, path);
        if (!function || !function->GetFunc())
        {
            return HookResult::NotLoadedYet;
        }

        if (outParamsSize)
        {
            *outParamsSize = function->GetPropertiesSize();
        }

        try
        {
            RC::Unreal::UObjectGlobals::RegisterHook(function, preCallback, postCallback, this);
            return HookResult::Hooked;
        }
        catch (const std::exception&)
        {
            return HookResult::Failed;
        }
    }

    auto TryRegisterHooks() -> void
    {
        if (m_rowHookRegistered && m_clickHooksRegistered)
        {
            return;
        }

        if (++m_framesSinceHookAttempt < 60)
        {
            return;
        }
        m_framesSinceHookAttempt = 0;

        if (!m_rowHookRegistered)
        {
            // Discovery state is carried in the DisplayInfo argument the game passes to Setup, not
            // in which Setup variant it calls (the game only ever calls Setup). The first parameter
            // is FPalUIPaldex_DisplayInfo, whose detailType (EPalUIPaldexPanelDetailType) sits at
            // 0x28, matching our Discovery enum values 1..3. Reading it here is what makes the
            // discovery and ownership filters work at all.
            // The pre-call runs while the frame locals (the DisplayInfo parameter) are still live;
            // reading them after the call can see already-popped stack, which is a read of garbage
            // at best and a crash surface at worst. Both recording jobs belong in the pre-call.
            constexpr int32_t DetailTypeOffset = 0x28;
            const auto result = TryHook(STR("/Game/Pal/Blueprint/UI/UserInterface/MainMenu/Paldex/WBP_Paldex_List.WBP_Paldex_List_C:Setup"),
                                        [](RC::Unreal::UnrealScriptFunctionCallableContext& context, void* data) {
                                            auto* mod = static_cast<PalpediaEnhancerMod*>(data);
                                            mod->m_ui.RequestAttach(context.Context);

                                            // The frame's own reflected size is the only thing that tells us the
                                            // offset still lands inside it. A patch that shrinks Setup's parameters
                                            // must disable this read rather than let it walk off the frame.
                                            if (mod->m_rowSetupParamsSize <= DetailTypeOffset)
                                            {
                                                if (++mod->m_discoveryFailures == 1)
                                                {
                                                    RC::Output::send<RC::LogLevel::Warning>(
                                                            STR("[PalpediaEnhancer] Setup's parameters are smaller than expected; discovery "
                                                                "state is unavailable and discovery/ownership filters will pass every row\n"));
                                                }
                                                return;
                                            }

                                            if (void* locals = context.TheStack.Locals())
                                            {
                                                // FPalUIPaldex_DisplayInfo: int32 Index at 0x0, FString IndexSuffix
                                                // at 0x8 (TCHAR* Data, int32 Num, int32 Max). Recorded as the row's
                                                // Paldex number for Default/By Number sorting; the Num guard keeps an
                                                // empty or unset FString from being dereferenced.
                                                const auto* bytes = reinterpret_cast<const char*>(locals);
                                                const int32_t index = *reinterpret_cast<const int32_t*>(bytes);
                                                RC::StringType suffix{};
                                                const auto* suffixData = *reinterpret_cast<const wchar_t* const*>(bytes + 0x8);
                                                const int32_t suffixNum = *reinterpret_cast<const int32_t*>(bytes + 0x10);
                                                if (suffixData && suffixNum > 1 && suffixNum < 16)
                                                {
                                                    suffix.assign(suffixData, static_cast<size_t>(suffixNum - 1));
                                                }
                                                mod->m_filter.NoteIndex(context.Context, index, std::move(suffix));

                                                const uint8_t detailType = *reinterpret_cast<const uint8_t*>(
                                                        reinterpret_cast<const char*>(locals) + DetailTypeOffset);

                                                // Never guess an unreadable property: a value outside the enum's
                                                // known range is not a fourth discovery state, it is offset 0x28
                                                // no longer being detailType. Leaving it unrecorded degrades to
                                                // "discovery unknown for this row" rather than a wrong state.
                                                if (detailType > static_cast<uint8_t>(PalpediaEnhancer::Discovery::Captured))
                                                {
                                                    if (++mod->m_discoveryFailures == 1)
                                                    {
                                                        RC::Output::send<RC::LogLevel::Warning>(std::format(
                                                                STR("[PalpediaEnhancer] Row setup detailType read {} is out of range; discovery "
                                                                    "state is unavailable for affected rows\n"),
                                                                static_cast<int>(detailType)));
                                                    }
                                                    return;
                                                }

                                                mod->m_filter.NoteDiscovery(context.Context,
                                                                             static_cast<PalpediaEnhancer::Discovery>(detailType));
                                            }
                                        },
                                        [](RC::Unreal::UnrealScriptFunctionCallableContext&, void*) {},
                                        &m_rowSetupParamsSize);

            if (result == HookResult::Hooked)
            {
                m_rowHookRegistered = true;
                RC::Output::send<RC::LogLevel::Normal>(STR("[PalpediaEnhancer] Hooked Palpedia row setup and discovery state\n"));
            }
            else if (result == HookResult::Failed && ++m_hookFailures >= 15)
            {
                RC::Output::send<RC::LogLevel::Error>(STR("[PalpediaEnhancer] Palpedia row setup refused to hook; the panel will not attach\n"));
                m_rowHookRegistered = true; // stop retrying a genuine refusal
            }
        }

        if (!m_clickHooksRegistered)
        {
            // CommonButtonBase::HandleButtonClicked fires only on a real click. The three
            // BndEvt functions I hooked previously all carry the delegate TYPE name
            // "CommonButtonBaseClicked", but that type is shared by OnClicked, OnHovered and
            // OnUnhovered, so hooking all three made hovering act like clicking.
            const auto result = TryHook(STR("/Script/CommonUI.CommonButtonBase:HandleButtonClicked"),
                                        [](RC::Unreal::UnrealScriptFunctionCallableContext& context, void* data) {
                                            static_cast<PalpediaEnhancerMod*>(data)->m_ui.NotifyButtonClicked(context.Context);
                                        });

            if (result == HookResult::Hooked)
            {
                m_clickHooksRegistered = true;
                RC::Output::send<RC::LogLevel::Normal>(STR("[PalpediaEnhancer] Hooked CommonButtonBase click\n"));
            }
            else if (result == HookResult::Failed)
            {
                m_clickHooksRegistered = true;
                RC::Output::send<RC::LogLevel::Error>(STR("[PalpediaEnhancer] Could not hook button clicks; buttons will not respond\n"));
            }
        }
    }

    bool m_disabled{};
    bool m_probed{};
    // Game thread only, after the constructor. The watcher never writes it; it writes
    // m_pendingSettings and lets ApplyReloadedSettings move the values across.
    PalpediaEnhancer::Settings m_settings{};
    std::filesystem::path m_settingsFile{};

    // The file watch. m_liveReloadSeconds is the only settings value the update thread reads, so
    // it is the only one that has to be atomic.
    std::atomic<int> m_liveReloadSeconds{2};
    std::atomic<bool> m_settingsReloadPending{};
    std::mutex m_settingsMutex;
    PalpediaEnhancer::Settings m_pendingSettings{}; // guarded by m_settingsMutex
    FileStamp m_appliedStamp{};                     // update thread only
    FileStamp m_candidateStamp{};                   // update thread only
    std::chrono::steady_clock::time_point m_lastPoll{};
    std::chrono::steady_clock::time_point m_candidateSince{};
    bool m_lastPollDone{};
    bool m_watchWarned{};
    int m_readRetries{}; // update thread only

    // The chords the input thread matches a press against, and every chord already registered
    // with UE4SS. The table is read off-thread, so it is guarded; the registry is touched from
    // the constructor and the game-thread reload only.
    std::mutex m_bindMutex;
    PalpediaEnhancer::KeyChord m_liveToggleUnread{}; // guarded by m_bindMutex
    PalpediaEnhancer::KeyChord m_liveClearAll{};     // guarded by m_bindMutex
    PalpediaEnhancer::KeyChord m_liveMarkAllSeen{};  // guarded by m_bindMutex
    std::unordered_set<uint32_t> m_registeredChords{};

    // Written once by the update thread before Done is released, read on the game thread after.
    struct UpdateState
    {
        std::optional<std::string> Newer{};
        std::atomic<bool> Done{};
    };
    std::shared_ptr<UpdateState> m_update{std::make_shared<UpdateState>()};
    bool m_updateStarted{};
    bool m_updateAnnounced{};
    std::chrono::steady_clock::time_point m_lastAnnounceTry{};

    std::atomic<bool> m_unreadTogglePending{};
    std::atomic<bool> m_clearAllPending{};
    std::atomic<bool> m_markAllSeenPending{};
    bool m_pendingRefresh{};
    bool m_sortPending{};
    bool m_rowHookRegistered{};
    bool m_clickHooksRegistered{};
    bool m_discoveryHooksRegistered{};
    int m_hookFailures{};
    int m_discoveryFailures{};
    int m_framesSinceHookAttempt{};
    int32_t m_rowSetupParamsSize{};
    PalpediaEnhancer::PalpediaFilter m_filter{};
    PalpediaEnhancer::PalpediaUi m_ui{};
};

#define PALPEDIA_ENHANCER_API __declspec(dllexport)
extern "C"
{
    PALPEDIA_ENHANCER_API RC::CppUserModBase* start_mod()
    {
        return new PalpediaEnhancerMod();
    }

    PALPEDIA_ENHANCER_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
