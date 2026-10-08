#include "dllmain.h"
#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/SteamMetadata/StatsClient.h"
#include "Utils/Tickets/AppTicket.h"

#include <lua.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "Advapi32.lib")
#endif

extern "C" {
    struct lua_State;
}

namespace LuaConfig{
    static lua_State* g_lua_state = nullptr;
    static bool g_hasManifestCodeFunc = false;
    static bool g_hasManifestCodeFuncEx = false;
    std::unordered_map<AppId_t, std::string>DepotKeySet{};
    std::unordered_map<AppId_t, uint64_t>AccessTokenSet{};
    std::unordered_map<AppId_t, std::string>LegacyCDKeySet{};
    std::unordered_set<AppId_t> PinnedApps{};
    std::unordered_map<uint64_t, ManifestOverride> ManifestOverrides{};
    std::unordered_map<AppId_t, uint64_t> StatSteamIdSet{};
    std::unordered_set<AppId_t> OwnedAppIdSet{};
    // Process exe name (lowercase) → appid; populated by addprocess() in Lua config.
    std::unordered_map<std::string, AppId_t> ProcessNameAppIdMap{};
    // App IDs that should bypass ProtectionScan and be treated as Denuvo games.
    std::unordered_set<AppId_t> ForcedDenuvoSet{};
    // On-demand eticket mint endpoint, set via seteticketurl() in Lua config.
    // Empty = disabled (EticketClient falls back to credential-store ticket).
    std::string EticketUrl{};

    // Per-file tracking: which depots each .lua file contributed.
    static std::string g_currentFile;
    static std::unordered_map<std::string, std::unordered_set<AppId_t>> g_fileDepots;
    static std::unordered_map<std::string, std::unordered_map<uint64_t, ManifestOverride>> g_fileManifestOverrides;
    static std::unordered_map<std::string, uint64_t> g_fileParseSequence;
    static uint64_t g_nextFileParseSequence = 0;
    // Reference count: how many files provide each depot.
    static std::unordered_map<AppId_t, uint32_t> g_depotRefCount;
    // mtime (unix epoch seconds) of each parsed .lua file, captured at ParseFile entry.
    static std::unordered_map<std::string, uint32_t> g_fileMtime;
    // Per-appId purchase time: max(mtime) across every file that currently contributes it.
    // Simple variant: never lowered on UnloadFile unless the refcount drops to zero.
    static std::unordered_map<AppId_t, uint32_t> g_purchaseTime;
    // Depot IDs removed by UnloadFile / added by ParseFile, consumed by NotifyLicenseChanged.
    static std::vector<AppId_t> g_pendingRemovals;
    static std::vector<AppId_t> g_pendingAdditions;
    
    // constexpr uint64_t kDefaultStatSteamId = 76561198028121353ULL;
    constexpr uint64_t kDefaultStatSteamId = 76561197960265729ULL;

    // Case-insensitive function registry: lowercase name → C function
    static std::unordered_map<std::string, lua_CFunction> g_func_registry;

    static bool ParseUInt64Decimal(const char* text, uint64_t* out) {
        if (!text || !out) return false;
        const auto parsed = OSTPlatform::Numbers::ParseUInt64(text);
        if (!parsed) return false;
        *out = *parsed;
        return true;
    }

    static uint8 ParseHexByte(std::string_view text) {
        return OSTPlatform::Numbers::ParseHexUInt8(text).value_or(0);
    }

    static void SetActiveManifestOverride(uint64_t depotId, const ManifestOverride& override) {
        ManifestOverrides[depotId] = override;
    }

    static void ClearActiveManifestOverride(uint64_t depotId) {
        ManifestOverrides.erase(depotId);
    }

    static void RebuildManifestOverride(uint64_t depotId) {
        const ManifestOverride* best = nullptr;
        uint64_t bestSeq = 0;

        for (const auto& [file, overrides] : g_fileManifestOverrides) {
            auto overrideIt = overrides.find(depotId);
            if (overrideIt == overrides.end()) continue;

            auto seqIt = g_fileParseSequence.find(file);
            if (seqIt == g_fileParseSequence.end()) continue;

            if (!best || seqIt->second > bestSeq) {
                best = &overrideIt->second;
                bestSeq = seqIt->second;
            }
        }

        if (best) {
            SetActiveManifestOverride(depotId, *best);
        } else {
            ClearActiveManifestOverride(depotId);
        }
    }

    // ── Lua HTTP helpers ──────────────────────────────────────────
    //   http_get(url [, headers]) → body, status_code
    //     headers: optional table, e.g. {["Accept"]="application/json"}
    //
    //   http_post(url, post_body [, headers]) → body, status_code
    //     post_body: string payload
    // ────────────────────────────────────────────────────────────────

