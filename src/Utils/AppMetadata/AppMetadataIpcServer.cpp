#include "AppMetadataIpcServer.h"
#include "VdfParser.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "OSTPlatform/include/Thread.h"

#include <windows.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <thread>

namespace AppMetadataIpcServer {

namespace {

    constexpr const char* kPipeName = "\\\\.\\pipe\\OpenSteamTool_IPC";
    constexpr DWORD kBufferSize = 65536;

    std::atomic<bool> g_running{false};
    HANDLE g_hStopEvent = nullptr;

    // Minimal JSON helpers
    size_t FindValueStart(std::string_view body, std::string_view key) {
        std::string needle = "\"" + std::string(key) + "\"";
        size_t k = body.find(needle);
        if (k == std::string_view::npos) return std::string_view::npos;
        size_t colon = body.find(':', k + needle.size());
        if (colon == std::string_view::npos) return std::string_view::npos;
        size_t v = colon + 1;
        while (v < body.size() && isspace(static_cast<unsigned char>(body[v]))) {
            v++;
        }
        return v < body.size() ? v : std::string_view::npos;
    }

    bool JsonExtractString(std::string_view body, std::string_view key, std::string& out) {
        size_t v = FindValueStart(body, key);
        if (v == std::string_view::npos || body[v] != '"') return false;
        size_t q1 = v;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string_view::npos) return false;
        out = std::string(body.substr(q1 + 1, q2 - q1 - 1));
        return true;
    }

    bool JsonExtractUInt64(std::string_view body, std::string_view key, uint64_t& out) {
        size_t v = FindValueStart(body, key);
        if (v == std::string_view::npos) return false;

        // Case 1: Quoted string "12345"
        if (body[v] == '"') {
            size_t q2 = body.find('"', v + 1);
            if (q2 == std::string_view::npos) return false;
            std::string_view numStr = body.substr(v + 1, q2 - v - 1);
            auto [p, ec] = std::from_chars(numStr.data(), numStr.data() + numStr.size(), out);
            return (ec == std::errc{});
        }

        // Case 2: Raw number 12345
        if (isdigit(static_cast<unsigned char>(body[v]))) {
            size_t end = v;
            while (end < body.size() && isdigit(static_cast<unsigned char>(body[end]))) {
                end++;
            }
            std::string_view numStr = body.substr(v, end - v);
            auto [p, ec] = std::from_chars(numStr.data(), numStr.data() + numStr.size(), out);
            return (ec == std::errc{});
        }

        return false;
    }

    bool JsonExtractBool(std::string_view body, std::string_view key, bool& out) {
        size_t v = FindValueStart(body, key);
        if (v == std::string_view::npos) return false;
        if (body.substr(v, 4) == "true") {
            out = true;
            return true;
        }
        if (body.substr(v, 5) == "false") {
            out = false;
            return true;
        }
        return false;
    }

    std::string HandleRequest(const std::string& requestStr) {
        std::string action = "get_dlc_manifests";
        JsonExtractString(requestStr, "action", action);

        uint64_t appId64 = 0;
        if (!JsonExtractUInt64(requestStr, "appId", appId64) || appId64 == 0) {
            return "{\"code\": 400, \"message\": \"Missing or invalid appId\"}";
        }
        uint32_t appId = static_cast<uint32_t>(appId64);

        // Check if token was provided
        uint64_t token = 0;
        if (JsonExtractUInt64(requestStr, "token", token) && token != 0) {
            LuaConfig::SetAccessToken(appId, token);
            LOG_INFO("AppMetadataIpcServer: Dynamically registered access_token {} for appId {}", token, appId);
        }

        bool forceRefresh = false;
        JsonExtractBool(requestStr, "forceRefresh", forceRefresh);

        // Query app metadata from appinfo.vdf cache
        auto result = VdfParser::QueryAppMetadata(appId);
        return result.ToJson();
    }

