#pragma once

#include <String/StringType.hpp>
#include <Unreal/Common.hpp>

namespace PalpediaEnhancer::Loc
{
    // Resolves the game's current language once and loads Assets/lang/<code>.ini over the
    // built-in English table. Safe to call repeatedly; only the first call does work. Must run on
    // the game thread after the engine is up (it calls a Kismet library).
    auto EnsureLoaded() -> void;

    // The string for a key, in the loaded language, else English, else the key itself. The
    // returned reference stays valid for the life of the process, so .c_str() can be stored.
    auto T(const RC::CharType* key) -> const RC::StringType&;

    // Language code actually in use ("en" until EnsureLoaded succeeds).
    auto Language() -> const RC::StringType&;
} // namespace PalpediaEnhancer::Loc
