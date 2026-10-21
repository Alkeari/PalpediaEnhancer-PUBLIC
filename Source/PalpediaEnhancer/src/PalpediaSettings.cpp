#include <PalpediaSettings.hpp>

#include <PalpediaFilter.hpp>

#include <algorithm>
#include <cwctype>
#include <format>
#include <fstream>
#include <string>

#include <DynamicOutput/DynamicOutput.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace PalpediaEnhancer
{
    using namespace RC;

    namespace
    {
        bool g_verbose{};

        // Function-local so nothing depends on static initialization order: the UI reads this
        // long after the mod's constructor published the settings it loaded.
        auto ActiveSettingsStorage() -> Settings&
        {
            static Settings settings{};
            return settings;
        }

        auto Trim(StringType text) -> StringType
        {
            const auto notSpace = [](wchar_t c) { return !std::iswspace(c); };
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
            text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
            return text;
        }

        auto Upper(StringType text) -> StringType
        {
            for (auto& c : text)
            {
                c = static_cast<wchar_t>(std::towupper(c));
            }
            return text;
        }

        auto ParseBool(const StringType& value) -> std::optional<bool>
        {
            const auto v = Upper(Trim(value));
            if (v == STR("1") || v == STR("TRUE") || v == STR("YES") || v == STR("ON"))
            {
                return true;
            }
            if (v == STR("0") || v == STR("FALSE") || v == STR("NO") || v == STR("OFF"))
            {
                return false;
            }
            return std::nullopt;
        }

        auto ParseInt(const StringType& value, int lowest, int highest) -> std::optional<int>
        {
            const auto text = Trim(value);
            if (text.empty())
            {
                return std::nullopt;
            }
            int number = 0;
            for (const auto c : text)
            {
                if (c < L'0' || c > L'9')
                {
                    return std::nullopt;
                }
                number = number * 10 + (c - L'0');
                if (number > highest)
                {
                    return std::nullopt;
                }
            }
            if (number < lowest)
            {
                return std::nullopt;
            }
            return number;
        }

        // The sort orders the Sort panel offers, by the name a player writes in Settings.ini.
        // SortMode::Number is deliberately absent: the panel cannot show or leave that state, so
        // a config could put the list in an order no button returns from.
        struct SortModeName
        {
            const CharType* Name;
            SortMode Mode;
        };
        constexpr SortModeName SortModeNames[] = {
                {STR("DEFAULT"), SortMode::None},
                {STR("NAME"), SortMode::Name},
                {STR("ELEMENT"), SortMode::Element},
                {STR("WORKTYPE"), SortMode::WorkSuitability},
                {STR("DISCOVERY"), SortMode::Discovery},
                {STR("OWNERSHIP"), SortMode::CapturedCount},
                {STR("UNREADFIRST"), SortMode::UnreadFirst},
        };

        struct SearchTargetName
        {
            const CharType* Name;
            SearchTarget Target;
        };
        constexpr SearchTargetName SearchTargetNames[] = {
                {STR("NAME"), SearchTarget::Name},
                {STR("DROPS"), SearchTarget::Drops},
                {STR("RANCH"), SearchTarget::Ranch},
                {STR("DESC"), SearchTarget::Description},
        };
    } // namespace

    auto AssetsDirectory() -> std::filesystem::path
    {
        HMODULE module{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&AssetsDirectory),
                           &module);
        wchar_t buffer[MAX_PATH]{};
        GetModuleFileNameW(module, buffer, MAX_PATH);
        return std::filesystem::path(buffer).parent_path().parent_path() / L"Assets";
    }

    auto SetVerboseLogging(bool enabled) -> void
    {
        g_verbose = enabled;
    }

    auto VerboseLogging() -> bool
    {
        return g_verbose;
    }

    auto Verbose(const StringType& message) -> void
    {
        if (g_verbose)
        {
            Output::send<LogLevel::Normal>(STR("{}"), message);
        }
    }

    auto Settings::ParseChord(StringType text) -> std::optional<KeyChord>
    {
        text = Upper(Trim(text));
        if (text.empty() || text == STR("NONE") || text == STR("OFF"))
        {
            return KeyChord{}; // explicitly unbound
        }

        KeyChord chord{};
        size_t start = 0;
        while (start <= text.size())
        {
            const auto plus = text.find(L'+', start);
            const auto part = Trim(text.substr(start, plus == StringType::npos ? StringType::npos : plus - start));
            const bool last = plus == StringType::npos;

            if (!last)
            {
                if (part == STR("CTRL") || part == STR("CONTROL"))
                {
                    chord.Ctrl = true;
                }
                else if (part == STR("SHIFT"))
                {
                    chord.Shift = true;
                }
                else if (part == STR("ALT"))
                {
                    chord.Alt = true;
                }
                else
                {
                    return std::nullopt;
                }
                start = plus + 1;
                continue;
            }

            // The final token is the key itself: a letter, a digit, or F1..F12.
            if (part.size() == 1 && ((part[0] >= L'A' && part[0] <= L'Z') || (part[0] >= L'0' && part[0] <= L'9')))
            {
                chord.Key = static_cast<Input::Key>(part[0]);
            }
            else if (part.size() >= 2 && part[0] == L'F')
            {
                int number = 0;
                for (size_t i = 1; i < part.size(); ++i)
                {
                    if (part[i] < L'0' || part[i] > L'9')
                    {
                        return std::nullopt;
                    }
                    number = number * 10 + (part[i] - L'0');
                }
                if (number < 1 || number > 12)
                {
                    return std::nullopt;
                }
                chord.Key = static_cast<Input::Key>(static_cast<int>(Input::Key::F1) + number - 1);
            }
            else
            {
                return std::nullopt;
            }
            chord.Bound = true;
            return chord;
        }
        return std::nullopt;
    }

    auto Settings::ParseSortMode(StringType text) -> std::optional<SortMode>
    {
        text = Upper(Trim(text));
        for (const auto& candidate : SortModeNames)
        {
            if (text == candidate.Name)
            {
                return candidate.Mode;
            }
        }
        return std::nullopt;
    }

    auto Settings::ParseSearchTargets(StringType text) -> std::optional<uint8_t>
    {
        uint8_t mask = 0;
        size_t start = 0;
        while (start <= text.size())
        {
            const auto comma = text.find(L',', start);
            const auto part = Upper(Trim(text.substr(start, comma == StringType::npos ? StringType::npos : comma - start)));
            if (part.empty())
            {
                return std::nullopt;
            }
            bool matched = false;
            for (const auto& candidate : SearchTargetNames)
            {
                if (part == candidate.Name)
                {
                    mask |= TargetBit(candidate.Target);
                    matched = true;
                    break;
                }
            }
            if (!matched)
            {
                return std::nullopt;
            }
            if (comma == StringType::npos)
            {
                break;
            }
            start = comma + 1;
        }
        if (mask == 0)
        {
            return std::nullopt;
        }
        return mask;
    }

    auto ReadSettings(const std::filesystem::path& file) -> SettingsReading
    {
        SettingsReading reading{};
        Settings& settings = reading.Values;

        std::wifstream in(file);
        if (!in)
        {
            return reading;
        }
        reading.Readable = true;

        std::wstring line{};
        int lineNumber = 0;
        while (std::getline(in, line))
        {
            ++lineNumber;
            auto text = Trim(line);
            if (text.empty() || text[0] == L';' || text[0] == L'#' || text[0] == L'[')
            {
                continue;
            }
            const auto equals = text.find(L'=');
            if (equals == StringType::npos)
            {
                continue;
            }
            const auto key = Upper(Trim(text.substr(0, equals)));
            const auto value = Trim(text.substr(equals + 1));

            const auto unreadable = [&](const CharType* name) {
                Output::send<LogLevel::Warning>(std::format(
                        STR("[PalpediaEnhancer] Settings.ini line {}: cannot read \"{}\" for {}; keeping the default\n"), lineNumber, value, name));
            };
            const auto chordSetting = [&](KeyChord& target, const CharType* name) {
                if (auto chord = Settings::ParseChord(value))
                {
                    target = *chord;
                }
                else
                {
                    unreadable(name);
                }
            };
            const auto boolSetting = [&](bool& target, const CharType* name) {
                if (auto parsed = ParseBool(value))
                {
                    target = *parsed;
                }
                else
                {
                    unreadable(name);
                }
            };

            if (key == STR("TOGGLEUNREADKEY"))
            {
                chordSetting(settings.ToggleUnread, STR("ToggleUnreadKey"));
            }
            else if (key == STR("CLEARALLKEY"))
            {
                chordSetting(settings.ClearAll, STR("ClearAllKey"));
            }
            else if (key == STR("MARKALLSEENKEY"))
            {
                chordSetting(settings.MarkAllSeen, STR("MarkAllSeenKey"));
            }
            else if (key == STR("VERBOSELOGGING"))
            {
                boolSetting(settings.VerboseLogging, STR("VerboseLogging"));
            }
            else if (key == STR("REMEMBERSTATE"))
            {
                boolSetting(settings.RememberState, STR("RememberState"));
            }
            else if (key == STR("SHOWMARKALLSEENBUTTON"))
            {
                boolSetting(settings.ShowMarkAllSeenButton, STR("ShowMarkAllSeenButton"));
            }
            else if (key == STR("CHECKFORUPDATES"))
            {
                boolSetting(settings.CheckForUpdates, STR("CheckForUpdates"));
            }
            else if (key == STR("DEFAULTSORTDESCENDING"))
            {
                boolSetting(settings.DefaultSortDescending, STR("DefaultSortDescending"));
            }
            else if (key == STR("DEFAULTUNREADONLY"))
            {
                boolSetting(settings.DefaultUnreadOnly, STR("DefaultUnreadOnly"));
            }
            else if (key == STR("DEFAULTSORT"))
            {
                if (auto mode = Settings::ParseSortMode(value))
                {
                    settings.DefaultSort = *mode;
                }
                else
                {
                    unreadable(STR("DefaultSort"));
                }
            }
            else if (key == STR("DEFAULTSEARCHTARGETS"))
            {
                if (auto targets = Settings::ParseSearchTargets(value))
                {
                    settings.DefaultSearchTargets = *targets;
                }
                else
                {
                    unreadable(STR("DefaultSearchTargets"));
                }
            }
            else if (key == STR("LIVERELOADSECONDS"))
            {
                if (auto seconds = ParseInt(value, 0, 60))
                {
                    settings.LiveReloadSeconds = *seconds;
                }
                else
                {
                    unreadable(STR("LiveReloadSeconds"));
                }
            }
            else
            {
                Output::send<LogLevel::Warning>(std::format(STR("[PalpediaEnhancer] Settings.ini line {}: unknown setting \"{}\"\n"), lineNumber, key));
                continue;
            }

            ++reading.Recognized;
        }

        return reading;
    }

    auto Settings::Summary(const Settings& settings) -> StringType
    {
        return std::format(STR("VerboseLogging={} RememberState={} ToggleUnread={} ClearAll={} MarkAllSeen={} ShowMarkAllSeenButton={} CheckForUpdates={} ")
                                   STR("DefaultSort={} DefaultSortDescending={} DefaultUnreadOnly={} DefaultSearchTargets={} LiveReloadSeconds={}"),
                           settings.VerboseLogging,
                           settings.RememberState,
                           settings.ToggleUnread.Bound,
                           settings.ClearAll.Bound,
                           settings.MarkAllSeen.Bound,
                           settings.ShowMarkAllSeenButton,
                           settings.CheckForUpdates,
                           static_cast<int>(settings.DefaultSort),
                           settings.DefaultSortDescending,
                           settings.DefaultUnreadOnly,
                           static_cast<int>(settings.DefaultSearchTargets),
                           settings.LiveReloadSeconds);
    }

    auto Settings::Load(const std::filesystem::path& file) -> Settings
    {
        const auto reading = ReadSettings(file);
        if (!reading.Readable)
        {
            Output::send<LogLevel::Normal>(std::format(STR("[PalpediaEnhancer] No Settings.ini at {}; using defaults\n"), file.wstring()));
            return reading.Values;
        }
        Output::send<LogLevel::Normal>(std::format(STR("[PalpediaEnhancer] Settings loaded: {}\n"), Summary(reading.Values)));
        return reading.Values;
    }

    auto SetActiveSettings(const Settings& settings) -> void
    {
        ActiveSettingsStorage() = settings;
    }

    auto ActiveSettings() -> const Settings&
    {
        return ActiveSettingsStorage();
    }
} // namespace PalpediaEnhancer
