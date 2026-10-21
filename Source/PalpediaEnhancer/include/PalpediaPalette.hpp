#pragma once

// The one place this mod declares color. Every value is the sRGB hex of section 2 of
// the Alkeari Design System, converted to linear here and nowhere else, so a
// color literal anywhere outside this file is a defect. In-game screens are exempt from the pixel
// grid and use the game's own font (design system section 3 and 4); what the mod draws itself still
// follows the palette, border and radius rules.

#include <cmath>
#include <cstdint>

namespace PalpediaEnhancer::Ui
{

    struct LinearColor
    {
        float R{};
        float G{};
        float B{};
        float A{};
    };

    // The palette is declared once, here, as the sRGB hex of section 2 of
    // the Alkeari Design System, and converted to linear in this one place.
    // FLinearColor is linear light, so an sRGB value written straight into it renders washed
    // out; doing the conversion here rather than at authoring time keeps every constant below
    // readable against the design document without a calculator. Alpha is not gamma-encoded and
    // passes through untouched.
    auto SrgbToLinear(float channel) -> float
    {
        return channel <= 0.04045f ? channel / 12.92f : std::pow((channel + 0.055f) / 1.055f, 2.4f);
    }

    auto FromSrgb(uint32_t rgb, float alpha = 1.0f) -> LinearColor
    {
        return LinearColor{SrgbToLinear(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f),
                           SrgbToLinear(static_cast<float>((rgb >> 8) & 0xFF) / 255.0f),
                           SrgbToLinear(static_cast<float>(rgb & 0xFF) / 255.0f),
                           alpha};
    }

    auto WithAlpha(LinearColor color, float alpha) -> LinearColor
    {
        color.A = alpha;
        return color;
    }

    namespace Palette
    {
        const LinearColor ObsidianBlack = FromSrgb(0x000000);
        const LinearColor ForgedGold = FromSrgb(0xFFDA00);
        const LinearColor SoftChampagne = FromSrgb(0xFFF1B3);
        const LinearColor AlertAmber = FromSrgb(0xFFB000);
        const LinearColor Hairline = FromSrgb(0x333333);
        const LinearColor RaisedFill = FromSrgb(0x2E2E2E);
    }

} // namespace PalpediaEnhancer::Ui
