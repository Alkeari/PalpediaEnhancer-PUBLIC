#include <PalpediaNotice.hpp>

#include <cstring>
#include <vector>

#include <Unreal/FProperty.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <PalpediaText.hpp>

namespace PalpediaEnhancer::Notice
{
    using namespace RC;
    using namespace RC::Unreal;

    namespace
    {
        constexpr uint8_t LogPriorityNormal = 1;

        auto ParamProperty(UFunction* function, const CharType* name) -> FProperty*
        {
            for (FProperty* property : function->ForEachProperty())
            {
                if (property->GetName() == name)
                {
                    return property;
                }
            }
            return nullptr;
        }

        // Lives for the whole process, so it is looked up once; FindFirstOf walks every UObject.
        auto GameInstance() -> UObject*
        {
            static UObject* cached = nullptr;
            if (!cached)
            {
                cached = UObjectGlobals::FindFirstOf(STR("PalGameInstance"));
            }
            return cached;
        }

        // Calls a static PalUtility function that takes a world context and returns an object.
        auto CallUtility(const CharType* functionName) -> UObject*
        {
            auto* utility = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            auto* context = GameInstance();
            auto* function = utility ? utility->GetFunctionByNameInChain(functionName) : nullptr;
            if (!function || !context)
            {
                return nullptr;
            }
            auto* contextProperty = ParamProperty(function, STR("WorldContextObject"));
            auto* returnProperty = function->GetReturnProperty();
            if (!contextProperty || !returnProperty || returnProperty->GetSize() != sizeof(UObject*))
            {
                return nullptr;
            }
            std::vector<uint8_t> parms(static_cast<size_t>(function->GetParmsSize()), 0);
            std::memcpy(parms.data() + contextProperty->GetOffset_Internal(), &context, sizeof(context));
            utility->ProcessEvent(function, parms.data());
            UObject* result = nullptr;
            std::memcpy(&result, parms.data() + returnProperty->GetOffset_Internal(), sizeof(result));
            return result;
        }
    }

    auto PlayerInWorld() -> bool
    {
        return CallUtility(STR("GetPlayerCharacter")) != nullptr;
    }

    auto PostGameLog(const StringType& text) -> bool
    {
        auto* manager = CallUtility(STR("GetLogManager"));
        auto* function = manager ? manager->GetFunctionByNameInChain(STR("AddLog")) : nullptr;
        if (!function)
        {
            return false;
        }
        auto* priority = ParamProperty(function, STR("logPriority"));
        auto* logText = ParamProperty(function, STR("LogText"));
        if (!priority || !logText || logText->GetSize() != static_cast<int32_t>(Text::RawText::Size))
        {
            return false;
        }
        const auto raw = Text::Make(text);
        if (!raw.HasData())
        {
            return false;
        }
        // logAdditionalData stays zeroed: an empty texture list, no tone, NAME_None font and
        // widget, which is the plain line the game writes for its own notices.
        std::vector<uint8_t> parms(static_cast<size_t>(function->GetParmsSize()), 0);
        parms[static_cast<size_t>(priority->GetOffset_Internal())] = LogPriorityNormal;
        std::memcpy(parms.data() + logText->GetOffset_Internal(), raw.Bytes, Text::RawText::Size);
        manager->ProcessEvent(function, parms.data());
        return true;
    }
}
