#include <PalpediaLocalization.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <unordered_map>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <PalpediaSettings.hpp>

namespace PalpediaEnhancer::Loc
{
    using namespace RC;
    using namespace RC::Unreal;

    namespace
    {
        // English is the built-in table and the fallback for any key a language file leaves out.
        // Keys are the single source of truth; every Assets/lang/*.ini carries the same keys.
        const std::unordered_map<StringType, StringType>& English()
        {
            static const std::unordered_map<StringType, StringType> table{
                    {STR("filters"), STR("Filters")},
                    {STR("sort"), STR("Sort")},
                    {STR("filter"), STR("Filter")},
                    {STR("close"), STR("Close")},
                    {STR("default"), STR("Default")},
                    {STR("by_name"), STR("By Name")},
                    {STR("by_element"), STR("By Element")},
                    {STR("by_work_type"), STR("By Work Type")},
                    {STR("by_discovery"), STR("By Discovery")},
                    {STR("by_ownership"), STR("By Ownership")},
                    {STR("unread_first"), STR("Unread First")},
                    {STR("descending"), STR("Descending")},
                    {STR("new"), STR("New")},
                    {STR("clear_all"), STR("Clear All")},
                    {STR("mark_all_seen"), STR("Mark All Seen")},
                    {STR("update_available"), STR("Palpedia Enhancer {0} is available.")},
                    {STR("unowned"), STR("Unowned")},
                    {STR("owned"), STR("Owned")},
                    {STR("owned_5plus"), STR("Owned 5+")},
                    {STR("undiscovered"), STR("Undiscovered")},
                    {STR("seen"), STR("Seen")},
                    {STR("caught"), STR("Caught")},
                    {STR("name"), STR("Name")},
                    {STR("drops"), STR("Drops")},
                    {STR("ranch"), STR("Ranch")},
                    {STR("desc"), STR("Desc")},
                    {STR("clear_search"), STR("X")},
                    {STR("search_hint"), STR("Search")},
                    {STR("sec_ownership"), STR("Ownership")},
                    {STR("sec_met"), STR("Met Status")},
                    {STR("sec_type"), STR("Pal Type")},
                    {STR("sec_work"), STR("Work Suitability")},
                    {STR("sec_sort_by"), STR("Sort By")},
                    {STR("sec_order"), STR("Order")},

                    {STR("tt_search_button"), STR("Search Palpedia entries by name, drop, ranch produce, or description.")},
                    {STR("tt_sort_button"), STR("Choose how the Palpedia list is ordered.")},
                    {STR("tt_filter_button"), STR("Choose which Palpedia entries are shown.")},
                    {STR("tt_close"), STR("Close this window.")},
                    {STR("tt_clear_search"), STR("Clear the search and go back to searching by name only.")},
                    {STR("tt_default"), STR("The game's own order. Picking an active sort again also returns here.")},
                    {STR("tt_by_name"), STR("Order by name.")},
                    {STR("tt_by_element"), STR("Group by element, in the game's own order. Dual-type Pals group under their first.")},
                    {STR("tt_by_work_type"), STR("Group by a Pal's first work suitability, in the game's own order. Pals with none go last.")},
                    {STR("tt_by_discovery"), STR("Group by whether a Pal is caught, seen, or new.")},
                    {STR("tt_by_ownership"), STR("Owned 5+ first, then owned, then seen but unowned, then never seen.")},
                    {STR("tt_unread_first"), STR("Float unopened entries to the top.")},
                    {STR("tt_descending"), STR("Flip the sort order: descending applies last or highest first.")},
                    {STR("tt_new"), STR("Show only entries you have not opened yet.")},
                    {STR("tt_clear_all"), STR("Turn every filter off.")},
                    {STR("tt_mark_all_seen"), STR("Mark every Palpedia entry as seen, clearing the new dots without opening each one.")},
                    {STR("tt_unowned"), STR("Show only Pals you have never caught.")},
                    {STR("tt_owned"), STR("Show only Pals caught once but not to bonus completion.")},
                    {STR("tt_owned_5plus"), STR("Show only Pals caught five or more times.")},
                    {STR("tt_undiscovered"), STR("Show only Pals you have never seen.")},
                    {STR("tt_seen"), STR("Show only Pals you have seen but not caught.")},
                    {STR("tt_caught"), STR("Show only Pals you have caught.")},
                    {STR("tt_element"), STR("Show only Pals of this element.")},
                    {STR("tt_work"), STR("Show only Pals with this work suitability.")},
                    {STR("tt_name"), STR("Search by Pal name.")},
                    {STR("tt_drops"), STR("Search by what a Pal drops when defeated or captured.")},
                    {STR("tt_ranch"), STR("Pals that produce the item at the ranch.")},
                    {STR("tt_desc"), STR("Search the Pal's partner skill and Palpedia description text.")},
            };
            return table;
        }