    uint32_t ServerWorkerThread() {
        LOG_INFO("AppMetadataIpcServer: Started listening on {}", kPipeName);

        while (g_running.load()) {
            HANDLE hPipe = CreateNamedPipeA(
                kPipeName,
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                kBufferSize,
                kBufferSize,
                0,
                nullptr
            );

            if (hPipe == INVALID_HANDLE_VALUE) {
                LOG_ERROR("AppMetadataIpcServer: CreateNamedPipe failed (err={})", GetLastError());
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            OVERLAPPED ovConnect = {};
            ovConnect.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

            BOOL connectOk = ConnectNamedPipe(hPipe, &ovConnect);
            if (!connectOk && GetLastError() == ERROR_IO_PENDING) {
                HANDLE waitHandles[2] = { g_hStopEvent, ovConnect.hEvent };
                DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                if (waitRes == WAIT_OBJECT_0) {
                    // Stop event signaled
                    CancelIo(hPipe);
                    CloseHandle(ovConnect.hEvent);
                    CloseHandle(hPipe);
                    break;
                }
                connectOk = TRUE;
            } else if (!connectOk && GetLastError() == ERROR_PIPE_CONNECTED) {
                connectOk = TRUE;
            }

            CloseHandle(ovConnect.hEvent);

            if (!connectOk || !g_running.load()) {
                CloseHandle(hPipe);
                continue;
            }

            // Client is connected. Process request.
            char inBuffer[kBufferSize] = {};
            DWORD bytesRead = 0;
            BOOL readOk = ReadFile(hPipe, inBuffer, sizeof(inBuffer) - 1, &bytesRead, nullptr);
            if (readOk && bytesRead > 0) {
                inBuffer[bytesRead] = '\0';
                std::string response = HandleRequest(inBuffer);

                DWORD bytesWritten = 0;
                WriteFile(hPipe, response.c_str(), static_cast<DWORD>(response.size()), &bytesWritten, nullptr);
                FlushFileBuffers(hPipe);
            }

            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }

        LOG_INFO("AppMetadataIpcServer: Stopped");
        return 0;
    }

} // anonymous namespace

    void Start() {
        if (g_running.exchange(true)) return;

        g_hStopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        OSTPlatform::Thread::StartDetached([] {
            return ServerWorkerThread();
        });
    }

    void Stop() {
        if (!g_running.exchange(false)) return;

        if (g_hStopEvent) {
            SetEvent(g_hStopEvent);
        }

        // Connect once locally to unblock any pending accept
        DWORD bytesRead = 0;
        char dummy[16] = {};
        char dummyReq[] = "{}";
        CallNamedPipeA(kPipeName, dummyReq, sizeof(dummyReq) - 1, dummy, sizeof(dummy), &bytesRead, 100);

        if (g_hStopEvent) {
            CloseHandle(g_hStopEvent);
            g_hStopEvent = nullptr;
        }
    }

    bool IsRunning() {
        return g_running.load();
    }

} // namespace AppMetadataIpcServer

// ── C-ABI Exported Functions ──────────────────────────────────────────────────

extern "C" {

    __declspec(dllexport) const char* __cdecl OST_QueryAppDlcAndManifests(
        uint32_t appId,
        const char* token,
        bool forceRefresh
    ) {
        std::string reqJson = std::format(
            "{{\"action\":\"get_dlc_manifests\",\"appId\":{},\"token\":\"{}\",\"forceRefresh\":{}}}",
            appId,
            token ? token : "",
            forceRefresh ? "true" : "false"
        );

        // 1. Try communicating via IPC Named Pipe first (if Steam is running)
        char respBuf[65536] = {};
        DWORD bytesRead = 0;
        if (CallNamedPipeA("\\\\.\\pipe\\OpenSteamTool_IPC",
                           const_cast<char*>(reqJson.data()), static_cast<DWORD>(reqJson.size()),
                           respBuf, sizeof(respBuf) - 1, &bytesRead, 2000))
        {
            respBuf[bytesRead] = '\0';
            return _strdup(respBuf);
        }

        // 2. Fallback: Standalone local execution (Steam is not running or pipe unreachable)
        if (token && *token) {
            uint64_t parsedToken = 0;
            auto [p, ec] = std::from_chars(token, token + strlen(token), parsedToken);
            if (ec == std::errc{} && parsedToken != 0) {
                LuaConfig::SetAccessToken(appId, parsedToken);
            }
        }

        auto res = VdfParser::QueryAppMetadata(appId);
        std::string json = res.ToJson();
        return _strdup(json.c_str());
    }

    __declspec(dllexport) void __cdecl OST_FreeString(const char* p) {
        if (p) {
            free(const_cast<char*>(p));
        }
    }

}
