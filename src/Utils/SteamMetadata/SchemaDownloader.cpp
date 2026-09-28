#include "SchemaDownloader.h"
#include "dllmain.h"
#include "Utils/Logging/Log.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace SchemaDownloader {
namespace {

    constexpr const wchar_t* kApiHost = L"www.niuplayer.xyz";
    constexpr INTERNET_PORT  kApiPort = INTERNET_DEFAULT_HTTPS_PORT;
    constexpr uint64_t kDefaultDonor = 76561198028121353ULL;

    std::mutex g_mutex;
    std::unordered_set<AppId_t> g_inFlight;

    std::filesystem::path GetStatsDirectory() {
        if (SteamInstallPath[0] != '\0') {
            return std::filesystem::path(SteamInstallPath) / "appcache" / "stats";
        }
        return std::filesystem::path("appcache/stats");
    }

    std::filesystem::path GetSchemaFilePath(AppId_t appId) {
        return GetStatsDirectory() / std::format("UserGameStatsSchema_{}.bin", appId);
    }

    void EnsureInitialUserStats(AppId_t appId) {
        if (SteamInstallPath[0] == '\0') return;

        static const uint8_t kDefaultCache[38] = {
            0x00, 0x63, 0x61, 0x63, 0x68, 0x65, 0x00, 
            0x02, 0x63, 0x72, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x02, 0x50, 0x65, 0x6E, 0x64, 0x69, 0x6E, 0x67, 0x43, 0x68, 0x61, 0x6E, 0x67, 0x65, 0x73, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x08, 0x08
        };

        const auto statsDir = GetStatsDirectory();
        const auto userdataDir = std::filesystem::path(SteamInstallPath) / "userdata";

        std::error_code ec;
        if (!std::filesystem::exists(userdataDir, ec)) return;

        for (const auto& entry : std::filesystem::directory_iterator(userdataDir, ec)) {
            if (!entry.is_directory()) continue;
            std::string dirName = entry.path().filename().string();
            bool allDigits = !dirName.empty() && std::all_of(dirName.begin(), dirName.end(), ::isdigit);
            if (!allDigits) continue;

            auto userStatsPath = statsDir / std::format("UserGameStats_{}_{}.bin", dirName, appId);
            if (!std::filesystem::exists(userStatsPath, ec)) {
                std::ofstream ofs(userStatsPath, std::ios::binary);
                if (ofs) {
                    ofs.write(reinterpret_cast<const char*>(kDefaultCache), sizeof(kDefaultCache));
                    LOG_ACHIEVEMENT_DEBUG("SchemaDownloader: created initial user stats cache {}", userStatsPath.string());
                }
            }
        }
    }

    bool PerformHttpDownload(AppId_t appId, uint64_t donorSteamId, std::string& outBody) {
        uint64_t targetDonor = (donorSteamId != 0) ? donorSteamId : kDefaultDonor;
        std::wstring path = std::format(L"/api/steam/stats?appid={}&steamid={}", appId, targetDonor);

        HINTERNET hSession = WinHttpOpen(
            L"OpenSteamTool/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!hSession) {
            LOG_ACHIEVEMENT_WARN("SchemaDownloader: WinHttpOpen failed (err={})", GetLastError());
            return false;
        }

        WinHttpSetTimeouts(hSession, 4000, 4000, 8000, 8000);

        HINTERNET hConnect = WinHttpConnect(hSession, kApiHost, kApiPort, 0);
        if (!hConnect) {
            LOG_ACHIEVEMENT_WARN("SchemaDownloader: WinHttpConnect failed (err={})", GetLastError());
            WinHttpCloseHandle(hSession);
            return false;
        }

        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect,
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!hRequest) {
            LOG_ACHIEVEMENT_WARN("SchemaDownloader: WinHttpOpenRequest failed (err={})", GetLastError());
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        // 忽略所有 SSL 证书校验错误 (如用户要求)
        DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                         SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE |
                         SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

        bool success = false;
        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, nullptr))
        {
            DWORD statusCode = 0;
            DWORD sz = sizeof(statusCode);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX))
            {
                LOG_ACHIEVEMENT_INFO("SchemaDownloader: HTTP status={} for appid={}", statusCode, appId);
                if (statusCode == 200) {
                    DWORD dwSize = 0;
                    while (WinHttpQueryDataAvailable(hRequest, &dwSize) && dwSize > 0) {
                        std::string chunk;
                        chunk.resize(dwSize);
                        DWORD dwDownloaded = 0;
                        if (WinHttpReadData(hRequest, chunk.data(), dwSize, &dwDownloaded)) {
                            chunk.resize(dwDownloaded);
                            outBody.append(chunk);
                        } else {
                            break;
                        }
                    }
                    if (!outBody.empty()) {
                        success = true;
                    }
                }
            }
        } else {
            LOG_ACHIEVEMENT_WARN("SchemaDownloader: HTTP request failed for appid={} (err={})", appId, GetLastError());
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return success;
    }

} // namespace

bool HasSchemaOnDisk(AppId_t appId) {
    if (appId == k_uAppIdInvalid) return false;
    std::error_code ec;
    auto path = GetSchemaFilePath(appId);
    return std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0;
}

bool FetchAndSaveSchema(AppId_t appId, uint64_t donorSteamId, std::string* outData) {
    if (appId == k_uAppIdInvalid) return false;

    // Concurrency guard
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_inFlight.count(appId)) {
            LOG_ACHIEVEMENT_DEBUG("SchemaDownloader: download already in flight for appid={}", appId);
            return false;
        }
        g_inFlight.insert(appId);
    }

    struct InFlightGuard {
        AppId_t id;
        ~InFlightGuard() {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_inFlight.erase(id);
        }
    } guard{appId};

    std::string body;
    if (!PerformHttpDownload(appId, donorSteamId, body)) {
        LOG_ACHIEVEMENT_WARN("SchemaDownloader: failed to download schema for appid={}", appId);
        return false;
    }

    // Save to disk
    std::error_code ec;
    const auto statsDir = GetStatsDirectory();
    std::filesystem::create_directories(statsDir, ec);

    const auto schemaPath = GetSchemaFilePath(appId);
    std::ofstream ofs(schemaPath, std::ios::binary);
    if (!ofs) {
        LOG_ACHIEVEMENT_WARN("SchemaDownloader: failed to open {} for writing", schemaPath.string());
        return false;
    }
    ofs.write(body.data(), body.size());
    ofs.close();

    LOG_ACHIEVEMENT_INFO("SchemaDownloader: saved {} bytes to {}", body.size(), schemaPath.string());

    EnsureInitialUserStats(appId);

    if (outData) {
        *outData = std::move(body);
    }
    return true;
}

void EnsureSchemaAsync(AppId_t appId, uint64_t donorSteamId) {
    if (appId == k_uAppIdInvalid) return;
    if (HasSchemaOnDisk(appId)) return;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_inFlight.count(appId)) return;
    }

    std::thread([appId, donorSteamId]() {
        FetchAndSaveSchema(appId, donorSteamId, nullptr);
    }).detach();
}

bool GetOrFetchSchema(AppId_t appId, uint64_t donorSteamId, std::string& outData) {
    if (appId == k_uAppIdInvalid) return false;

    auto path = GetSchemaFilePath(appId);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        auto sz = std::filesystem::file_size(path, ec);
        if (sz > 0) {
            std::ifstream ifs(path, std::ios::binary);
            if (ifs) {
                outData.resize(sz);
                ifs.read(outData.data(), sz);
                return true;
            }
        }
    }

    return FetchAndSaveSchema(appId, donorSteamId, &outData);
}

} // namespace SchemaDownloader
