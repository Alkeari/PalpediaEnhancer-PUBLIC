#include <PalpediaUi.hpp>
#include <PalpediaPalette.hpp>
#include <PalpediaLocalization.hpp>
#include <PalpediaSettings.hpp>
#include <PalpediaText.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <vector>

// Windows.h defines min and max as macros, which breaks std::numeric_limits::max() inside
// UEPseudo's headers. Both guards are required before it is included.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <Unreal/Core/Containers/FString.hpp>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace PalpediaEnhancer
{
    using namespace RC;
    using namespace RC::Unreal;

    namespace
    {
        constexpr uint8_t SlateVisible = 0;
        constexpr uint8_t SlateCollapsed = 1;
        constexpr uint8_t SlateHidden = 2;

        struct Vec2
        {
            double X{};
            double Y{};
        };

        // FAnchors: a minimum and maximum corner. A CanvasPanelSlot's position is an offset from
        // its anchor, not an absolute coordinate, so copying a position without its anchors puts
        // the widget somewhere else entirely.
        struct Anchors
        {
            Vec2 Minimum{};
            Vec2 Maximum{};
        };

        struct SlotLayout
        {
            Anchors SlotAnchors{};
            Vec2 Alignment{};
            bool Valid{};
        };

        auto CallNoArgs(UObject* target, const CharType* functionName) -> UObject*
        {
            if (!target)
            {
                return nullptr;
            }
            auto* function = target->GetFunctionByNameInChain(functionName);
            if (!function)
            {
                return nullptr;
            }
            struct Params
            {
                UObject* ReturnValue{};
            } params{};
            target->ProcessEvent(function, &params);
            return params.ReturnValue;
        }

        auto CallWithVec2(UObject* target, const CharType* functionName, Vec2 value) -> void
        {
            if (!target)
            {
                return;
            }
            if (auto* function = target->GetFunctionByNameInChain(functionName))
            {
                target->ProcessEvent(function, &value);
            }
        }

        auto CallReturningVec2(UObject* target, const CharType* functionName) -> Vec2
        {
            Vec2 result{};
            if (auto* function = target->GetFunctionByNameInChain(functionName))
            {
                struct Params
                {
                    Vec2 ReturnValue{};
                } params{};
                target->ProcessEvent(function, &params);
                result = params.ReturnValue;
            }
            return result;
        }

        auto CallWithBool(UObject* target, const CharType* functionName, bool value) -> void
        {
            if (!target)
            {
                return;
            }
            if (auto* function = target->GetFunctionByNameInChain(functionName))
            {
                struct Params
                {
                    bool Value{};
                } params{value};
                target->ProcessEvent(function, &params);
            }
        }

        auto CallReturningBool(UObject* target, const CharType* functionName) -> bool
        {
            if (auto* function = target->GetFunctionByNameInChain(functionName))
            {
                struct Params
                {
                    bool ReturnValue{};
                } params{};
                target->ProcessEvent(function, &params);
                return params.ReturnValue;
            }
            return false;
        }

        auto CallVoid(UObject* target, const CharType* functionName) -> void
        {
            if (auto* function = target->GetFunctionByNameInChain(functionName))
            {
                target->ProcessEvent(function, nullptr);
            }
        }

        // Direct property write, no vtable call involved.
        auto SetBoolProperty(UObject* target, const CharType* propertyName, bool value) -> void
        {
            if (auto* field = target->GetValuePtrByPropertyNameInChain<bool>(propertyName))
            {
                *field = value;
            }
        }

        // Color lives in exactly one file: include/PalpediaPalette.hpp.
        using PalpediaEnhancer::Ui::FromSrgb;
        using PalpediaEnhancer::Ui::LinearColor;
        using PalpediaEnhancer::Ui::SrgbToLinear;
        using PalpediaEnhancer::Ui::WithAlpha;
        namespace Palette = PalpediaEnhancer::Ui::Palette;
        // FSlateColor is an FLinearColor followed by a styling mode. Writing the mode matters: a
        // color left on any rule other than Specified is resolved from the widget's inherited
        // foreground instead of the value we just wrote.
        constexpr size_t SlateColorRuleOffset = 0x10;
        constexpr uint8_t SlateColorUseSpecified = 0;

        auto WriteSlateColor(uint8_t* structBase, size_t offset, LinearColor color) -> void
        {
            auto* channels = reinterpret_cast<float*>(structBase + offset);
            channels[0] = color.R;
            channels[1] = color.G;
            channels[2] = color.B;
            channels[3] = color.A;
            structBase[offset + SlateColorRuleOffset] = SlateColorUseSpecified;
        }

        // A text box built with NewObject carries the engine's editor defaults, which are a pale
        // box with pale text and unreadable against this UI. The offsets are FEditableTextBoxStyle's
        // own, taken from the running game's reflection data against the vanilla EditableTextBox.
        // CreateWidgetPreferred tries the game's own PalEditableTextBox first, and its WidgetStyle
        // struct was never confirmed to be the same size: writing these offsets into a smaller
        // struct would walk off the end of it. The property's own reflected size is checked before
        // any write, so a size mismatch degrades to unstyled rather than corrupting adjacent memory.
        auto StyleSearchField(UObject* box) -> void
        {
            constexpr size_t brushTint = 0x38;
            constexpr size_t backgroundNormal = 0x10;
            constexpr size_t backgroundHovered = 0xE0;
            constexpr size_t backgroundFocused = 0x1B0;
            constexpr size_t backgroundReadOnly = 0x280;
            constexpr size_t textStyleColor = 0x360 + 0x68;
            constexpr size_t foreground = 0x6B0;
            constexpr size_t readOnlyForeground = 0x6D8;
            constexpr size_t focusedForeground = 0x6EC;
            // Highest offset written, plus the four-float color and the one-byte rule that follow it.
            constexpr size_t requiredSize = focusedForeground + (sizeof(LinearColor)) + 1;

            auto* property = box->GetPropertyByNameInChain(STR("WidgetStyle"));
            if (!property || static_cast<size_t>(property->GetSize()) < requiredSize)
            {
                static bool warned = false;
                if (!warned)
                {
                    warned = true;
                    Output::send<LogLevel::Warning>(std::format(
                            STR("[PalpediaEnhancer] Search field WidgetStyle is {} bytes, expected at least {}; leaving it unstyled\n"),
                            property ? property->GetSize() : -1,
                            requiredSize));
                }
                return;
            }

            auto* style = box->GetValuePtrByPropertyNameInChain<uint8_t>(STR("WidgetStyle"));
            if (!style)
            {
                return;
            }

            // Section 8: a text box is a dark field with champagne text that goes gold while focused.
            const LinearColor field = WithAlpha(Palette::RaisedFill, 0.96f);
            const LinearColor text = Palette::SoftChampagne;
            const LinearColor focused = Palette::ForgedGold;

            WriteSlateColor(style, backgroundNormal + brushTint, field);
            WriteSlateColor(style, backgroundHovered + brushTint, field);
            WriteSlateColor(style, backgroundFocused + brushTint, field);
            WriteSlateColor(style, backgroundReadOnly + brushTint, field);

            WriteSlateColor(style, textStyleColor, text);
            WriteSlateColor(style, foreground, text);
            WriteSlateColor(style, readOnlyForeground, text);
            WriteSlateColor(style, focusedForeground, focused);
        }

        auto SetWidgetVisibility(UObject* widget, uint8_t visibility) -> void
        {
            if (auto* function = widget->GetFunctionByNameInChain(STR("SetVisibility")))
            {
                struct Params
                {
                    uint8_t InVisibility{};
                } params{visibility};
                widget->ProcessEvent(function, &params);
            }
        }

        auto CallWithText(UObject* widget, const CharType* functionName, const StringType& text) -> void
        {
            if (auto* function = widget->GetFunctionByNameInChain(functionName))
            {
                struct Params
                {
                    Text::RawText InText;
                } params{Text::Make(text)};
                if (!params.InText.HasData())
                {
                    return;
                }
                widget->ProcessEvent(function, &params);
            }
        }

        auto SetWidgetText(UObject* widget, const StringType& text) -> void
        {
            CallWithText(widget, STR("SetText"), text);
        }

        // Every interactive control carries a one-sentence tooltip (house rule). The outer button
        // and its invisible inner button both accept the text, so the show is set on whichever the
        // cursor actually sits over.
        auto SetTooltip(UObject* widget, const CharType* text) -> void
        {
            if (!widget)
            {
                return;
            }
            if (auto* function = widget->GetFunctionByNameInChain(STR("SetToolTipText")))
            {
                struct Params
                {
                    Text::RawText InToolTipText;
                } params{Text::Make(text)};
                if (!params.InToolTipText.HasData())
                {
                    return;
                }
                widget->ProcessEvent(function, &params);
            }
        }

        auto SetRowTooltip(UObject* button, UObject* inner, const CharType* text) -> void
        {
            if (!button)
            {
                return;
            }
            SetTooltip(button, text);
            if (inner && inner != button)
            {
                SetTooltip(inner, text);
            }
        }

        auto ButtonTooltip(const FilterAction& action) -> const CharType*
        {
            switch (action.What)
            {
            case FilterAction::Kind::Unread:
                return Loc::T(STR("tt_new")).c_str();
            case FilterAction::Kind::ClearAll:
                return Loc::T(STR("tt_clear_all")).c_str();
            case FilterAction::Kind::MarkAllSeen:
                return Loc::T(STR("tt_mark_all_seen")).c_str();
            case FilterAction::Kind::Ownership:
                switch (static_cast<Ownership>(action.Value))
                {
                case Ownership::Unowned:
                    return Loc::T(STR("tt_unowned")).c_str();
                case Ownership::Owned:
                    return Loc::T(STR("tt_owned")).c_str();
                case Ownership::Complete:
                    return Loc::T(STR("tt_owned_5plus")).c_str();
                default:
                    return STR("");
                }
            case FilterAction::Kind::Discovery:
                switch (static_cast<Discovery>(action.Value))
                {
                case Discovery::NotEncountered:
                    return Loc::T(STR("tt_undiscovered")).c_str();
                case Discovery::Encountered:
                    return Loc::T(STR("tt_seen")).c_str();
                case Discovery::Captured:
                    return Loc::T(STR("tt_caught")).c_str();
                default:
                    return STR("");
                }
            case FilterAction::Kind::Element:
                return Loc::T(STR("tt_element")).c_str();
            case FilterAction::Kind::WorkSuitability:
                return Loc::T(STR("tt_work")).c_str();
            case FilterAction::Kind::SortDescending:
                return Loc::T(STR("tt_descending")).c_str();
            default:
                return STR("");
            }
        }

        // The Palpedia's close on Backspace is not a UMG input-mode concern: UI-only input did not
        // stop it in-game. It is a common-UI action, so the game's own flag registry is the lever.
        // SetEnableCommonUIInput(flag, false) holds common-UI actions (cancel/close mappings) off
        // while a flag is set, and SetEnableCommonUIInput(flag, true) releases it. The flag is
        // unique to this mod so nothing else's flags are touched.
        auto SetCommonUiInputGate(bool enabled) -> void
        {
            auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUIUtility"));
            static UObject* gameInstance = nullptr;
            if (!gameInstance)
            {
                gameInstance = UObjectGlobals::FindFirstOf(STR("PalGameInstance"));
            }
            if (!library || !gameInstance)
            {
                return;
            }
            auto* function = library->GetFunctionByNameInChain(STR("SetEnableCommonUIInput"));
            if (!function)
            {
                return;
            }
            struct Params
            {
                UObject* WorldContextObject{};
                FName FlagName{};
                bool IsEnable{};
            } params{gameInstance, FName(STR("PalpediaEnhancerTyping")), enabled};
            library->ProcessEvent(function, &params);
        }

        // Mirrors the test the game's own screens use before deciding a cancel press was meant for
        // the menu: the box wrapper never holds focus itself, its inner editable text does, so
        // HasKeyboardFocus alone reports false while the player is typing.
        auto IsEditingText(UObject* box) -> bool
        {
            return CallReturningBool(box, STR("HasKeyboardFocus")) || CallReturningBool(box, STR("HasAnyUserFocus")) ||
                   CallReturningBool(box, STR("HasFocusedDescendants"));
        }

        // Hidden and pushed off screen, never Collapsed. Collapsing removes a widget from layout,
        // and the vanilla Filter button is a CommonUI focus target, which crashed the game with
        // "Pure virtual function being called".
        auto MoveAside(UObject* widget) -> void
        {
            SetWidgetVisibility(widget, SlateHidden);
            if (auto** slot = widget->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot")); slot && *slot)
            {
                CallWithVec2(*slot, STR("SetPosition"), Vec2{-10000.0, -10000.0});
            }
        }

        // UE's TArray layout, read as a plain struct. Elements are only compared by address.
        struct RawObjectArray
        {
            UObject** Data{};
            int32_t Num{};
            int32_t Max{};
        };

        // FindAllOf is the only trustworthy source of an object pointer here: UE4SS filters its
        // results to live objects. An object pointer read out of another object's property is NOT
        // validated, and the inactive Palpedia screen holds a stale scroll list pointer, which is
        // what crashed the game when we called GetParent on it.
        auto LiveScrollLists() -> std::vector<UObject*>
        {
            std::vector<UObject*> lists{};
            UObjectGlobals::FindAllOf(STR("WBP_PaldexScrollList_C"), lists);
            return lists;
        }

        auto ListRows(UObject* list) -> std::vector<UObject*>
        {
            std::vector<UObject*> rows{};
            auto* created = list->GetValuePtrByPropertyNameInChain<RawObjectArray>(STR("CreatedChildren"));
            if (!created || !created->Data || created->Num <= 0)
            {
                return rows;
            }
            rows.reserve(static_cast<size_t>(created->Num));
            for (int32_t index = 0; index < created->Num; ++index)
            {
                if (created->Data[index])
                {
                    rows.push_back(created->Data[index]);
                }
            }
            return rows;
        }

        auto ListOwnsRow(UObject* list, UObject* row) -> bool
        {
            auto* created = list->GetValuePtrByPropertyNameInChain<RawObjectArray>(STR("CreatedChildren"));
            if (!created || !created->Data || created->Num <= 0)
            {
                return false;
            }
            for (int32_t index = 0; index < created->Num; ++index)
            {
                if (created->Data[index] == row)
                {
                    return true;
                }
            }
            return false;
        }

        // The Palpedia's list canvas is only ~420px wide (the vanilla list column), so popups
        // anchored to it stay small no matter the fraction. Climbing the parent chain to the
        // outermost CanvasPanel lands on the full-viewport overlay canvas, which is where the
        // Filter and Sort windows must live to get real size. The search row stays on the inner
        // canvas because its geometry is copied from the vanilla button there.
        auto ResolvePopupCanvas(UObject* innerCanvas) -> UObject*
        {
            UObject* best = nullptr;
            UObject* current = innerCanvas;
            while (current)
            {
                if (auto* classInfo = current->GetClassPrivate())
                {
                    if (classInfo->GetPathName().find(STR("CanvasPanel")) != StringType::npos)
                    {
                        best = current;
                    }
                }
                current = CallNoArgs(current, STR("GetParent"));
            }
            return best ? best : innerCanvas;
        }

        // Byte offset of a named parameter inside a UFunction's Parms block, from reflection.
        auto ParamOffset(UFunction* function, const CharType* name) -> int32_t
        {
            for (FProperty* property : function->ForEachProperty())
            {
                if (property->GetName() == name)
                {
                    return property->GetOffset_Internal();
                }
            }
            return -1;
        }

        // A widget's last-painted FGeometry as raw bytes, sized from reflection. Its layout is
        // opaque here; it is only ever handed back to Slate's own Blueprint library.
        auto CachedGeometry(UObject* widget, std::vector<uint8_t>& out) -> bool
        {
            auto* function = widget ? widget->GetFunctionByNameInChain(STR("GetCachedGeometry")) : nullptr;
            auto* returnProperty = function ? function->GetReturnProperty() : nullptr;
            if (!returnProperty)
            {
                return false;
            }
            std::vector<uint8_t> parms(static_cast<size_t>(function->GetPropertiesSize()), 0);
            widget->ProcessEvent(function, parms.data());
            const auto offset = static_cast<size_t>(returnProperty->GetOffset_Internal());
            const auto size = static_cast<size_t>(returnProperty->GetSize());
            out.assign(parms.begin() + offset, parms.begin() + offset + size);
            return true;
        }

        // One SlateBlueprintLibrary call of the shape f(Geometry, FVector2D) -> FVector2D.
        auto GeometryPointCall(const CharType* functionName, const CharType* pointParam, const std::vector<uint8_t>& geometry, Vec2 point, Vec2& out)
                -> bool
        {
            auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__SlateBlueprintLibrary"));
            auto* function = library ? library->GetFunctionByNameInChain(functionName) : nullptr;
            if (!function)
            {
                return false;
            }
            const auto geometryOffset = ParamOffset(function, STR("Geometry"));
            const auto pointOffset = ParamOffset(function, pointParam);
            const auto returnOffset = ParamOffset(function, STR("ReturnValue"));
            const auto parmsSize = static_cast<size_t>(function->GetPropertiesSize());
            if (geometryOffset < 0 || pointOffset < 0 || returnOffset < 0 ||
                static_cast<size_t>(geometryOffset) + geometry.size() > parmsSize || static_cast<size_t>(returnOffset) + sizeof(Vec2) > parmsSize)
            {
                return false;
            }
            std::vector<uint8_t> parms(parmsSize, 0);
            std::memcpy(parms.data() + geometryOffset, geometry.data(), geometry.size());
            std::memcpy(parms.data() + pointOffset, &point, sizeof(Vec2));
            library->ProcessEvent(function, parms.data());
            std::memcpy(&out, parms.data() + returnOffset, sizeof(Vec2));
            return true;
        }

        auto LocalSizeOf(const std::vector<uint8_t>& geometry, Vec2& out) -> bool
        {
            auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__SlateBlueprintLibrary"));
            auto* function = library ? library->GetFunctionByNameInChain(STR("GetLocalSize")) : nullptr;
            if (!function)
            {
                return false;
            }
            const auto geometryOffset = ParamOffset(function, STR("Geometry"));
            const auto returnOffset = ParamOffset(function, STR("ReturnValue"));
            const auto parmsSize = static_cast<size_t>(function->GetPropertiesSize());
            if (geometryOffset < 0 || returnOffset < 0 || static_cast<size_t>(geometryOffset) + geometry.size() > parmsSize)
            {
                return false;
            }
            std::vector<uint8_t> parms(parmsSize, 0);
            std::memcpy(parms.data() + geometryOffset, geometry.data(), geometry.size());
            library->ProcessEvent(function, parms.data());
            std::memcpy(&out, parms.data() + returnOffset, sizeof(Vec2));
            return true;
        }

        // Converts a point in `from`'s local space into `to`'s local space, through absolute
        // (desktop) space, using the two widgets' last-painted geometry. Both must have painted.
        auto ConvertLocalPoint(UObject* from, UObject* to, Vec2 point, Vec2& out) -> bool
        {
            std::vector<uint8_t> fromGeometry{};
            std::vector<uint8_t> toGeometry{};
            if (!CachedGeometry(from, fromGeometry) || !CachedGeometry(to, toGeometry))
            {
                return false;
            }
            Vec2 absolute{};
            return GeometryPointCall(STR("LocalToAbsolute"), STR("LocalCoordinate"), fromGeometry, point, absolute) &&
                   GeometryPointCall(STR("AbsoluteToLocal"), STR("AbsoluteCoordinate"), toGeometry, absolute, out);
        }

        // The mouse position in Slate absolute (platform desktop) space, the same space
        // IsUnderLocation tests a geometry against. Works regardless of input routing, unlike
        // IsHovered, which reports false for a CommonUI button while the CommonUI gate is held.
        auto PlatformMousePosition(Vec2& out) -> bool
        {
            auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetLayoutLibrary"));
            auto* function = library ? library->GetFunctionByNameInChain(STR("GetMousePositionOnPlatform")) : nullptr;
            if (!function)
            {
                return false;
            }
            const auto returnOffset = ParamOffset(function, STR("ReturnValue"));
            const auto parmsSize = static_cast<size_t>(function->GetPropertiesSize());
            if (returnOffset < 0 || static_cast<size_t>(returnOffset) + sizeof(Vec2) > parmsSize)
            {
                return false;
            }
            std::vector<uint8_t> parms(parmsSize, 0);
            library->ProcessEvent(function, parms.data());
            std::memcpy(&out, parms.data() + returnOffset, sizeof(Vec2));
            return true;
        }

        auto GeometryUnderLocation(const std::vector<uint8_t>& geometry, Vec2 point) -> bool
        {
            auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__SlateBlueprintLibrary"));
            auto* function = library ? library->GetFunctionByNameInChain(STR("IsUnderLocation")) : nullptr;
            if (!function)
            {
                return false;
            }
            const auto geometryOffset = ParamOffset(function, STR("Geometry"));
            const auto pointOffset = ParamOffset(function, STR("AbsoluteCoordinate"));
            const auto returnOffset = ParamOffset(function, STR("ReturnValue"));
            const auto parmsSize = static_cast<size_t>(function->GetPropertiesSize());
            if (geometryOffset < 0 || pointOffset < 0 || returnOffset < 0 ||
                static_cast<size_t>(geometryOffset) + geometry.size() > parmsSize || static_cast<size_t>(returnOffset) >= parmsSize)
            {
                return false;
            }
            std::vector<uint8_t> parms(parmsSize, 0);
            std::memcpy(parms.data() + geometryOffset, geometry.data(), geometry.size());
            std::memcpy(parms.data() + pointOffset, &point, sizeof(Vec2));
            library->ProcessEvent(function, parms.data());
            return parms[static_cast<size_t>(returnOffset)] != 0;
        }

        // Pointer-over test from last-painted geometry, immune to input gating.
        auto WidgetUnderPoint(UObject* widget, Vec2 point) -> bool
        {
            if (!widget)
            {
                return false;
            }
            std::vector<uint8_t> geometry{};
            return CachedGeometry(widget, geometry) && GeometryUnderLocation(geometry, point);
        }

        auto CollectChildren(UObject* panel) -> std::vector<UObject*>
        {
            std::vector<UObject*> children{};

            auto* countFunction = panel->GetFunctionByNameInChain(STR("GetChildrenCount"));
            auto* childFunction = panel->GetFunctionByNameInChain(STR("GetChildAt"));
            if (!countFunction || !childFunction)
            {
                return children;
            }

            struct CountParams
            {
                int32_t ReturnValue{};
            } countParams{};
            panel->ProcessEvent(countFunction, &countParams);

            for (int32_t index = 0; index < countParams.ReturnValue; ++index)
            {
                struct ChildParams
                {
                    int32_t Index{};
                    int32_t Padding{};
                    UObject* ReturnValue{};
                } childParams{index, 0, nullptr};
                panel->ProcessEvent(childFunction, &childParams);
                if (childParams.ReturnValue)
                {
                    children.push_back(childParams.ReturnValue);
                }
            }

            return children;
        }

        // A widget's outer must be the WidgetTree that owns the screen, never the panel it is
        // added to. Every mod that does this successfully on Palworld constructs against the
        // WidgetTree; outering to a panel leaves the widget outside the tree that manages its
        // lifetime, and it is dereferenced after the tree tears down.
        auto FindWidgetTree(UObject* canvas) -> UObject*
        {
            auto* widgetTreeClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/UMG.WidgetTree"));
            if (!widgetTreeClass)
            {
                return nullptr;
            }
            return canvas->GetTypedOuter(widgetTreeClass);
        }

        // Total ownership requires the native filter to be empty, not just hidden: the vanilla
        // Filter window (or a prior session) can leave FPalUIPaldexFilterInfo narrowing
        // displayInfoArray underneath our own row-level filtering, which our predicates would never
        // see. ResetFiltering takes no parameters, so a wrong stack frame here would corrupt memory;
        // every step is resolved and checked before the call, and a failure is logged rather than
        // guessed past, per the project's fail-safe suppression rules.
        auto ResetVanillaFiltering(UObject* canvas) -> void
        {
            auto* screenClass = UObjectGlobals::StaticFindObject<UClass*>(
                    nullptr, nullptr, STR("/Game/Pal/Blueprint/UI/UserInterface/MainMenu/Paldex/WBP_Paldex.WBP_Paldex_C"));
            if (!screenClass)
            {
                Output::send<LogLevel::Warning>(
                        STR("[PalpediaEnhancer] WBP_Paldex_C class not found; vanilla filter state may still be active\n"));
                return;
            }

            auto* screen = canvas->GetTypedOuter(screenClass);
            if (!screen)
            {
                Output::send<LogLevel::Warning>(
                        STR("[PalpediaEnhancer] Could not reach the Palpedia screen from its canvas; vanilla filter state may still be active\n"));
                return;
            }

            auto* function = screen->GetFunctionByNameInChain(STR("ResetFiltering"));
            if (!function)
            {
                Output::send<LogLevel::Warning>(
                        STR("[PalpediaEnhancer] ResetFiltering not found on the Palpedia screen; vanilla filter state may still be active\n"));
                return;
            }

            screen->ProcessEvent(function, nullptr);
            Verbose(STR("[PalpediaEnhancer] Reset vanilla Palpedia filtering\n"));
        }

        // The rows live in a ScrollBox, and the panel's child order is the display order. The box
        // is NOT reachable by the generated name ScrollBox_0 (that lookup resolved stale objects);
        // it must be found the way the working prior art finds it: scan the list's widget-tree root
        // children for the one whose class name contains "ScrollBox".
        auto ResolveListScrollBox(UObject* list) -> UObject*
        {
            // WBP_PaldexScrollList owns its WidgetTree as a property. GetTypedOuter is not
            // reliable for this Blueprint instance and was why every sort attempt reached the
            // resolver but found no ScrollBox.
            UObject* tree = nullptr;
            if (auto** widgetTree = list->GetValuePtrByPropertyNameInChain<UObject*>(STR("WidgetTree")); widgetTree && *widgetTree)
            {
                tree = *widgetTree;
            }
            if (!tree)
            {
                return nullptr;
            }
            UObject* root = nullptr;
            if (auto** rootWidget = tree->GetValuePtrByPropertyNameInChain<UObject*>(STR("RootWidget")); rootWidget && *rootWidget)
            {
                root = *rootWidget;
            }
            if (!root)
            {
                return nullptr;
            }

            for (auto* child : CollectChildren(root))
            {
                if (!child || !child->GetClassPrivate())
                {
                    continue;
                }
                if (child->GetClassPrivate()->GetPathName().find(STR("ScrollBox")) != StringType::npos)
                {
                    return child;
                }
            }
            return nullptr;
        }

        // Moves one child to the end of its panel: RemoveChild then AddChild, which appends. That
        // is exactly the shape the prior art proved in-game: AddChild appends and the panel itself
        // keeps its state consistent, unlike raw RemoveFromParent on the wrong object.
        auto PanelMoveToEnd(UObject* panel, UObject* child) -> bool
        {
            if (!panel || !child)
            {
                return false;
            }
            auto* remove = panel->GetFunctionByNameInChain(STR("RemoveChild"));
            auto* add = panel->GetFunctionByNameInChain(STR("AddChild"));
            if (!remove || !add)
            {
                return false;
            }

            struct RemoveParams
            {
                UObject* Content{};
                bool ReturnValue{};
            } removeParams{child, false};
            panel->ProcessEvent(remove, &removeParams);
            if (!removeParams.ReturnValue)
            {
                return false;
            }

            struct AddParams
            {
                UObject* Content{};
                UObject* ReturnValue{};
            } addParams{child, nullptr};
            panel->ProcessEvent(add, &addParams);
            if (!addParams.ReturnValue)
            {
                return false;
            }
            return true;
        }

        // Our artwork ships beside the dll, so the path is derived from this module's own location
        // rather than assumed from the game directory.
        // KismetRenderingLibrary can build a Texture2D from a file on disk at runtime, which is why
        // this mod needs no cooked pak for its artwork.
        auto LoadTexture(UObject* worldContext, const wchar_t* fileName) -> UObject*
        {
            auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetRenderingLibrary"));
            if (!library)
            {
                return nullptr;
            }

            auto* import = library->GetFunctionByNameInChain(STR("ImportFileAsTexture2D"));
            if (!import)
            {
                return nullptr;
            }

            const auto path = (AssetsDirectory() / fileName).wstring();
            if (!std::filesystem::exists(path))
            {
                Output::send<LogLevel::Warning>(std::format(STR("[PalpediaEnhancer] Missing artwork: {}\n"), StringType{path.begin(), path.end()}));
                return nullptr;
            }

            struct Params
            {
                UObject* WorldContextObject{};
                FString Filename{};
                UObject* ReturnValue{};
            } params{worldContext, FString(StringType{path.begin(), path.end()}), nullptr};

            library->ProcessEvent(import, &params);
            return params.ReturnValue;
        }

        // Every widget this mod creates is rooted for as long as the mod holds a raw pointer to
        // it, and unrooted again in ForgetWidgets once the screen that owned it is gone. The
        // engine's own widget tree keeps them alive in practice; the root is belt and braces
        // against a GC pass between construction and the AddChild call that parents them.
        std::vector<UObject*> g_rootedWidgets;

        auto RootCreatedWidget(UObject* widget) -> void
        {
            if (widget)
            {
                widget->SetRootSet();
                g_rootedWidgets.push_back(widget);
            }
        }

        auto UnrootWidgets() -> void
        {
            for (auto* widget : g_rootedWidgets)
            {
                widget->ClearRootSet();
            }
            g_rootedWidgets.clear();
        }

        auto CreateWidget(UObject* widgetTree, const CharType* classPath) -> UObject*
        {
            auto* widgetClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, classPath);
            if (!widgetClass)
            {
                return nullptr;
            }
            auto* widget = UObjectGlobals::NewObject<UObject>(widgetTree, widgetClass);
            RootCreatedWidget(widget);
            return widget;
        }

        // Prefer the game's own widget classes. They are styled like the rest of the UI and are
        // built for this game's input handling; the engine primitives are the fallback.
        auto CreateWidgetPreferred(UObject* widgetTree, const CharType* preferred, const CharType* fallback) -> UObject*
        {
            if (auto* widget = CreateWidget(widgetTree, preferred))
            {
                return widget;
            }
            return CreateWidget(widgetTree, fallback);
        }

        auto SameAnchors(const Anchors& left, const Anchors& right) -> bool
        {
            const auto close = [](double a, double b) {
                return (a - b) < 0.001 && (b - a) < 0.001;
            };
            return close(left.Minimum.X, right.Minimum.X) && close(left.Minimum.Y, right.Minimum.Y) &&
                   close(left.Maximum.X, right.Maximum.X) && close(left.Maximum.Y, right.Maximum.Y);
        }

        auto ReadSlotLayout(UObject* slot) -> SlotLayout
        {
            SlotLayout layout{};
            if (auto* getAnchors = slot->GetFunctionByNameInChain(STR("GetAnchors")))
            {
                struct Params
                {
                    Anchors ReturnValue{};
                } params{};
                slot->ProcessEvent(getAnchors, &params);
                layout.SlotAnchors = params.ReturnValue;
                layout.Valid = true;
            }
            layout.Alignment = CallReturningVec2(slot, STR("GetAlignment"));
            return layout;
        }

        // FMargin: four floats, the slot's Offsets (Left, Top, Right, Bottom). For a point-anchored
        // axis Right/Bottom hold the size, for a stretched axis the inset from the far anchor.
        struct Margin
        {
            float Left{};
            float Top{};
            float Right{};
            float Bottom{};
        };

        auto ReadSlotOffsets(UObject* slot, Margin& out) -> bool
        {
            auto* function = slot ? slot->GetFunctionByNameInChain(STR("GetOffsets")) : nullptr;
            if (!function || static_cast<size_t>(function->GetStructureSize()) != sizeof(Margin))
            {
                return false;
            }
            struct Params
            {
                Margin ReturnValue{};
            } params{};
            slot->ProcessEvent(function, &params);
            out = params.ReturnValue;
            return true;
        }

        // One axis of a widget's rectangle in the space every canvas on the Palpedia screen
        // shares: coordinates relative to the root canvas's center. Every panel on that screen
        // hangs off the root at anchor 0.5, so the root's own size never has to be known.
        struct AxisFrame
        {
            bool SizeKnown{};
            double Left{};
            double Size{};
            double Center{};
        };
        struct Frame
        {
            AxisFrame X{};
            AxisFrame Y{};
        };

        // Applies one canvas slot to its parent's frame along one axis. A point anchor resolves
        // against a known parent edge, or against the center when the anchor is 0.5; a stretched
        // axis needs the parent's size, except the full-stretch zero-offset case, which inherits
        // the parent's frame outright. Anything else is unknown rather than guessed.
        auto ApplyAxis(const AxisFrame& parent, double anchorMin, double anchorMax, double offsetNear, double offsetFar, double alignment, AxisFrame& out)
                -> bool
        {
            constexpr double epsilon = 1e-6;
            if (std::abs(anchorMin - anchorMax) < epsilon)
            {
                double anchorPosition{};
                if (parent.SizeKnown)
                {
                    anchorPosition = parent.Left + anchorMin * parent.Size;
                }
                else if (std::abs(anchorMin - 0.5) < epsilon)
                {
                    anchorPosition = parent.Center;
                }
                else
                {
                    return false;
                }
                out.Left = anchorPosition + offsetNear - alignment * offsetFar;
                out.Size = offsetFar;
            }
            else if (parent.SizeKnown)
            {
                out.Left = parent.Left + anchorMin * parent.Size + offsetNear;
                out.Size = (parent.Left + anchorMax * parent.Size - offsetFar) - out.Left;
            }
            else if (anchorMin < epsilon && std::abs(anchorMax - 1.0) < epsilon && std::abs(offsetNear) < epsilon && std::abs(offsetFar) < epsilon)
            {
                out = parent;
                return true;
            }
            else
            {
                return false;
            }
            out.SizeKnown = true;
            out.Center = out.Left + out.Size / 2.0;
            return true;
        }

        // A widget's frame, walking its canvas slots up to the screen's root widget (the one with
        // no slot), which is returned so two frames can be checked to share a root. Depth-limited:
        // the chain is a few panels deep here and a cycle must never hang the game thread.
        auto FrameOf(UObject* widget, Frame& out, UObject*& root) -> bool
        {
            std::vector<UObject*> slots{};
            UObject* current = widget;
            for (int depth = 0; current && depth < 12; ++depth)
            {
                auto** slot = current->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot"));
                if (!slot || !*slot)
                {
                    break;
                }
                slots.push_back(*slot);
                auto** parent = (*slot)->GetValuePtrByPropertyNameInChain<UObject*>(STR("Parent"));
                current = (parent && *parent) ? *parent : nullptr;
            }
            if (!current)
            {
                return false;
            }
            if (auto** slot = current->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot")); slot && *slot)
            {
                return false; // depth limit hit before the root
            }
            root = current;
            Frame frame{};
            for (auto it = slots.rbegin(); it != slots.rend(); ++it)
            {
                const auto layout = ReadSlotLayout(*it);
                Margin offsets{};
                if (!layout.Valid || !ReadSlotOffsets(*it, offsets))
                {
                    return false;
                }
                Frame next{};
                if (!ApplyAxis(frame.X, layout.SlotAnchors.Minimum.X, layout.SlotAnchors.Maximum.X, offsets.Left, offsets.Right, layout.Alignment.X, next.X) ||
                    !ApplyAxis(frame.Y, layout.SlotAnchors.Minimum.Y, layout.SlotAnchors.Maximum.Y, offsets.Top, offsets.Bottom, layout.Alignment.Y, next.Y))
                {
                    return false;
                }
                frame = next;
            }
            out = frame;
            return true;
        }

        // Palworld v1.0.4 added its own search strip to the Palpedia: a text box, the magnifier
        // that applies the search, and a filter button, in one container along the bottom of the
        // list column. This mod already provides all three in a richer form, so the strip is
        // consumed: hidden, and its place taken by this mod's own row. The strip is found from the
        // text box, which is a named member of WBP_Paldex, walked up to whichever ancestor is a
        // direct child of our canvas, so no assumption is made about how deep the container sits.
        // A build before v1.0.4 has no such member and nothing here fires.
        auto NativeSearchStrip(UObject* canvas) -> UObject*
        {
            if (!canvas)
            {
                return nullptr;
            }
            auto* screenClass = UObjectGlobals::StaticFindObject<UClass*>(
                    nullptr, nullptr, STR("/Game/Pal/Blueprint/UI/UserInterface/MainMenu/Paldex/WBP_Paldex.WBP_Paldex_C"));
            if (!screenClass)
            {
                return nullptr;
            }
            auto* screen = canvas->GetTypedOuter(screenClass);
            if (!screen)
            {
                return nullptr;
            }
            auto** box = screen->GetValuePtrByPropertyNameInChain<UObject*>(STR("PalEditableTextBox_Search"));
            if (!box || !*box)
            {
                return nullptr;
            }

            UObject* current = *box;
            for (int depth = 0; current && depth < 12; ++depth)
            {
                auto** slot = current->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot"));
                if (!slot || !*slot)
                {
                    return nullptr;
                }
                auto** parent = (*slot)->GetValuePtrByPropertyNameInChain<UObject*>(STR("Parent"));
                if (!parent || !*parent)
                {
                    return nullptr;
                }
                if (*parent == canvas)
                {
                    return current;
                }
                current = *parent;
            }
            return nullptr;
        }
        // The strip is anchored to the canvas bottom while the list is anchored to its top, so the
        // strip's own slot numbers cannot be handed to a row placed in the list's anchor space.
        // Both frames are resolved against the same root and subtracted instead.
        auto StripTopInCanvas(UObject* canvas, UObject* strip, double& outTop) -> bool
        {
            if (!canvas || !strip)
            {
                return false;
            }
            Frame canvasFrame{};
            Frame stripFrame{};
            UObject* canvasRoot{};
            UObject* stripRoot{};
            if (!FrameOf(canvas, canvasFrame, canvasRoot) || !FrameOf(strip, stripFrame, stripRoot))
            {
                return false;
            }
            if (canvasRoot != stripRoot || !canvasFrame.Y.SizeKnown || !stripFrame.Y.SizeKnown)
            {
                return false;
            }
            outTop = stripFrame.Y.Left - canvasFrame.Y.Left;
            return true;
        }
        auto AttachToCanvas(UObject* canvas, UObject* widget, Vec2 position, Vec2 size, bool autoSize, const SlotLayout& layout = {}, int32_t zOrder = 50)
                -> bool
        {
            auto* addChild = canvas->GetFunctionByNameInChain(STR("AddChildToCanvas"));
            if (!addChild)
            {
                return false;
            }

            struct AddParams
            {
                UObject* Content{};
                UObject* ReturnValue{};
            } addParams{widget, nullptr};
            canvas->ProcessEvent(addChild, &addParams);

            auto* slot = addParams.ReturnValue;
            if (!slot)
            {
                return false;
            }

            // Match the anchors of the widget whose coordinates we copied, or our position means
            // something different from theirs.
            if (layout.Valid)
            {
                if (auto* setAnchors = slot->GetFunctionByNameInChain(STR("SetAnchors")))
                {
                    struct AnchorParams
                    {
                        Anchors InAnchors{};
                    } anchorParams{layout.SlotAnchors};
                    slot->ProcessEvent(setAnchors, &anchorParams);
                }
                CallWithVec2(slot, STR("SetAlignment"), layout.Alignment);
            }

            // Above the list, so our row always wins the hit test. The filter window asks for a
            // higher order still, because it has to sit over the row that opened it.
            if (auto* setZOrder = slot->GetFunctionByNameInChain(STR("SetZOrder")))
            {
                struct ZParams
                {
                    int32_t InZOrder{};
                } zParams{zOrder};
                slot->ProcessEvent(setZOrder, &zParams);
            }

            // A canvas slot defaults to zero size and renders nothing.
            if (auto* setAutoSize = slot->GetFunctionByNameInChain(STR("SetAutoSize")))
            {
                struct AutoSizeParams
                {
                    bool bInAutoSize{};
                } autoSizeParams{autoSize};
                slot->ProcessEvent(setAutoSize, &autoSizeParams);
            }

            if (!autoSize)
            {
                CallWithVec2(slot, STR("SetSize"), size);
            }
            CallWithVec2(slot, STR("SetPosition"), position);
            return true;
        }

        // GetOwningPlayer() on a widget reached via reflection rather than normal UMG construction
        // unreliably returns null - already documented and worked around once for the search box
        // (see TypingPlayerController). Every button this file creates asks its not-yet-attached
        // canvas/window for an owning player the exact same unreliable way, so it is very likely
        // that most or all of them have been getting created with no owning player at all. A
        // CommonUI button with no owning player has nothing to register itself against in that
        // player's input/focus subsystem, which is a plausible root cause for a periodic,
        // engine-driven crash unrelated to anything this mod calls per tick - see the design doc,
        // 2026-08-21. Falling back to a live player controller found by class, same as
        // TypingPlayerController already does, means a button never ends up with none.
        auto ReliableOwningPlayer(UObject* canvas) -> UObject*
        {
            if (canvas)
            {
                if (auto* controller = CallNoArgs(canvas, STR("GetOwningPlayer")))
                {
                    return controller;
                }
            }
            if (auto* controller = UObjectGlobals::FindFirstOf(STR("PalPlayerController")))
            {
                return controller;
            }
            return UObjectGlobals::FindFirstOf(STR("PlayerController"));
        }

        // A UUserWidget Blueprint cannot be built with NewObject; it goes through
        // WidgetBlueprintLibrary::Create. Using the game's own button class is what makes these
        // look native: it carries the styling, the activation overlay, hover and click audio.
        //
        // initialVisibility is applied before the button ever joins the canvas, not after. A
        // caller that means to hide this button right away (the search-target toggles are the
        // only one) previously attached it to a live, visible canvas first - Visible by
        // default - and hid it with a separate call microseconds later. That gave the button a
        // real, if brief, window of being live and focus-eligible on a visible canvas, which the
        // Filter/Sort popup windows were already fixed to never do (v1.1.2 collapses the whole
        // window before adding any child). Setting the final visibility first means a button
        // meant to start hidden never has that window at all. See the design doc, 2026-08-21.
        auto MakeVanillaButton(UObject* canvas,
                                const StringType& caption,
                                Vec2 position,
                                Vec2 size,
                                const SlotLayout& layout,
                                uint8_t initialVisibility = SlateVisible) -> UObject*
        {
            auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
            auto* buttonClass = UObjectGlobals::StaticFindObject<UClass*>(
                    nullptr, nullptr, STR("/Game/Pal/Blueprint/UI/UserInterface/Common/WBP_CommonButton_Activation.WBP_CommonButton_Activation_C"));
            if (!library || !buttonClass)
            {
                return nullptr;
            }

            auto* create = library->GetFunctionByNameInChain(STR("Create"));
            if (!create)
            {
                return nullptr;
            }

            struct CreateParams
            {
                UObject* WorldContextObject{};
                UClass* WidgetType{};
                UObject* OwningPlayer{};
                UObject* ReturnValue{};
            } createParams{canvas, buttonClass, ReliableOwningPlayer(canvas), nullptr};
            library->ProcessEvent(create, &createParams);

            auto* button = createParams.ReturnValue;
            if (!button)
            {
                return nullptr;
            }
            RootCreatedWidget(button);

            SetWidgetVisibility(button, initialVisibility);

            if (auto* setText = button->GetFunctionByNameInChain(STR("SetText")))
            {
                struct TextParams
                {
                    Text::RawText Text;
                } textParams{Text::Make(caption)};
                if (textParams.Text.HasData())
                {
                    button->ProcessEvent(setText, &textParams);
                }
            }

            if (!AttachToCanvas(canvas, button, position, size, false, layout))
            {
                return nullptr;
            }
            return button;
        }

        // The artwork sits on top of the vanilla button, which keeps handling clicks, hover audio
        // and focus underneath. HitTestInvisible is what lets the click pass straight through.
        auto MakeArtwork(UObject* canvas, UObject* widgetTree, const wchar_t* fileName, Vec2 center, Vec2 size, const SlotLayout& layout)
                -> UObject*
        {
            auto* texture = LoadTexture(canvas, fileName);
            if (!texture)
            {
                return nullptr;
            }

            auto* image = CreateWidget(widgetTree, STR("/Script/UMG.Image"));
            if (!image)
            {
                return nullptr;
            }

            if (auto* setBrush = image->GetFunctionByNameInChain(STR("SetBrushFromTexture")))
            {
                struct BrushParams
                {
                    UObject* Texture{};
                    bool bMatchSize{};
                } brushParams{texture, false};
                image->ProcessEvent(setBrush, &brushParams);
            }

            if (!AttachToCanvas(canvas, image, center, size, false, layout))
            {
                return nullptr;
            }

            SetWidgetVisibility(image, 3); // HitTestInvisible
            return image;
        }

        // A frame texture drawn as a Slate "Box": the corners keep their native pixel size, the
        // edges stretch along one axis and the center along both. One 160x90 texture therefore
        // serves every button size in the mod without the corner ornaments ever distorting. The
        // margins are fractions of the texture, per FSlateBrush::Margin for ESlateBrushDrawType::Box.
        // ESlateBrushDrawType: Box = 1 (nine-slice), Image = 3 (stretched, or tiled per Tiling).
        // ESlateBrushTileType: NoTile = 0, Both = 3.
        struct NineSlice
        {
            const wchar_t* File;
            float Left;
            float Top;
            float Right;
            float Bottom;
            uint8_t DrawAs = 1;
            uint8_t Tiling = 0;
        };
        constexpr NineSlice kButtonFrame{L"button.png", 0.2f, 0.2f, 0.2f, 0.2f};
        // The window background is three layers: a stretched vertical gradient in the game's
        // menu palette, a faint tiled chevron like the Palpedia's own detail panel, and the frame
        // (interior transparent) on top.
        constexpr NineSlice kPanelGradient{L"panel_gradient.png", 0.0f, 0.0f, 0.0f, 0.0f, 3, 0};
        constexpr NineSlice kPanelPattern{L"panel_pattern.png", 0.0f, 0.0f, 0.0f, 0.0f, 3, 3};
        constexpr NineSlice kPanelPlate{L"panel.png", 0.2f, 0.2f, 0.2f, 0.2f};

        // FSlateBrush layout, from the game's own reflection dump (SlateCore.hpp): DrawAs at 0x11,
        // Margin (FMargin, four floats) at 0x28. SetBrushFromTexture with bMatchSize=true fills
        // ImageSize with the texture size, which is what the Box corner size is computed from.
        constexpr size_t kBrushDrawAsOffset = 0x11;
        constexpr size_t kBrushTilingOffset = 0x12;
        constexpr size_t kBrushMarginOffset = 0x28;

        auto MakeFrame(UObject* canvas,
                       UObject* widgetTree,
                       const NineSlice& art,
                       Vec2 center,
                       Vec2 size,
                       const SlotLayout& layout,
                       int32_t zOrder,
                       uint8_t initialVisibility) -> UObject*
        {
            auto* texture = LoadTexture(canvas, art.File);
            if (!texture)
            {
                return nullptr;
            }

            auto* image = CreateWidget(widgetTree, STR("/Script/UMG.Image"));
            if (!image)
            {
                return nullptr;
            }

            SetWidgetVisibility(image, initialVisibility);

            if (auto* setBrush = image->GetFunctionByNameInChain(STR("SetBrushFromTexture")))
            {
                struct BrushParams
                {
                    UObject* Texture{};
                    bool bMatchSize{};
                } brushParams{texture, true};
                image->ProcessEvent(setBrush, &brushParams);
            }

            // SImage reads the UImage's brush by pointer at paint time, so editing it in place
            // after SetBrushFromTexture is seen on the next frame with no further call.
            if (auto* brush = image->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Brush")))
            {
                brush[kBrushDrawAsOffset] = art.DrawAs;
                brush[kBrushTilingOffset] = art.Tiling;
                const float margin[4]{art.Left, art.Top, art.Right, art.Bottom};
                std::memcpy(brush + kBrushMarginOffset, margin, sizeof(margin));
            }

            if (!AttachToCanvas(canvas, image, center, size, false, layout, zOrder))
            {
                return nullptr;
            }
            return image;
        }

        // Stretched-anchor variant for the popup windows, where every control is a fraction of
        // the window rather than a pixel center and size.
        auto MakeFrameIn(UObject* window, UObject* widgetTree, const NineSlice& art, double x0, double y0, double x1, double y1,
                         uint8_t initialVisibility = 3) -> UObject*
        {
            SlotLayout layout{};
            layout.Valid = true;
            layout.SlotAnchors = Anchors{Vec2{x0, y0}, Vec2{x1, y1}};
            layout.Alignment = Vec2{0.0, 0.0};
            return MakeFrame(window, widgetTree, art, Vec2{0.0, 0.0}, Vec2{0.0, 0.0}, layout, 0, initialVisibility);
        }

        // The game's own UI font, read off any live PalTextBlockBase. FSlateFontInfo layout from
        // the reflection dump: FontObject at 0x0, a TSharedPtr CompositeFont at 0x38 (unreflected,
        // zeroed in the copy: with a FontObject set, Slate never reads it), Size at 0x50.
        constexpr size_t kFontInfoSize = 0x60;
        constexpr size_t kFontCompositeOffset = 0x38;
        constexpr size_t kFontSizeOffset = 0x50;

        struct GameFont
        {
            uint8_t Info[kFontInfoSize]{};
            int32_t Size{};
            bool Valid{};
        };

        auto FindGameFont() -> GameFont
        {
            GameFont found{};
            auto* textClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Pal.PalTextBlockBase"));
            if (!textClass)
            {
                return found;
            }

            UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
                if (!object || !object->IsA(textClass) ||
                    object->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)))
                {
                    return LoopAction::Continue;
                }
                const auto* font = object->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Font"));
                if (!font || !*reinterpret_cast<UObject* const*>(font))
                {
                    return LoopAction::Continue;
                }
                const auto size = *reinterpret_cast<const int32_t*>(font + kFontSizeOffset);
                if (size < 12 || size > 48)
                {
                    return LoopAction::Continue;
                }
                std::memcpy(found.Info, font, kFontInfoSize);
                std::memset(found.Info + kFontCompositeOffset, 0, 16);
                found.Size = size;
                found.Valid = true;
                return LoopAction::Break;
            });

            if (found.Valid)
            {
                auto* fontObject = *reinterpret_cast<UObject**>(found.Info);
                Output::send<LogLevel::Normal>(std::format(STR("[PalpediaEnhancer] Using the game's UI font {} at size {}\n"),
                                                           fontObject->GetName(),
                                                           found.Size));
            }
            else
            {
                Output::send<LogLevel::Warning>(STR("[PalpediaEnhancer] No game text block found to copy the UI font from; labels use the engine default\n"));
            }
            return found;
        }

        // Resolved once per screen build (see EnsureWidgets); every label made afterwards uses it.
        GameFont g_gameFont{};

        auto ApplyGameFont(UObject* label, double scale) -> void
        {
            if (!label || !g_gameFont.Valid)
            {
                return;
            }
            auto* setFont = label->GetFunctionByNameInChain(STR("SetFont"));
            if (!setFont)
            {
                return;
            }
            alignas(16) uint8_t info[kFontInfoSize];
            std::memcpy(info, g_gameFont.Info, kFontInfoSize);
            *reinterpret_cast<int32_t*>(info + kFontSizeOffset) = static_cast<int32_t>(g_gameFont.Size * scale + 0.5);
            label->ProcessEvent(setFont, info);
        }

        // UTextBlock::SetColorAndOpacity takes an FSlateColor, not an FLinearColor: the color
        // plus a one-byte rule that must read "use the specified color". Passing only the four
        // floats left that byte to whatever was on the stack, which is why the active gold
        // showed up on some buttons and not others.
        auto SetLabelColor(UObject* label, LinearColor color) -> void
        {
            if (!label)
            {
                return;
            }
            alignas(16) uint8_t params[0x20]{};
            WriteSlateColor(params, 0, color);
            if (auto* setColor = label->GetFunctionByNameInChain(STR("SetColorAndOpacity")))
            {
                label->ProcessEvent(setColor, params);
            }
        }

        // Off: the plain frame with a champagne caption. On: gold caption and a gold frame, so a
        // toggled button reads as lit from across the room rather than by caption color alone.
        const LinearColor kLabelIdle = Palette::SoftChampagne;
        const LinearColor kLabelActive = Palette::ForgedGold;
        // Not a color choice: the multiply identity, which leaves the button art as authored.
        constexpr LinearColor kFrameIdle{1.0f, 1.0f, 1.0f, 1.0f};
        // Clear All is an action, not a state, and it undoes everything around it. The palette has
        // no red (section 2.2 rule 5), so a destructive control is an amber fill carrying black
        // text: still the one control in the panel that is not a filter, without a hue of its own.
        const LinearColor kLabelClear = Palette::ObsidianBlack;
        const LinearColor kFrameClear = Palette::AlertAmber;
        const LinearColor kFillClear = WithAlpha(Palette::AlertAmber, 0.92f);

        const LinearColor kFrameActive = Palette::ForgedGold;

        auto SetImageTint(UObject* image, LinearColor color) -> void
        {
            if (!image)
            {
                return;
            }
            struct ColorParams
            {
                LinearColor Color{};
            } colorParams{color};
            if (auto* setColor = image->GetFunctionByNameInChain(STR("SetColorAndOpacity")))
            {
                image->ProcessEvent(setColor, &colorParams);
            }
        }

        // A flat color plate: a textureless UMG Image (the default brush is a white box) tinted
        // and stretched over a fraction of the window, inset so the frame art around it stays
        // visible. HitTestInvisible; the button underneath keeps the click.
        auto MakeFillIn(UObject* window, UObject* widgetTree, double x0, double y0, double x1, double y1, LinearColor color) -> UObject*
        {
            auto* image = CreateWidget(widgetTree, STR("/Script/UMG.Image"));
            if (!image)
            {
                return nullptr;
            }
            SetWidgetVisibility(image, 3);
            SlotLayout layout{};
            layout.Valid = true;
            layout.SlotAnchors = Anchors{Vec2{x0, y0}, Vec2{x1, y1}};
            layout.Alignment = Vec2{0.0, 0.0};
            if (!AttachToCanvas(window, image, Vec2{0.0, 0.0}, Vec2{0.0, 0.0}, false, layout, 0))
            {
                return nullptr;
            }
            SetImageTint(image, color);
            return image;
        }

        // The game's own localized name for an element or work suitability, from the same
        // PalUIUtility calls the vanilla filter window uses, so those captions follow the player's
        // language. Everything else on the panels is English; see README "Languages".
        auto LocalizedCaption(UObject* worldContext, const FilterAction& action) -> StringType
        {
            const CharType* functionName = nullptr;
            const CharType* enumParam = nullptr;
            if (action.What == FilterAction::Kind::Element)
            {
                functionName = STR("GetPalElementTypeName");
                enumParam = STR("ElementType");
            }
            else if (action.What == FilterAction::Kind::WorkSuitability)
            {
                functionName = STR("GetWorkSuitabilityName");
                enumParam = STR("WorkSuitability");
            }
            else
            {
                return {};
            }

            auto* utility = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUIUtility"));
            auto* function = utility ? utility->GetFunctionByNameInChain(functionName) : nullptr;
            if (!function)
            {
                return {};
            }
            const auto contextOffset = ParamOffset(function, STR("WorldContextObject"));
            const auto enumOffset = ParamOffset(function, enumParam);
            const auto outOffset = ParamOffset(function, STR("outName"));
            if (contextOffset < 0 || enumOffset < 0 || outOffset < 0)
            {
                return {};
            }

            std::vector<uint8_t> parms(static_cast<size_t>(function->GetPropertiesSize()), 0);
            *reinterpret_cast<UObject**>(parms.data() + contextOffset) = worldContext;
            parms[static_cast<size_t>(enumOffset)] = static_cast<uint8_t>(action.Value);
            utility->ProcessEvent(function, parms.data());

            auto text = Text::ToString(parms.data() + outOffset);
            // The engine hands back its own key (or nothing) for an unknown value; keep English then.
            if (text.empty() || text.front() == L'?' || text.find(STR("_")) != StringType::npos)
            {
                return {};
            }
            return text;
        }

        // The vanilla button widgets do not stretch their internal text to our cell size, which is
        // why the earlier captions spilled past the button edges. The reliable fix is to stop
        // asking the button to draw its own caption and instead lay our own text block over the
        // button, centered in the cell and sized entirely by us. The label is HitTestInvisible so
        // the click still lands on the button underneath.
        auto MakeCenteredLabel(UObject* window, UObject* widgetTree, const CharType* text, double x0, double y0, double x1,
                               double y1, double fontScale = 1.0) -> UObject*
        {
            // The standard UMG.TextBlock class is used rather than PalTextBlockBase: it is the same
            // class the window title already renders with, and constructing a PalTextBlockBase from
            // NewObject on a fresh window proved a crash surface on open.
            auto* label = CreateWidget(widgetTree, STR("/Script/UMG.TextBlock"));
            if (!label)
            {
                return nullptr;
            }

            SetWidgetText(label, text);
            ApplyGameFont(label, fontScale);

            SetLabelColor(label, kLabelIdle);

            if (auto* setJustification = label->GetFunctionByNameInChain(STR("SetJustification")))
            {
                struct JustifyParams
                {
                    uint8_t InJustification{1}; // ETextJustify::Center
                } justifyParams{};
                label->ProcessEvent(setJustification, &justifyParams);
            }

            // Anchored to the cell's center point and auto-sized, so the text block is exactly
            // as big as its text and its (0.5, 0.5) alignment lands it dead center both ways.
            // A stretched anchor would fill the cell and leave the text top-left inside it.
            SlotLayout layout{};
            layout.Valid = true;
            const Vec2 center{(x0 + x1) * 0.5, (y0 + y1) * 0.5};
            layout.SlotAnchors = Anchors{center, center};
            layout.Alignment = Vec2{0.5, 0.5};
            AttachToCanvas(window, label, Vec2{0.0, 0.0}, Vec2{0.0, 0.0}, true, layout, 1);
            SetWidgetVisibility(label, 3); // HitTestInvisible, centered over the button
            return label;
        }

        // Same idea as MakeCenteredLabel, for a button positioned the way the main row's buttons
        // are: a pixel center and size against a point anchor (MakeVanillaButton/MakeArtwork's
        // coordinate system), not a window's stretched fractional anchors. Returns the label so it
        // can be repositioned if the caller ever needs to.
        // initialVisibility is applied before the label ever joins the canvas - see the matching
        // note on MakeVanillaButton. Defaults to HitTestInvisible, this function's original
        // always-on behavior; the search-target toggle labels pass SlateHidden instead so they
        // are never live on the visible outer canvas even briefly.
        auto MakeCenteredLabelAt(UObject* canvas,
                                  UObject* widgetTree,
                                  const CharType* text,
                                  Vec2 center,
                                  Vec2 size,
                                  const SlotLayout& layout,
                                  uint8_t initialVisibility = 3) -> UObject*
        {
            auto* label = CreateWidget(widgetTree, STR("/Script/UMG.TextBlock"));
            if (!label)
            {
                return nullptr;
            }

            SetWidgetVisibility(label, initialVisibility);
            SetWidgetText(label, text);
            ApplyGameFont(label, 1.0);

            SetLabelColor(label, kLabelIdle);

            if (auto* setJustification = label->GetFunctionByNameInChain(STR("SetJustification")))
            {
                struct JustifyParams
                {
                    uint8_t InJustification{1}; // ETextJustify::Center
                } justifyParams{};
                label->ProcessEvent(setJustification, &justifyParams);
            }

            // Auto-sized at the center point, like MakeCenteredLabel: the block is exactly as big
            // as its text, so its alignment centers the text rather than parking it top-left
            // inside a block the size of the button. CenterLabels trims any remaining offset.
            (void)size;
            if (!AttachToCanvas(canvas, label, center, Vec2{0.0, 0.0}, true, layout, 51))
            {
                return nullptr;
            }
            return label;
        }

        // Everything we take over is identified from the canvas's own child list, so no object
        // pointer is ever read out of a property. Vanilla assets live under /Game/Pal/ and mod
        // assets under /Game/Mods/, so the path tells us who owns a widget when its name does not:
        // PalboxSearchPlus's search bar is called WBP_KeyGuard_C.
        auto StripControls(const std::vector<UObject*>& children,
                            const std::vector<UObject*>& ours,
                            bool logEveryChild,
                            std::unordered_set<UObject*>& alreadySuppressed,
                            UObject* consumed) -> void
        {
            int32_t index = 0;
            for (auto* child : children)
            {
                const int32_t currentIndex = index++;

                auto* childClass = child->GetClassPrivate();
                if (!childClass)
                {
                    continue;
                }

                // Our own buttons are instances of the same class as the vanilla Filter button, so
                // a class based rule hides them too. Identity wins over class. Checked after
                // logging, so ours still appear in the listing.
                const bool isOurs = std::find(ours.begin(), ours.end(), child) != ours.end();

                const auto classPath = childClass->GetPathName();
                if (logEveryChild)
                {
                    // Full slot geometry, because a position alone is meaningless without the
                    // anchors it is measured against, and the list and the vanilla button turned
                    // out to be anchored differently.
                    StringType geometry{};
                    if (auto** slot = child->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot")); slot && *slot)
                    {
                        const auto position = CallReturningVec2(*slot, STR("GetPosition"));
                        const auto size = CallReturningVec2(*slot, STR("GetSize"));
                        const auto layout = ReadSlotLayout(*slot);
                        geometry = std::format(STR("  pos=({},{}) size=({},{}) anchors=({},{})-({},{}) align=({},{})"),
                                               position.X,
                                               position.Y,
                                               size.X,
                                               size.Y,
                                               layout.SlotAnchors.Minimum.X,
                                               layout.SlotAnchors.Minimum.Y,
                                               layout.SlotAnchors.Maximum.X,
                                               layout.SlotAnchors.Maximum.Y,
                                               layout.Alignment.X,
                                               layout.Alignment.Y);
                    }

                    Verbose(std::format(STR("[PalpediaEnhancer] Canvas child {}{}: {}{}\n"),
                                                               currentIndex,
                                                               isOurs ? STR(" (ours)") : STR(""),
                                                               classPath,
                                                               geometry));
                }

                if (isOurs)
                {
                    continue;
                }

                const bool foreignMod = classPath.find(STR("/Game/Mods/")) != StringType::npos;
                const bool vanillaFilterButton = classPath.find(STR("WBP_CommonButton_Activation")) != StringType::npos;
                // v1.0.4's search strip, whose text box, magnifier and filter button this mod
                // replaces. Matched by identity, since it is an ordinary container class that
                // nothing else about it distinguishes.
                const bool nativeSearchStrip = consumed != nullptr && child == consumed;

                if (!foreignMod && !vanillaFilterButton && !nativeSearchStrip)
                {
                    continue;
                }

                // Only ever touch a given widget once. This loop runs every Update() tick, not
                // just when the screen rebuilds, so without this check MoveAside ran on the same
                // already-hidden widget on every single frame for as long as the Palpedia stayed
                // open - see the design doc, 2026-08-21.
                if (!alreadySuppressed.insert(child).second)
                {
                    continue;
                }

                if (logEveryChild)
                {
                    // Logged before, not just after, MoveAside: if this ever crashes inside
                    // MoveAside itself, the log names exactly which widget it was touching instead
                    // of leaving that to be inferred from the last child index printed.
                    Verbose(std::format(STR("[PalpediaEnhancer] Suppressing: {}\n"), classPath));
                }
                MoveAside(child);
            }
        }
    } // namespace

    // Expanding hides Sort and Filter so the field can occupy their space; folding restores them.
    // The typed value is never cleared, so a folded search is still an active filter.
    //
    // Hidden, never Collapsed, for every WBP_CommonButton_Activation instance here (Sort, Filter,
    // and the search-target toggles): that is the same "Pure virtual function being called" crash
    // the vanilla Filter button's suppression was already written around (see StripControls /
    // MoveAside), and it turns out to apply just as much to our own buttons of that class as to a
    // vanilla one. Collapsing removes a widget from layout; a CommonUI button still registered as a
    // focus candidate does not survive that. A Canvas child's siblings are unaffected by its
    // visibility either way, so Hidden costs nothing here.
    // The main row's Sort and Filter tiles have their captions baked into the artwork, centered by
    // eye. Every other caption is a live text block whose box the engine sizes from the font's
    // full line height, and where the glyphs sit inside that box depends on the font. Rather than
    // guess, this measures: once a panel has painted, each label's box center is compared with its
    // button's box center through Slate's own geometry, and the slot is moved by the difference.
    // Then a fixed glyph nudge, a fraction of the font size, puts the caps where the baked tiles
    // put theirs. Runs once per label.
    auto PalpediaUi::CenterLabels() -> void
    {
        constexpr double kGlyphNudge = 0.0; // fraction of font size, positive moves the text down
        int measured = 0;
        for (auto& toggle : m_centered)
        {
            if (toggle.Done || !toggle.Label || !toggle.Button)
            {
                continue;
            }
            std::vector<uint8_t> labelGeometry{};
            std::vector<uint8_t> buttonGeometry{};
            Vec2 labelSize{};
            Vec2 buttonSize{};
            if (!CachedGeometry(toggle.Label, labelGeometry) || !CachedGeometry(toggle.Button, buttonGeometry) ||
                !LocalSizeOf(labelGeometry, labelSize) || !LocalSizeOf(buttonGeometry, buttonSize) || labelSize.Y <= 0.0 ||
                buttonSize.Y <= 0.0)
            {
                continue; // not painted yet (collapsed panel); try again when it opens
            }
            // Shrink a caption that is wider than its tile (long work names in 7 columns, long
            // translations), then let the next pass measure again before centering.
            const double available = buttonSize.X * 0.90;
            if (labelSize.X > available && toggle.Attempts < 3)
            {
                toggle.FontScale *= available / labelSize.X;
                ++toggle.Attempts;
                ApplyGameFont(toggle.Label, toggle.FontScale);
                m_labelCenterPolls = 1;
                continue;
            }
            Vec2 labelCenter{};
            if (!ConvertLocalPoint(toggle.Label, toggle.Button, Vec2{labelSize.X * 0.5, labelSize.Y * 0.5}, labelCenter))
            {
                continue;
            }
            const Vec2 delta{buttonSize.X * 0.5 - labelCenter.X, buttonSize.Y * 0.5 - labelCenter.Y + labelSize.Y * kGlyphNudge};
            // Relative, never absolute: panel labels start at (0, 0) but the main row's and the
            // search toggles' labels are positioned in canvas pixels, and replacing that position
            // with the small correction teleported them to the canvas origin.
            if (auto** slot = toggle.Label->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot")); slot && *slot)
            {
                const Vec2 current = CallReturningVec2(*slot, STR("GetPosition"));
                CallWithVec2(*slot, STR("SetPosition"), Vec2{current.X + delta.X, current.Y + delta.Y});
            }
            toggle.Done = true;
            if (++measured <= 3)
            {
                Verbose(std::format(STR("[PalpediaEnhancer] Label center: button {}x{} label {}x{} center at ({}, {}) -> moved by ({}, {})\n"),
                                    buttonSize.X, buttonSize.Y, labelSize.X, labelSize.Y, labelCenter.X, labelCenter.Y, delta.X, delta.Y));
            }
        }
    }

    auto PalpediaUi::LiveRef::Set(UObject* object) -> void
    {
        Object = object;
        Index = object ? object->GetInternalIndex() : -1;
        Class = (object && object->GetClassPrivate()) ? object->GetClassPrivate()->GetNamePrivate() : FName{};
    }

    auto PalpediaUi::LiveRef::Reset() -> void
    {
        Object = nullptr;
        Index = -1;
        Class = FName{};
    }

    auto PalpediaUi::LiveRef::Alive() const -> bool
    {
        if (!Object || Index < 0)
        {
            Verbose(STR("[PalpediaEnhancer] LiveRef: unset\n"));
            return false;
        }
        auto* item = FUObjectArray::IndexToObject(Index);
        if (!item)
        {
            Verbose(std::format(STR("[PalpediaEnhancer] LiveRef: no item at index {}\n"), Index));
            return false;
        }
        if (item->GetUObject() != Object)
        {
            Verbose(std::format(STR("[PalpediaEnhancer] LiveRef: slot {} now holds {:#x}, expected {:#x}\n"),
                                Index, reinterpret_cast<uintptr_t>(item->GetUObject()), reinterpret_cast<uintptr_t>(Object)));
            return false;
        }
        if (item->IsUnreachable())
        {
            Verbose(std::format(STR("[PalpediaEnhancer] LiveRef: slot {} unreachable\n"), Index));
            return false;
        }
        auto* objectClass = Object->GetClassPrivate();
        if (!objectClass || !(objectClass->GetNamePrivate() == Class))
        {
            Verbose(std::format(STR("[PalpediaEnhancer] LiveRef: slot {} class changed to {}\n"), Index,
                                objectClass ? objectClass->GetNamePrivate().ToString() : StringType{STR("null")}));
            return false;
        }
        return true;
    }

    auto PalpediaUi::SearchToggleHovered() const -> bool
    {
        // Geometry-based on purpose, never IsHovered: while the typing session holds the
        // CommonUI gate, the game does not route pointer events to CommonUI buttons at all, so
        // IsHovered reports false on exactly the frames this test exists for. That dead test is
        // why the search-row buttons showed no hover feedback and ate the first click for the
        // whole life of the hover-release design (observed 2026-08-30, build 24575825). The
        // last-painted geometry and the platform mouse position work regardless of routing.
        // The set covers the magnifier, the clear X, and the search-target toggles: hovering any
        // of them opens the gate ahead of the click so the button reacts and takes the first
        // click, without committing the half-typed search.
        Vec2 mouse{};
        if (!PlatformMousePosition(mouse))
        {
            return false;
        }
        if (WidgetUnderPoint(m_searchButton, mouse) || WidgetUnderPoint(m_clearSearchButton, mouse))
        {
            return true;
        }
        for (const auto& toggle : m_toggles)
        {
            if (toggle.Action.What == FilterAction::Kind::SearchTarget && WidgetUnderPoint(toggle.Button, mouse))
            {
                return true;
            }
        }
        return false;
    }

    auto PalpediaUi::SetSearchExpanded(bool expanded) -> void
    {
        m_searchVisible = expanded;

        if (m_searchBox)
        {
            SetWidgetVisibility(m_searchBox, expanded ? SlateVisible : SlateCollapsed);
        }
        if (m_sortButton)
        {
            SetWidgetVisibility(m_sortButton, expanded ? SlateHidden : SlateVisible);
        }
        if (m_filterButton)
        {
            SetWidgetVisibility(m_filterButton, expanded ? SlateHidden : SlateVisible);
        }
        // The field covers the Sort/Filter span, so their frames and captions fold with them.
        for (const auto& overlay : m_sortFilterOverlays)
        {
            if (overlay.Widget)
            {
                SetWidgetVisibility(overlay.Widget, expanded ? SlateHidden : overlay.ShownVisibility);
            }
        }
        for (const auto& mode : m_searchModeWidgets)
        {
            if (mode.Widget)
            {
                SetWidgetVisibility(mode.Widget, expanded ? mode.ShownVisibility : SlateHidden);
            }
        }

        m_focusGrantsRemaining = expanded ? 12 : 0;
        m_unfocusedPolls = 0;

        if (expanded)
        {
            m_labelCenterPolls = 1;
        }

        // The input mode is taken when the field opens rather than when focus is confirmed. The
        // close action fires on key RELEASE, so waiting for the first keystroke to prove focus
        // means the very first Backspace has already escaped. Update() then keeps it in step
        // with where the keyboard actually is.
        SetTypingInputMode(expanded);

        // The typing session is over: the next expand is a new session and captures the cursor
        // state fresh (see m_cursorCaptured).
        if (!expanded)
        {
            m_cursorCaptured = false;
        }
    }

    // One call, because the controls live inside the window rather than beside it. Collapsing the
    // container takes its whole subtree with it.
    auto PalpediaUi::SetPanelExpanded(bool expanded) -> void
    {
        m_panelVisible = expanded;
        if (m_panel)
        {
            SetWidgetVisibility(m_panel, expanded ? SlateVisible : SlateCollapsed);
        }
        if (expanded)
        {
            m_labelCenterPolls = 2;
        }
    }

    // UI-only input stops the game's own action bindings from firing while the player is typing.
    // That is the whole fix for Backspace closing the Palpedia: the close is a game input action
    // that runs on key RELEASE, and Slate only ever sees the key DOWN, so a focused text box has no
    // opportunity to consume it. No property on the Palpedia guards this.
    //
    // The controller must not come from the search box alone. Our box is built with NewObject
    // against the WidgetTree, so GetOwningPlayer on it can return null (the widget was never given
    // an owning player at construction), which previously made this whole function silently no-op
    // and left Backspace closing the screen.
    auto PalpediaUi::TypingPlayerController() -> UObject*
    {
        if (m_searchBox)
        {
            if (auto* controller = CallNoArgs(m_searchBox, STR("GetOwningPlayer")))
            {
                return controller;
            }
        }
        if (m_canvas)
        {
            if (auto* controller = CallNoArgs(m_canvas, STR("GetOwningPlayer")))
            {
                return controller;
            }
        }
        if (auto* controller = UObjectGlobals::FindFirstOf(STR("PalPlayerController")))
        {
            return controller;
        }
        return UObjectGlobals::FindFirstOf(STR("PlayerController"));
    }

    auto PalpediaUi::SetTypingInputMode(bool typing) -> void
    {
        // NEVER call the legacy SetInputMode_* helpers here (SetInputMode_UIOnlyEx,
        // SetInputMode_GameAndUIEx). Palworld does not use that API: it drives input through
        // CommonUI input configs applied on menu transitions, so any viewport state a legacy
        // call sets is state the game will NEVER reset. Both were tried and both corrupted the
        // session (2026-08-30, build 24575825): UIOnlyEx sets the viewport's ignore-input flag,
        // which outlives the menu and left the player unable to move in the world after closing
        // the Palpedia; GameAndUIEx sets capture-during-mouse-down, which stole the mouse from
        // the Habitat map's pan for the rest of the menu. The CommonUI gate is the whole
        // mechanism: it alone stops the Backspace close action (see SetCommonUiInputGate).
        if (typing)
        {
            if (m_inputModeHeld)
            {
                return;
            }

            auto* controller = TypingPlayerController();
            if (!controller)
            {
                // Never silent: a missing controller is the one failure that reopens the
                // Backspace-closes-Palpedia bug, so it has to show in the log.
                Output::send<LogLevel::Warning>(STR("[PalpediaEnhancer] No player controller for typing input mode; Backspace may close the Palpedia\n"));
                return;
            }

            // The cursor state is captured rather than assumed. Restoring a hardcoded value would
            // fight whatever the menu had set for itself.
            //
            // Captured once per typing session, not per take: the game hides the cursor in
            // reaction to our own CommonUI gate toggle, so a hover-release followed by a retake
            // would recapture that game-perturbed false and the final restore would then hide
            // the menu cursor for good (observed live 2026-08-30, build 24575825: the polls
            // flipped false one frame after the first take and true again on the final release).
            //
            // bShowMouseCursor is a C++ bitfield bool sharing its byte with sibling flags
            // (confirmed by reflection: bEnableMouseOverEvents reports the same offset and size).
            // GetValuePtrByPropertyNameInChain<bool> returns a raw pointer at that byte with no
            // bitmask awareness, so a plain `*ptr = value` write stomps every other flag packed
            // into it - RE-UE4SS's own PropertyMacros.hpp explicitly calls out bools as unsafe for
            // that convenience API for exactly this reason. FBoolProperty's own
            // Get/SetPropertyValueInContainer apply the field mask correctly.
            if (!m_cursorCaptured)
            {
                if (auto* boolProperty = CastField<FBoolProperty>(controller->GetPropertyByNameInChain(STR("bShowMouseCursor"))))
                {
                    m_cursorWasShown = boolProperty->GetPropertyValueInContainer(controller);
                    m_cursorCaptured = true;
                }
            }

            // Swallow any press already in progress (the click that focused the box) so the
            // click-away poll in Update() only reacts to a fresh press.
            m_mouseWasDown = true;
            m_sessionCommitDone = false;

            SetCommonUiInputGate(false);

            m_typingController = controller;
            m_inputModeHeld = true;
            Verbose(STR("[PalpediaEnhancer] Typing session started (CommonUI gate disabled)\n"));
            return;
        }

        ReleaseInputMode();
    }

    // Ends a typing session: re-enables the CommonUI gate and restores the cursor flag. The held
    // flag is only cleared on a release that actually went through, because clearing it after a
    // failure is how a mod leaves the gate stuck off for the rest of the session.
    auto PalpediaUi::ReleaseInputMode() -> void
    {
        if (!m_inputModeHeld)
        {
            return;
        }

        if (!m_typingController)
        {
            // Never silent: this is the one failure that leaves the CommonUI gate held for the
            // rest of the session (see the comment on this function).
            Output::send<LogLevel::Warning>(
                    STR("[PalpediaEnhancer] ReleaseInputMode: controller missing; typing session stays held\n"));
            return;
        }

        // Deliberately NO legacy SetInputMode_* call here - see the comment at the top of
        // SetTypingInputMode for the two session-corrupting failures that rule caps.

        // Same bitfield-safe restore as the capture in SetTypingInputMode - see the comment there.
        // This is correct regardless (a raw-pointer write would still stomp sibling flags packed
        // into the same byte, bEnableMouseOverEvents among them), but logged evidence from a live
        // session (2026-08-30) showed those sibling flags false both before and after a search
        // session, unrelated to Palpedia Enhancer entirely - so this restore is not, on its own,
        // what was leaving mouse-move-driven gestures like the Habitat tab's map drag dead.
        if (auto* boolProperty = CastField<FBoolProperty>(m_typingController->GetPropertyByNameInChain(STR("bShowMouseCursor"))))
        {
            boolProperty->SetPropertyValueInContainer(m_typingController, m_cursorWasShown);
        }

        // Deliberately no SetFocusToGameViewport here: this release also runs when the player
        // merely hovers a search toggle mid-typing, and yanking keyboard focus off the box there
        // would fold the search the moment the pointer wanders. The click-away path in Update()
        // is the one that drops focus, because only there is it known the player clicked
        // somewhere else on purpose.
        SetCommonUiInputGate(true);

        m_inputModeHeld = false;
        m_typingController = nullptr;
        Verbose(STR("[PalpediaEnhancer] Typing session ended (CommonUI gate restored)\n"));
    }

    auto PalpediaUi::NotifyButtonClicked(UObject* button) -> void
    {
        // Address comparison only. This runs inside the game's click handler, so it does nothing
        // but record which of ours was pressed.
        // The hook fires for every CommonUI button in the game, so an unmatched address is normal
        // and must be ignored silently.
        // Note this hook is useless for detecting a click-away from the search box: while a
        // typing session holds the CommonUI gate, foreign buttons never receive their click, so
        // this never fires for the very clicks that should end typing. Update()'s raw mouse poll
        // handles those instead.
        if (button == m_searchInner || button == m_searchButton)
        {
            Verbose(STR("[PalpediaEnhancer] Click: search\n"));
            m_pendingSearchClick = true;
            return;
        }
        if (button == m_sortInner || button == m_sortButton)
        {
            Verbose(STR("[PalpediaEnhancer] Click: sort\n"));
            m_pendingSortClick = true;
            return;
        }
        if (button == m_filterInner || button == m_filterButton)
        {
            Verbose(STR("[PalpediaEnhancer] Click: filter\n"));
            m_pendingFilterClick = true;
            return;
        }
        if (button == m_closeInner || button == m_closeButton || button == m_filterCloseInner || button == m_filterCloseButton)
        {
            m_pendingCloseClick = true;
            return;
        }
        if (button == m_clearSearchInner || button == m_clearSearchButton)
        {
            Verbose(STR("[PalpediaEnhancer] Click: clear search\n"));
            m_pendingClearSearch = true;
            // Same focus hand-back as the target toggles: the click took the keyboard.
            if (m_searchVisible)
            {
                m_focusGrantsRemaining = 12;
            }
            return;
        }

        for (const auto& toggle : m_toggles)
        {
            if (button == toggle.Inner || button == toggle.Button)
            {
                m_pendingActions.push_back(toggle.Action);

                // Clicking a search-target toggle takes keyboard focus off the search field, and
                // the field folds the moment it loses focus. That made the toggles unusable: the
                // first click on any of them closed the search. Re-granting focus here means the
                // next pass hands the keyboard straight back to the field instead of folding it.
                if (toggle.Action.What == FilterAction::Kind::SearchTarget && m_searchVisible)
                {
                    m_focusGrantsRemaining = 12;
                }
                return;
            }
        }
    }

    auto PalpediaUi::ForgetWidgets() -> void
    {
        if (m_canvas)
        {
            Verbose(STR("[PalpediaEnhancer] ForgetWidgets: dropping attached screen\n"));
        }
        // The screen is going away while we may still be holding the player's input mode. Releasing
        // it first is the difference between closing the Palpedia mid-search and losing movement
        // for the rest of the session.
        ReleaseInputMode();
        UnrootWidgets();

        m_canvas = nullptr;
        m_list = nullptr;
        m_canvasRef.Reset();
        m_listRef.Reset();
        m_searchBox = nullptr;
        m_searchButton = nullptr;
        m_sortButton = nullptr;
        m_filterButton = nullptr;
        m_panel = nullptr;
        m_sortPanel = nullptr;
        m_directionButton = nullptr;
        m_closeButton = nullptr;
        m_filterCloseButton = nullptr;
        m_searchInner = nullptr;
        m_sortInner = nullptr;
        m_filterInner = nullptr;
        m_directionInner = nullptr;
        m_closeInner = nullptr;
        m_filterCloseInner = nullptr;
        m_clearSearchButton = nullptr;
        m_clearSearchInner = nullptr;
        m_markSeenButton = nullptr;
        m_markSeenInner = nullptr;
        m_pendingClearSearch = false;
        m_toggles.clear();
        m_centered.clear();
        m_pendingActions.clear();
        m_searchModeWidgets.clear();
        m_sortFilterOverlays.clear();
        m_suppressedWidgets.clear();
        m_cachedChildren.clear();
        m_searchVisible = false;
        m_panelVisible = false;
        m_sortPanelVisible = false;
        m_reorderCooldownFrames = 0;
        m_pendingSearchClick = false;
        m_pendingSortClick = false;
        m_pendingFilterClick = false;
        m_focusGrantsRemaining = 0;
        m_unfocusedPolls = 0;
        // m_lastQuery deliberately survives: the committed query is a session preference, like
        // the search targets, and EnsureWidgets writes it back into the rebuilt box.
    }

    auto PalpediaUi::ForgetQuery() -> void
    {
        m_lastQuery.clear();
        if (m_searchBox && m_canvasRef.Alive())
        {
            CallWithText(m_searchBox, STR("SetText"), STR(""));
        }
    }

    // Reflects the filter's state onto the buttons, so the panel always shows what is actually
    // applied. CommonUI draws a selected button in its own selected style, which is the same
    // affordance the game's own filter checkboxes use.
    //
    // Skips a toggle whose panel is not currently shown. This was added after a crash traced to
    // this function running immediately after EnsureWidgets built ~70 fresh buttons (the filter
    // and sort panels' own buttons plus the three search-target toggles) in the same tick, and
    // calling SetIsSelected on all of them before any had been through a normal engine tick - see
    // the design doc, 2026-08-21. The filter and sort panels both start collapsed and the
    // search-target row starts folded, so this same check also means nothing here is ever touched
    // on the tick it was created: a panel only becomes visible in response to a later click, by
    // which point its buttons are no longer new.
    // The Mark All Seen button sits in the gap between the list's right edge and the Stats tab,
    // at the tab row's own top and height, with the same clearance on both sides. Both neighbors
    // live in other canvases of the screen, so every rectangle is brought into one shared space
    // (see FrameOf) instead of copying a slot position whose anchors mean something else.
    auto PalpediaUi::BuildMarkSeenButton(UObject* canvas, UObject* widgetTree, UObject* list) -> void
    {
        // Turned off in Settings.ini: nothing is built, which is the same shape as the two
        // layout failures below, so every other control is unaffected. The MarkAllSeenKey
        // hotkey does not go through this button and keeps working.
        if (!ActiveSettings().ShowMarkAllSeenButton)
        {
            Verbose(STR("[PalpediaEnhancer] Mark All Seen: ShowMarkAllSeenButton is off; button not built\n"));
            return;
        }

        auto* screenClass = UObjectGlobals::StaticFindObject<UClass*>(
                nullptr, nullptr, STR("/Game/Pal/Blueprint/UI/UserInterface/MainMenu/Paldex/WBP_Paldex.WBP_Paldex_C"));
        auto* screen = screenClass ? canvas->GetTypedOuter(screenClass) : nullptr;
        auto** tabset = screen ? screen->GetValuePtrByPropertyNameInChain<UObject*>(STR("WBP_Paldex_tabset")) : nullptr;
        if (!list || !tabset || !*tabset)
        {
            Output::send<LogLevel::Warning>(STR("[PalpediaEnhancer] Mark All Seen: the list or the Stats/Habitat tab row was not found; button not built\n"));
            return;
        }

        Frame canvasFrame{};
        Frame listFrame{};
        Frame tabFrame{};
        UObject* canvasRoot = nullptr;
        UObject* listRoot = nullptr;
        UObject* tabRoot = nullptr;
        if (!FrameOf(canvas, canvasFrame, canvasRoot) || !FrameOf(list, listFrame, listRoot) || !FrameOf(*tabset, tabFrame, tabRoot) ||
            canvasRoot != listRoot || canvasRoot != tabRoot || !canvasFrame.X.SizeKnown || !listFrame.X.SizeKnown || !tabFrame.X.SizeKnown ||
            !tabFrame.Y.SizeKnown)
        {
            Output::send<LogLevel::Warning>(STR("[PalpediaEnhancer] Mark All Seen: the screen's layout is not the one this build knows; button not built\n"));
            return;
        }

        // Clearance to the list on the left and to the Stats tab on the right, in the screen's
        // own layout units.
        constexpr double clearance = 6.0;
        const double left = listFrame.X.Left + listFrame.X.Size + clearance;
        const double right = tabFrame.X.Left - clearance;
        const Vec2 size{right - left, tabFrame.Y.Size};
        if (size.X < 40.0 || size.Y < 8.0)
        {
            Output::send<LogLevel::Warning>(std::format(STR("[PalpediaEnhancer] Mark All Seen: no room between the list and the tabs ({}x{}); button not built\n"),
                                                        size.X,
                                                        size.Y));
            return;
        }

        // Attached to the screen's ROOT canvas, appended after every vanilla panel, not to the
        // list's canvas: the gap between the list and the tabs is covered by the Pal viewer
        // (WBP_PalInframeRender, 760x760 centered), a later sibling in paint order that took every
        // click meant for a button living in the list's canvas (observed live 2026-09-02, build
        // 24575825: no click ever reached it). The root's children paint last of all, and the
        // shared-space rectangle is already relative to the root's center, so the slot hangs off
        // anchor 0.5/0.5 with no conversion.
        SlotLayout layout{};
        layout.Valid = true;
        layout.SlotAnchors = Anchors{Vec2{0.5, 0.5}, Vec2{0.5, 0.5}};
        layout.Alignment = Vec2{0.0, 0.0};
        const Vec2 position{left, tabFrame.Y.Left};
        UObject* host = canvasRoot;

        m_markSeenButton = MakeVanillaButton(host, STR(""), position, size, layout);
        if (!m_markSeenButton)
        {
            Output::send<LogLevel::Warning>(STR("[PalpediaEnhancer] Mark All Seen: the button could not be created\n"));
            return;
        }
        auto* frame = MakeFrame(host, widgetTree, kButtonFrame, position, size, layout, 50, 3);
        SlotLayout centeredLayout = layout;
        centeredLayout.Alignment = Vec2{0.5, 0.5};
        auto* label = MakeCenteredLabelAt(host, widgetTree, Loc::T(STR("mark_all_seen")).c_str(),
                                          Vec2{position.X + size.X / 2.0, position.Y + size.Y / 2.0}, size, centeredLayout);
        m_centered.push_back(Centered{m_markSeenButton, label});

        auto** inner = m_markSeenButton->GetValuePtrByPropertyNameInChain<UObject*>(STR("WBP_PalInvisibleButton"));
        m_markSeenInner = (inner && *inner) ? *inner : nullptr;
        m_toggles.push_back(Toggle{m_markSeenButton, m_markSeenInner, FilterAction{FilterAction::Kind::MarkAllSeen, 0}, label, frame});
        SetRowTooltip(m_markSeenButton, m_markSeenInner, Loc::T(STR("tt_mark_all_seen")).c_str());
        Verbose(std::format(STR("[PalpediaEnhancer] Mark All Seen button at ({}, {}) size {}x{}\n"), position.X, position.Y, size.X, size.Y));
    }

    auto PalpediaUi::ApplyMarkSeenSetting(bool show) -> void
    {
        // No screen attached: EnsureWidgets reads the setting on the next open, so there is
        // nothing to correct and nothing safe to touch.
        if (!m_canvas || !m_canvasRef.Alive())
        {
            return;
        }

        if (m_markSeenButton)
        {
            for (const auto& toggle : m_toggles)
            {
                if (toggle.Action.What != FilterAction::Kind::MarkAllSeen)
                {
                    continue;
                }
                // The frame art and the caption were built HitTestInvisible so a click passes
                // through them into the button underneath; all three go together, or the button
                // vanishes and leaves its own frame and label painted on the screen.
                if (toggle.Button)
                {
                    SetWidgetVisibility(toggle.Button, show ? SlateVisible : SlateCollapsed);
                }
                if (toggle.Frame)
                {
                    SetWidgetVisibility(toggle.Frame, show ? 3 : SlateCollapsed);
                }
                if (toggle.Label)
                {
                    SetWidgetVisibility(toggle.Label, show ? 3 : SlateCollapsed);
                }
            }
            Verbose(std::format(STR("[PalpediaEnhancer] Mark All Seen button {} by a settings reload\n"), show ? STR("shown") : STR("hidden")));
            return;
        }

        if (!show)
        {
            return;
        }

        // Turned on while the Palpedia is open, on a screen that was built with it off. This is
        // the same call EnsureWidgets makes, from the children it already cached for this screen;
        // BuildMarkSeenButton reports its own reasons for declining and leaves everything else
        // alone, exactly as it does during construction.
        auto* widgetTree = FindWidgetTree(m_canvas);
        if (!widgetTree)
        {
            return;
        }
        UObject* listWidget = nullptr;
        for (auto* child : m_cachedChildren)
        {
            auto* childClass = child ? child->GetClassPrivate() : nullptr;
            if (childClass && childClass->GetPathName().find(STR("WBP_PaldexScrollList")) != StringType::npos)
            {
                listWidget = child;
                break;
            }
        }
        BuildMarkSeenButton(m_canvas, widgetTree, listWidget);
    }

    auto PalpediaUi::MarkAllSeen(const std::vector<UObject*>& rows) -> int
    {
        if (!m_canvas || !m_canvasRef.Alive())
        {
            return 0;
        }
        // The game clears one entry's unread flag in WBP_Paldex_ForDisplay's UpdateNewFlag when
        // that entry is clicked (Local_PalDexNewFlag on the local player's record data), and that
        // function is also what broadcasts OnAllNewFlagCleared for the menu tab's own dot. Calling
        // it per unread row is exactly what opening each entry would have done, with the game's
        // own guards, instead of writing into its map by hand.
        // The Paldex screen is not outered under its ForDisplay host (GetTypedOuter found nothing,
        // observed live 2026-09-02), so the host is found by class and matched by its WBP_Paldex
        // property pointing at our screen. FindAllOf walks every object, which is acceptable on a
        // click and nowhere else.
        auto* screenClass = UObjectGlobals::StaticFindObject<UClass*>(
                nullptr, nullptr, STR("/Game/Pal/Blueprint/UI/UserInterface/MainMenu/Paldex/WBP_Paldex.WBP_Paldex_C"));
        auto* screen = screenClass ? m_canvas->GetTypedOuter(screenClass) : nullptr;
        std::vector<UObject*> hosts{};
        UObjectGlobals::FindAllOf(STR("WBP_Paldex_ForDisplay_C"), hosts);
        UObject* display = nullptr;
        for (auto* host : hosts)
        {
            auto** inner = host ? host->GetValuePtrByPropertyNameInChain<UObject*>(STR("WBP_Paldex")) : nullptr;
            if (inner && *inner == screen)
            {
                display = host;
                break;
            }
        }
        auto* update = display ? display->GetFunctionByNameInChain(STR("UpdateNewFlag")) : nullptr;
        // UpdateNewFlag is a Blueprint function: its structure size covers its local variables
        // too (136 bytes live, 2026-09-02), while ProcessEvent copies only ParmsSize bytes from
        // the caller's block and allocates the locals itself. The parameter size is the contract.
        constexpr uint16_t nameSize = 8;
        if (!screen || !display || !update || update->GetParmsSize() != nameSize)
        {
            Output::send<LogLevel::Warning>(std::format(
                    STR("[PalpediaEnhancer] Mark All Seen: UpdateNewFlag(FName) not reachable (screen={} hosts={} matched={} function={} parms={}); nothing marked\n"),
                    screen != nullptr,
                    hosts.size(),
                    display != nullptr,
                    update != nullptr,
                    update ? static_cast<int>(update->GetParmsSize()) : -1));
            return 0;
        }

        int marked = 0;
        for (auto* row : rows)
        {
            if (!row)
            {
                continue;
            }
            auto** dot = row->GetValuePtrByPropertyNameInChain<UObject*>(STR("WBP_MainMenu_NewDot"));
            auto* visibility = (dot && *dot) ? (*dot)->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Visibility")) : nullptr;
            if (!visibility || *visibility == SlateCollapsed)
            {
                continue;
            }
            auto* characterId = row->GetValuePtrByPropertyNameInChain<FName>(STR("ChachedBaseCharacterID"));
            if (!characterId)
            {
                continue;
            }
            struct UpdateParams
            {
                FName CharacterID{};
            } updateParams{*characterId};
            display->ProcessEvent(update, &updateParams);

            // The same call the game's own row click makes on its row: the dot is collapsed now,
            // and the next Setup reads the cleared flag.
            if (auto* setMark = row->GetFunctionByNameInChain(STR("SetVisibilityNewMark")))
            {
                struct MarkParams
                {
                    uint8_t NewVisibility{};
                } markParams{SlateCollapsed};
                row->ProcessEvent(setMark, &markParams);
            }
            ++marked;
        }
        Output::send<LogLevel::Normal>(std::format(STR("[PalpediaEnhancer] Mark All Seen: {} of {} entries marked\n"), marked, rows.size()));
        return marked;
    }

    auto PalpediaUi::SyncToggleStates(const PalpediaFilter& filter) -> void
    {
        for (const auto& toggle : m_toggles)
        {
            if (!toggle.Inner)
            {
                continue;
            }

            switch (toggle.Action.What)
            {
            case FilterAction::Kind::Unread:
            case FilterAction::Kind::Discovery:
            case FilterAction::Kind::Element:
            case FilterAction::Kind::WorkSuitability:
            case FilterAction::Kind::Ownership:
                if (!m_panelVisible)
                {
                    continue;
                }
                break;
            case FilterAction::Kind::SortMode:
            case FilterAction::Kind::SortDescending:
                if (!m_sortPanelVisible)
                {
                    continue;
                }
                break;
            case FilterAction::Kind::SearchTarget:
                if (!m_searchVisible)
                {
                    continue;
                }
                break;
            default:
                break;
            }

            bool active = false;
            switch (toggle.Action.What)
            {
            case FilterAction::Kind::Unread:
                active = filter.UnreadOnly();
                break;
            case FilterAction::Kind::Discovery:
                active = filter.DiscoveryActive(static_cast<Discovery>(toggle.Action.Value));
                break;
            case FilterAction::Kind::Element:
                active = filter.ElementActive(toggle.Action.Value);
                break;
            case FilterAction::Kind::WorkSuitability:
                active = filter.WorkSuitabilityActive(toggle.Action.Value);
                break;
            case FilterAction::Kind::Ownership:
                active = filter.OwnershipActive(static_cast<Ownership>(toggle.Action.Value));
                break;
            case FilterAction::Kind::SortMode:
                active = filter.CurrentSortMode() == static_cast<SortMode>(toggle.Action.Value);
                break;
            case FilterAction::Kind::SortDescending:
                active = filter.SortDescending();
                break;
            case FilterAction::Kind::SearchTarget:
                active = filter.SearchTargetActive(static_cast<SearchTarget>(toggle.Action.Value));
                break;
            default:
                continue;
            }

            struct Params
            {
                bool InSelected{};
                bool bGiveClickFeedback{};
            } params{active, false};
            if (auto* function = toggle.Inner->GetFunctionByNameInChain(STR("SetIsSelected")))
            {
                toggle.Inner->ProcessEvent(function, &params);
            }
            if (toggle.Action.What == FilterAction::Kind::ClearAll)
            {
                SetLabelColor(toggle.Label, kLabelClear);
                SetImageTint(toggle.Frame, kFrameClear);
                continue;
            }
            // The frame art sits under the button, so whatever CommonUI paints for "selected"
            // still shows; the caption color is the state cue that does not depend on it.
            SetLabelColor(toggle.Label, active ? kLabelActive : kLabelIdle);
            SetImageTint(toggle.Frame, active ? kFrameActive : kFrameIdle);
        }
    }

    namespace
    {
        constexpr double kPanelMarginX = 0.035;
        constexpr double kPanelTitleY0 = 0.025;
        constexpr double kPanelTitleY1 = 0.095;
        constexpr double kPanelContentY0 = 0.11;
        constexpr double kPanelHeaderH = 0.045;
        constexpr double kPanelRowH = 0.072;
        constexpr double kPanelGapX = 0.012;
        constexpr double kPanelGapY = 0.012;
        constexpr double kPanelSectionGap = 0.018;
        constexpr double kPanelCloseY0 = 0.905;
        constexpr double kPanelCloseY1 = 0.97;
        const LinearColor kHeaderColor = Palette::ForgedGold;
        const LinearColor kRuleColor = Palette::Hairline;
    }

    // One engine for both windows so they read as one system. Every coordinate is a fraction of
    // the window; the window itself is a fraction of the screen. Resolution independent without
    // measuring a pixel, except captions, which CenterLabels measures and shrinks to fit.
    auto PalpediaUi::BuildPanel(UObject* canvas, UObject* widgetTree, const PanelSpec& spec) -> PanelResult
    {
        PanelResult result{};
        auto* window = CreateWidget(widgetTree, STR("/Script/UMG.CanvasPanel"));
        if (!window)
        {
            Output::send<LogLevel::Warning>(std::format(STR("[PalpediaEnhancer] Could not create the {} window\n"), spec.Title));
            return result;
        }
        SlotLayout windowLayout{};
        windowLayout.Valid = true;
        windowLayout.SlotAnchors = Anchors{Vec2{spec.WindowX0, spec.WindowY0}, Vec2{spec.WindowX1, spec.WindowY1}};
        windowLayout.Alignment = Vec2{0.0, 0.0};
        if (!AttachToCanvas(canvas, window, Vec2{0.0, 0.0}, Vec2{0.0, 0.0}, false, windowLayout, 200))
        {
            return result;
        }
        result.Window = window;

        // Collapsed before a single child joins: an empty container has no focus-registered
        // descendants, so however long construction takes, nothing is live on screen.
        SetWidgetVisibility(window, SlateCollapsed);

        // Background: gradient (Visible, so it absorbs clicks meant for the window), pattern, frame.
        MakeFrameIn(window, widgetTree, kPanelGradient, 0.0, 0.0, 1.0, 1.0, SlateVisible);
        MakeFrameIn(window, widgetTree, kPanelPattern, 0.004, 0.008, 0.996, 0.992);
        MakeFrameIn(window, widgetTree, kPanelPlate, 0.0, 0.0, 1.0, 1.0);

        const auto place = [&](UObject* child, double x0, double y0, double x1, double y1, int32_t z = 0) {
            SlotLayout layout{};
            layout.Valid = true;
            layout.SlotAnchors = Anchors{Vec2{x0, y0}, Vec2{x1, y1}};
            layout.Alignment = Vec2{0.0, 0.0};
            return AttachToCanvas(window, child, Vec2{0.0, 0.0}, Vec2{0.0, 0.0}, false, layout, z);
        };

        // Title, left, at 1.5x the game font.
        if (auto* title = CreateWidget(widgetTree, STR("/Script/UMG.TextBlock")))
        {
            if (place(title, kPanelMarginX, kPanelTitleY0, 0.6, kPanelTitleY1))
            {
                SetWidgetText(title, spec.Title);
                ApplyGameFont(title, 1.5);
                SetLabelColor(title, kLabelIdle);
                SetWidgetVisibility(title, 3);
            }
        }

        // A button in a cell: vanilla button for input, frame over it, optional accent plate,
        // centered caption; registered for state sync, centering and tooltips.
        const auto makeButton = [&](const PanelItem& item, double x0, double y0, double x1, double y1) -> UObject* {
            auto* button = MakeVanillaButton(window, STR(""), Vec2{0.0, 0.0}, Vec2{0.0, 0.0}, SlotLayout{});
            if (!button || !place(button, x0, y0, x1, y1))
            {
                return nullptr;
            }
            auto* frame = MakeFrameIn(window, widgetTree, kButtonFrame, x0, y0, x1, y1);
            if (item.Accent)
            {
                const double insetX = (x1 - x0) * 0.06;
                const double insetY = (y1 - y0) * 0.14;
                MakeFillIn(window, widgetTree, x0 + insetX, y0 + insetY, x1 - insetX, y1 - insetY, kFillClear);
            }
            auto** inner = button->GetValuePtrByPropertyNameInChain<UObject*>(STR("WBP_PalInvisibleButton"));
            auto* innerButton = (inner && *inner) ? *inner : nullptr;
            if (innerButton && item.Toggleable)
            {
                CallWithBool(innerButton, STR("SetIsSelectable"), true);
                CallWithBool(innerButton, STR("SetIsToggleable"), true);
            }
            const auto localized = LocalizedCaption(canvas, item.Action);
            auto* label = MakeCenteredLabel(window, widgetTree, localized.empty() ? item.Caption : localized.c_str(), x0, y0, x1, y1);
            m_centered.push_back(Centered{button, label});
            if (item.Action.What != FilterAction::Kind::None)
            {
                m_toggles.push_back(Toggle{button, innerButton, item.Action, label, frame});
            }
            SetRowTooltip(button, innerButton, item.Tooltip ? item.Tooltip : ButtonTooltip(item.Action));
            return button;
        };

        // Title-row actions, right-aligned, same height as a cell.
        {
            const double width = 0.16;
            double x1 = 1.0 - kPanelMarginX;
            const double y0 = kPanelTitleY0 + 0.005;
            const double y1 = kPanelTitleY1 - 0.005;
            for (auto it = spec.TitleActions.rbegin(); it != spec.TitleActions.rend(); ++it)
            {
                makeButton(*it, x1 - width, y0, x1, y1);
                x1 -= width + kPanelGapX;
            }
        }

        // Sections.
        double y = kPanelContentY0;
        const double usable = 1.0 - 2.0 * kPanelMarginX;
        for (const auto& section : spec.Sections)
        {
            if (auto* header = CreateWidget(widgetTree, STR("/Script/UMG.TextBlock")))
            {
                if (place(header, kPanelMarginX, y, 1.0 - kPanelMarginX, y + kPanelHeaderH))
                {
                    SetWidgetText(header, section.Header);
                    ApplyGameFont(header, 1.1);
                    SetLabelColor(header, kHeaderColor);
                    SetWidgetVisibility(header, 3);
                }
            }
            MakeFillIn(window, widgetTree, kPanelMarginX, y + kPanelHeaderH - 0.004, 1.0 - kPanelMarginX, y + kPanelHeaderH, kRuleColor);
            y += kPanelHeaderH + 0.008;

            const int columns = std::max(1, section.Columns);
            const double cellW = (usable - kPanelGapX * (columns - 1)) / columns;
            int column = 0;
            for (const auto& item : section.Items)
            {
                const double x0 = kPanelMarginX + column * (cellW + kPanelGapX);
                makeButton(item, x0, y, x0 + cellW, y + kPanelRowH);
                if (++column == columns)
                {
                    column = 0;
                    y += kPanelRowH + kPanelGapY;
                }
            }
            if (column != 0)
            {
                y += kPanelRowH + kPanelGapY;
            }
            y += kPanelSectionGap;
        }
        if (y > kPanelCloseY0)
        {
            Output::send<LogLevel::Warning>(std::format(STR("[PalpediaEnhancer] {} window content overflows by {:.3f}; shrink rows or widen the window\n"), spec.Title, y - kPanelCloseY0));
        }

        // Close, centered at the bottom: a window with no visible way out is what players panic at.
        PanelItem closeItem{Loc::T(STR("close")).c_str(), FilterAction{}, Loc::T(STR("tt_close")).c_str(), false, false};
        if (auto* close = makeButton(closeItem, 0.40, kPanelCloseY0, 0.60, kPanelCloseY1))
        {
            auto** inner = close->GetValuePtrByPropertyNameInChain<UObject*>(STR("WBP_PalInvisibleButton"));
            result.CloseButton = close;
            result.CloseInner = (inner && *inner) ? *inner : nullptr;
        }
        return result;
    }

    // The filter surface, in the order a player reads it: the state they are hunting for first
    // (ownership, then met status), then the two trait categories the vanilla panel offered.
    // Captions use the game's own words where the game has them (elements, work suitabilities).
    auto PalpediaUi::BuildFilterPanel(UObject* canvas, UObject* widgetTree) -> void
    {
        const auto element = [](const CharType* caption, int value) {
            return PanelItem{caption, {FilterAction::Kind::Element, value}};
        };
        const auto work = [](const CharType* caption, int value) {
            return PanelItem{caption, {FilterAction::Kind::WorkSuitability, value}};
        };
        PanelSpec spec{};
        spec.Title = Loc::T(STR("filters")).c_str();
        // Starts below the game's tab bar and compass, which own the top 12% of the screen.
        spec.WindowX0 = 0.12; spec.WindowY0 = 0.13; spec.WindowX1 = 0.88; spec.WindowY1 = 0.95;
        spec.TitleActions = {
                {Loc::T(STR("new")).c_str(), {FilterAction::Kind::Unread, 0}},
                {Loc::T(STR("clear_all")).c_str(), {FilterAction::Kind::ClearAll, 0}, nullptr, false, true},
        };
        spec.Sections = {
                {Loc::T(STR("sec_ownership")).c_str(), {
                        {Loc::T(STR("unowned")).c_str(), {FilterAction::Kind::Ownership, static_cast<int>(Ownership::Unowned)}},
                        {Loc::T(STR("owned")).c_str(), {FilterAction::Kind::Ownership, static_cast<int>(Ownership::Owned)}},
                        {Loc::T(STR("owned_5plus")).c_str(), {FilterAction::Kind::Ownership, static_cast<int>(Ownership::Complete)}},
                }, 5},
                {Loc::T(STR("sec_met")).c_str(), {
                        {Loc::T(STR("undiscovered")).c_str(), {FilterAction::Kind::Discovery, static_cast<int>(Discovery::NotEncountered)}},
                        {Loc::T(STR("seen")).c_str(), {FilterAction::Kind::Discovery, static_cast<int>(Discovery::Encountered)}},
                        {Loc::T(STR("caught")).c_str(), {FilterAction::Kind::Discovery, static_cast<int>(Discovery::Captured)}},
                }, 5},
                {Loc::T(STR("sec_type")).c_str(), {
                        element(STR("Neutral"), 1), element(STR("Fire"), 2), element(STR("Water"), 3), element(STR("Grass"), 4), element(STR("Electric"), 5),
                        element(STR("Ice"), 6), element(STR("Ground"), 7), element(STR("Dark"), 8), element(STR("Dragon"), 9),
                }, 5},
                {Loc::T(STR("sec_work")).c_str(), {
                        work(STR("Kindling"), 1), work(STR("Watering"), 2), work(STR("Planting"), 3), work(STR("Generating Electricity"), 4),
                        work(STR("Handiwork"), 5), work(STR("Gathering"), 6), work(STR("Lumbering"), 7), work(STR("Mining"), 8),
                        work(STR("Medicine"), 10), work(STR("Cooling"), 11), work(STR("Transport"), 12), work(STR("Farming"), 13),
                }, 7},
        };
        const auto built = BuildPanel(canvas, widgetTree, spec);
        m_panel = built.Window;
        m_panelVisible = false;
        m_filterCloseButton = built.CloseButton;
        m_filterCloseInner = built.CloseInner;
        Verbose(std::format(STR("[PalpediaEnhancer] Filter window built with {} controls\n"), m_toggles.size()));
    }

    // The Sort panel: a single-select list of sort modes (Default returns to natural Paldex order)
    // and a direction toggle. "By Number" was dropped: the game's own order IS Paldex-number order.
    auto PalpediaUi::BuildSortPanel(UObject* canvas, UObject* widgetTree) -> void
    {
        const auto mode = [](const CharType* key, const CharType* tip, SortMode value) {
            return PanelItem{Loc::T(key).c_str(), {FilterAction::Kind::SortMode, static_cast<int>(value)}, Loc::T(tip).c_str()};
        };
        PanelSpec spec{};
        spec.Title = Loc::T(STR("sort")).c_str();
        spec.WindowX0 = 0.27; spec.WindowY0 = 0.2; spec.WindowX1 = 0.73; spec.WindowY1 = 0.8;
        spec.Sections = {
                {Loc::T(STR("sec_sort_by")).c_str(), {
                        mode(STR("default"), STR("tt_default"), SortMode::None),
                        mode(STR("by_name"), STR("tt_by_name"), SortMode::Name),
                        mode(STR("by_element"), STR("tt_by_element"), SortMode::Element),
                        mode(STR("by_work_type"), STR("tt_by_work_type"), SortMode::WorkSuitability),
                        mode(STR("by_discovery"), STR("tt_by_discovery"), SortMode::Discovery),
                        mode(STR("by_ownership"), STR("tt_by_ownership"), SortMode::CapturedCount),
                        mode(STR("unread_first"), STR("tt_unread_first"), SortMode::UnreadFirst),
                }, 4},
                {Loc::T(STR("sec_order")).c_str(), {
                        {Loc::T(STR("descending")).c_str(), {FilterAction::Kind::SortDescending, 0}, Loc::T(STR("tt_descending")).c_str()},
                }, 4},
        };
        const auto built = BuildPanel(canvas, widgetTree, spec);
        m_sortPanel = built.Window;
        m_sortPanelVisible = false;
        m_closeButton = built.CloseButton;
        m_closeInner = built.CloseInner;
        for (const auto& toggle : m_toggles)
        {
            if (toggle.Action.What == FilterAction::Kind::SortDescending)
            {
                m_directionButton = toggle.Button;
                m_directionInner = toggle.Inner;
            }
        }
        Verbose(std::format(STR("[PalpediaEnhancer] Sort window built with {} controls\n"), m_toggles.size()));
    }

    auto PalpediaUi::ChangeSortPanel(bool expanded) -> void
    {
        m_sortPanelVisible = expanded;
        if (m_sortPanel)
        {
            SetWidgetVisibility(m_sortPanel, expanded ? SlateVisible : SlateCollapsed);
        }
        if (expanded)
        {
            m_labelCenterPolls = 2;
        }
    }

    // Reorders the rows by moving them to the end of their ScrollBox in the wanted order. The
    // panel's child order IS the display order (verified by the working prior art), and moving
    // each wanted row to the end in sequence realises exactly the wanted order at the end of the
    // box. The box must be found by class-scanning the widget-tree root, never by the generated
    // name ScrollBox_0. The first move is proven before the rest run; a failure latches sorting
    // off for the session (the SetDisplayCharacterIdArray lesson: a native call that fails once is
    // not retried and not proceeded on). A successful reorder is not latched forever: the player
    // can pick a different sort mode more than once in one Palpedia session, but not on the very
    // next frame. See m_reorderCooldownFrames for why. The caller isolates this call so nothing
    // else is harmed.
    auto PalpediaUi::ReorderActiveList(const std::vector<UObject*>& order) -> bool
    {
        if (m_reorderBroken || order.empty() || !m_canvas)
        {
            return true;
        }
        if (m_reorderCooldownFrames > 0)
        {
            return false;
        }
        for (auto* list : LiveScrollLists())
        {
            if (!list || CallNoArgs(list, STR("GetParent")) != m_canvas)
            {
                continue;
            }

            auto* panel = ResolveListScrollBox(list);
            if (!panel)
            {
                Output::send<LogLevel::Warning>(STR("[PalpediaEnhancer] Sort: could not find the row ScrollBox\n"));
                return true;
            }

            Verbose(std::format(STR("[PalpediaEnhancer] Sort: found ScrollBox {} ({} children)\n"),
                                                       panel->GetClassPrivate() ? panel->GetClassPrivate()->GetPathName() : StringType{STR("?")},
                                                       CollectChildren(panel).size()));

            const auto before = CollectChildren(panel);
            if (before.size() < 2 || before.size() != order.size())
            {
                Output::send<LogLevel::Warning>(std::format(STR("[PalpediaEnhancer] Sort: row mismatch (panel {} vs order {})\n"),
                                                            before.size(),
                                                            order.size()));
                return true;
            }

            // Already in the requested order (Default picked on an unsorted list, or the same
            // sort twice): nothing to move, and moving every row for no visible change is not free.
            if (before == order)
            {
                Verbose(STR("[PalpediaEnhancer] Sort: list already in the requested order\n"));
                return true;
            }

            if (!PanelMoveToEnd(panel, order[0]))
            {
                m_reorderBroken = true;
                Output::send<LogLevel::Error>(STR("[PalpediaEnhancer] Sort: first row move failed; sorting disabled for this session\n"));
                return true;
            }
            const auto afterFirst = CollectChildren(panel);
            if (afterFirst.size() != before.size())
            {
                m_reorderBroken = true;
                Output::send<LogLevel::Error>(STR("[PalpediaEnhancer] Sort: row count changed during the first move; sorting disabled\n"));
                return true;
            }

            for (size_t index = 1; index < order.size(); ++index)
            {
                if (order[index])
                {
                    PanelMoveToEnd(panel, order[index]);
                }
            }

            // Do not call ScrollToStart or walk invalidation ancestors here. The ScrollBox move
            // already changes the live child order, while those post-reorder calls ran after the
            // panel had changed and were the remaining crash surface when descending was applied.
            const auto after = CollectChildren(panel);
            if (after.size() != before.size())
            {
                m_reorderBroken = true;
                Output::send<LogLevel::Error>(STR("[PalpediaEnhancer] Sort: row count changed after reorder; sorting disabled\n"));
                return true;
            }
            // Conservative: gives Slate several frames to fully finish painting and invalidating
            // the widget tree from this reorder before another one is allowed to touch the same
            // rows. Not proven to be the exact cause of the historical crash, only a reasoned
            // mitigation; the change is worth stress-testing with rapid sort switching.
            m_reorderCooldownFrames = 30;
            Verbose(std::format(STR("[PalpediaEnhancer] Sort applied to {} rows (children {} -> {})\n"),
                                                       order.size(),
                                                       before.size(),
                                                       after.size()));
            return true;
        }

        Output::send<LogLevel::Warning>(STR("[PalpediaEnhancer] Sort: no live scroll list matched the canvas\n"));
        return true;
    }
    auto PalpediaUi::Update(const PalpediaFilter& filter) -> Poll
    {
        Poll poll{};

        // Reads the box's text and applies it as the query, the same way the game's own search
        // bars work (the crafting terminal filters on Enter, not per keystroke). Nothing is
        // searched while typing; this runs only at a commit point (Enter, click-away, fold,
        // collapsing the bar), one filter pass per completed search instead of one per
        // keystroke, and deduped by comparison so an unchanged query costs nothing.
        // An FText with no Data is uninitialized rather than empty, and reading it is unsafe;
        // an untouched box legitimately produces this.
        const auto commitQuery = [this, &poll] {
            if (!m_searchBox || !m_canvasRef.Alive())
            {
                return;
            }
            auto* text = m_searchBox->GetValuePtrByPropertyNameInChain<Text::RawText>(STR("Text"));
            if (!text)
            {
                return;
            }
            StringType query = Text::ToString(*text);
            // Unconditional on purpose: an identical query still re-runs the search. Skipping it
            // as a no-op (the first commit-based build) made re-committing the same text dead,
            // and re-committing is exactly how a player asks for a re-search when the results
            // are not what they expect (observed 2026-08-30: "unchanged query = nothing ever
            // happens"). A redundant pass at a user-triggered signal is the budget this design
            // spends; the saving is the per-keystroke passes, not these.
            Verbose(std::format(STR("[PalpediaEnhancer] Query committed: '{}' -> '{}'\n"), m_lastQuery, query));
            m_lastQuery = query;
            poll.QueryChanged = true;
            poll.Query = query;
        };

        // Ticks every frame regardless of the throttling below, so the cooldown reflects wall
        // time since the last reorder rather than however many polling passes happened to run.
        if (m_reorderCooldownFrames > 0)
        {
            --m_reorderCooldownFrames;
        }

        // Click-away detection, every frame while a typing session holds the input mode. The
        // CommonUI gate blocks every foreign button click while held, so no click can ever pull
        // keyboard focus off the search box, and without this the session never ends: observed
        // live 2026-08-30 (build 24575825), the mod held UIOnly plus the gate from mid-search
        // through a Habitat tab switch, leaving the map's mouse input dead until the Palpedia
        // closed. A press outside the search UI ends the session here on mouse DOWN; the CommonUI
        // click lands on mouse UP, after the gate is back open, so the same click that ends
        // typing also presses the row, tab, or button the player aimed at.
        // Win32 key state, not engine input: UIOnly mode stops the viewport from feeding
        // PlayerInput, and UE4SS's RegisterKeyBind wedges on mouse buttons (category rule).
        // Checking both physical buttons also makes swapped-button setups a non-issue.
        // Pointer over a search-row button while typing: open the gate NOW, every frame, so the
        // button gets its hover feedback and takes the first click as a genuine press. This must
        // not wait for the 8-frame pass: a player can move onto a button and click inside one
        // poll window, and the gate has to already be open when the mouse-down arrives.
        // Deliberately no commit here - a toggle click mid-typing must not submit the half-typed
        // search; the click's focus re-grant resumes the session with the new targets.
        if (m_inputModeHeld && m_canvasRef.Alive() && SearchToggleHovered())
        {
            SetTypingInputMode(false);
        }

        if (m_inputModeHeld)
        {
            const bool mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            const bool pressed = mouseDown && !m_mouseWasDown;
            m_mouseWasDown = mouseDown;
            if (pressed && m_canvasRef.Alive())
            {
                Vec2 mouse{};
                const bool overBox = PlatformMousePosition(mouse) && WidgetUnderPoint(m_searchBox, mouse);
                if (!overBox)
                {
                    Verbose(STR("[PalpediaEnhancer] Click away from search: ending the typing session\n"));

                    // Focus is handed to the game viewport so the box genuinely stops editing;
                    // otherwise nothing ever takes keyboard focus from it (the clicked widget's
                    // own focus change is exactly what the gate was suppressing) and the next
                    // pass would retake the input mode straight back.
                    auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
                    if (auto* focusToViewport = library ? library->GetFunctionByNameInChain(STR("SetFocusToGameViewport")) : nullptr)
                    {
                        struct FocusParams
                        {
                            bool ReturnValue{};
                        } focusParams{};
                        library->ProcessEvent(focusToViewport, &focusParams);
                    }
                    ReleaseInputMode();
                    commitQuery();
                }
            }
        }

        // Row setup fires once per row, so requests arrive in a burst. Wait for it to stop.
        if (m_attachRequested)
        {
            m_attachRequested = false;
            m_settleFrames = 10;
        }

        const bool settling = m_settleFrames > 0;
        if (settling && --m_settleFrames > 0)
        {
            return poll;
        }

        const bool rebuilt = settling && m_settleFrames == 0;

        if (!rebuilt && ++m_framesSincePass < 8)
        {
            return poll;
        }
        m_framesSincePass = 0;

        UObject* canvas = nullptr;
        UObject* foundList = nullptr;
        std::vector<UObject*> activeRows{};
        if (!rebuilt)
        {
            // Idle: nothing is attached and no row setup has fired, so there is nothing to look
            // for. This used to fall through to FindAllOf below - a walk of every UObject in the
            // process - every eight frames for the whole session, menus and loading screens
            // included. That walk is the cost that made world loads crawl.
            if (!m_canvas)
            {
                return poll;
            }
            // Attached: the screen is alive exactly when the engine's weak references still
            // resolve. Constant time, no object walk, and the same liveness test the engine
            // itself uses.
            if (!m_canvasRef.Alive() || !m_listRef.Alive())
            {
                Verbose(std::format(STR("[PalpediaEnhancer] Screen gone: canvas alive={} list alive={}; forgetting widgets\n"),
                                    m_canvasRef.Alive(), m_listRef.Alive()));
                ForgetWidgets();
                return poll;
            }
            canvas = m_canvas;
            activeRows = ListRows(m_list);
        }
        else
        {
            // A rebuild is the one time the list has to be found from scratch. FindAllOf is the
            // only trustworthy source then: UE4SS filters its results to live objects, whereas a
            // pointer read out of another object's property is not validated, and the inactive
            // Palpedia screen holds a stale scroll list pointer (that crashed GetParent once).
            for (auto* list : LiveScrollLists())
            {
                if (!list || !m_pendingRow || !ListOwnsRow(list, m_pendingRow))
                {
                    continue;
                }
                canvas = CallNoArgs(list, STR("GetParent"));
                activeRows = ListRows(list);
                foundList = list;
                break;
            }
        }

        if (rebuilt)
        {
            m_pendingRow = nullptr;
        }

        if (!canvas)
        {
            // The Palpedia is gone, or was rebuilt into a different screen. Every widget pointer we
            // hold is now meaningless and must not be touched again.
            ForgetWidgets();
            return poll;
        }

        EnsureWidgets(canvas);

        // Recorded AFTER EnsureWidgets: on a rebuild it calls ForgetWidgets, which clears the
        // list record along with everything else. Setting it before that call (v1.2.9-1.2.11)
        // left the record unset, so the very next poll declared the screen gone and dropped
        // every button pointer while the buttons stayed on screen, dead.
        if (foundList)
        {
            m_list = foundList;
            m_listRef.Set(foundList);
        }

        // A freshly built screen may still carry native filter state from the vanilla Filter
        // window or a prior session. Clearing it here, once per open, is what makes total
        // ownership hold: without it, the native list could stay narrowed under our own filtering.
        if (rebuilt)
        {
            ResetVanillaFiltering(canvas);
        }

        // CollectChildren and StripControls only ever run again here on the tick the screen
        // rebuilds. CollectChildren calls GetChildrenCount/GetChildAt via ProcessEvent on the
        // canvas - a real call into engine/Blueprint code, not a free C++ read - and
        // m_suppressedWidgets already made StripControls's own work one-shot per widget, so
        // calling it again on a later tick had nothing left to do. A diagnostic build that
        // skipped every per-tick call after initial construction survived an idle soak that
        // otherwise reliably crashed; this pair was the last thing still touching the canvas's
        // live children on every single tick after that. m_cachedChildren holds what
        // CollectChildren last saw, valid until the next rebuild - nothing changes this canvas's
        // children outside of that path. See the design doc, 2026-08-21.
        if (rebuilt)
        {
            m_cachedChildren = CollectChildren(canvas);

            // The filter and sort windows' own controls are not canvas children, they belong to
            // the windows, so the suppression pass never sees them. Only the windows themselves
            // have to be exempted. The search-target toggle buttons ARE direct canvas children
            // like the row's main three buttons, so they must be listed too: an unlisted toggle
            // button is the same class as the vanilla Filter button, so identity-matching fails,
            // the class-based fallback catches it, and MoveAside re-hides and repositions a
            // button EnsureWidgets is still actively managing. A crash's UE4SS.log cut off
            // mid-`StripControls` loop, immediately after suppressing one of these un-exempted
            // toggle buttons (see the design doc, 2026-08-21) - that is the evidence this
            // specific fix is based on.
            std::vector<UObject*> ours{m_searchButton, m_sortButton, m_filterButton, m_searchBox, m_panel, m_sortPanel};
            for (const auto& mode : m_searchModeWidgets)
            {
                ours.push_back(mode.Widget);
            }
            StripControls(m_cachedChildren, ours, rebuilt, m_suppressedWidgets, NativeSearchStrip(canvas));

            // Marks whether the loop above actually ran to completion, so a future crash log shows
            // whether the fault is inside StripControls or in whatever runs after it.
            Verbose(STR("[PalpediaEnhancer] StripControls done\n"));

            poll.ScreenRebuilt = true;
        }

        const auto& children = m_cachedChildren;

        // Only the active list's own rows. FindAllOf would also return rows belonging to the
        // previous screen, which is never destroyed, and the counts doubled on every reopen.
        poll.Rows = activeRows;

        const auto owns = [&children](UObject* widget) {
            return widget && std::find(children.begin(), children.end(), widget) != children.end();
        };

        // Only now, having seen them in the canvas's current child list, is it safe to call into
        // our own widgets.
        // Clicks arrive from the hook, not from polling, so they are already edge triggered.
        if (m_pendingFilterClick)
        {
            Verbose(STR("[PalpediaEnhancer] Handling filter click\n"));
            m_pendingFilterClick = false;
            // Opening either panel closes the search field, because the field is drawn over the
            // same strip the panels hang from and the two would overlap. Closing the field is a
            // commit point: whatever was typed becomes the query.
            if (m_searchVisible)
            {
                commitQuery();
                SetSearchExpanded(false);
            }
            if (m_sortPanelVisible)
            {
                ChangeSortPanel(false);
            }
            SetPanelExpanded(!m_panelVisible);
        }

        if (m_pendingCloseClick)
        {
            m_pendingCloseClick = false;
            SetPanelExpanded(false);
            ChangeSortPanel(false);
        }

        // The Sort button opens the sort panel, exactly as the Filter button opens the filter
        // panel; the two never show at once.
        if (m_pendingSortClick)
        {
            m_pendingSortClick = false;
            if (m_searchVisible)
            {
                commitQuery();
                SetSearchExpanded(false);
            }
            if (m_panelVisible)
            {
                SetPanelExpanded(false);
            }
            ChangeSortPanel(!m_sortPanelVisible);
        }

        if (m_pendingSearchClick && owns(m_searchBox))
        {
            m_pendingSearchClick = false;
            if (m_panelVisible)
            {
                SetPanelExpanded(false);
            }
            if (m_sortPanelVisible)
            {
                ChangeSortPanel(false);
            }
            // Collapsing the bar with its own button is a commit point, like the magnifier on
            // the game's crafting-terminal search.
            if (m_searchVisible)
            {
                commitQuery();
            }
            SetSearchExpanded(!m_searchVisible);
        }

        // The X: empty the field itself, report an empty query, and let the filter reset its
        // targets through the action so the toggles re-sync on the next pass.
        if (m_pendingClearSearch)
        {
            m_pendingClearSearch = false;
            if (owns(m_searchBox))
            {
                CallWithText(m_searchBox, STR("SetText"), STR(""));
            }
            m_lastQuery.clear();
            poll.QueryChanged = true;
            poll.Query.clear();
            m_pendingActions.push_back(FilterAction{FilterAction::Kind::ClearSearch, 0});
        }

        // Only actions whose button is still a live child of this canvas are reported.
        if (!m_pendingActions.empty())
        {
            for (const auto& action : m_pendingActions)
            {
                poll.FilterActions.push_back(action);
            }
            m_pendingActions.clear();
        }

        SyncToggleStates(filter);

        if (m_labelCenterPolls > 0 && --m_labelCenterPolls == 0)
        {
            CenterLabels();
        }

        // While expanded, the field owns the row. Three things are decided here every poll:
        //
        // 1. Keyboard: right after opening (or after a toggle click handed focus away) the field
        //    is granted focus for up to 12 polls, until it reports editing.
        // 2. Typing session: the common-UI gate is held ONLY while the box is actually being
        //    edited and the pointer is not over a search toggle. The gate exists to stop
        //    Backspace closing the Palpedia mid-typing; it also stops CommonUI buttons from
        //    taking clicks, which is why the Name/Drops/Desc toggles never registered while
        //    the field held it. Hovering a toggle releases it ahead of the click; the click's
        //    focus re-grant takes it back once the box is editing again.
        // 3. Folding: the field never folds while a toggle is hovered (the click lands on
        //    mouse-up, after focus has already left the box), and otherwise folds only after the
        //    keyboard has been gone for six polls, so a slow click cannot lose the race.
        if (m_searchVisible && owns(m_searchBox))
        {
            const bool editing = IsEditingText(m_searchBox);
            const bool hovered = SearchToggleHovered();

            if (m_focusGrantsRemaining > 0)
            {
                if (editing)
                {
                    m_focusGrantsRemaining = 0;
                }
                else if (!hovered)
                {
                    --m_focusGrantsRemaining;
                    CallVoid(m_searchBox, STR("SetKeyboardFocus"));
                }
            }

            if (editing && !hovered)
            {
                SetTypingInputMode(true);
            }
            // Enter both commits the box (dropping focus, which is what lands here) and, on its
            // RELEASE, fires the game's CommonUI confirm action - the same fires-on-release rule
            // the Backspace close follows. Ending the session between the key's down and its up
            // re-enables the gate just in time for that action to fire and re-run the vanilla
            // Palpedia filtering, wiping this mod's search results (observed 2026-08-30, build
            // 24575825: the query survived the commit untouched, yet every row was re-shown).
            // Holding the session until the key is physically up means the action lands while
            // the gate is still closed and gets blocked like any other CommonUI action. The
            // typed query is applied right here, at the Enter, not when the gate finally drops.
            else if (m_inputModeHeld && (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0)
            {
                if (!m_sessionCommitDone)
                {
                    m_sessionCommitDone = true;
                    commitQuery();
                }
            }
            else
            {
                // A TAPPED Enter is down and up between two polls and never reaches the branch
                // above (its key-up already landed while the gate was closed, so the confirm
                // action was blocked either way). Committing on any true session end catches it
                // at the very next poll instead of waiting the extra second for the fold
                // (observed 2026-08-30: tapped Enter looked broken, held Enter worked).
                if (m_inputModeHeld && !editing && !m_sessionCommitDone)
                {
                    m_sessionCommitDone = true;
                    commitQuery();
                }
                SetTypingInputMode(false);
            }

            if (editing || hovered)
            {
                m_unfocusedPolls = 0;
            }
            else if (m_focusGrantsRemaining == 0 && ++m_unfocusedPolls >= 6)
            {
                m_unfocusedPolls = 0;
                Verbose(STR("[PalpediaEnhancer] Search folded after focus loss\n"));
                commitQuery();
                SetSearchExpanded(false);
            }
        }

        // The box's text is deliberately not read here every poll. It used to be (first via the
        // GetText() UFunction, whose per-tick FText copies were implicated in the 2026-08-21
        // idle-crash investigation, then via a direct property read), emitting a query change on
        // every keystroke. Since the commit-based search (2026-08-30), the text is read only
        // inside commitQuery at a commit point, so an idle or typing session costs nothing.
        return poll;
    }

    auto PalpediaUi::EnsureWidgets(UObject* canvas) -> void
    {
        // The already-built case is the common one -- this runs every ~8 frames for as long as the
        // Palpedia stays open. Checking it against m_cachedChildren first means that common case
        // never calls CollectChildren (GetChildrenCount/GetChildAt via ProcessEvent, a real engine
        // call), matching the same one-shot-per-rebuild reasoning Update() already applies to its
        // own CollectChildren/StripControls pair (see the design doc, 2026-08-21). The real
        // CollectChildren below only has to run on the construction/rebuild path.
        //
        // The search field is deliberately not part of this test. If creating it ever fails,
        // rebuilding the whole row every pass makes things worse, and that runaway rebuild is what
        // marched the buttons across the screen.
        const auto cachedOwns = [this](UObject* widget) {
            return widget && std::find(m_cachedChildren.begin(), m_cachedChildren.end(), widget) != m_cachedChildren.end();
        };
        if (m_canvas == canvas && cachedOwns(m_searchButton) && cachedOwns(m_sortButton) && cachedOwns(m_filterButton))
        {
            return;
        }

        const auto children = CollectChildren(canvas);
        const auto owns = [&children](UObject* widget) {
            return widget && std::find(children.begin(), children.end(), widget) != children.end();
        };

        ForgetWidgets();
        m_canvas = canvas;
        m_canvasRef.Set(canvas);

        auto* widgetTree = FindWidgetTree(canvas);
        if (!widgetTree)
        {
            Output::send<LogLevel::Warning>(STR("[PalpediaEnhancer] Canvas has no WidgetTree outer; not attaching\n"));
            return;
        }

        // Every label built below copies the game's own UI font; resolve it once per screen.
        g_gameFont = FindGameFont();
        Loc::EnsureLoaded();

        // Lay out against real geometry, never guessed coordinates. The list gives the left edge and
        // width; the vanilla Filter button gives the row's Y and its native height.
        //
        // Measured once per canvas. After we create our own buttons the canvas contains several
        // widgets of the vanilla button's class, and measuring again would pick one of ours.
        Vec2 listPosition{80.0, 4.0};
        Vec2 listSize{500.0, 700.0};
        Vec2 rowPosition{80.0, 760.0};
        Vec2 rowSize{240.0, 40.0};
        constexpr double RowGap = 8.0; // Breathing room between the row and whatever it sits under or above.
        SlotLayout rowLayout{};
        SlotLayout listLayout{};

        // v1.0.4's search strip is what our row replaces, so it is the row's anchor when present.
        UObject* nativeStrip = NativeSearchStrip(canvas);

        for (auto* child : (m_geometryCanvas == canvas && m_geometryValid) ? std::vector<UObject*>{} : children)
        {
            auto* childClass = child->GetClassPrivate();
            if (!childClass)
            {
                continue;
            }
            const auto path = childClass->GetPathName();
            auto** slot = child->GetValuePtrByPropertyNameInChain<UObject*>(STR("Slot"));
            if (!slot || !*slot)
            {
                continue;
            }

            if (nativeStrip && child == nativeStrip)
            {
                // Read before StripControls moves it off screen: this is where our row goes.
                rowPosition = CallReturningVec2(*slot, STR("GetPosition"));
                rowSize = CallReturningVec2(*slot, STR("GetSize"));
                rowLayout = ReadSlotLayout(*slot);
            }
            else if (path.find(STR("WBP_PaldexScrollList")) != StringType::npos)
            {
                listPosition = CallReturningVec2(*slot, STR("GetPosition"));
                listSize = CallReturningVec2(*slot, STR("GetSize"));
                listLayout = ReadSlotLayout(*slot);
            }
            // The trailing dot is load-bearing: v1.0.4 added WBP_CommonButton_Activation_Icon, the
            // magnifier, whose path contains this one without it.
            else if (path.find(STR("WBP_CommonButton_Activation.")) != StringType::npos)
            {
                // Read before it is moved aside. Its anchors are what make its position mean what
                // it means; copying the number alone put our row at the top of the screen.
                rowPosition = CallReturningVec2(*slot, STR("GetPosition"));
                rowSize = CallReturningVec2(*slot, STR("GetSize"));
                rowLayout = ReadSlotLayout(*slot);
            }
        }

        const double rowHeight = rowSize.Y > 8.0 ? rowSize.Y : 40.0;

        // The list is anchored top-center and the button bottom-center, so their anchors differ on
        // Y but agree on X: both use anchor X 0.5 and alignment X 0.5. Only the X axis matters for
        // width, so compare that rather than rejecting the whole thing.
        const bool sameSpace = rowLayout.Valid && listLayout.Valid && rowLayout.SlotAnchors.Minimum.X == listLayout.SlotAnchors.Minimum.X &&
                               rowLayout.Alignment.X == listLayout.Alignment.X;
        if (!sameSpace)
        {
            Output::send<LogLevel::Warning>(STR("[PalpediaEnhancer] List and button differ on the X axis; row spans the button instead\n"));
        }

        if (!(m_geometryCanvas == canvas && m_geometryValid))
        {
            // v1.0.4 moved the vanilla Filter button out of this canvas, so its slot can no longer
            // give the row its place. The list is still here and still measured, so the row comes
            // off the list instead: same column, same width, immediately below it. Without this the
            // measurement silently fell back to the constants above, which is how the row ended up
            // drawn over the game's new search strip for a whole game version.
            // The row takes the strip's place: the list gives the column and the anchors, the strip
            // gives the Y. The strip's own anchors are not reused, since it hangs off the canvas
            // bottom while the row is placed in the list's top-anchored space.
            double stripTop = 0.0;
            const bool useStrip = nativeStrip != nullptr && listLayout.Valid && StripTopInCanvas(canvas, nativeStrip, stripTop);
            const bool useList = useStrip || (!rowLayout.Valid && listLayout.Valid);
            const SlotLayout& anchorSource = useList ? listLayout : rowLayout;
            m_rowX = (sameSpace || useList) ? listPosition.X : rowPosition.X;
            m_rowY = useStrip ? stripTop : (useList ? listPosition.Y + listSize.Y + RowGap : rowPosition.Y);
            m_rowWidth = (sameSpace || useList) ? listSize.X : std::max(420.0, rowSize.X * 2.0);
            m_rowHeight = rowHeight;
            if (useStrip)
            {
                Verbose(std::format(STR("[PalpediaEnhancer] Consuming the game's search strip; row at y={} h={}\n"), m_rowY, m_rowHeight));
            }

            m_anchorsValid = anchorSource.Valid;
            m_anchorMinX = anchorSource.SlotAnchors.Minimum.X;
            m_anchorMinY = anchorSource.SlotAnchors.Minimum.Y;
            m_anchorMaxX = anchorSource.SlotAnchors.Maximum.X;
            m_anchorMaxY = anchorSource.SlotAnchors.Maximum.Y;
            m_alignX = anchorSource.Alignment.X;
            m_alignY = anchorSource.Alignment.Y;
            m_geometryCanvas = canvas;
            m_geometryValid = true;
            Verbose(
                    std::format(STR("[PalpediaEnhancer] Row geometry: x={} y={} w={} h={}\n"), m_rowX, m_rowY, m_rowWidth, m_rowHeight));
        }

        SlotLayout cachedLayout{};
        cachedLayout.Valid = m_anchorsValid;
        cachedLayout.SlotAnchors = Anchors{Vec2{m_anchorMinX, m_anchorMinY}, Vec2{m_anchorMaxX, m_anchorMaxY}};
        cachedLayout.Alignment = Vec2{m_alignX, m_alignY};

        // The scroll list's frame is inset from the widget's own bounds, so spanning the full list
        // width put the row outside the corner brackets the player reads as the panel's edge. The
        // same inset shrinks the artwork's height with it, which is what stopped the row from
        // reaching down over the PALPEDIA caption.
        constexpr double frameInset = 0.86;

        // Raised off the vanilla button's top edge by a fraction of its own height. The artwork is
        // taller than the button it replaces and there is empty space above it, so the band it
        // occupies is centered on the gap rather than hanging below.
        constexpr double liftFraction = 0.12;

        const double totalWidth = m_rowWidth * frameInset;

        // Proportions come from the artwork itself, measured in the mockup: a 63 wide search tile,
        // two 253 wide tiles, roughly 4.5 between them, 90 tall, spanning 575 across. Scaling that
        // to the row's width keeps the buttons looking exactly like the sheet at any resolution.
        constexpr double sheetSearch = 63.0;
        constexpr double sheetWide = 253.0;
        constexpr double sheetGap = 4.5;
        constexpr double sheetHeight = 90.0;
        constexpr double sheetSpan = 575.0;

        const double sheetScale = totalWidth / sheetSpan;
        const double searchWidth = sheetSearch * sheetScale;
        const double wideWidth = sheetWide * sheetScale;
        const double artHeight = sheetHeight * sheetScale;

        const double rowY = m_rowY - (artHeight * liftFraction);

        // Alignment X is 0.5 on this canvas, so a slot's X is its CENTER, not its left edge. The
        // row is therefore built by walking a left edge and converting each element to a center.
        // Treating X as a left edge is what stacked Sort on top of the search button and pushed
        // the row off to the right.
        const double artGap = sheetGap * sheetScale;
        double cursor = -(totalWidth / 2.0);
        const auto centerOf = [&cursor, artGap](double width) {
            const double center = cursor + (width / 2.0);
            cursor += width + artGap;
            return center;
        };

        const double searchCenter = centerOf(searchWidth);
        const double sortCenter = centerOf(wideWidth);
        const double filterCenter = centerOf(wideWidth);

        // Same construction as every other control in the mod: a vanilla button for input, the
        // nine-slice frame drawn over it, then a live caption (or, for Search, an icon) that
        // CenterLabels lines up against the button's painted geometry.
        m_searchButton = MakeVanillaButton(canvas, STR(""), Vec2{searchCenter, rowY}, Vec2{searchWidth, artHeight}, cachedLayout);
        m_sortButton = MakeVanillaButton(canvas, STR(""), Vec2{sortCenter, rowY}, Vec2{wideWidth, artHeight}, cachedLayout);
        m_filterButton = MakeVanillaButton(canvas, STR(""), Vec2{filterCenter, rowY}, Vec2{wideWidth, artHeight}, cachedLayout);

        MakeFrame(canvas, widgetTree, kButtonFrame, Vec2{searchCenter, rowY}, Vec2{searchWidth, artHeight}, cachedLayout, 50, 3);
        if (auto* frame = MakeFrame(canvas, widgetTree, kButtonFrame, Vec2{sortCenter, rowY}, Vec2{wideWidth, artHeight}, cachedLayout, 50, 3))
        {
            m_sortFilterOverlays.push_back(SearchModeWidget{frame, 3});
        }
        if (auto* frame = MakeFrame(canvas, widgetTree, kButtonFrame, Vec2{filterCenter, rowY}, Vec2{wideWidth, artHeight}, cachedLayout, 50, 3))
        {
            m_sortFilterOverlays.push_back(SearchModeWidget{frame, 3});
        }

        {
            // The row's buttons are placed with the game's own slot alignment (copied from the
            // vanilla row), which pins their anchor to an edge rather than their middle. An
            // auto-sized label or icon placed at that same point therefore hugs that edge. So
            // the overlays get a center-aligned copy of the layout, and their position is moved
            // to where the button's middle actually is: position + size * (0.5 - alignment).
            SlotLayout centeredLayout = cachedLayout;
            centeredLayout.Alignment = Vec2{0.5, 0.5};
            const auto middleOf = [&](double center, double width) {
                return Vec2{center + width * (0.5 - cachedLayout.Alignment.X), rowY + artHeight * (0.5 - cachedLayout.Alignment.Y)};
            };
            const double iconSide = artHeight * 0.42;
            auto* icon = MakeArtwork(canvas, widgetTree, L"icon_search.png", middleOf(searchCenter, searchWidth), Vec2{iconSide, iconSide}, centeredLayout);
            m_centered.push_back(Centered{m_searchButton, icon});
            auto* sortLabel = MakeCenteredLabelAt(canvas, widgetTree, Loc::T(STR("sort")).c_str(), middleOf(sortCenter, wideWidth), Vec2{wideWidth, artHeight}, centeredLayout);
            auto* filterLabel = MakeCenteredLabelAt(canvas, widgetTree, Loc::T(STR("filter")).c_str(), middleOf(filterCenter, wideWidth), Vec2{wideWidth, artHeight}, centeredLayout);
            m_centered.push_back(Centered{m_sortButton, sortLabel});
            m_centered.push_back(Centered{m_filterButton, filterLabel});
            for (auto* label : {sortLabel, filterLabel})
            {
                if (label)
                {
                    m_sortFilterOverlays.push_back(SearchModeWidget{label, 3});
                }
            }
        }

        // The inner CommonButtonBase is what actually reports a click. Cache each one now so the
        // click hook only has to compare addresses.
        const auto innerOf = [](UObject* button) -> UObject* {
            if (!button)
            {
                return nullptr;
            }
            auto** inner = button->GetValuePtrByPropertyNameInChain<UObject*>(STR("WBP_PalInvisibleButton"));
            return (inner && *inner) ? *inner : nullptr;
        };
        m_searchInner = innerOf(m_searchButton);
        m_sortInner = innerOf(m_sortButton);
        m_filterInner = innerOf(m_filterButton);

        SetRowTooltip(m_searchButton, m_searchInner, Loc::T(STR("tt_search_button")).c_str());
        SetRowTooltip(m_sortButton, m_sortInner, Loc::T(STR("tt_sort_button")).c_str());
        SetRowTooltip(m_filterButton, m_filterInner, Loc::T(STR("tt_filter_button")).c_str());

        {
            UObject* listWidget = nullptr;
            for (auto* child : children)
            {
                auto* childClass = child->GetClassPrivate();
                if (childClass && childClass->GetPathName().find(STR("WBP_PaldexScrollList")) != StringType::npos)
                {
                    listWidget = child;
                    break;
                }
            }
            BuildMarkSeenButton(canvas, widgetTree, listWidget);
        }

        // The search field folds out of the search button and covers the Sort and Filter tiles
        // exactly, edge to edge, so the whole interface stays inside the three button row. Derived
        // from those two tiles rather than recomputed, or it lands a few pixels short of each.
        const double fieldLeft = sortCenter - (wideWidth / 2.0);
        const double fieldRight = filterCenter + (wideWidth / 2.0);
        const double fullFieldWidth = fieldRight - fieldLeft;

        // What the search box matches. A plain query with no prefix searches whichever of these is
        // selected. Ranch production is not offered: no reachable engine function returns it yet
        // (drops and description both do, through the same runtime database read every other
        // filter uses), and a mode that always matches nothing would be worse than not having it.
        //
        // Placed inside the same row as the field itself, at the same Y and height, rather than on
        // a row of their own below it: this geometry's vertical anchor math (bottom-anchored, point
        // anchor, alignment 0) was only ever proven correct for Y values already used by a working
        // widget. A second row would need a new, unverified Y, and there is no way to confirm one
        // without a live game to test against.
        const struct
        {
            const CharType* Caption;
            const CharType* Tooltip;
            SearchTarget Target;
        } modes[] = {
                {Loc::T(STR("name")).c_str(), Loc::T(STR("tt_name")).c_str(), SearchTarget::Name},
                {Loc::T(STR("drops")).c_str(), Loc::T(STR("tt_drops")).c_str(), SearchTarget::Drops},
                {Loc::T(STR("ranch")).c_str(), Loc::T(STR("tt_ranch")).c_str(), SearchTarget::Ranch},
                {Loc::T(STR("desc")).c_str(), Loc::T(STR("tt_desc")).c_str(), SearchTarget::Description},
        };

        // The field takes the whole Sort+Filter span. The three target toggles sit in a row to the
        // RIGHT of the Filter tile's right edge, each the same size as a Sort/Filter tile and
        // separated by the same gap those tiles have between them, so they read as a continuation
        // of the button row rather than as a cramped strip squeezed beside the field. The earlier
        // layout gave all three 46% of the field's width between them, which left each one too
        // narrow for even a four-letter caption and spilled the captions into one another.
        const double modeWidth = wideWidth;
        const double modeGap = artGap;

        // X -> field -> Name/Drops/Ranch/Desc. The X is a magnifier-sized square at the left
        // edge of the old field span; the field gives up that width and starts after it.
        const double clearWidth = searchWidth;
        const double clearCenter = fieldLeft + (clearWidth / 2.0);
        const double searchFieldWidth = fullFieldWidth - clearWidth - artGap;
        const double searchFieldX = fieldLeft + clearWidth + artGap + (searchFieldWidth / 2.0);
        double modeCursor = fieldRight + modeGap;

        // Built the same way as every other control (button, frame over it, centered caption),
        // hidden until the field opens, and shown/hidden with it through m_searchModeWidgets.
        if (auto* clear = MakeVanillaButton(canvas, STR(""), Vec2{clearCenter, rowY}, Vec2{clearWidth, artHeight}, cachedLayout, SlateHidden))
        {
            auto** inner = clear->GetValuePtrByPropertyNameInChain<UObject*>(STR("WBP_PalInvisibleButton"));
            m_clearSearchButton = clear;
            m_clearSearchInner = (inner && *inner) ? *inner : nullptr;
            m_searchModeWidgets.push_back(SearchModeWidget{clear, SlateVisible});
            if (auto* frame = MakeFrame(canvas, widgetTree, kButtonFrame, Vec2{clearCenter, rowY}, Vec2{clearWidth, artHeight}, cachedLayout, 50, SlateHidden))
            {
                m_searchModeWidgets.push_back(SearchModeWidget{frame, 3});
            }
            SlotLayout centeredLayout = cachedLayout;
            centeredLayout.Alignment = Vec2{0.5, 0.5};
            const Vec2 middle{clearCenter + clearWidth * (0.5 - cachedLayout.Alignment.X), rowY + artHeight * (0.5 - cachedLayout.Alignment.Y)};
            if (auto* label = MakeCenteredLabelAt(canvas, widgetTree, Loc::T(STR("clear_search")).c_str(), middle, Vec2{clearWidth, artHeight}, centeredLayout, SlateHidden))
            {
                m_searchModeWidgets.push_back(SearchModeWidget{label, 3});
                m_centered.push_back(Centered{clear, label});
            }
            SetRowTooltip(clear, m_clearSearchInner, Loc::T(STR("tt_clear_search")).c_str());
        }

        if (auto* search = CreateWidgetPreferred(widgetTree, STR("/Script/Pal.PalEditableTextBox"), STR("/Script/UMG.EditableTextBox")))
        {
            // Styled before it is attached. The Slate widget is built on the first paint after
            // that, so a style set now is the one it is built with.
            StyleSearchField(search);

            // Collapsed before it ever joins the canvas, not after: every earlier fix in this
            // file's crash history addressed a WBP_CommonButton_Activation instance, but this
            // field is an EditableTextBox - a widget whose entire purpose is holding keyboard
            // focus - and it was attaching to the live, visible outer canvas and only being
            // collapsed a few calls later, the exact same "built live, hidden after" ordering
            // already fixed for the Filter/Sort popup windows (v1.1.2) and the search-target
            // toggle buttons (v1.1.7), just never applied here. Found by process of elimination
            // after a diagnostic build proved the ~67 filter/sort panel buttons are not the
            // cause: this field is one of the few remaining widgets present in that reduced
            // build. See the design doc, 2026-08-21.
            SetWidgetVisibility(search, SlateCollapsed);

            if (AttachToCanvas(canvas, search, Vec2{searchFieldX, rowY}, Vec2{searchFieldWidth, artHeight}, false, cachedLayout))
            {
                m_searchBox = search;
                m_searchVisible = false;

                // Same configuration PalboxSearchPlus applies to its hosted field. The donor
                // defaults drop keyboard focus on commit, which silently kills typing.
                SetBoolProperty(search, STR("SelectAllTextWhenFocused"), false);
                SetBoolProperty(search, STR("ClearKeyboardFocusOnCommit"), false);
                SetBoolProperty(search, STR("IsCaretMovedWhenGainFocus"), true);

                CallWithText(search, STR("SetHintText"), Loc::T(STR("search_hint")).c_str());

                // The committed query survives the screen rebuild (it is a session preference,
                // like the search targets), so the rebuilt box shows it again. dllmain forgets
                // it via ForgetQuery when RememberState is off.
                if (!m_lastQuery.empty())
                {
                    CallWithText(search, STR("SetText"), m_lastQuery.c_str());
                }
            }
        }

        // The toggles sit past the right edge of the list column, over the detail panel. On the
        // list's own canvas they drew there but never received a click: the detail panel is
        // painted after the list and took every hit, which also pulled focus off the field and
        // folded it. So they go on the full-viewport overlay canvas instead, the one the popup
        // windows already click through reliably, at a position translated from where they would
        // have sat on the list canvas. That translation needs both canvases' last-painted
        // geometry; if it is unavailable the toggles fall back to the list canvas and a warning
        // says so, rather than landing at a guessed spot.
        auto* popupCanvas = ResolvePopupCanvas(canvas);
        Vec2 innerCanvasSize{};
        std::vector<uint8_t> innerGeometry{};
        const bool overlayToggles = popupCanvas && popupCanvas != canvas && CachedGeometry(canvas, innerGeometry) &&
                                    LocalSizeOf(innerGeometry, innerCanvasSize) && m_anchorsValid;
        if (!overlayToggles)
        {
            Output::send<LogLevel::Warning>(
                    STR("[PalpediaEnhancer] Search toggles: could not translate to the overlay canvas; placing them on the list canvas\n"));
        }
        SlotLayout overlayLayout{};
        overlayLayout.Valid = true;
        overlayLayout.SlotAnchors = Anchors{Vec2{0.0, 0.0}, Vec2{0.0, 0.0}};
        overlayLayout.Alignment = Vec2{0.0, 0.0};

        for (const auto& mode : modes)
        {
            const double center = modeCursor + (modeWidth / 2.0);
            modeCursor += modeWidth + modeGap;
            Vec2 modeCenter{center, rowY};
            const Vec2 modeSize{modeWidth, artHeight};
            UObject* toggleCanvas = canvas;
            SlotLayout toggleLayout = cachedLayout;

            if (overlayToggles)
            {
                // Top-left corner in the list canvas's local space: the slot anchor is a point at
                // (anchorX, anchorY) of the canvas, X is the center (alignment 0.5) and Y the top.
                const Vec2 innerTopLeft{innerCanvasSize.X * m_anchorMinX + center - (modeWidth / 2.0), innerCanvasSize.Y * m_anchorMinY + rowY};
                Vec2 overlayTopLeft{};
                if (ConvertLocalPoint(canvas, popupCanvas, innerTopLeft, overlayTopLeft))
                {
                    modeCenter = overlayTopLeft;
                    toggleCanvas = popupCanvas;
                    toggleLayout = overlayLayout;
                }
            }

            // Blank caption, same as every other button on this row: WBP_CommonButton_Activation
            // does not stretch its own text to a custom cell size, so the label is drawn separately.
            //
            // Hidden, not Collapsed: see the note on SetSearchExpanded. This button is a
            // WBP_CommonButton_Activation instance same as the vanilla one that crash was found on.
            // Passed as the initial visibility, not set afterward: this is the one caller in the
            // whole file that attaches a button directly to the live, visible outer canvas and
            // means to hide it immediately, so it is the one caller that must never let the button
            // join that canvas already-visible even for an instant. See the design doc, 2026-08-21.
            auto* button = MakeVanillaButton(toggleCanvas, STR(""), modeCenter, modeSize, toggleLayout, SlateHidden);
            if (!button)
            {
                continue;
            }
            auto* frame = MakeFrame(toggleCanvas, widgetTree, kButtonFrame, modeCenter, modeSize, toggleLayout, 50, SlateHidden);
            if (frame)
            {
                m_searchModeWidgets.push_back(SearchModeWidget{frame, 3}); // HitTestInvisible
            }

            auto** inner = button->GetValuePtrByPropertyNameInChain<UObject*>(STR("WBP_PalInvisibleButton"));
            auto* innerButton = (inner && *inner) ? *inner : nullptr;
            if (innerButton)
            {
                CallWithBool(innerButton, STR("SetIsSelectable"), true);
                CallWithBool(innerButton, STR("SetIsToggleable"), true);
            }

            SetRowTooltip(button, innerButton, mode.Tooltip);
            m_searchModeWidgets.push_back(SearchModeWidget{button, SlateVisible});

            // Center-aligned copy of the slot layout at the button's true middle; see the main
            // row's overlays for why (the button's own alignment pins it to an edge).
            SlotLayout labelLayout = toggleLayout;
            labelLayout.Alignment = Vec2{0.5, 0.5};
            const Vec2 labelCenter{modeCenter.X + modeSize.X * (0.5 - toggleLayout.Alignment.X),
                                   modeCenter.Y + modeSize.Y * (0.5 - toggleLayout.Alignment.Y)};
            auto* label = MakeCenteredLabelAt(toggleCanvas, widgetTree, mode.Caption, labelCenter, modeSize, labelLayout, SlateHidden);
            if (label)
            {
                m_searchModeWidgets.push_back(SearchModeWidget{label, 3}); // HitTestInvisible
            }
            m_toggles.push_back(Toggle{button, innerButton, FilterAction{FilterAction::Kind::SearchTarget, static_cast<int>(mode.Target)}, label, frame});
            m_centered.push_back(Centered{button, label});
        }

        Verbose(std::format(STR("[PalpediaEnhancer] Search mode toggles built: {} widgets (expect 15)\n"),
                                                   m_searchModeWidgets.size()));

        // The popup windows get real size on the full-viewport overlay canvas, not the 420px list
        // column that the search row is laid out against.
        BuildFilterPanel(popupCanvas, widgetTree);
        BuildSortPanel(popupCanvas, widgetTree);

        Verbose(std::format(STR("[PalpediaEnhancer] Buttons created: search={} sort={} filter={}\n"),
                                                   m_searchButton != nullptr,
                                                   m_sortButton != nullptr,
                                                   m_filterButton != nullptr));

        // No focus is taken on open. It is granted only when the search button is used, because
        // stealing the keyboard from list navigation is exactly the complaint this replaces.
        m_focusGrantsRemaining = 0;

        m_labelCenterPolls = 1;
        Output::send<LogLevel::Normal>(STR("[PalpediaEnhancer] Attached Palpedia controls\n"));
    }
} // namespace PalpediaEnhancer