    // Serialise a Lua {key=val, ...} table at stack index idx into a
    // wstring of "Key: Val\r\n" lines. Returns L"" if no table.
    static std::wstring LuaHeadersToWstr(lua_State* L, int idx) {
        std::wstring headers;
        if (idx < 1 || !lua_istable(L, idx)) return headers;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            std::string key(lua_tostring(L, -2));
            std::string val(lua_tostring(L, -1));
            headers += std::wstring(key.begin(), key.end())
                    + L": " + std::wstring(val.begin(), val.end()) + L"\r\n";
            lua_pop(L, 1);
        }
        return headers;
    }

    static int lua_http_get(lua_State* L) {
        // http_get(url [, headers]) → body, status_code
        auto hdrs = LuaHeadersToWstr(L, lua_gettop(L) >= 2 ? 2 : -1);
        auto r = OSTPlatform::Http::Execute(L"GET", luaL_checkstring(L, 1),
                                            nullptr, 0, hdrs.empty() ? nullptr : hdrs.c_str());
        if (r.ok) {
            lua_pushstring(L, r.body.c_str());
            lua_pushinteger(L, r.status);
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "HTTP request failed");
        }
        return 2;
    }

    static int lua_http_post(lua_State* L) {
        // http_post(url, post_body [, headers]) → body, status_code
        size_t bodyLen = 0;
        const char* body = luaL_checklstring(L, 2, &bodyLen);
        auto hdrs = LuaHeadersToWstr(L, lua_gettop(L) >= 3 ? 3 : -1);
        auto r = OSTPlatform::Http::Execute(L"POST", luaL_checkstring(L, 1),
                                            body, static_cast<uint32_t>(bodyLen),
                                            hdrs.empty() ? nullptr : hdrs.c_str());
        if (r.ok) {
            lua_pushstring(L, r.body.c_str());
            lua_pushinteger(L, r.status);
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "HTTP request failed");
        }
        return 2;
    }

    // ── Case-insensitive global function lookup ─────────────────
    // __index metamethod on _G: when a global name isn't found,
    // lower-case the name and look it up in g_func_registry.
    static int case_insensitive_global_index(lua_State* L) {
        const char* name = lua_tostring(L, 2);
        if (!name) {
            lua_pushnil(L);
            return 1;
        }
        std::string lower;
        for (const char* p = name; *p; ++p) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        }
        auto it = g_func_registry.find(lower);
        if (it != g_func_registry.end()) {
            lua_pushcfunction(L, it->second);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    // Register a C function both in _G (lowercase name) and in the
    // case-insensitive lookup table so any case variant resolves.
    static void register_func(lua_State* L, const char* lowercase_name, lua_CFunction fn) {
        g_func_registry[lowercase_name] = fn;
        lua_pushcfunction(L, fn);
        lua_setglobal(L, lowercase_name);
    }

    // ── Lua: addappid / addtoken / pinApp / setManifestid ────────
    static int lua_addappid(lua_State* L) {
        // addappid(integer, integer, string)
        int argc = lua_gettop(L);
        // Validate argument count and required argument types.
        if (argc == 0) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }

        // Read the first argument as app/depot id.
        lua_Integer value = lua_tointeger(L, 1);
        // Ensure the value fits into uint32_t range.
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t DepotId = (uint32_t)value;
        // Read the optional third argument as a key.
        std::string Key = "";
        if (argc > 2) {
            if (!lua_isstring(L, 3))
                return luaL_error(L, "");
            const char* key = lua_tostring(L, 3);
            // Keep only keys with exactly 64 characters.
            if (strlen(key) == 64) {
                Key = std::string(key);
            }
        }
        // Non-empty keys have priority over existing empty keys.
        if (!Key.empty() || !DepotKeySet.count(DepotId)) {
            DepotKeySet[DepotId] = Key;
        }

        if (!g_currentFile.empty()) {
            if (g_fileDepots[g_currentFile].insert(DepotId).second) {
                if (++g_depotRefCount[DepotId] == 1)
                    g_pendingAdditions.push_back(DepotId);
                
                // Update the appId's purchase time with the current file's mtime,
                // keeping the maximum across every contributing file.
                auto mtIt = g_fileMtime.find(g_currentFile);
                if (mtIt != g_fileMtime.end()) {
                    uint32_t mt = mtIt->second;
                    auto& slot = g_purchaseTime[DepotId];
                    if (mt > slot) slot = mt;
                }
            }
        }

        return 0;
    }

    static int lua_addtoken(lua_State* L) {
        // addtoken(integer, string(uint64_t))
        int argc = lua_gettop(L);
        // Validate argument count and required argument types.
        if (argc == 0) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }

        // Read the first argument as app/depot id.
        lua_Integer value = lua_tointeger(L, 1);
        // Ensure the value fits into uint32_t range.
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t AppId = (uint32_t)value;
        // Read the second argument as a token.
        if (argc > 1) {
            if (!lua_isstring(L, 2))
                return luaL_error(L, "");
            const char* token = lua_tostring(L, 2);
            uint64_t parsedToken = 0;
            if (!ParseUInt64Decimal(token, &parsedToken)) {
                return luaL_error(L, "");
            }
            AccessTokenSet[AppId] = parsedToken;
        }

        return 0;
    }

    static int lua_setlegacycdkey(lua_State* L) {
        // setlegacycdkey(integer appid, string key)
        int argc = lua_gettop(L);
        if (argc < 2) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }
        // Read the first argument as appid.
        lua_Integer value = lua_tointeger(L, 1);
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t AppId = (uint32_t)value;
        // Read the second argument as the CD key, stored verbatim.
        if (!lua_isstring(L, 2))
            return luaL_error(L, "");
        LegacyCDKeySet[AppId] = lua_tostring(L, 2);
        return 0;
    }

    static int lua_addprocess(lua_State* L) {
        // addprocess(appid, "ExeName.exe")
        // Maps a process exe name to an appid so OST can identify games
        // that launch without exporting SteamAppId env vars.
        int argc = lua_gettop(L);
        if (argc < 2 || !lua_isinteger(L, 1) || !lua_isstring(L, 2))
            return luaL_error(L, "addprocess requires (appid: integer, exename: string)");
        lua_Integer value = lua_tointeger(L, 1);
        if (value <= 0 || value > static_cast<lua_Integer>(UINT32_MAX))
            return luaL_error(L, "addprocess: appid out of range");
        std::string name(lua_tostring(L, 2));
        for (char& ch : name)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        ProcessNameAppIdMap[name] = static_cast<AppId_t>(value);
        return 0;
    }

    static int lua_forcedenuvo(lua_State* L) {
        // forcedenuvo(appid) — bypass ProtectionScan for games where the heuristic fails.
        if (lua_gettop(L) < 1 || !lua_isinteger(L, 1))
            return luaL_error(L, "forcedenuvo requires (appid: integer)");
        lua_Integer value = lua_tointeger(L, 1);
        if (value <= 0 || value > static_cast<lua_Integer>(UINT32_MAX))
            return luaL_error(L, "forcedenuvo: appid out of range");
        ForcedDenuvoSet.insert(static_cast<AppId_t>(value));
        return 0;
    }

    static int lua_seteticketurl(lua_State* L) {
        // seteticketurl("http://your-backend/eticket")
        // Endpoint that mints fresh nonce-bound encrypted app tickets for
        // strict Denuvo titles. Set to "" (or omit the call) to disable.
        if (lua_gettop(L) < 1 || !lua_isstring(L, 1))
            return luaL_error(L, "seteticketurl requires (url: string)");
        EticketUrl = std::string(lua_tostring(L, 1));
        return 0;
    }

    static int lua_pinApp(lua_State* L) {
        // pinApp(integer)
        int argc = lua_gettop(L);
        // Validate argument count and required argument types.
        if (argc == 0) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }

        // Read the first argument as appid.
        lua_Integer value = lua_tointeger(L, 1);
        // Ensure the value fits into uint32_t range.
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t AppId = (uint32_t)value;

        PinnedApps.insert(AppId);

        return 0;
    }

    static int lua_setManifestid(lua_State* L)
    {
        // setManifestid(depotId, gid_string [, size])
        // size is always forced to 0 to prevent incorrect size from breaking Steam.
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setManifestid: need depotId, gid");

        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setManifestid: depotId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setManifestid: gid must be decimal string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setManifestid: depotId out of range");

        uint64_t depotId = (uint64_t)(uint32_t)val;
        const char* gidStr = lua_tostring(L, 2);

        uint64_t gid = 0;
        if (!ParseUInt64Decimal(gidStr, &gid))
            return luaL_error(L, "setManifestid: gid must be all digits");

        ManifestOverride override{ gid, 0 };
        if (!g_currentFile.empty()) {
            g_fileManifestOverrides[g_currentFile][depotId] = override;
            RebuildManifestOverride(depotId);
        } else {
            SetActiveManifestOverride(depotId, override);
        }
        return 0;
    }

    // ── Lua: setAppTicket / setETicket ──────────────────────────
    static int lua_setAppticket(lua_State* L) {
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setAppTicket: need appId and hex string");
        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setAppTicket: appId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setAppTicket: ticket must be hex string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setAppTicket: appId out of range");
        AppId_t appId = static_cast<uint32_t>(val);

        size_t hexLen;
        const char* hex = lua_tolstring(L, 2, &hexLen);

        std::vector<uint8_t> binary;
        binary.reserve((hexLen + 1) / 2);
        for (size_t i = 0; i < hexLen; i += 2) {
            char byteStr[3] = {
                hex[i],
                i + 1 < hexLen ? hex[i + 1] : '0',
                '\0'
            };
            binary.push_back(ParseHexByte(byteStr));
        }

        if (!AppTicket::WriteAppOwnershipTicket(appId, binary))
            return luaL_error(L, "setAppTicket: failed to write credential store");

        return 0;
    }

    static int lua_setEticket(lua_State* L) {
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setETicket: need appId and hex string");
        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setETicket: appId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setETicket: ticket must be hex string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setETicket: appId out of range");
        AppId_t appId = static_cast<uint32_t>(val);

        size_t hexLen;
        const char* hex = lua_tolstring(L, 2, &hexLen);

        std::vector<uint8_t> binary;
        binary.reserve((hexLen + 1) / 2);
        for (size_t i = 0; i < hexLen; i += 2) {
            char byteStr[3] = {
                hex[i],
                i + 1 < hexLen ? hex[i + 1] : '0',
                '\0'
            };
            binary.push_back(ParseHexByte(byteStr));
        }

        if (!AppTicket::WriteEncryptedTicket(appId, binary))
            return luaL_error(L, "setETicket: failed to write credential store");

        return 0;
    }

    
    // ── Lua: setStat ────────────────────────────────────────────
    static int lua_setStat(lua_State* L) {
        // setStat(appid, "steamid")
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setStat: need appId and steamId string");
        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setStat: appId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setStat: steamId must be string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setStat: appId out of range");
        AppId_t appId = static_cast<uint32_t>(val);

        const char* sidStr = lua_tostring(L, 2);
        uint64_t steamId = 0;
        if (!ParseUInt64Decimal(sidStr, &steamId))
            return luaL_error(L, "setStat: steamId must be all digits");

        StatSteamIdSet[appId] = steamId;
        return 0;
    }

    // ── init / cleanup ───────────────────────────────────────────
    static bool Initialize() {
        if (g_lua_state)
            return true;
        g_lua_state = luaL_newstate();
        if (!g_lua_state)
            return false;
        // Load standard Lua libraries.
        luaL_openlibs(g_lua_state);

        // Set up case-insensitive global lookup via __index on _G's metatable.
        lua_getglobal(g_lua_state, "_G");
        if (!lua_getmetatable(g_lua_state, -1)) {
            lua_newtable(g_lua_state);
        }
        lua_pushcfunction(g_lua_state, case_insensitive_global_index);
        lua_setfield(g_lua_state, -2, "__index");
        lua_setmetatable(g_lua_state, -2);
        lua_pop(g_lua_state, 1);  // pop _G

        // Register custom helper functions for scripts.
        // All functions use register_func() so any case variant works
        // (e.g. setAppTICKET, addAppId, SETManifestid, etc.).
        register_func(g_lua_state, "addappid", lua_addappid);
        register_func(g_lua_state, "addtoken", lua_addtoken);
        register_func(g_lua_state, "setlegacycdkey", lua_setlegacycdkey);
        register_func(g_lua_state, "addprocess", lua_addprocess);
        register_func(g_lua_state, "forcedenuvo", lua_forcedenuvo);
        register_func(g_lua_state, "seteticketurl", lua_seteticketurl);
        // we don't need it?
        // register_func(g_lua_state, "pinapp", lua_pinApp);
        register_func(g_lua_state, "setmanifestid", lua_setManifestid);
        register_func(g_lua_state, "http_get", lua_http_get);
        register_func(g_lua_state, "http_post", lua_http_post);
        register_func(g_lua_state, "setappticket", lua_setAppticket);
        register_func(g_lua_state, "seteticket", lua_setEticket);
        register_func(g_lua_state, "setstat", lua_setStat);
        return true;
    }

    static void Cleanup() {
        if (g_lua_state) {
            lua_close(g_lua_state);
            g_lua_state = nullptr;
        }
        g_hasManifestCodeFunc = false;
    }

    // ── public query API ─────────────────────────────────────────
    AppId_t GetAppIdForProcess(const std::string& imageName) {
        std::string lower(imageName);
        for (char& ch : lower)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const auto it = ProcessNameAppIdMap.find(lower);
        return it != ProcessNameAppIdMap.end() ? it->second : k_uAppIdInvalid;
    }

    bool IsForcedDenuvo(AppId_t appId) {
        return ForcedDenuvoSet.count(appId) > 0;
    }

    const std::string& GetEticketUrl() {
        return EticketUrl;
    }

    bool HasDepot(AppId_t DepotId,bool excludeOwned) {
        return DepotKeySet.count(DepotId) && (!excludeOwned || !IsOwned(DepotId));
    }

    bool IsOwned(AppId_t AppId) {
        return OwnedAppIdSet.count(AppId);
    }

    void MarkOwned(AppId_t AppId) {
        if(!OwnedAppIdSet.count(AppId)) {
            LOG_PACKAGE_INFO("Marking app {} as owned", AppId);
            OwnedAppIdSet.insert(AppId);
        }
    }

    std::vector<AppId_t> GetAllDepotIds() {
        std::vector<AppId_t> DepotIds;
        for (const auto& pair : DepotKeySet) {
            DepotIds.push_back(pair.first);
        }
        return DepotIds;
    }

    std::vector<uint8> GetDecryptionKey(AppId_t DepotId) {
        std::vector<uint8> keyBytes;
        if (DepotKeySet.count(DepotId)) {
            const std::string& keyStr = DepotKeySet[DepotId];
            // Convert hex string to byte vector.
            for (size_t i = 0; i < keyStr.length(); i += 2) {
                keyBytes.push_back(ParseHexByte(std::string_view(keyStr).substr(i, 2)));
            }
        }
        return keyBytes;
    }

    uint64_t GetAccessToken(AppId_t AppId) {
        if (AccessTokenSet.count(AppId)) {
            return AccessTokenSet[AppId];
        }
        return 0;
    }

    void SetAccessToken(AppId_t AppId, uint64_t token) {
        if (token) {
            AccessTokenSet[AppId] = token;
        }
    }

    std::optional<std::string> GetLegacyCDKey(AppId_t AppId) {
        auto it = LegacyCDKeySet.find(AppId);
        if (it != LegacyCDKeySet.end())
            return it->second;
        return std::nullopt;
    }

    bool pinApp(AppId_t AppId) {
        return PinnedApps.count(AppId);
    }

    uint64_t GetStatSteamId(AppId_t AppId) {
        if (StatSteamIdSet.count(AppId))
            return StatSteamIdSet[AppId];
        uint64_t apiSteamId = 0;
        if (StatsClient::FetchStatSteamId(AppId, &apiSteamId))
            return apiSteamId;
        return kDefaultStatSteamId;
    }

    uint32_t GetPurchaseTime(AppId_t AppId) {
        auto it = g_purchaseTime.find(AppId);
        return it != g_purchaseTime.end() ? it->second : 0;
    }

    const std::unordered_map<uint64_t, ManifestOverride>& GetManifestOverrides() {
      return ManifestOverrides;
    }

    bool HasManifestCodeFunc() {
        return g_hasManifestCodeFunc;
    }

    bool CallManifestFetchCode(uint64_t gid, uint64_t* outCode) {
        if (!g_hasManifestCodeFunc || !g_lua_state)
            return false;

        lua_getglobal(g_lua_state, "fetch_manifest_code");
        lua_pushinteger(g_lua_state, static_cast<lua_Integer>(gid));

        if (lua_pcall(g_lua_state, 1, 1, 0) != LUA_OK) {
            LOG_MANIFEST_WARN("fetch_manifest_code({}) error: {}", gid,
                             lua_tostring(g_lua_state, -1));
            lua_pop(g_lua_state, 1);
            return false;
        }

        // nil → fallback to config url (normal, not an error)
        if (lua_isnil(g_lua_state, -1)) {
            LOG_MANIFEST_WARN("fetch_manifest_code({}) returned nil", gid);
            lua_pop(g_lua_state, 1);
            return false;
        }

        // The function must return a digit string (uint64 as decimal).
        // tonumber() loses precision for values > 2^53; string is safe.
        if (lua_isinteger(g_lua_state, -1)) {
            *outCode = static_cast<uint64_t>(lua_tointeger(g_lua_state, -1));
        } else if (lua_isstring(g_lua_state, -1)) {
            const char* s = lua_tostring(g_lua_state, -1);
            uint64_t parsed = 0;
            if (!ParseUInt64Decimal(s, &parsed)) {
                LOG_MANIFEST_WARN("fetch_manifest_code({}) returned invalid numeric string '{}'",
                                 gid, s);
                lua_pop(g_lua_state, 1);
                return false;
            }
            *outCode = parsed;
        } else {
            LOG_MANIFEST_WARN("fetch_manifest_code({}) unexpected type (expected digit-string)",
                             gid);
            lua_pop(g_lua_state, 1);
            return false;
        }

        LOG_MANIFEST_INFO("fetch_manifest_code({}) = {}", gid, *outCode);
        lua_pop(g_lua_state, 1);
        return true;
    }

    bool HasManifestCodeFuncEx() {
        return g_hasManifestCodeFuncEx;
    }

    bool CallManifestFetchCodeEx(uint64_t app_id, uint64_t depot_id, uint64_t gid, uint64_t* outCode) {
        if (!g_hasManifestCodeFuncEx || !g_lua_state)
            return false;

        lua_getglobal(g_lua_state, "fetch_manifest_code_ex");
        lua_pushinteger(g_lua_state, static_cast<lua_Integer>(app_id));
        lua_pushinteger(g_lua_state, static_cast<lua_Integer>(depot_id));
        lua_pushinteger(g_lua_state, static_cast<lua_Integer>(gid));

        if (lua_pcall(g_lua_state, 3, 1, 0) != LUA_OK) {
            LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) error: {}", app_id, depot_id, gid,
                             lua_tostring(g_lua_state, -1));
            lua_pop(g_lua_state, 1);
            return false;
        }

        if (lua_isnil(g_lua_state, -1)) {
            LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) returned nil", app_id, depot_id, gid);
            lua_pop(g_lua_state, 1);
            return false;
        }

        if (lua_isinteger(g_lua_state, -1)) {
            *outCode = static_cast<uint64_t>(lua_tointeger(g_lua_state, -1));
        } else if (lua_isstring(g_lua_state, -1)) {
            const char* s = lua_tostring(g_lua_state, -1);
            uint64_t parsed = 0;
            if (!ParseUInt64Decimal(s, &parsed)) {
                LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) returned invalid numeric string '{}'",
                                 app_id, depot_id, gid, s);
                lua_pop(g_lua_state, 1);
                return false;
            }
            *outCode = parsed;
        } else {
            LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) unexpected type (expected digit-string)",
                             app_id, depot_id, gid);
            lua_pop(g_lua_state, 1);
            return false;
        }

        LOG_MANIFEST_INFO("fetch_manifest_code_ex({}, {}, {}) = {}", app_id, depot_id, gid, *outCode);
        lua_pop(g_lua_state, 1);
        return true;
    }

    // ── per-file unload ────────────────────────────────────────
    void UnloadFile(const std::string& filePath) {
        auto depotsIt = g_fileDepots.find(filePath);
        auto manifestIt = g_fileManifestOverrides.find(filePath);
        if (depotsIt == g_fileDepots.end() && manifestIt == g_fileManifestOverrides.end()) return;

        if (depotsIt != g_fileDepots.end()) {
            for (AppId_t id : depotsIt->second) {
                LOG_PACKAGE_DEBUG("UnloadFile:Ref count for AppId {} is {}", id, g_depotRefCount[id]);
                if (--g_depotRefCount[id] == 0) {
                    g_depotRefCount.erase(id);
                    DepotKeySet.erase(id);
                    g_purchaseTime.erase(id);
                    g_pendingRemovals.push_back(id);
                }
            }

            LOG_PACKAGE_INFO("UnloadFile: removed {} depots from {}", depotsIt->second.size(), filePath);
            g_fileDepots.erase(depotsIt);
        }

        if (manifestIt != g_fileManifestOverrides.end()) {
            std::vector<uint64_t> affectedDepots;
            affectedDepots.reserve(manifestIt->second.size());
            for (const auto& [depotId, _] : manifestIt->second) {
                affectedDepots.push_back(depotId);
            }

            g_fileManifestOverrides.erase(manifestIt);
            for (uint64_t depotId : affectedDepots) {
                RebuildManifestOverride(depotId);
            }

            LOG_MANIFEST_INFO("UnloadFile: removed {} manifest override(s) from {}", affectedDepots.size(), filePath);
        }

        g_fileParseSequence.erase(filePath);
        g_fileMtime.erase(filePath);
    }

    std::vector<AppId_t> TakePendingRemovals() {
        std::vector<AppId_t> result;
        result.swap(g_pendingRemovals);
        return result;
    }

    std::vector<AppId_t> TakePendingAdditions() {
        std::vector<AppId_t> result;
        result.swap(g_pendingAdditions);
        return result;
    }

    // ── NP Encrypted Configuration Decryption ───────────────────
    namespace {

    static constexpr uint32_t K256[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    static inline uint32_t rotr(uint32_t x, uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }

    static std::string Sha256PureHex(const void* dataPtr, size_t len) {
        const uint8_t* data = static_cast<const uint8_t*>(dataPtr);
        uint64_t bit_length = static_cast<uint64_t>(len) * 8;

        std::vector<uint8_t> msg;
        msg.reserve(len + 64);
        if (len > 0 && data) {
            msg.assign(data, data + len);
        }
        msg.push_back(0x80);
        while ((msg.size() % 64) != 56) {
            msg.push_back(0x00);
        }
        for (int i = 7; i >= 0; --i) {
            msg.push_back(static_cast<uint8_t>((bit_length >> (i * 8)) & 0xff));
        }

        uint32_t h[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
        };

        for (size_t offset = 0; offset < msg.size(); offset += 64) {
            const uint8_t* chunk = msg.data() + offset;
            uint32_t w[64];
            for (int i = 0; i < 16; ++i) {
                w[i] = (static_cast<uint32_t>(chunk[i * 4]) << 24) |
                       (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(chunk[i * 4 + 2]) << 8) |
                       (static_cast<uint32_t>(chunk[i * 4 + 3]));
            }
            for (int i = 16; i < 64; ++i) {
                uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }

            uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            uint32_t e = h[4], f = h[5], g = h[6], z = h[7];

            for (int i = 0; i < 64; ++i) {
                uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                uint32_t ch = (e & f) ^ ((~e) & g);
                uint32_t temp1 = z + s1 + ch + K256[i] + w[i];

                uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t temp2 = s0 + maj;

                z = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }

            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
            h[4] += e; h[5] += f; h[6] += g; h[7] += z;
        }

        char hexBuf[65];
        snprintf(hexBuf, sizeof(hexBuf),
                 "%08x%08x%08x%08x%08x%08x%08x%08x",
                 h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
        return std::string(hexBuf);
    }

    static inline uint8_t HexCharToNibble(char c) {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        return 0;
    }

    static std::string BytesToHex(const uint8_t* data, size_t len) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string s;
        s.reserve(len * 2);
        for (size_t i = 0; i < len; ++i) {
            s += kHex[(data[i] >> 4) & 0x0F];
            s += kHex[data[i] & 0x0F];
        }
        return s;
    }

    static std::vector<uint8_t> GenerateKeystream(const std::string& keyHex, const uint8_t* nonce, size_t nonceLen, size_t length) {
        std::string nonceHex = BytesToHex(nonce, nonceLen);
        std::vector<uint8_t> stream;
        stream.reserve(length + 32);
        uint32_t counter = 0;
        char counterBuf[16];
        while (stream.size() < length) {
            snprintf(counterBuf, sizeof(counterBuf), "%08x", counter);
            std::string blockSeed = keyHex + ":" + nonceHex + ":" + counterBuf;
            std::string blockHashHex = Sha256PureHex(blockSeed.data(), blockSeed.size());
            for (size_t i = 0; i + 1 < blockHashHex.size(); i += 2) {
                uint8_t b = (HexCharToNibble(blockHashHex[i]) << 4) | HexCharToNibble(blockHashHex[i+1]);
                stream.push_back(b);
            }
            ++counter;
        }
        stream.resize(length);
        return stream;
    }

    static std::string GetSavedAccountFromRegistry() {
#ifdef _WIN32
        HKEY hKey = nullptr;
        LSTATUS status = RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\Local Settings\\Software\\.dev2x",
            0,
            KEY_QUERY_VALUE,
            &hKey);
        if (status != ERROR_SUCCESS) return "";

        DWORD type = 0;
        DWORD size = 0;
        status = RegQueryValueExW(hKey, L"LocalAccount", nullptr, &type, nullptr, &size);
        if (status != ERROR_SUCCESS || size == 0) {
            RegCloseKey(hKey);
            return "";
        }

        std::vector<BYTE> encryptedData(size);
        status = RegQueryValueExW(hKey, L"LocalAccount", nullptr, &type, encryptedData.data(), &size);
        RegCloseKey(hKey);
        if (status != ERROR_SUCCESS) return "";

        DATA_BLOB inBlob;
        inBlob.cbData = static_cast<DWORD>(encryptedData.size());
        inBlob.pbData = encryptedData.data();

        static const char kEntropy[] = "niuplayer.local.credentials.v1";
        DATA_BLOB entropyBlob;
        entropyBlob.cbData = static_cast<DWORD>(sizeof(kEntropy) - 1);
        entropyBlob.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(kEntropy));

        DATA_BLOB outBlob = {0, nullptr};

        HMODULE hCrypt = LoadLibraryW(L"crypt32.dll");
        if (!hCrypt) return "";

        using CryptUnprotectData_t = BOOL (WINAPI*)(
            DATA_BLOB*, LPWSTR*, DATA_BLOB*, PVOID,
            CRYPTPROTECT_PROMPTSTRUCT*, DWORD, DATA_BLOB*);
        auto pCryptUnprotectData = reinterpret_cast<CryptUnprotectData_t>(
            GetProcAddress(hCrypt, "CryptUnprotectData"));

        std::string account;
        if (pCryptUnprotectData && pCryptUnprotectData(&inBlob, nullptr, &entropyBlob, nullptr, nullptr, 0, &outBlob)) {
            if (outBlob.pbData && outBlob.cbData > 0) {
                account.assign(reinterpret_cast<const char*>(outBlob.pbData), outBlob.cbData);
            }
            if (outBlob.pbData) {
                LocalFree(outBlob.pbData);
            }
        }
        FreeLibrary(hCrypt);
        return account;
#else
        return "";
#endif
    }

    static std::string NormalizeUsername(std::string username) {
        while (!username.empty() && (username.front() == ' ' || username.front() == '\t' || username.front() == '\r' || username.front() == '\n')) {
            username.erase(username.begin());
        }
        while (!username.empty() && (username.back() == ' ' || username.back() == '\t' || username.back() == '\r' || username.back() == '\n')) {
            username.pop_back();
        }

        if (username.empty() || username.rfind("anonymous-", 0) == 0) {
#ifdef _WIN32
            char envBuf[256] = {0};
            DWORD len = GetEnvironmentVariableA("NIUPLAY_USERNAME", envBuf, sizeof(envBuf));
            if (len > 0 && len < sizeof(envBuf)) {
                username = envBuf;
            } else {
                username = "-1@qq.com";
            }
#else
            username = "-1@qq.com";
#endif
        }

        bool allDigits = !username.empty();
        for (char c : username) {
            if (!isdigit(static_cast<unsigned char>(c))) {
                allDigits = false;
                break;
            }
        }
        if (allDigits) {
            username += "@qq.com";
        }
        return username;
    }

    static std::vector<std::string> GetCandidateKeys() {
        std::vector<std::string> keys;
        std::string rawAccount = GetSavedAccountFromRegistry();
        if (!rawAccount.empty()) {
            std::string norm = NormalizeUsername(rawAccount);
            keys.push_back(Sha256PureHex(norm.data(), norm.size()));
            if (norm != rawAccount) {
                keys.push_back(Sha256PureHex(rawAccount.data(), rawAccount.size()));
            }
        }

#ifdef _WIN32
        char envBuf[256] = {0};
        DWORD len = GetEnvironmentVariableA("NIUPLAY_USERNAME", envBuf, sizeof(envBuf));
        if (len > 0 && len < sizeof(envBuf)) {
            std::string envUser = NormalizeUsername(std::string(envBuf));
            keys.push_back(Sha256PureHex(envUser.data(), envUser.size()));
        }
#endif

        std::string fallback = "-1@qq.com";
        keys.push_back(Sha256PureHex(fallback.data(), fallback.size()));

        std::vector<std::string> uniqueKeys;
        for (const auto& k : keys) {
            if (std::find(uniqueKeys.begin(), uniqueKeys.end(), k) == uniqueKeys.end()) {
                uniqueKeys.push_back(k);
            }
        }
        return uniqueKeys;
    }

    static bool DecryptNpData(const uint8_t* data, size_t size, std::string& outPlaintext) {
        if (size < 4 + 16 + 32) return false;
        if (data[0] != 'N' || data[1] != 'P' || data[2] != '0' || data[3] != '1') return false;

        const uint8_t* nonce = data + 4;
        const uint8_t* expectedMac = data + 20;
        const uint8_t* ciphertext = data + 52;
        size_t cipherLen = size - 52;

        std::string nonceHex = BytesToHex(nonce, 16);
        std::string cipherHex = BytesToHex(ciphertext, cipherLen);
        std::string expectedMacHex = BytesToHex(expectedMac, 32);

        auto candidateKeys = GetCandidateKeys();
        for (const auto& keyHex : candidateKeys) {
            std::string macInput = keyHex + nonceHex + cipherHex;
            std::string calculatedMacHex = Sha256PureHex(macInput.data(), macInput.size());
            if (calculatedMacHex == expectedMacHex) {
                auto keystream = GenerateKeystream(keyHex, nonce, 16, cipherLen);
                std::vector<uint8_t> plain(cipherLen);
                for (size_t i = 0; i < cipherLen; ++i) {
                    plain[i] = ciphertext[i] ^ keystream[i];
                }
                outPlaintext.assign(reinterpret_cast<const char*>(plain.data()), plain.size());
                return true;
            }
        }
        return false;
    }

    } // namespace

    static std::vector<std::string> CollectLuaFiles(const std::string& directory) {
        std::vector<std::string> files;

        std::error_code ec;
        if (!std::filesystem::exists(directory, ec))
            std::filesystem::create_directories(directory, ec);
        if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec))
            return files;

        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext != ".lua" && ext != ".np") continue;
            files.push_back(entry.path().string());
        }
        return files;
    }

    // ── single-file parser ──────────────────────────────────────
    void ParseFile(const std::string& filePath) {
        if (!Initialize()) return;

        // Remove old entries from this file before re-parsing.
        UnloadFile(filePath);
        g_currentFile = filePath;

        std::filesystem::path path(filePath);
        std::ifstream file;
        std::istringstream npStream;
        bool isNp = false;
        {
            auto ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext == ".np") {
                isNp = true;
            }
        }

        if (isNp) {
            std::ifstream npFile(path, std::ios::binary);
            if (!npFile) {
                LOG_WARN("ParseFile: failed to open {}", path.filename().string());
                g_currentFile.clear();
                return;
            }
            std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(npFile)), std::istreambuf_iterator<char>());
            std::string decryptedContent;
            if (!DecryptNpData(buffer.data(), buffer.size(), decryptedContent)) {
                LOG_WARN("ParseFile: failed to decrypt {}", path.filename().string());
                g_currentFile.clear();
                return;
            }
            npStream.str(std::move(decryptedContent));
        } else {
            file.open(path);
            if (!file) {
                LOG_WARN("ParseFile: failed to open {}", path.filename().string());
                g_currentFile.clear();
                return;
            }
        }

        g_fileParseSequence[filePath] = ++g_nextFileParseSequence;
        
        // Capture the file's last-modified time (unix epoch, seconds) so
        // lua_addappid can stamp it onto every appId this file contributes.
        // Portable conversion that does not require C++20 clock_cast.
        {
            std::error_code ec;
            auto ftime = std::filesystem::last_write_time(path, ec);
            uint32_t mtime = 0;
            if (!ec) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
                mtime = static_cast<uint32_t>(std::chrono::system_clock::to_time_t(sctp));
            }
            g_fileMtime[filePath] = mtime;
        }

        std::istream& in = isNp ? static_cast<std::istream&>(npStream) : static_cast<std::istream&>(file);

        std::string chunk, line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            if (!chunk.empty()) chunk += '\n';
            chunk += line;

            lua_settop(g_lua_state, 0);
            int rc = luaL_loadstring(g_lua_state, chunk.c_str());
            if (rc == LUA_OK) {
                if (lua_pcall(g_lua_state, 0, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(g_lua_state, -1);
                    LOG_WARN("{}:{}: {}", path.filename().string(), lineNo,
                             err ? err : "unknown");
                }
                chunk.clear();
            } else if (rc == LUA_ERRSYNTAX) {
                lua_pop(g_lua_state, 1);
            } else {
                const char* err = lua_tostring(g_lua_state, -1);
                LOG_WARN("{}:{}: {}", path.filename().string(), lineNo, err ? err : "unknown");
                lua_pop(g_lua_state, 1);
                chunk.clear();
            }
        }
        if (!chunk.empty()) {
            LOG_WARN("{}: incomplete statement at end of file", path.filename().string());
        }

        // Check for manifest code functions after parsing.
        g_hasManifestCodeFunc = false;
        lua_getglobal(g_lua_state, "fetch_manifest_code");
        if (lua_isfunction(g_lua_state, -1)) {
            g_hasManifestCodeFunc = true;
            LOG_INFO("manifest.lua: fetch_manifest_code found");
        }
        lua_pop(g_lua_state, 1);

        g_hasManifestCodeFuncEx = false;
        lua_getglobal(g_lua_state, "fetch_manifest_code_ex");
        if (lua_isfunction(g_lua_state, -1)) {
            g_hasManifestCodeFuncEx = true;
            LOG_INFO("manifest.lua: fetch_manifest_code_ex found");
        }
        lua_pop(g_lua_state, 1);

        g_currentFile.clear();
    }

    // ── directory scanner ────────────────────────────────────────
    std::vector<std::string> MergeWatchDirs(const std::vector<std::string>& configured,
                                            const std::string& defaultDir) {
        namespace fs = std::filesystem;

        // Canonical, case-folded key for a directory so relative and absolute spellings
        // of the same location compare equal. weakly_canonical resolves against the
        // current working directory (Steam's install root at runtime), matching how the
        // paths are later opened.
        auto key = [](const std::string& p) -> std::string {
            std::error_code ec;
            fs::path c = fs::weakly_canonical(p, ec);
            if (ec || c.empty()) c = fs::absolute(fs::path(p), ec);
            if (ec || c.empty()) c = fs::path(p).lexically_normal();
            std::string s = c.string();
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return s;
        };

        std::vector<std::string> out;
        std::vector<std::string> seen;
        auto add = [&](const std::string& dir) {
            if (dir.empty()) return;
            const std::string k = key(dir);
            if (std::find(seen.begin(), seen.end(), k) != seen.end()) return;
            seen.push_back(k);
            out.push_back(dir);
        };

        for (const auto& d : configured) add(d);
        add(defaultDir);   // appended only if it isn't already covered above
        return out;
    }

    void ParseDirectory(const std::string& directory) {
        if (!Initialize()) return;

        for (const auto& filePath : CollectLuaFiles(directory)) {
            ParseFile(filePath);
        }

        // Initial parse — discard pending additions so NotifyLicenseChanged
        // only sees changes that happen after startup.
        g_pendingAdditions.clear();
    }

    void ReloadDirectories(const std::vector<std::string>& directories, bool clearPendingAdditions) {
        if (!Initialize()) return;

        std::unordered_set<std::string> activeFiles;
        std::vector<std::string> orderedFiles;

        for (const auto& directory : directories) {
            for (const auto& filePath : CollectLuaFiles(directory)) {
                if (activeFiles.insert(filePath).second) {
                    orderedFiles.push_back(filePath);
                }
            }
        }

        std::unordered_set<std::string> trackedSet;
        std::vector<std::string> trackedFiles;
        auto rememberTracked = [&](const std::string& filePath) {
            if (trackedSet.insert(filePath).second) {
                trackedFiles.push_back(filePath);
            }
        };

        for (const auto& [filePath, _] : g_fileDepots) {
            rememberTracked(filePath);
        }
        for (const auto& [filePath, _] : g_fileManifestOverrides) {
            rememberTracked(filePath);
        }

        for (const auto& filePath : trackedFiles) {
            if (!activeFiles.contains(filePath)) {
                UnloadFile(filePath);
            }
        }

        for (const auto& filePath : orderedFiles) {
            ParseFile(filePath);
        }

        if (clearPendingAdditions) {
            g_pendingAdditions.clear();
        }
    }

}
