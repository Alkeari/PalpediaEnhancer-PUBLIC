#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

#include <Unreal/Common.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>

#include <PalpediaFilter.hpp>

namespace PalpediaEnhancer
{
    // What a click on one of the filter panel's buttons means. Kept as data rather than a callback
    // so the click hook only ever records, and the filter is mutated on the next safe pass.
    struct FilterAction
    {
        enum class Kind : uint8_t
        {
            None,
            Unread,
            Discovery,
            Element,
            WorkSuitability,
            Ownership,
            SortMode,
            SortDescending,
            ClearAll,
            SearchTarget,
            ClearSearch, // the X beside the search field: empty query, targets back to Name
            MarkAllSeen, // the button beside the Stats tab: every unread entry marked as opened
        };

        Kind What{};
        int Value{};
    };

    // Safety rule for this whole class: a stored widget pointer is only ever compared by address.
    // It is dereferenced only after a freshly resolved canvas confirms it is still one of its
    // children. The Palpedia can close at any moment, taking every widget with it, and UEPseudo
    // offers no validity check on a UObject pointer.
    class PalpediaUi
    {
    public:
        struct Poll
        {
            bool ScreenRebuilt{};
            bool QueryChanged{};
            RC::StringType Query{};
            std::vector<FilterAction> FilterActions{};

            // Rows belonging to the active list only. The previous screen is never destroyed, so a
            // global search returns its rows too and every count doubles on reopen.
            std::vector<RC::Unreal::UObject*> Rows{};
        };

        // Called from the click hook on the vanilla button class. The context object identifies
        // which button was pressed, so no delegate binding is needed.
        auto NotifyButtonClicked(RC::Unreal::UObject* button) -> void;

        // Called from the row-setup hook. The row identifies which of the two live Palpedia screens
        // is being built, since FindAllOf order between them is not stable.
        auto RequestAttach(RC::Unreal::UObject* row) -> void
        {
            m_attachRequested = true;
            m_pendingRow = row;
        }

        // One throttled pass that resolves the screen, keeps our widgets attached, suppresses
        // vanilla and foreign controls, and reports what the player did. Everything touching a
        // widget happens inside this pass.
        auto Update(const PalpediaFilter& filter) -> Poll;

        // Clears the remembered query and empties the search box. Called by the entry point on
        // a screen rebuild when RememberState is off; everything else keeps the query alive as a
        // session preference.
        auto ForgetQuery() -> void;

        // Ends a typing session if one is still holding the CommonUI gate. Safe to call when
        // nothing is held, and called on shutdown so unloading the mod cannot leave the gate
        // stuck off.
        auto ReleaseInputMode() -> void;

        // Applied by the mod entry point after a refresh when a sort is active. Reorders the active
        // list's rows in place to match the given order.
        // Returns false only when the request was deferred by the post-reorder cooldown and
        // should be retried; every other outcome (applied, no-op, or failed) returns true.
        auto ReorderActiveList(const std::vector<RC::Unreal::UObject*>& order) -> bool;

        // Marks every row that still shows the unread dot as opened, through the game's own
        // per-entry UpdateNewFlag, and collapses its dot. Returns how many rows were marked.
        auto MarkAllSeen(const std::vector<RC::Unreal::UObject*>& rows) -> int;

        // Puts a live ShowMarkAllSeenButton edit onto a Palpedia that is already open: the
        // existing control is shown or collapsed, and one that was never built because the
        // setting was off is built now. Does nothing when no screen is attached, since the next
        // open reads the setting itself.
        auto ApplyMarkSeenSetting(bool show) -> void;

    private:
        struct Toggle
        {
            RC::Unreal::UObject* Button{};
            RC::Unreal::UObject* Inner{};
            FilterAction Action{};
            RC::Unreal::UObject* Label{}; // caption drawn over the button; recolored when active
            RC::Unreal::UObject* Frame{}; // frame art over the button; tinted when active
        };

        // Every caption or icon drawn over a button, centered against that button's painted
        // geometry once it has been seen on screen (see CenterLabels).
        struct Centered
        {
            RC::Unreal::UObject* Button{};
            RC::Unreal::UObject* Label{};
            bool Done{};
            double FontScale{1.0}; // shrunk by CenterLabels when the caption is wider than its tile
            int Attempts{};
        };
        std::vector<Centered> m_centered{};

        auto EnsureWidgets(RC::Unreal::UObject* canvas) -> void;
        auto ForgetWidgets() -> void;
        auto SearchToggleHovered() const -> bool;
        auto CenterLabels() -> void;
        auto SetSearchExpanded(bool expanded) -> void;
        auto SetPanelExpanded(bool expanded) -> void;
        auto BuildFilterPanel(RC::Unreal::UObject* canvas, RC::Unreal::UObject* widgetTree) -> void;
        auto BuildSortPanel(RC::Unreal::UObject* canvas, RC::Unreal::UObject* widgetTree) -> void;
        auto BuildMarkSeenButton(RC::Unreal::UObject* canvas, RC::Unreal::UObject* widgetTree, RC::Unreal::UObject* list) -> void;

