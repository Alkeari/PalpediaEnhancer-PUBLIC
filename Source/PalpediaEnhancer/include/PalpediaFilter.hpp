#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Unreal/Common.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>

#include <PalpediaQuery.hpp>

namespace PalpediaEnhancer
{
    // Mirrors EPalUIPaldexPanelDetailType. Recorded from the row's own setup call rather than
    // inferred from what the row looks like: the game tells us outright which of the three it built.
    enum class Discovery : uint8_t
    {
        Unknown = 0,
        NotEncountered = 1,
        Encountered = 2,
        Captured = 3,
    };

    // Ownership tiers read off the row's own signals. A Pal caught zero times is Unowned, caught
    // once through four is Owned, and caught five or more has its capture bonus complete, which is
    // the Complete tier. The threshold is the game's own: the complete-capture-bonus mark is drawn
    // on the row and is what we read.
    enum class Ownership : uint8_t
    {
        Undefined = 0,
        Unowned = 1,
        Owned = 2,
        Complete = 3,
    };

    // Sort orders the Sort panel can apply. None means the list's natural Paldex order.
    enum class SortMode : uint8_t
    {
        None = 0,
        Number,
        Name,
        Element,
        WorkSuitability,
        Discovery,
        CapturedCount,
        UnreadFirst,
    };

    // EPalElementType values 1..9. None and MAX are not filterable states.
    constexpr int FirstElement = 1;
    constexpr int LastElement = 9;

    // EPalWorkSuitability values 1..13. Anyone (14) is not a Pal trait.
    constexpr int FirstWorkSuitability = 1;
    constexpr int LastWorkSuitability = 13;

    struct Entry
    {
        RC::Unreal::UObject* Row{};
        RC::Unreal::FName CharacterId{};
        RC::StringType DisplayName{};
        bool Readable{};
        bool IsUnread{};
        bool CaptureBonusComplete{};
        Discovery State{};
        Ownership Owning{};
        uint16_t Elements{};
        uint16_t WorkSuitabilities{};
        // Paldex number and variant suffix; Index is INT32_MAX when the row's Setup was never seen.
        int32_t PaldexNumber{INT32_MAX};
        RC::StringType PaldexSuffix{};
        // Lowercased search haystacks, filled only for the active search targets.
        RC::StringType Description{};
        std::vector<RC::StringType> DropIds{};
        std::vector<RC::StringType> RanchItems{};
    };

    // Every category is ANDed with the others; choices within a category are ORed. Selecting Fire
    // and Water means "Fire or Water", while also selecting Captured means "(Fire or Water) and
    // Captured", which is what a player expects from a checkbox list.
    class PalpediaFilter
    {
    public:
        auto ToggleUnreadOnly() -> void;
        auto SetQuery(const RC::StringType& query) -> void;

        auto ToggleDiscovery(Discovery state) -> void;
        auto ToggleElement(int element) -> void;
        auto ToggleWorkSuitability(int suitability) -> void;
        auto ToggleOwnership(Ownership state) -> void;
        auto ToggleSortMode(SortMode mode) -> void;
        auto ClearAll() -> void;

        auto UnreadOnly() const -> bool
        {
            return m_unreadOnly;
        }

        // Set outright rather than flipped: used to put a fresh Palpedia into the state
        // Settings.ini asks for, where a toggle would depend on what came before.
        auto SetUnreadOnly(bool unreadOnly) -> void
        {
            m_unreadOnly = unreadOnly;
        }

        auto DiscoveryActive(Discovery state) const -> bool;
        auto ElementActive(int element) const -> bool;
        auto WorkSuitabilityActive(int suitability) const -> bool;
        auto OwnershipActive(Ownership state) const -> bool;

        auto SetSortMode(SortMode mode) -> void
        {
            m_sortMode = mode;
        }

        auto CurrentSortMode() const -> SortMode
        {
            return m_sortMode;
        }

        auto SetSortDescending(bool descending) -> void
        {
            m_sortDescending = descending;
        }

        auto SortDescending() const -> bool
        {
            return m_sortDescending;
        }

        // Search targets are multi-select: a query matches a row if ANY selected target contains
        // it. At least one is always selected; turning off the last one falls back to Name rather
        // than leaving a search that can never match anything.
        auto SearchTargetActive(SearchTarget target) const -> bool
        {
            return (m_searchTargets & TargetBit(target)) != 0;
        }

        // Called from the search row's target toggle buttons, and to re-parse the retained raw
        // query whenever the mask changes (see SetQuery/m_rawQuery). A typed name:/drops:/ranch:/
        // desc: prefix now scopes to the one atom it precedes, not the whole query - this mask is
        // only the default target for atoms with no explicit prefix of their own.
        auto ToggleSearchTarget(SearchTarget target) -> void
        {
            m_searchTargets ^= TargetBit(target);
            if (m_searchTargets == 0)
            {
                m_searchTargets = TargetBit(SearchTarget::Name);
            }
            m_compiledQuery = Query::CompiledQuery::Parse(m_rawQuery, m_searchTargets);
        }

