#pragma once

#include <cstdint>

#include <Unreal/UObject.hpp>

namespace PalpediaEnhancer::Text
{
    // The game's FText carried as opaque bytes, never as UE4SS's own FText type. UE4SS's FText
    // differs between the two UE4SS builds this mod has to load on: the 2026-07 line has a 0x18
    // struct with a public Data pointer, the 2026-08 line an opaque 0x30 buffer whose copy, move
    // and destroy are exported functions the older UE4SS.dll does not have. Either choice would
    // mis-size a ProcessEvent parameter block on one build or fail to load on the other. Both
    // conversions here go through KismetTextLibrary, engine functions that exist on every build.
    struct RawText
    {
        static constexpr size_t Size = 0x18;
        alignas(8) uint8_t Bytes[Size]{};

        auto HasData() const -> bool;
    };

    // Empty (HasData() false) when the conversion is unavailable. An empty FText must never be
    // handed to an engine function: a widget holding one crashes whoever reads it next
    // (EXCEPTION_ACCESS_VIOLATION reading 0xffffffffffffffff, observed live 2026-09-02).
    auto Make(const RC::StringType& text) -> RawText;
    auto ToString(const RawText& text) -> RC::StringType;
    auto ToString(const void* textBytes) -> RC::StringType;
} // namespace PalpediaEnhancer::Text