        // One button in a panel. Toggleable buttons get CommonUI's selectable/toggleable state;
        // Accent draws a solid plate inside the frame (Clear All).
        struct PanelItem
        {
            const RC::CharType* Caption{};
            FilterAction Action{};
            const RC::CharType* Tooltip{};
            bool Toggleable{true};
            bool Accent{};
        };
        // A titled group of buttons laid out in a grid of Columns.
        struct PanelSection
        {
            const RC::CharType* Header{};
            std::vector<PanelItem> Items{};
            int Columns{};
        };
        struct PanelSpec
        {
            const RC::CharType* Title{};
            std::vector<PanelItem> TitleActions{}; // drawn on the title row, right-aligned
            std::vector<PanelSection> Sections{};
            double WindowX0{}, WindowY0{}, WindowX1{}, WindowY1{};
        };
        struct PanelResult
        {
            RC::Unreal::UObject* Window{};
            RC::Unreal::UObject* CloseButton{};
            RC::Unreal::UObject* CloseInner{};
        };
        // Lays out a spec: title row, sections (header + rule + grid), Close at the bottom.
        auto BuildPanel(RC::Unreal::UObject* canvas, RC::Unreal::UObject* widgetTree, const PanelSpec& spec) -> PanelResult;
        auto SyncToggleStates(const PalpediaFilter& filter) -> void;
        auto ChangeSortPanel(bool expanded) -> void;
        auto SetTypingInputMode(bool typing) -> void;
        auto TypingPlayerController() -> RC::Unreal::UObject*;

        bool m_attachRequested{};
        int m_settleFrames{};
        int m_framesSincePass{};
        RC::Unreal::UObject* m_pendingRow{};

        RC::Unreal::UObject* m_canvas{};
        // The scroll list the canvas was attached through, and a liveness record for both. The
        // record answers "is this exact object still alive" from the global object array in
        // constant time: the slot it occupied must still hold the same pointer, of the same
        // class, and not be marked unreachable. Deliberately NOT UE4SS's FWeakObjectPtr: its
        // assignment allocates a serial number through a Kismet native that crashed this game
        // (v1.2.9, EXCEPTION_ACCESS_VIOLATION at 0xffffffffffffffff inside UE4SS).
        struct LiveRef
        {
            RC::Unreal::UObject* Object{};
            int32_t Index{-1};
            RC::Unreal::FName Class{};

            auto Set(RC::Unreal::UObject* object) -> void;
            auto Reset() -> void;
            auto Alive() const -> bool;
        };
        RC::Unreal::UObject* m_list{};
        LiveRef m_canvasRef{};
        LiveRef m_listRef{};
        RC::Unreal::UObject* m_searchBox{};
        RC::Unreal::UObject* m_searchButton{};
        RC::Unreal::UObject* m_sortButton{};
        RC::Unreal::UObject* m_filterButton{};
        RC::Unreal::UObject* m_panel{};
        RC::Unreal::UObject* m_sortPanel{};
        RC::Unreal::UObject* m_directionButton{};
        // The inner CommonButtonBase of each button. That object, not the outer widget, is what
        // HandleButtonClicked reports.
        RC::Unreal::UObject* m_searchInner{};
        RC::Unreal::UObject* m_sortInner{};
        RC::Unreal::UObject* m_filterInner{};
        RC::Unreal::UObject* m_directionInner{};
        // Each panel owns its Close button. A single shared pair was overwritten by whichever
        // panel built last, which is why the Filter window's Close did nothing.
        RC::Unreal::UObject* m_closeButton{};
        RC::Unreal::UObject* m_closeInner{};
        RC::Unreal::UObject* m_filterCloseButton{};
        RC::Unreal::UObject* m_filterCloseInner{};
        RC::Unreal::UObject* m_clearSearchButton{};
        RC::Unreal::UObject* m_clearSearchInner{};
        RC::Unreal::UObject* m_markSeenButton{};
        RC::Unreal::UObject* m_markSeenInner{};

        std::vector<Toggle> m_toggles{};
        std::vector<FilterAction> m_pendingActions{};

        // The Name/Drops/Description search-target buttons and their text labels. Tracked
        // separately from m_toggles so their visibility can be tied to the search field's own
        // expanded/folded state. ShownVisibility differs between the two: a button becomes Visible,
        // a label stays HitTestInvisible so the click still reaches the button underneath it.
        struct SearchModeWidget
        {
            RC::Unreal::UObject* Widget{};
            uint8_t ShownVisibility{};
        };
        std::vector<SearchModeWidget> m_searchModeWidgets{};
        // Frames and captions drawn over the Sort and Filter buttons; hidden with them while the
        // search field covers that span.
        std::vector<SearchModeWidget> m_sortFilterOverlays{};