        std::unordered_map<StringType, StringType> g_loaded{};
        StringType g_language{STR("en")};
        bool g_attempted = false;

        auto Trim(StringType text) -> StringType
        {
            const auto first = text.find_first_not_of(STR(" \t\r\n"));
            if (first == StringType::npos)
            {
                return {};
            }
            const auto last = text.find_last_not_of(STR(" \t\r\n"));
            return text.substr(first, last - first + 1);
        }

        // The engine's view of the player's language: "en", "ja", "zh-Hans", "pt-BR", ...
        auto CurrentLanguage() -> StringType
        {
            auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetInternationalizationLibrary"));
            auto* function = library ? library->GetFunctionByNameInChain(STR("GetCurrentLanguage")) : nullptr;
            if (!function)
            {
                return {};
            }
            struct Params
            {
                FString ReturnValue{};
            } params{};
            library->ProcessEvent(function, &params);
            const CharType* chars = *params.ReturnValue;
            return chars ? Trim(StringType{chars}) : StringType{};
        }

        // key=value lines, UTF-8, '#' or ';' comments. Values may contain '=' after the first.
        auto LoadFile(const std::filesystem::path& file, std::unordered_map<StringType, StringType>& into) -> int
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
            {
                return -1;
            }
            std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
                static_cast<unsigned char>(bytes[2]) == 0xBF)
            {
                bytes.erase(0, 3);
            }
            const auto text = RC::to_wstring(bytes);
            int count = 0;
            size_t pos = 0;
            while (pos <= text.size())
            {
                auto end = text.find(L'\n', pos);
                if (end == StringType::npos)
                {
                    end = text.size();
                }
                auto line = Trim(text.substr(pos, end - pos));
                pos = end + 1;
                if (line.empty() || line[0] == L'#' || line[0] == L';' || line[0] == L'[')
                {
                    continue;
                }
                const auto equals = line.find(L'=');
                if (equals == StringType::npos)
                {
                    continue;
                }
                const auto key = Trim(line.substr(0, equals));
                const auto value = Trim(line.substr(equals + 1));
                if (!key.empty() && !value.empty())
                {
                    into[key] = value;
                    ++count;
                }
            }
            return count;
        }
    } // namespace

    auto EnsureLoaded() -> void
    {
        if (g_attempted)
        {
            return;
        }
        g_attempted = true;

        const auto language = CurrentLanguage();
        if (language.empty() || language == STR("en"))
        {
            Output::send<LogLevel::Normal>(std::format(STR("[PalpediaEnhancer] UI language: {} (built-in)\n"), language.empty() ? STR("unknown") : language));
            return;
        }

        const auto dir = AssetsDirectory() / L"lang";
        // Base language first, then the regional file over it, so pt-BR only has to carry what
        // differs from pt, and an unknown region still gets its base language.
        std::unordered_map<StringType, StringType> loaded{};
        int total = 0;
        StringType used{};
        const auto dash = language.find(L'-');
        if (dash != StringType::npos)
        {
            const auto base = language.substr(0, dash);
            const auto n = LoadFile(dir / (base + STR(".ini")), loaded);
            if (n > 0)
            {
                total += n;
                used = base;
            }
        }
        const auto n = LoadFile(dir / (language + STR(".ini")), loaded);
        if (n > 0)
        {
            total += n;
            used = language;
        }

        if (total <= 0)
        {
            Output::send<LogLevel::Normal>(std::format(STR("[PalpediaEnhancer] UI language {}: no translation file, using English\n"), language));
            return;
        }
        g_loaded = std::move(loaded);
        g_language = used;
        Output::send<LogLevel::Normal>(std::format(STR("[PalpediaEnhancer] UI language {}: {} strings loaded from {}.ini\n"), language, total, used));
    }

    auto T(const CharType* key) -> const StringType&
    {
        if (const auto found = g_loaded.find(key); found != g_loaded.end())
        {
            return found->second;
        }
        const auto& english = English();
        if (const auto found = english.find(key); found != english.end())
        {
            return found->second;
        }
        // A missing key is a programming error; show it rather than a blank button.
        static std::unordered_map<StringType, StringType> missing{};
        return missing.try_emplace(key, key).first->second;
    }

    auto Language() -> const StringType&
    {
        return g_language;
    }
} // namespace PalpediaEnhancer::Loc