        // The whole mask at once, for the configured default. An empty mask would be a search
        // that can never match, so it falls back to Name exactly as the toggles do.
        auto SetSearchTargets(uint8_t targets) -> void
        {
            m_searchTargets = targets != 0 ? targets : TargetBit(SearchTarget::Name);
            m_compiledQuery = Query::CompiledQuery::Parse(m_rawQuery, m_searchTargets);
        }

        auto ResetSearchTargets() -> void
        {
            m_searchTargets = TargetBit(SearchTarget::Name);
            m_compiledQuery = Query::CompiledQuery::Parse(m_rawQuery, m_searchTargets);
        }

        auto IsFiltering() const -> bool;

        auto ShownCount() const -> size_t
        {
            return m_shown;
        }

        auto TotalCount() const -> size_t
        {
            return m_total;
        }

        // Called from the row-setup hooks. Records only, never touches a widget, because it runs
        // inside the game's own call stack.
        auto NoteDiscovery(RC::Unreal::UObject* row, Discovery state) -> void;
        // The Paldex number and variant suffix ("B" for subspecies) the game passed to the row's
        // Setup, recorded by the same hook as discovery. This is the only reliable number source:
        // the list's own row order is not Paldex order (bosses and variants are interleaved).
        auto NoteIndex(RC::Unreal::UObject* row, int32_t index, RC::StringType suffix) -> void;

        // Restores everything it previously hid, then hides whatever fails the active conditions.
        // Rows are supplied by the caller, taken from the active list only.
        auto Refresh(const std::vector<RC::Unreal::UObject*>& rows) -> void;

        // Returns the rows in the order the active sort demands, stable for ties. Used by the UI to
        // reorder the list through the native display path.
        auto SortRowOrder(const std::vector<RC::Unreal::UObject*>& rows) const -> std::vector<RC::Unreal::UObject*>;

        // Drop remembered rows without touching widgets. Used when the Palpedia is rebuilt, since
        // every pointer we were holding belongs to the previous screen.
        auto Reset() -> void;

    private:
        auto RestoreHidden(const std::vector<RC::Unreal::UObject*>& rows) -> void;
        auto Matches(const Entry& entry) const -> bool;
        // The active-trait flag controls the two runtime database reads (element and work
        // suitability), which cost a reflected call each and are skipped unless a filter or sort
        // actually needs them.
        auto ReadEntry(RC::Unreal::UObject* row, RC::Unreal::UObject* characterDatabase, bool needTraits) const -> Entry;

        bool m_unreadOnly{};
        uint8_t m_discoveryMask{};
        uint8_t m_ownershipMask{};
        uint16_t m_elementMask{};
        uint16_t m_workSuitabilityMask{};
        SortMode m_sortMode{SortMode::None};
        bool m_sortDescending{};
        uint8_t m_searchTargets{TargetBit(SearchTarget::Name)};
        RC::StringType m_rawQuery{};
        Query::CompiledQuery m_compiledQuery{};
        size_t m_shown{};
        size_t m_total{};
        std::vector<std::pair<RC::Unreal::UObject*, uint8_t>> m_hidden{};

        // Rows are recycled by the scroll list, so a stale entry is overwritten rather than leaked.
        // Keyed by address only; nothing here is ever dereferenced.
        std::unordered_map<RC::Unreal::UObject*, Discovery> m_discovery{};
        struct PaldexIndex
        {
            int32_t Index{};
            RC::StringType Suffix{};
        };
        std::unordered_map<RC::Unreal::UObject*, PaldexIndex> m_paldexIndex{};

        // Everything searchable about a species that is not its name, read from the game once per
        // species per session and kept. PalboxSearchPlus ships this as an offline catalog; here it
        // is built lazily from the same game data so it follows the player's language and any
        // Pal-adding mod. Keyed by character id text (FName is not hashable here).
        struct SpeciesText
        {
            std::vector<RC::StringType> DropIds{};    // normalized item ids and localized item names
            std::vector<RC::StringType> RanchItems{}; // same form, from the ranch sentence of the partner skill
            RC::StringType Description{};             // partner skill name + text, short and long descriptions, lowercased
            bool DropsRead{};
            bool PartnerRead{};
        };
        // Mutable: filled from the const read paths (ReadEntry, SortRowOrder); a cache, not state.
        mutable std::unordered_map<RC::StringType, SpeciesText> m_speciesText{};
        mutable std::unordered_map<RC::StringType, RC::StringType> m_itemNames{};
        auto SpeciesTextFor(RC::Unreal::UObject* database, RC::Unreal::FName characterId, bool drops, bool partner) const -> const SpeciesText&;
    };
} // namespace PalpediaEnhancer