        // StripControls runs every Update() tick, not just on rebuild. Before this set existed, that
        // meant it called MoveAside on the same already-hidden vanilla Filter button on every single
        // tick, thousands of times a minute, for as long as the Palpedia stayed open - a real defect,
        // fixed here by suppressing each widget at most once per screen (ForgetWidgets clears this on
        // rebuild, so a genuinely new widget instance still gets suppressed). This was this session's
        // leading theory for the 2026-08-21 "Pure virtual function" crash, but the next retest hit a
        // different crash before this fix's target hash ever recurred, so treat it as an unconfirmed
        // fix for a confirmed defect, not a proven crash fix - see the design doc.
        std::unordered_set<RC::Unreal::UObject*> m_suppressedWidgets{};

        // Canvas children as of the last rebuild, not recomputed on every tick. CollectChildren
        // calls GetChildrenCount/GetChildAt via ProcessEvent on the canvas - real engine calls,
        // not free C++ reads - and StripControls has had nothing new to do since m_suppressedWidgets
        // made it one-shot per widget. A diagnostic build that skipped every per-tick call after
        // initial construction did not crash during an idle soak that otherwise reliably did; this
        // was the last remaining call still hitting the canvas's own children array on every single
        // tick after that. Recomputed only when the screen rebuilds, since nothing changes this
        // canvas's children outside of that path. See the design doc, 2026-08-21.
        std::vector<RC::Unreal::UObject*> m_cachedChildren{};

        // Vanilla geometry is read once per canvas and cached. Our own buttons are instances of the
        // same class as the vanilla Filter button, so re-scanning after we have created any would
        // measure ourselves and walk the row across the screen.
        RC::Unreal::UObject* m_geometryCanvas{};
        double m_rowX{};
        double m_rowY{};
        double m_rowWidth{};
        double m_rowHeight{};
        double m_anchorMinX{};
        double m_anchorMinY{};
        double m_anchorMaxX{};
        double m_anchorMaxY{};
        double m_alignX{};
        double m_alignY{};
        bool m_anchorsValid{};
        bool m_geometryValid{};

        bool m_searchVisible{};
        bool m_panelVisible{};
        bool m_sortPanelVisible{};
        bool m_pendingSearchClick{};
        bool m_pendingSortClick{};
        bool m_pendingFilterClick{};
        bool m_pendingCloseClick{};
        bool m_pendingClearSearch{};
        bool m_reorderBroken{};
        // Frames remaining before another reorder may touch the ScrollBox. Two historical crash
        // dumps from repeated sorting in one session show a deep, repetitive recursive call
        // pattern consistent with Slate still walking the widget tree (paint or invalidation) when
        // a second RemoveChild/AddChild pass reparents the same rows again. This gives that walk
        // several frames to finish before the rows are touched a second time, rather than either
        // allowing back-to-back reorders (the original crash) or blocking sorting for the rest of
        // the screen's life (the interim fix).
        int m_reorderCooldownFrames{};
        int m_focusGrantsRemaining{};
        int m_unfocusedPolls{};
        int m_labelCenterPolls{}; // polls to wait after a panel opens before measuring labels
        // The last COMMITTED query - what the filter is actually running. The box's live text
        // becomes the query only at a commit point (Enter, click-away, fold, or collapsing the
        // bar), one search per session instead of a full filter pass on every keystroke.
        RC::StringType m_lastQuery{};

        // Typing session debt (CommonUI gate held, cursor flag captured). The flag is only
        // cleared by a release that actually succeeded, so a failed one is retried instead of
        // leaving the gate stuck off.
        RC::Unreal::UObject* m_typingController{};
        bool m_inputModeHeld{};
        bool m_cursorWasShown{};
        // One bShowMouseCursor capture per typing session, not per take. The game hides the
        // cursor in reaction to our own CommonUI gate toggle, so a hover-release followed by a
        // retake would recapture that game-perturbed false and the final restore would then
        // hide the menu cursor for good (observed live 2026-08-30). Cleared when the search
        // folds or the screen goes away.
        bool m_cursorCaptured{};
        // Edge detection for the click-away poll in Update(). While a typing session holds the
        // CommonUI gate, no foreign button receives clicks, so no click can pull keyboard focus
        // off the search box; the poll watches raw mouse state instead and ends the session on a
        // press outside the search UI. Win32 state, not engine input: UIOnly mode stops the
        // viewport from feeding PlayerInput, and UE4SS keybinds wedge on mouse buttons.
        bool m_mouseWasDown{};
        // The current typing session has already committed its query (commits are unconditional,
        // so without this a held Enter would re-run the search on every poll until the key came
        // up). Reset when a session starts.
        bool m_sessionCommitDone{};
    };
} // namespace PalpediaEnhancer
