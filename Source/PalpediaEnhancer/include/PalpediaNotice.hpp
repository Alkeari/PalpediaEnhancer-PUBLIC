#pragma once

#include <Unreal/UObject.hpp>

namespace PalpediaEnhancer::Notice
{
    // Whether a player character is in a loaded world, so a log line would be drawn on the HUD
    // rather than dropped by the title screen. Game thread only.
    auto PlayerInWorld() -> bool;

    // Adds one line to the game's own on-screen log through PalLogManager::AddLog. False when the
    // log manager or the function is not there, or the text could not be converted. Game thread only.
    auto PostGameLog(const RC::StringType& text) -> bool;
}
