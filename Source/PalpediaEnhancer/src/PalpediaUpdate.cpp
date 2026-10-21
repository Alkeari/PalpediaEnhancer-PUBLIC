#include <PalpediaUpdate.hpp>

#include <cctype>
#include <vector>

namespace PalpediaEnhancer::Update
{
    namespace
    {
        auto Parts(std::string_view version) -> std::vector<long long>
        {
            std::vector<long long> parts;
            long long current = 0;
            bool any = false;
            for (const char c : version)
            {
                if (std::isdigit(static_cast<unsigned char>(c)))
                {
                    current = current * 10 + (c - '0');
                    any = true;
                }
                else if (c == '.')
                {
                    parts.push_back(current);
                    current = 0;
                }
                else
                {
                    break;
                }
            }
            if (any || !parts.empty())
            {
                parts.push_back(current);
            }
            return parts;
        }

        auto SkipSpace(std::string_view text, size_t at) -> size_t
        {
            while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
            {
                ++at;
            }
            return at;
        }
    }

    auto CompareVersions(std::string_view left, std::string_view right) -> int
    {
        const auto a = Parts(left);
        const auto b = Parts(right);
        const size_t count = a.size() > b.size() ? a.size() : b.size();
        for (size_t i = 0; i < count; ++i)
        {
            const long long x = i < a.size() ? a[i] : 0;
            const long long y = i < b.size() ? b[i] : 0;
            if (x != y)
            {
                return x < y ? -1 : 1;
            }
        }
        return 0;
    }

    auto LatestVersion(std::string_view feedJson) -> std::optional<std::string>
    {
        constexpr std::string_view key = "\"version\"";
        std::optional<std::string> best;
        size_t at = 0;
        while ((at = feedJson.find(key, at)) != std::string_view::npos)
        {
            at = SkipSpace(feedJson, at + key.size());
            if (at >= feedJson.size() || feedJson[at] != ':')
            {
                continue;
            }
            at = SkipSpace(feedJson, at + 1);
            if (at >= feedJson.size() || feedJson[at] != '"')
            {
                continue;
            }
            const size_t end = feedJson.find('"', at + 1);
            if (end == std::string_view::npos)
            {
                break;
            }
            const std::string value(feedJson.substr(at + 1, end - at - 1));
            at = end + 1;
            if (Parts(value).empty())
            {
                continue;
            }
            if (!best || CompareVersions(value, *best) > 0)
            {
                best = value;
            }
        }
        return best;
    }

    auto NewerThanInstalled(std::string_view feedJson, std::string_view installed) -> std::optional<std::string>
    {
        const auto latest = LatestVersion(feedJson);
        if (latest && CompareVersions(*latest, installed) > 0)
        {
            return latest;
        }
        return std::nullopt;
    }
}
