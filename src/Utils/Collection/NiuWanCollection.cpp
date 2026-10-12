#include "NiuWanCollection.h"
#include "Utils/Logging/Log.h"

#include <windows.h>
#include <mutex>
#include <unordered_set>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <format>
#include <algorithm>

// 使用内嵌的 nlohmann::json
#include "json.hpp"

using json = nlohmann::json;

namespace NiuWanCollection {

    namespace {
        std::mutex g_mutex;
        std::unordered_set<AppId_t> g_npAppIds;
        constexpr const char* kTargetCollectionName = "牛玩";

        // 从注册表读取当前活跃的 Steam 32位 AccountID
        uint32_t GetActiveSteamAccountId() {
            HKEY hKey;
            uint32_t accountId = 0;
            if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                DWORD data = 0;
                DWORD cbData = sizeof(data);
                DWORD dwType = REG_DWORD;
                if (RegQueryValueExA(hKey, "ActiveUser", nullptr, &dwType, reinterpret_cast<LPBYTE>(&data), &cbData) == ERROR_SUCCESS) {
                    accountId = static_cast<uint32_t>(data);
                }
                RegCloseKey(hKey);
            }
            return accountId;
        }
    }

    void RegisterNpAppId(AppId_t appId) {
        if (appId == 0) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_npAppIds.insert(appId);
        LOG_INFO("NiuWanCollection: registered np appId {}", appId);
    }

    void UnregisterNpAppId(AppId_t appId) {
        if (appId == 0) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_npAppIds.erase(appId);
        LOG_INFO("NiuWanCollection: unregistered np appId {}", appId);
    }

    bool IsNpAppId(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_npAppIds.contains(appId);
    }

    std::vector<AppId_t> GetNpAppIds() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return std::vector<AppId_t>(g_npAppIds.begin(), g_npAppIds.end());
    }

    void SyncCollection(const char* steamInstallPath) {
        if (!steamInstallPath || !steamInstallPath[0]) return;

        uint32_t accountId = GetActiveSteamAccountId();
        if (accountId == 0) {
            LOG_WARN("NiuWanCollection: no active Steam user detected");
            return;
        }

        namespace fs = std::filesystem;
        fs::path cloudStorageDir = fs::path(steamInstallPath) / "userdata" / std::to_string(accountId) / "config" / "cloudstorage";
        fs::path jsonPath = cloudStorageDir / "cloud-storage-namespace-1.json";

        std::error_code ec;
        if (!fs::exists(cloudStorageDir, ec)) {
            fs::create_directories(cloudStorageDir, ec);
        }

        // 读取当前记录的所有 .np 游戏的 AppId
        std::unordered_set<AppId_t> currentNpIds;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            currentNpIds = g_npAppIds;
        }

        json data = json::array();
        if (fs::exists(jsonPath, ec)) {
            std::ifstream in(jsonPath, std::ios::binary);
            if (in.is_open()) {
                try {
                    in >> data;
                } catch (const std::exception& ex) {
                    LOG_ERROR("NiuWanCollection: failed to parse existing JSON: {}", ex.what());
                    data = json::array();
                }
            }
        }

        if (!data.is_array()) {
            data = json::array();
        }

        // 寻找名称为“牛玩”的分类项
        int foundIndex = -1;
        json foundValObj;
        std::string targetKey;

        uint64_t maxVersionNum = 0;

        for (size_t i = 0; i < data.size(); ++i) {
            if (!data[i].is_array() || data[i].size() < 2) continue;
            std::string key = data[i][0].is_string() ? data[i][0].get<std::string>() : "";
            const auto& itemObj = data[i][1];

            if (itemObj.is_object() && itemObj.contains("version")) {
                if (itemObj["version"].is_string()) {
                    try {
                        uint64_t v = std::stoull(itemObj["version"].get<std::string>());
                        if (v > maxVersionNum) maxVersionNum = v;
                    } catch (...) {}
                }
            }

            if (key.starts_with("user-collections.")) {
                if (itemObj.is_object() && (!itemObj.contains("is_deleted") || !itemObj["is_deleted"].get<bool>())) {
                    if (itemObj.contains("value") && itemObj["value"].is_string()) {
                        try {
                            json vObj = json::parse(itemObj["value"].get<std::string>());
                            if (vObj.is_object() && vObj.value("name", "") == kTargetCollectionName) {
                                foundIndex = static_cast<int>(i);
                                foundValObj = vObj;
                                targetKey = key;
                            }
                        } catch (...) {}
                    }
                }
            }
        }

        auto nowSec = static_cast<uint64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
        uint64_t nextVersion = maxVersionNum + 1;

        // 同步 Steam Web 前端热重载文件（niuwan_appids.json, niuwan.js 以及 index.html 自动注入）
        auto SyncSteamUI = [&](const fs::path& uiDir) {
            std::error_code uiEc;
            if (!fs::exists(uiDir, uiEc)) return;

            // 1. 写入 niuwan_appids.json
            fs::path appidsPath = uiDir / "niuwan_appids.json";
            try {
                json appidsArr = json::array();
                for (AppId_t id : currentNpIds) {
                    appidsArr.push_back(id);
                }
                std::ofstream out(appidsPath, std::ios::binary | std::ios::trunc);
                if (out.is_open()) {
                    out << appidsArr.dump();
                    out.close();
                }
            } catch (...) {}

            // 2. 确保 niuwan.js 存在且最新
            fs::path jsPath = uiDir / "niuwan.js";
            const char* kNiuWanJs = R"JS((function () {
    console.log("[NiuWan] Hot-reload helper initialized.");
    let isSyncing = false;

    async function syncNiuWan() {
        if (isSyncing) return;
        if (!window.collectionStore || !window.collectionStore.collectionsFromStorage) {
            return;
        }

        let targetAppIds;
        try {
            const resp = await fetch('/niuwan_appids.json?t=' + Date.now());
            if (!resp.ok) return;
            targetAppIds = await resp.json();
            if (!Array.isArray(targetAppIds)) return;
        } catch (e) {
            return;
        }

        isSyncing = true;
        try {
            const targetSet = new Set(targetAppIds.map(Number));

            let collections = window.collectionStore.GetUserCollectionsByName("牛玩") || [];
            let col = collections.length > 0 ? collections[0] : null;

            if (!col) {
                if (targetSet.size === 0) return;
                col = window.collectionStore.NewUnsavedCollection("牛玩", undefined, []);
                for (let id of targetSet) {
                    col.m_setAddedManually.add(id);
                }
                col.UpdateAllApps();
                await window.collectionStore.SaveCollection(col);
                console.log("[NiuWan] Created collection with " + targetSet.size + " apps.");
                return;
            }

            let curAdded = col.m_setAddedManually;
            if (!curAdded) return;

            let changed = false;

            // 1. 新增
            for (let id of targetSet) {
                if (!curAdded.has(id)) {
                    curAdded.add(id);
                    if (col.m_setRemovedManually) col.m_setRemovedManually.delete(id);
                    changed = true;
                }
            }

            // 2. 移除
            for (let id of Array.from(curAdded)) {
                if (!targetSet.has(id)) {
                    curAdded.delete(id);
                    changed = true;
                }
            }

            if (changed) {
                col.UpdateAllApps();
                await col.Save();
                console.log("[NiuWan] Updated collection, count: " + curAdded.size);
            }
        } catch (err) {
            console.error("[NiuWan] Error syncing:", err);
        } finally {
            isSyncing = false;
        }
    }

    setInterval(syncNiuWan, 1500);
    setTimeout(syncNiuWan, 1000);
    setTimeout(syncNiuWan, 3000);
})();
)JS";
            try {
                std::ofstream jsOut(jsPath, std::ios::binary | std::ios::trunc);
                if (jsOut.is_open()) {
                    jsOut << kNiuWanJs;
                    jsOut.close();
                }
            } catch (...) {}

            // 3. 检查并注入 index.html
            fs::path indexPath = uiDir / "index.html";
            try {
                if (fs::exists(indexPath, uiEc)) {
                    std::string htmlContent;
                    {
                        std::ifstream in(indexPath, std::ios::binary);
                        if (in.is_open()) {
                            htmlContent.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                        }
                    }
                    if (!htmlContent.empty() && htmlContent.find("niuwan.js") == std::string::npos) {
                        const std::string targetTag = "<script defer=\"defer\" src=\"/library.js\"></script>";
                        size_t pos = htmlContent.find(targetTag);
                        if (pos != std::string::npos) {
                            htmlContent.insert(pos + targetTag.length(), "<script defer=\"defer\" src=\"/niuwan.js\"></script>");
                        } else {
                            size_t headPos = htmlContent.find("</head>");
                            if (headPos != std::string::npos) {
                                htmlContent.insert(headPos, "<script defer=\"defer\" src=\"/niuwan.js\"></script>");
                            }
                        }
                        std::ofstream out(indexPath, std::ios::binary | std::ios::trunc);
                        if (out.is_open()) {
                            out << htmlContent;
                            out.close();
                            LOG_INFO("NiuWanCollection: injected niuwan.js into index.html");
                        }
                    }
                }
            } catch (...) {}
        };

        if (foundIndex >= 0) {
            // 已存在非删除状态的“牛玩”分类：读取原有的 added 列表，增删同步
            std::vector<uint32_t> oldAdded;
            if (foundValObj.contains("added") && foundValObj["added"].is_array()) {
                for (const auto& elem : foundValObj["added"]) {
                    if (elem.is_number_unsigned()) {
                        oldAdded.push_back(elem.get<uint32_t>());
                    }
                }
            }

            // 同步：保证分类中的 np 游戏与当前存在的 np 游戏严格一致
            // 踢出已不在 currentNpIds 中的 np 游戏，新增新载入的 np 游戏
            std::vector<uint32_t> newAdded;
            std::unordered_set<uint32_t> addedSet;

            for (uint32_t id : oldAdded) {
                // 如果当前仍属于 np 游戏集合，则保留
                if (currentNpIds.contains(id)) {
                    if (addedSet.insert(id).second) {
                        newAdded.push_back(id);
                    }
                }
            }

            for (AppId_t npId : currentNpIds) {
                if (addedSet.insert(npId).second) {
                    newAdded.push_back(npId);
                }
            }

            std::sort(newAdded.begin(), newAdded.end());
            std::sort(oldAdded.begin(), oldAdded.end());

            if (newAdded == oldAdded) {
                LOG_INFO("NiuWanCollection: collection '{}' is already up to date ({} items)", kTargetCollectionName, newAdded.size());
                SyncSteamUI(fs::path(steamInstallPath) / "steamui");
                return;
            }

            foundValObj["added"] = newAdded;
            if (!foundValObj.contains("removed") || !foundValObj["removed"].is_array()) {
                foundValObj["removed"] = json::array();
            }

            json& entry = data[foundIndex][1];
            entry["key"] = targetKey;
            entry["timestamp"] = nowSec;
            entry["value"] = foundValObj.dump();
            entry["version"] = std::to_string(nextVersion);
            entry["conflictResolutionMethod"] = "custom";
            entry["strMethodId"] = "union-collections";
            entry.erase("is_deleted");

            LOG_INFO("NiuWanCollection: updated collection '{}' with {} apps", kTargetCollectionName, newAdded.size());
        } else {
            // 不存在未删除的“牛玩”分类：新建
            // 清理可能存在的历史同 key 的 is_deleted 残留项
            std::string colId = "uc-54mb546p"; // 稳定且独立的 ID
            targetKey = std::string("user-collections.") + colId;

            // 过滤掉同名或历史 marked-deleted 的同 key 条目，防止出现两个相同 key
            for (auto it = data.begin(); it != data.end(); ) {
                if (it->is_array() && it->size() >= 1 && (*it)[0] == targetKey) {
                    it = data.erase(it);
                } else {
                    ++it;
                }
            }

            std::vector<uint32_t> newAdded(currentNpIds.begin(), currentNpIds.end());
            std::sort(newAdded.begin(), newAdded.end());

            json newValObj = {
                {"id", colId},
                {"name", kTargetCollectionName},
                {"added", newAdded},
                {"removed", json::array()}
            };

            json entry = {
                {"key", targetKey},
                {"timestamp", nowSec},
                {"value", newValObj.dump()},
                {"version", std::to_string(nextVersion)},
                {"conflictResolutionMethod", "custom"},
                {"strMethodId", "union-collections"}
            };

            data.push_back(json::array({targetKey, entry}));
            LOG_INFO("NiuWanCollection: created collection '{}' with {} apps", kTargetCollectionName, newAdded.size());
        }

        // 写回 cloud-storage-namespace-1.json (单行无换行格式)
        try {
            std::ofstream out(jsonPath, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                out << data.dump();
                out.close();
                LOG_INFO("NiuWanCollection: successfully saved {}", jsonPath.string());
            } else {
                LOG_ERROR("NiuWanCollection: failed to open {} for writing", jsonPath.string());
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("NiuWanCollection: exception while writing {}: {}", jsonPath.string(), ex.what());
        }

        // 更新 namespaces.json 中的版本号，通知 Steam 配置已修改
        fs::path nsPath = cloudStorageDir / "cloud-storage-namespaces.json";
        try {
            json nsData = json::array();
            bool ns1Found = false;
            if (fs::exists(nsPath, ec)) {
                std::ifstream nsIn(nsPath, std::ios::binary);
                if (nsIn.is_open()) {
                    try { nsIn >> nsData; } catch (...) { nsData = json::array(); }
                }
            }
            if (!nsData.is_array()) nsData = json::array();

            for (auto& item : nsData) {
                if (item.is_array() && item.size() >= 2 && item[0] == 1) {
                    item[1] = std::to_string(nextVersion);
                    ns1Found = true;
                    break;
                }
            }
            if (!ns1Found) {
                nsData.push_back(json::array({1, std::to_string(nextVersion)}));
            }

            std::ofstream nsOut(nsPath, std::ios::binary | std::ios::trunc);
            if (nsOut.is_open()) {
                nsOut << nsData.dump();
                nsOut.close();
            }
        } catch (...) {}

        // 同步 Steam Web 前端热重载文件
        SyncSteamUI(fs::path(steamInstallPath) / "steamui");
    }
}
