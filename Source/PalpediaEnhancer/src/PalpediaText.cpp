#include <PalpediaText.hpp>

#include <cstring>
#include <format>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace PalpediaEnhancer::Text
{
    using namespace RC;
    using namespace RC::Unreal;

    namespace
    {
        constexpr size_t kStringSize = 0x10;
        static_assert(sizeof(FString) == kStringSize, "FString is copied by bytes into and out of parameter blocks");

        struct Conversion
        {
            UFunction* Function{};
            int32_t InOffset{-1};
            int32_t OutOffset{-1};
            size_t ParmsSize{};
            bool Resolved{};
            bool Usable{};
        };

        // Both conversions take exactly one parameter besides ReturnValue. The input is the one
        // property that is not the return value, never a name: a name typed from memory (inString
        // for what the game declares as InString) failed silently on 2026-09-02.
        auto InputOffset(UFunction* function, int32_t& returnOffset, int& inputCount) -> int32_t
        {
            int32_t inputOffset = -1;
            returnOffset = -1;
            inputCount = 0;
            for (FProperty* property : function->ForEachProperty())
            {
                if (property->GetName() == STR("ReturnValue"))
                {
                    returnOffset = property->GetOffset_Internal();
                }
                else
                {
                    inputOffset = property->GetOffset_Internal();
                    ++inputCount;
                }
            }
            return inputOffset;
        }

        // KismetTextLibrary and its two conversion functions live for the whole process, so they
        // are resolved once. The parameter block is checked against the sizes this file writes
        // and reads; a mismatch disables the conversion with one log line instead of guessing.
        auto Resolve(Conversion& conversion, const CharType* functionName, size_t inSize, size_t outSize) -> bool
        {
            if (conversion.Resolved)
            {
                return conversion.Usable;
            }
            conversion.Resolved = true;
            auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));
            auto* function = library ? library->GetFunctionByNameInChain(functionName) : nullptr;
            if (!function)
            {
                Output::send<LogLevel::Warning>(std::format(STR("[PalpediaEnhancer] KismetTextLibrary:{} not found; text conversion is unavailable\n"), functionName));
                return false;
            }
            int32_t outOffset = -1;
            int inputCount = 0;
            const auto inOffset = InputOffset(function, outOffset, inputCount);
            const auto parmsSize = static_cast<size_t>(function->GetStructureSize());
            if (inputCount != 1 || inOffset < 0 || outOffset < 0 || static_cast<size_t>(inOffset) + inSize > parmsSize ||
                static_cast<size_t>(outOffset) + outSize > parmsSize)
            {
                Output::send<LogLevel::Warning>(std::format(STR("[PalpediaEnhancer] KismetTextLibrary:{} parameters are {} bytes with {} inputs at {} and ReturnValue at {}; "
                                                                "text conversion is unavailable\n"),
                                                            functionName,
                                                            parmsSize,
                                                            inputCount,
                                                            inOffset,
                                                            outOffset));
                return false;
            }
            conversion.Function = function;
            conversion.InOffset = inOffset;
            conversion.OutOffset = outOffset;
            conversion.ParmsSize = parmsSize;
            conversion.Usable = true;
            return true;
        }

        auto Library() -> UObject*
        {
            static UObject* cached = nullptr;
            if (!cached)
            {
                cached = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetTextLibrary"));
            }
            return cached;
        }

        Conversion g_stringToText{};
        Conversion g_textToString{};
    } // namespace

    auto RawText::HasData() const -> bool
    {
        void* data = nullptr;
        std::memcpy(&data, Bytes, sizeof(data));
        return data != nullptr;
    }

    auto Make(const StringType& text) -> RawText
    {
        RawText result{};
        auto* library = Library();
        if (!library || !Resolve(g_stringToText, STR("Conv_StringToText"), kStringSize, RawText::Size))
        {
            return result;
        }
        // The FString stays alive across the call and frees its buffer on scope exit; the callee
        // only reads it. The returned FText's shared text data is handed to whichever widget
        // receives it and is never released here, the same one-way hand-off the earlier
        // FText(const wchar_t*) construction did.
        FString input{text};
        std::vector<uint8_t> parms(g_stringToText.ParmsSize, 0);
        std::memcpy(parms.data() + g_stringToText.InOffset, &input, kStringSize);
        library->ProcessEvent(g_stringToText.Function, parms.data());
        std::memcpy(result.Bytes, parms.data() + g_stringToText.OutOffset, RawText::Size);
        return result;
    }

    auto ToString(const void* textBytes) -> StringType
    {
        auto* library = Library();
        if (!textBytes || !library || !Resolve(g_textToString, STR("Conv_TextToString"), RawText::Size, kStringSize))
        {
            return {};
        }
        std::vector<uint8_t> parms(g_textToString.ParmsSize, 0);
        std::memcpy(parms.data() + g_textToString.InOffset, textBytes, RawText::Size);
        library->ProcessEvent(g_textToString.Function, parms.data());
        // Adopted into a real FString so its buffer is freed when this scope ends.
        FString output{};
        std::memcpy(&output, parms.data() + g_textToString.OutOffset, kStringSize);
        const CharType* chars = *output;
        return chars ? StringType{chars} : StringType{};
    }

    auto ToString(const RawText& text) -> StringType
    {
        return text.HasData() ? ToString(text.Bytes) : StringType{};
    }
} // namespace PalpediaEnhancer::Text
