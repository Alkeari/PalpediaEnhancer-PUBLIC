#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace PalpediaEnhancer::Update
{
    // The published-version feed for this mod. It lists each release line with its version and
    // where it is published; nothing is sent to it but the request itself.
    inline constexpr std::wstring_view FeedHost = L"alkeari.com";
    inline constexpr std::wstring_view FeedPath = L"/updates/palpedia-enhancer.json";

    // The highest "version" value anywhere in the feed, or nothing when it carries none.
    auto LatestVersion(std::string_view feedJson) -> std::optional<std::string>;

    // Compares dotted numeric versions ("1.11.1"): negative, zero or positive like strcmp.
    // A missing part counts as 0, so "1.2" equals "1.2.0".
    auto CompareVersions(std::string_view left, std::string_view right) -> int;

    // The version to announce, only when the feed names one newer than the installed version.
    auto NewerThanInstalled(std::string_view feedJson, std::string_view installed) -> std::optional<std::string>;

    // Fetches the feed over HTTPS on the calling thread. Nothing on any failure.
    auto FetchFeed() -> std::optional<std::string>;
}
