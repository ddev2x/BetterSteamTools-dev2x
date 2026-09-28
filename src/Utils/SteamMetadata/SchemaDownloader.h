#pragma once

#include "Steam/Types.h"
#include <string>

namespace SchemaDownloader {

    // Check if the binary schema file exists on disk (appcache/stats/UserGameStatsSchema_<appId>.bin)
    bool HasSchemaOnDisk(AppId_t appId);

    // Fetch schema binary from https://www.niuplayer.xyz/api/steam/stats, ignoring SSL errors,
    // save to appcache/stats/UserGameStatsSchema_<appId>.bin and return contents in outData.
    bool FetchAndSaveSchema(AppId_t appId, uint64_t donorSteamId = 0, std::string* outData = nullptr);

    // Asynchronously pre-fetch and save schema in background thread if missing on disk.
    void EnsureSchemaAsync(AppId_t appId, uint64_t donorSteamId = 0);

    // Synchronously get schema: loads from disk if exists, otherwise fetches from server.
    bool GetOrFetchSchema(AppId_t appId, uint64_t donorSteamId, std::string& outData);

} // namespace SchemaDownloader
