#include <PalpediaUpdate.hpp>

#include <Windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace PalpediaEnhancer::Update
{
    namespace
    {
        struct Handle
        {
            HINTERNET Value{};
            ~Handle()
            {
                if (Value)
                {
                    WinHttpCloseHandle(Value);
                }
            }
        };

        // Large enough for the feed many times over; a response past it is not this feed.
        constexpr size_t MaxBytes = 256 * 1024;
    }

    auto FetchFeed() -> std::optional<std::string>
    {
        Handle session{WinHttpOpen(L"PalpediaEnhancer", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.Value)
        {
            return std::nullopt;
        }
        WinHttpSetTimeouts(session.Value, 5000, 5000, 5000, 5000);
        Handle connection{WinHttpConnect(session.Value, std::wstring(FeedHost).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
        if (!connection.Value)
        {
            return std::nullopt;
        }
        Handle request{WinHttpOpenRequest(connection.Value, L"GET", std::wstring(FeedPath).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
        if (!request.Value)
        {
            return std::nullopt;
        }
        if (!WinHttpSendRequest(request.Value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.Value, nullptr))
        {
            return std::nullopt;
        }
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(request.Value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) || status != 200)
        {
            return std::nullopt;
        }
        std::string body;
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.Value, &available))
            {
                return std::nullopt;
            }
            if (available == 0)
            {
                break;
            }
            if (body.size() + available > MaxBytes)
            {
                return std::nullopt;
            }
            const size_t start = body.size();
            body.resize(start + available);
            DWORD read = 0;
            if (!WinHttpReadData(request.Value, body.data() + start, available, &read))
            {
                return std::nullopt;
            }
            body.resize(start + read);
        }
        return body;
    }
}
