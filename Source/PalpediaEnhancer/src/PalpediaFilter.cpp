#include <PalpediaFilter.hpp>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <format>
#include <functional>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <PalpediaText.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UStruct.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace PalpediaEnhancer
{
    using namespace RC;
    using namespace RC::Unreal;

    namespace
    {
        constexpr uint8_t SlateVisible = 0;
        constexpr uint8_t SlateCollapsed = 1;

        // Visibility is declared on UMG.Widget, not on the Blueprint class, so the lookup has to
        // walk the class chain. Returns -1 when it cannot be read, which is never a visibility
        // state: reporting a guess here silently turned every row into a match.
        auto ReadVisibility(UObject* widget) -> int
        {
            if (!widget)
            {
                return -1;
            }
            auto* value = widget->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility"));
            return value ? static_cast<int>(*value) : -1;
        }

        auto WriteVisibility(UObject* widget, uint8_t visibility) -> void
        {
            if (!widget)
            {
                return;
            }
            // Writing the Visibility property alone leaves the underlying Slate widget untouched,
            // so the change would not render. SetVisibility is the only route that takes effect.
            auto* function = widget->GetFunctionByNameInChain(STR("SetVisibility"));
            if (!function)
            {
                return;
            }
            struct Params
            {
                uint8_t InVisibility;
            } params{visibility};
            widget->ProcessEvent(function, &params);
        }

        // Unicode-aware: std::towlower only folds ASCII in the default C locale, which made a
        // Cyrillic, Greek or accented query case-sensitive. CharLowerBuffW folds every script
        // Windows knows, so search behaves the same in every game language.
        auto ToLower(StringType value) -> StringType
        {
            if (!value.empty())
            {
                CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
            }
            return value;
        }

        // Drop item ids are internal names like "Flame_Organ", while players type "flame organ".
        // Stripping both separators makes the two compare equal without touching name search, where
        // spaces are meaningful.
        auto NormalizeForDrops(StringType value) -> StringType
        {
            value.erase(std::remove_if(value.begin(), value.end(), [](auto character) {
                            return character == L'_' || character == L' ';
                        }),
                        value.end());
            return value;
        }

        // FindFirstOf walks every UObject in the process; it was being called once per text read,
        // several times per species, for ~300 species per search. The game instance lives for the
        // whole process, so it is resolved once and kept.
        auto GameInstance() -> UObject*
        {
            static UObject* cached = nullptr;
            if (!cached)
            {
                cached = UObjectGlobals::FindFirstOf(STR("PalGameInstance"));
            }
            return cached;
        }

        // ProcessEvent writes into our local Params struct using the function's own reflected
        // parameter layout, not sizeof(Params). If a patch changes that layout and our guessed
        // struct is smaller than the real one, the engine writes past our stack local instead of
        // failing loudly. GetStructureSize (properties size aligned to the struct's own minimum
        // alignment) is what actually corresponds to a C++ sizeof(), unlike the unaligned
        // GetPropertiesSize. Checked once per call site and logged once, never guessed past.
        auto ParamsSizeMatches(UFunction* function, size_t expectedSize, const CharType* functionName, bool& warned) -> bool
        {
            const auto actual = static_cast<size_t>(function->GetStructureSize());
            if (actual == expectedSize)
            {
                return true;
            }
            if (!warned)
            {
                warned = true;
                Output::send<LogLevel::Warning>(
                        std::format(STR("[PalpediaEnhancer] {} parameters are {} bytes, expected {}; skipping this read\n"),
                                    functionName,
                                    actual,
                                    expectedSize));
            }
            return false;
        }

        // UE's TArray layout, read as a plain struct. Only ever read, never resized.
        template <typename T>
        struct RawArray
        {
            T* Data{};
            int32_t Num{};
            int32_t Max{};
        };

        // The character database, reached through the game instance rather than a cached pointer.
        // Reading it live is what makes element and work filters correct on a modded setup:
        // PalSchema patches these rows at runtime, so a table shipped with the mod would be wrong
        // for exactly the players most likely to notice.
        auto CharacterDatabase() -> UObject*
        {
            auto* gameInstance = GameInstance();
            if (!gameInstance)
            {
                return nullptr;
            }
            auto** database = gameInstance->GetValuePtrByPropertyNameInChain<UObject*>(STR("DatabaseCharacterParameter"));
            return (database && *database) ? *database : nullptr;
        }

        auto ReadElements(UObject* database, FName characterId) -> uint16_t
        {
            auto* function = database->GetFunctionByNameInChain(STR("GetElementType"));
            if (!function)
            {
                return 0;
            }

            struct Params
            {
                FName RowName{};
                uint8_t Element1{};
                uint8_t Element2{};
            } params{characterId, 0, 0};

            static bool warned = false;
            if (!ParamsSizeMatches(function, sizeof(Params), STR("GetElementType"), warned))
            {
                return 0;
            }
            database->ProcessEvent(function, &params);

            uint16_t mask = 0;
            for (const uint8_t element : {params.Element1, params.Element2})
            {
                if (element >= FirstElement && element <= LastElement)
                {
                    mask |= static_cast<uint16_t>(1u << element);
                }
            }
            return mask;
        }

        auto ReadWorkSuitabilities(UObject* database, FName characterId) -> uint16_t
        {
            auto* function = database->GetFunctionByNameInChain(STR("GetWorkSuitability"));
            if (!function)
            {
                return 0;
            }

            // The out array is allocated by the callee. It is read and then left alone; releasing
            // it is the engine's business, and freeing it here would be freeing another
            // allocator's memory.
            struct Params
            {
                FName RowName{};
                RawArray<uint8_t> WorkSuitabilities{};
            } params{characterId, {}};

            static bool warned = false;
            if (!ParamsSizeMatches(function, sizeof(Params), STR("GetWorkSuitability"), warned))
            {
                return 0;
            }
            database->ProcessEvent(function, &params);

            uint16_t mask = 0;
            const auto& array = params.WorkSuitabilities;
            if (!array.Data || array.Num <= 0)
            {
                return mask;
            }

            for (int32_t index = 0; index < array.Num; ++index)
            {
                const uint8_t suitability = array.Data[index];
                if (suitability >= FirstWorkSuitability && suitability <= LastWorkSuitability)
                {
                    mask |= static_cast<uint16_t>(1u << suitability);
                }
            }
            return mask;
        }

        auto PropertyOffset(UStruct* owner, const CharType* name) -> int32_t
        {
            if (!owner)
            {
                return -1;
            }
            for (FProperty* property : owner->ForEachProperty())
            {
                if (property->GetName() == name)
                {
                    return property->GetOffset_Internal();
                }
            }
            return -1;
        }

        // FText out-parameter from a (WorldContextObject, FName, FText&) PalUIUtility call, or from
        // the localized text tables. Offsets come from reflection so no layout is ever guessed.
        auto CallTextOut(UObject* target, UFunction* function, const CharType* idParam, FName id, const CharType* outParam, UObject* context) -> StringType
        {
            const auto contextOffset = PropertyOffset(function, STR("WorldContextObject"));
            const auto idOffset = PropertyOffset(function, idParam);
            const auto outOffset = PropertyOffset(function, outParam);
            if (idOffset < 0 || outOffset < 0)
            {
                return {};
            }
            std::vector<uint8_t> parms(static_cast<size_t>(function->GetStructureSize()), 0);
            if (contextOffset >= 0)
            {
                *reinterpret_cast<UObject**>(parms.data() + contextOffset) = context;
            }
            *reinterpret_cast<FName*>(parms.data() + idOffset) = id;
            target->ProcessEvent(function, parms.data());
            return Text::ToString(*reinterpret_cast<const Text::RawText*>(parms.data() + outOffset));
        }

        auto UiUtilityText(const CharType* functionName, FName characterId) -> StringType
        {
            auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUIUtility"));
            auto* gameInstance = GameInstance();
            auto* function = library ? library->GetFunctionByNameInChain(functionName) : nullptr;
            if (!function || !gameInstance)
            {
                return {};
            }
            // PalUIUtility names the out text either outName or OutText depending on the function.
            auto text = CallTextOut(library, function, STR("CharacterID"), characterId, STR("outName"), gameInstance);
            if (text.empty())
            {
                text = CallTextOut(library, function, STR("CharacterID"), characterId, STR("OutText"), gameInstance);
            }
            return text;
        }

        auto ItemName(FName itemId) -> StringType
        {
            auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUIUtility"));
            auto* gameInstance = GameInstance();
            auto* function = library ? library->GetFunctionByNameInChain(STR("GetItemName")) : nullptr;
            if (!function || !gameInstance)
            {
                return {};
            }
            return CallTextOut(library, function, STR("StaticItemId"), itemId, STR("outName"), gameInstance);
        }

        // The partner skill description is the "first activated info" text
        // (PAL_FIRST_SPAWN_DESC_<CharacterID>), reachable through PalUIUtility's own accessor. It
        // is rich text: items appear as <itemName id=|Wool| style=|...|/>, the ranch as
        // <mapObjectName id=|MonsterFarm|/>, work types as <uiCommon id=|COMMON_WORK_...|/>. The
        // tags are expanded here into searchable words: the tag id itself, plus the localized item
        // name for itemName tags, plus "ranch" for the ranch object. Other markup is stripped.
        auto ExpandRichText(StringType text, const std::function<StringType(const StringType&)>& itemName) -> StringType
        {
            StringType out{};
            out.reserve(text.size());
            size_t pos = 0;
            while (pos < text.size())
            {
                const auto open = text.find(L'<', pos);
                if (open == StringType::npos)
                {
                    out += text.substr(pos);
                    break;
                }
                out += text.substr(pos, open - pos);
                const auto close = text.find(L'>', open);
                if (close == StringType::npos)
                {
                    break;
                }
                const auto tag = text.substr(open + 1, close - open - 1);
                const auto idPos = tag.find(STR("id=|"));
                if (idPos != StringType::npos)
                {
                    const auto idEnd = tag.find(L'|', idPos + 4);
                    const auto id = tag.substr(idPos + 4, idEnd == StringType::npos ? StringType::npos : idEnd - idPos - 4);
                    out += L' ';
                    out += id;
                    out += L' ';
                    if (tag.rfind(STR("itemName"), 0) == 0)
                    {
                        out += itemName(id);
                        out += L' ';
                    }
                    else if (id == STR("MonsterFarm"))
                    {
                        out += STR("ranch ");
                    }
                }
                pos = close + 1;
            }
            for (auto& c : out)
            {
                if (c == L'\r' || c == L'\n' || c == L'_')
                {
                    c = L' ';
                }
            }
            return out;
        }

        // The items named in the ranch sentence of the partner skill text and nothing else: every
        // <itemName id=|X|/> tag that shares a sentence with <mapObjectName id=|MonsterFarm|/>.
        // Working on the tags rather than the words keeps it true in every language and handles
        // "Sometimes drops X and Y when assigned to Ranch" for free.
        auto RanchItemIds(const StringType& richText) -> std::vector<StringType>
        {
            std::vector<StringType> ids{};
            size_t start = 0;
            while (start < richText.size())
            {
                auto end = richText.find_first_of(STR(".\r\n"), start);
                if (end == StringType::npos)
                {
                    end = richText.size();
                }
                const auto sentence = richText.substr(start, end - start);
                if (sentence.find(STR("id=|MonsterFarm|")) != StringType::npos)
                {
                    size_t pos = 0;
                    while ((pos = sentence.find(STR("<itemName"), pos)) != StringType::npos)
                    {
                        const auto idPos = sentence.find(STR("id=|"), pos);
                        const auto idEnd = idPos == StringType::npos ? StringType::npos : sentence.find(L'|', idPos + 4);
                        if (idPos == StringType::npos || idEnd == StringType::npos)
                        {
                            break;
                        }
                        ids.push_back(sentence.substr(idPos + 4, idEnd - idPos - 4));
                        pos = idEnd;
                    }
                }
                start = end + 1;
            }
            return ids;
        }

        // Ten ItemIdN slots of FPalDropItemDatabaseRow, located by reflection on the row struct
        // rather than a mirrored C++ struct: the engine packs that row at four-byte alignment,
        // which is exactly what a hand-written mirror got wrong (248 vs 240 bytes) and why the
        // drop search silently read nothing.
        auto ReadDropIds(UObject* database, FName characterId, int32_t level, std::vector<FName>& out) -> bool
        {
            auto* function = database ? database->GetFunctionByNameInChain(STR("GetDropItemData")) : nullptr;
            auto* rowStruct = UObjectGlobals::StaticFindObject<UStruct*>(nullptr, nullptr, STR("/Script/Pal.PalDropItemDatabaseRow"));
            if (!function || !rowStruct)
            {
                return false;
            }
            const auto idOffset = PropertyOffset(function, STR("CharacterID"));
            const auto levelOffset = PropertyOffset(function, STR("Level"));
            const auto outOffset = PropertyOffset(function, STR("OutData"));
            const auto returnOffset = PropertyOffset(function, STR("ReturnValue"));
            if (idOffset < 0 || levelOffset < 0 || outOffset < 0)
            {
                return false;
            }
            std::vector<uint8_t> parms(static_cast<size_t>(function->GetStructureSize()), 0);
            *reinterpret_cast<FName*>(parms.data() + idOffset) = characterId;
            *reinterpret_cast<int32_t*>(parms.data() + levelOffset) = level;
            database->ProcessEvent(function, parms.data());
            if (returnOffset >= 0 && parms[static_cast<size_t>(returnOffset)] == 0)
            {
                return false;
            }
            for (int slot = 1; slot <= 10; ++slot)
            {
                const auto name = std::format(STR("ItemId{}"), slot);
                const auto itemOffset = PropertyOffset(rowStruct, name.c_str());
                if (itemOffset < 0)
                {
                    continue;
                }
                const auto item = *reinterpret_cast<const FName*>(parms.data() + outOffset + itemOffset);
                if (!item.ToString().empty() && item.ToString() != STR("None"))
                {
                    out.push_back(item);
                }
            }
            return true;
        }
    } // namespace

    // These only change state. Applying it needs the active row list, which the caller supplies on
    // the next safe pass.
    auto PalpediaFilter::ToggleUnreadOnly() -> void
    {
        m_unreadOnly = !m_unreadOnly;
    }

    auto PalpediaFilter::SetQuery(const StringType& query) -> void
    {
        m_rawQuery = query;
        m_compiledQuery = Query::CompiledQuery::Parse(query, m_searchTargets);
    }

    auto PalpediaFilter::ToggleDiscovery(Discovery state) -> void
    {
        m_discoveryMask ^= static_cast<uint8_t>(1u << static_cast<uint8_t>(state));
    }

    auto PalpediaFilter::ToggleElement(int element) -> void
    {
        if (element < FirstElement || element > LastElement)
        {
            return;
        }
        m_elementMask ^= static_cast<uint16_t>(1u << element);
    }

    auto PalpediaFilter::ToggleWorkSuitability(int suitability) -> void
    {
        if (suitability < FirstWorkSuitability || suitability > LastWorkSuitability)
        {
            return;
        }
        m_workSuitabilityMask ^= static_cast<uint16_t>(1u << suitability);
    }

    auto PalpediaFilter::ToggleOwnership(Ownership state) -> void
    {
        constexpr uint8_t first = static_cast<uint8_t>(Ownership::Unowned);
        constexpr uint8_t last = static_cast<uint8_t>(Ownership::Complete);
        const uint8_t bit = static_cast<uint8_t>(state);
        if (bit < first || bit > last)
        {
            return;
        }
        m_ownershipMask ^= static_cast<uint8_t>(1u << bit);
    }

    auto PalpediaFilter::ToggleSortMode(SortMode mode) -> void
    {
        // The Sort panel is a single-select list where the active choice toggles itself off:
        // clicking the already-active mode returns to the natural Paldex order.
        m_sortMode = (m_sortMode == mode) ? SortMode::None : mode;
    }

    auto PalpediaFilter::ClearAll() -> void
    {
        m_unreadOnly = false;
        m_discoveryMask = 0;
        m_ownershipMask = 0;
        m_elementMask = 0;
        m_workSuitabilityMask = 0;
        // The query belongs to the search field, which keeps showing whatever the player typed.
        // Clearing it here would leave the box reading one thing and the list showing another.
    }

    auto PalpediaFilter::DiscoveryActive(Discovery state) const -> bool
    {
        return (m_discoveryMask & static_cast<uint8_t>(1u << static_cast<uint8_t>(state))) != 0;
    }

    auto PalpediaFilter::ElementActive(int element) const -> bool
    {
        if (element < FirstElement || element > LastElement)
        {
            return false;
        }
        return (m_elementMask & static_cast<uint16_t>(1u << element)) != 0;
    }

    auto PalpediaFilter::WorkSuitabilityActive(int suitability) const -> bool
    {
        if (suitability < FirstWorkSuitability || suitability > LastWorkSuitability)
        {
            return false;
        }
        return (m_workSuitabilityMask & static_cast<uint16_t>(1u << suitability)) != 0;
    }

    auto PalpediaFilter::OwnershipActive(Ownership state) const -> bool
    {
        constexpr uint8_t first = static_cast<uint8_t>(Ownership::Unowned);
        constexpr uint8_t last = static_cast<uint8_t>(Ownership::Complete);
        const uint8_t bit = static_cast<uint8_t>(state);
        if (bit < first || bit > last)
        {
            return false;
        }
        return (m_ownershipMask & static_cast<uint8_t>(1u << bit)) != 0;
    }

    auto PalpediaFilter::IsFiltering() const -> bool
    {
        return m_unreadOnly || m_discoveryMask != 0 || m_ownershipMask != 0 || m_elementMask != 0 ||
               m_workSuitabilityMask != 0 || !m_compiledQuery.IsEmpty();
    }

    auto PalpediaFilter::NoteDiscovery(UObject* row, Discovery state) -> void
    {
        if (!row)
        {
            return;
        }
        // Caught implies encountered; the row that built as captured is never downgraded by a later
        // setup hook, and the two live screens share this map by address.
        m_discovery[row] = state;
    }

    auto PalpediaFilter::NoteIndex(UObject* row, int32_t index, StringType suffix) -> void
    {
        if (!row)
        {
            return;
        }
        m_paldexIndex[row] = PaldexIndex{index, std::move(suffix)};
    }

    auto PalpediaFilter::ReadEntry(UObject* row, UObject* characterDatabase, bool needTraits) const -> Entry
    {
        Entry entry{};
        entry.Row = row;
        if (const auto found = m_paldexIndex.find(row); found != m_paldexIndex.end())
        {
            entry.PaldexNumber = found->second.Index;
            entry.PaldexSuffix = found->second.Suffix;
        }

        // Spelled with that typo in the game. GetCharacterID exists but returns nothing.
        if (auto* id = row->GetValuePtrByPropertyNameInChain<FName>(STR("ChachedBaseCharacterID")))
        {
            entry.CharacterId = *id;
        }

        // The localized name a player would actually type lives on the row's own text block.
        // baseCharacterName is the internal id, which is not what anyone searches for. Stored
        // lowercased so both search and name sorting agree on case.
        if (auto** nameWidget = row->GetValuePtrByPropertyNameInChain<UObject*>(STR("Text_PalName")); nameWidget && *nameWidget)
        {
            // An FText with no Data is uninitialized rather than empty, and reading it is
            // documented as unsafe. Rows that have not been filled in yet hit this.
            if (auto* text = (*nameWidget)->GetValuePtrByPropertyNameInChain<Text::RawText>(STR("Text")); text && text->HasData())
            {
                entry.DisplayName = ToLower(Text::ToString(*text));
            }
        }

        // The dot's own visibility is the unread state: Collapsed means read. Its inner Dot and
        // Flare images are always Visible and carry no information.
        auto** dot = row->GetValuePtrByPropertyNameInChain<UObject*>(STR("WBP_MainMenu_NewDot"));
        const int visibility = (dot && *dot) ? ReadVisibility(*dot) : -1;
        entry.Readable = visibility >= 0;
        entry.IsUnread = entry.Readable && visibility != SlateCollapsed;

        // The tick shown once every capture bonus tier for this Pal is claimed. The shown state is
        // HitTestInvisible, exactly like the unread dot, so "not Collapsed" is the test. Comparing
        // against Visible instead is what made every complete-bonus Pal read as incomplete.
        if (auto** complete = row->GetValuePtrByPropertyNameInChain<UObject*>(STR("Image_Complete")); complete && *complete)
        {
            entry.CaptureBonusComplete = ReadVisibility(*complete) != SlateCollapsed;
        }

        if (const auto found = m_discovery.find(row); found != m_discovery.end())
        {
            entry.State = found->second;
        }

        // Ownership tiers fall out of the two signals already read. Capturing five or more Pals is
        // exactly when the game marks the capture bonus complete, so Complete means the 5+ tier.
        entry.Owning = entry.CaptureBonusComplete ? Ownership::Complete
                      : (entry.State == Discovery::Captured ? Ownership::Owned : Ownership::Unowned);

        // The two trait reads are the expensive reflected calls, so they are skipped unless a
        // filter or sort actually consumes them.
        const auto idText = entry.CharacterId.ToString();
        if (needTraits && characterDatabase && !idText.empty())
        {
            entry.Elements = ReadElements(characterDatabase, entry.CharacterId);
            entry.WorkSuitabilities = ReadWorkSuitabilities(characterDatabase, entry.CharacterId);
        }

        // Search haystacks come from the per-species cache, so only the first look at a species
        // touches the engine; every later refresh, keystroke included, is string matching.
        const auto referencedTargets = m_compiledQuery.ReferencedTargets();
        const bool wantDrops = SearchTargetActive(SearchTarget::Drops) ||
                                (referencedTargets & TargetBit(SearchTarget::Drops)) != 0;
        const bool wantRanch = SearchTargetActive(SearchTarget::Ranch) ||
                                (referencedTargets & TargetBit(SearchTarget::Ranch)) != 0;
        const bool wantDescription = SearchTargetActive(SearchTarget::Description) ||
                                      (referencedTargets & TargetBit(SearchTarget::Description)) != 0;
        if ((wantDrops || wantRanch || wantDescription) && !idText.empty())
        {
            const auto& text = SpeciesTextFor(characterDatabase, entry.CharacterId, wantDrops, wantRanch || wantDescription);
            if (wantDrops)
            {
                entry.DropIds = text.DropIds;
            }
            if (wantRanch)
            {
                entry.RanchItems = text.RanchItems;
            }
            if (wantDescription)
            {
                entry.Description = text.Description;
            }
        }

        return entry;
    }

    auto PalpediaFilter::SpeciesTextFor(UObject* database, FName characterId, bool drops, bool partner) const -> const SpeciesText&
    {
        auto& text = m_speciesText[characterId.ToString()];

        if (drops && !text.DropsRead)
        {
            text.DropsRead = true;
            if (!database)
            {
                database = CharacterDatabase();
            }
            // The drop row is level scoped. Three bands cover the table's usual splits.
            std::vector<FName> items{};
            for (const int32_t level : {1, 30, 60})
            {
                ReadDropIds(database, characterId, level, items);
            }
            for (const auto& item : items)
            {
                const auto id = item.ToString();
                const auto normalizedId = NormalizeForDrops(ToLower(id));
                if (normalizedId.empty() || std::find(text.DropIds.begin(), text.DropIds.end(), normalizedId) != text.DropIds.end())
                {
                    continue;
                }
                text.DropIds.push_back(normalizedId);
                auto& name = m_itemNames[id];
                if (name.empty())
                {
                    name = ToLower(ItemName(item));
                }
                const auto normalizedName = NormalizeForDrops(name);
                if (!normalizedName.empty() && normalizedName != normalizedId)
                {
                    text.DropIds.push_back(normalizedName);
                }
            }
        }

        if (partner && !text.PartnerRead)
        {
            text.PartnerRead = true;
            const auto itemNameOf = [this](const StringType& id) -> StringType {
                auto& name = m_itemNames[id];
                if (name.empty())
                {
                    name = ToLower(ItemName(FName(id, FNAME_Add)));
                }
                return name;
            };

            const auto partnerText = UiUtilityText(STR("GetPalFirstActivatedInfo"), characterId);

            // Ranch: only the items in the ranch sentence, in the same normalized id + localized
            // name form the drop search uses, so "wool", "Wool" and "flame organ" all work.
            for (const auto& id : RanchItemIds(partnerText))
            {
                const auto normalizedId = NormalizeForDrops(ToLower(id));
                if (normalizedId.empty() || std::find(text.RanchItems.begin(), text.RanchItems.end(), normalizedId) != text.RanchItems.end())
                {
                    continue;
                }
                text.RanchItems.push_back(normalizedId);
                const auto normalizedName = NormalizeForDrops(itemNameOf(id));
                if (!normalizedName.empty() && normalizedName != normalizedId)
                {
                    text.RanchItems.push_back(normalizedName);
                }
            }

            // Desc: everything the Palpedia shows about the species, as plain lowercase words.
            StringType haystack{};
            for (const auto& part : {UiUtilityText(STR("GetPartnerSkillName"), characterId),
                                     ExpandRichText(partnerText, itemNameOf),
                                     UiUtilityText(STR("GetPalShortDescription"), characterId),
                                     UiUtilityText(STR("GetPalLongDescription"), characterId)})
            {
                if (!part.empty())
                {
                    haystack += ToLower(part);
                    haystack += L' ';
                }
            }
            text.Description = haystack;
        }

        return text;
    }

    auto PalpediaFilter::Matches(const Entry& entry) const -> bool
    {
        if (m_unreadOnly && !entry.IsUnread)
        {
            return false;
        }

        // An unknown state passes a discovery filter rather than failing it. The state comes from a
        // setup hook, and a row built before the hook registered has no record: hiding those would
        // blank the screen for a reason the player cannot see.
        if (m_discoveryMask != 0 && entry.State != Discovery::Unknown && !DiscoveryActive(entry.State))
        {
            return false;
        }

        // Same reasoning as discovery: a row whose ownership tier could not be derived passes
        // rather than being hidden.
        if (m_ownershipMask != 0 && entry.Owning != Ownership::Undefined && !OwnershipActive(entry.Owning))
        {
            return false;
        }

        // Trait categories are ANDed within themselves: selecting Farming and Mining means a Pal
        // must carry both, which is the intended behavior. Discovery and ownership stay ORed
        // above because their states are mutually exclusive: a Pal is exactly one of
        // Undiscovered/Seen/Caught, so an AND of two states can never match anything.
        if (m_elementMask != 0 && (entry.Elements & m_elementMask) != m_elementMask)
        {
            return false;
        }

        if (m_workSuitabilityMask != 0 && (entry.WorkSuitabilities & m_workSuitabilityMask) != m_workSuitabilityMask)
        {
            return false;
        }

        if (!m_compiledQuery.IsEmpty())
        {
            const Query::CompiledQuery::Haystacks haystacks{
                &entry.DisplayName,
                &entry.Description,
                &entry.DropIds,
                &entry.RanchItems,
            };
            if (!m_compiledQuery.Matches(haystacks))
            {
                return false;
            }
        }

        return true;
    }

    auto PalpediaFilter::Reset() -> void
    {
        m_hidden.clear();
        // m_discovery is deliberately NOT cleared here. The hooks repopulate it while the rows are
        // built, and Reset runs after that and before Refresh: clearing it wipes the very state the
        // discovery and ownership filters read, which made them pass everything. The map is keyed
        // by row address and the scroll list recycles rows, so stale entries from a dead screen are
        // overwritten rather than leaked.
        m_shown = 0;
        m_total = 0;
        // The query, the chosen filters and the selected search targets are all deliberately
        // kept: session preferences, not screen state. The UI writes the surviving query back
        // into the rebuilt search box (see EnsureWidgets), so what the player sees matches what
        // is applied.
    }

    auto PalpediaFilter::RestoreHidden(const std::vector<UObject*>& rows) -> void
    {
        if (m_hidden.empty())
        {
            return;
        }

        for (const auto& [row, visibility] : m_hidden)
        {
            // The Palpedia is rebuilt on every open and has no Destruct, so a remembered row can
            // already be gone. Only touch pointers the engine still knows about.
            if (std::find(rows.begin(), rows.end(), row) == rows.end())
            {
                continue;
            }
            WriteVisibility(row, visibility);
        }
        m_hidden.clear();
    }

    auto PalpediaFilter::Refresh(const std::vector<UObject*>& rows) -> void
    {
        RestoreHidden(rows);

        m_total = rows.size();
        m_shown = 0;

        if (rows.empty())
        {
            return;
        }

        if (!IsFiltering())
        {
            m_shown = m_total;
            return;
        }

        // Resolved once for the whole pass, and only when a filter actually needs it. A database
        // read per row per refresh would be several hundred reflected calls for nothing.
        const bool needsTraits = m_elementMask != 0 || m_workSuitabilityMask != 0;
        const bool needsDatabase = needsTraits || SearchTargetActive(SearchTarget::Drops) ||
                                    (m_compiledQuery.ReferencedTargets() & TargetBit(SearchTarget::Drops)) != 0;
        UObject* database = needsDatabase ? CharacterDatabase() : nullptr;

        if (needsDatabase && !database)
        {
            Output::send<LogLevel::Warning>(STR("[PalpediaEnhancer] Character database unavailable; element, work and drop filters are inactive\n"));
        }

        size_t unreadable = 0;

        for (auto* row : rows)
        {
            auto entry = ReadEntry(row, database, needsTraits);

            // A row we cannot read stays visible. Hiding what we cannot identify is how a single
            // failed property lookup empties the whole screen.
            if (!entry.Readable)
            {
                ++unreadable;
                ++m_shown;
                continue;
            }

            if (needsTraits && (!database || entry.CharacterId.ToString().empty()))
            {
                // Cannot evaluate the trait filters for this row, so it is not hidden by them.
                entry.Elements = m_elementMask;
                entry.WorkSuitabilities = m_workSuitabilityMask;
            }

            if (Matches(entry))
            {
                ++m_shown;
                continue;
            }

            // Remember the row's own visibility rather than assuming Visible. Rows sit at
            // SelfHitTestInvisible normally, so restoring to Visible would change the screen.
            m_hidden.emplace_back(row, static_cast<uint8_t>(std::max(ReadVisibility(row), 0)));
            WriteVisibility(row, SlateCollapsed);
        }

        if (unreadable == rows.size())
        {
            Output::send<LogLevel::Warning>(STR("[PalpediaEnhancer] No row could be read, so nothing was filtered\n"));
        }
    }

    auto PalpediaFilter::SortRowOrder(const std::vector<UObject*>& rows) const -> std::vector<UObject*>
    {
        if (rows.empty())
        {
            return rows;
        }

        // Traits are needed by exactly two modes; skip the per-row database reads for the rest.
        const bool needTraits = m_sortMode == SortMode::Element || m_sortMode == SortMode::WorkSuitability;
        UObject* database = needTraits ? CharacterDatabase() : nullptr;

        struct RankedRow
        {
            UObject* Row{};
            Entry Info{};
            size_t OriginalIndex{};
            int64_t Primary{};
            StringType Name{};
        };

        std::vector<RankedRow> ranked;
        ranked.reserve(rows.size());

        const bool alphabetical = m_sortMode == SortMode::Name;
        for (size_t index = 0; index < rows.size(); ++index)
        {
            auto* row = rows[index];
            Entry entry = row ? ReadEntry(row, database, needTraits) : Entry{};
            ranked.push_back(RankedRow{row, entry, index, 0, alphabetical ? entry.DisplayName : StringType{}});
        }

        // Every key is "ascending = what a player reads as the natural top of the list" and the
        // Paldex number is the tiebreaker within a group. The supplied row order is NOT Paldex
        // order (the first row is a boss), which is why OriginalIndex is never a key: the number
        // the game passed to each row's Setup is.
        const auto primaryOf = [](const RankedRow& r, SortMode mode) -> int64_t {
            switch (mode)
            {
            case SortMode::None:
            case SortMode::Number:
                return 0; // Paldex number alone, via the tiebreaker below
            case SortMode::Element:
            {
                // Lowest set element bit is the primary element; Neutral(1) sorts first.
                if (r.Info.Elements == 0)
                {
                    return LastElement + 1;
                }
                int bit = FirstElement;
                while (bit <= LastElement && (r.Info.Elements & (1u << bit)) == 0)
                {
                    ++bit;
                }
                return bit;
            }
            case SortMode::WorkSuitability:
            {
                if (r.Info.WorkSuitabilities == 0)
                {
                    return LastWorkSuitability + 1;
                }
                int bit = FirstWorkSuitability;
                while (bit <= LastWorkSuitability && (r.Info.WorkSuitabilities & (1u << bit)) == 0)
                {
                    ++bit;
                }
                return bit;
            }
            case SortMode::Discovery:
                // Caught, then seen, then never encountered; unknown state last.
                return r.Info.State == Discovery::Unknown ? 99 : 10 - static_cast<int64_t>(r.Info.State);
            case SortMode::CapturedCount:
                // Most progress first: owned 5+, owned, seen but unowned, never seen; unknown last.
                switch (r.Info.Owning)
                {
                case Ownership::Complete:
                    return 0;
                case Ownership::Owned:
                    return 1;
                case Ownership::Unowned:
                    return r.Info.State == Discovery::NotEncountered ? 3 : 2;
                default:
                    return 99;
                }
            case SortMode::UnreadFirst:
                return r.Info.IsUnread ? 0 : 1;
            case SortMode::Name:
                // Names the player cannot read yet ("???" for undiscovered Pals) go after real ones.
                return r.Info.DisplayName.empty() || r.Info.DisplayName.front() == L'?' ? 1 : 0;
            default:
                return 0;
            }
        };

        for (auto& rankedRow : ranked)
        {
            rankedRow.Primary = primaryOf(rankedRow, m_sortMode);
        }

        const bool descending = m_sortDescending;
        std::stable_sort(ranked.begin(), ranked.end(), [&](const RankedRow& left, const RankedRow& right) {
            int cmp = (left.Primary < right.Primary) ? -1 : ((left.Primary > right.Primary) ? 1 : 0);
            if (cmp == 0 && alphabetical)
            {
                cmp = left.Name.compare(right.Name);
            }
            if (cmp == 0)
            {
                cmp = left.Info.PaldexNumber < right.Info.PaldexNumber ? -1 : (left.Info.PaldexNumber > right.Info.PaldexNumber ? 1 : 0);
            }
            if (cmp == 0)
            {
                cmp = left.Info.PaldexSuffix.compare(right.Info.PaldexSuffix);
            }
            if (cmp == 0)
            {
                cmp = left.OriginalIndex < right.OriginalIndex ? -1 : (left.OriginalIndex > right.OriginalIndex ? 1 : 0);
            }
            return descending ? cmp > 0 : cmp < 0;
        });

        std::vector<UObject*> result;
        result.reserve(ranked.size());
        for (const auto& rankedRow : ranked)
        {
            result.push_back(rankedRow.Row);
        }
        return result;
    }
} // namespace PalpediaEnhancer
