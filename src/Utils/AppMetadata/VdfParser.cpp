#include "VdfParser.h"
#include "dllmain.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace VdfParser {

namespace {

    constexpr uint32_t kAppInfoMagicV29 = 0x07564429;
    constexpr uint32_t kAppRecordHeaderSize = 60; // infoState(4)+lastUpdated(4)+token(8)+sha1(20)+changeNumber(4)+binarySha1(20)

    enum EKVType : uint8_t {
        KV_TYPE_NONE = 0,
        KV_TYPE_STRING = 1,
        KV_TYPE_INT = 2,
        KV_TYPE_FLOAT = 3,
        KV_TYPE_PTR = 4,
        KV_TYPE_WSTRING = 5,
        KV_TYPE_COLOR = 6,
        KV_TYPE_UINT64 = 7,
        KV_TYPE_END = 8,
        KV_TYPE_INT64 = 10,
    };

    struct KVNode {
        std::string name;
        uint8_t type = KV_TYPE_NONE;
        std::string strVal;
        int32_t intVal = 0;
        uint64_t uint64Val = 0;
        std::vector<KVNode> children;

        const KVNode* Find(std::string_view path) const {
            if (path.empty()) return this;
            size_t slash = path.find('/');
            std::string_view head = (slash == std::string_view::npos) ? path : path.substr(0, slash);
            std::string_view tail = (slash == std::string_view::npos) ? std::string_view{} : path.substr(slash + 1);

            for (const auto& child : children) {
                if (_stricmp(child.name.c_str(), std::string(head).c_str()) == 0) {
                    if (tail.empty()) return &child;
                    return child.Find(tail);
                }
            }
            return nullptr;
        }

        uint64_t AsUInt64() const {
            if (type == KV_TYPE_UINT64 || type == KV_TYPE_INT64) return uint64Val;
            if (type == KV_TYPE_INT) return static_cast<uint64_t>(intVal);
            if (!strVal.empty()) {
                uint64_t val = 0;
                auto [ptr, ec] = std::from_chars(strVal.data(), strVal.data() + strVal.size(), val);
                if (ec == std::errc{}) return val;
            }
            return 0;
        }

        uint32_t AsUInt32() const {
            return static_cast<uint32_t>(AsUInt64());
        }
    };

    class BufferReader {
    public:
        BufferReader(const uint8_t* data, size_t size) : m_data(data), m_size(size), m_pos(0) {}

        bool HasBytes(size_t n) const { return m_pos + n <= m_size; }

        uint8_t ReadUInt8() {
            if (!HasBytes(1)) return 0;
            return m_data[m_pos++];
        }

        int32_t ReadInt32() {
            if (!HasBytes(4)) return 0;
            int32_t val = 0;
            memcpy(&val, m_data + m_pos, 4);
            m_pos += 4;
            return val;
        }

        uint32_t ReadUInt32() {
            return static_cast<uint32_t>(ReadInt32());
        }

        uint64_t ReadUInt64() {
            if (!HasBytes(8)) return 0;
            uint64_t val = 0;
            memcpy(&val, m_data + m_pos, 8);
            m_pos += 8;
            return val;
        }

        float ReadFloat() {
            if (!HasBytes(4)) return 0.0f;
            float val = 0.0f;
            memcpy(&val, m_data + m_pos, 4);
            m_pos += 4;
            return val;
        }

        std::string ReadCString() {
            size_t start = m_pos;
            while (m_pos < m_size && m_data[m_pos] != 0) {
                m_pos++;
            }
            std::string res(reinterpret_cast<const char*>(m_data + start), m_pos - start);
            if (m_pos < m_size && m_data[m_pos] == 0) m_pos++; // skip \0
            return res;
        }

        void SkipWString() {
            while (m_pos + 1 < m_size) {
                if (m_data[m_pos] == 0 && m_data[m_pos + 1] == 0) {
                    m_pos += 2;
                    break;
                }
                m_pos += 2;
            }
        }

    private:
        const uint8_t* m_data;
        size_t m_size;
        size_t m_pos;
    };

    KVNode ParseKVNode(BufferReader& reader, const std::vector<std::string>& strings) {
        KVNode node;
        while (reader.HasBytes(1)) {
            uint8_t type = reader.ReadUInt8();
            if (type == KV_TYPE_END) {
                break;
            }

            int32_t keyIdx = reader.ReadInt32();
            std::string keyName;
            if (keyIdx >= 0 && keyIdx < static_cast<int32_t>(strings.size())) {
                keyName = strings[keyIdx];
            } else {
                keyName = std::format("key_{}", keyIdx);
            }

            KVNode child;
            child.name = keyName;
            child.type = type;

            switch (type) {
            case KV_TYPE_NONE: // Subkey
                child.children = ParseKVNode(reader, strings).children;
                break;
            case KV_TYPE_STRING:
                child.strVal = reader.ReadCString();
                break;
            case KV_TYPE_INT:
                child.intVal = reader.ReadInt32();
                break;
            case KV_TYPE_FLOAT:
                reader.ReadFloat();
                break;
            case KV_TYPE_PTR:
                reader.ReadUInt32();
                break;
            case KV_TYPE_WSTRING:
                reader.SkipWString();
                break;
            case KV_TYPE_COLOR:
                reader.ReadUInt32();
                break;
            case KV_TYPE_UINT64:
            case KV_TYPE_INT64:
                child.uint64Val = reader.ReadUInt64();
                break;
            default:
                // Unknown type, stop parsing this subtree
                return node;
            }

            node.children.push_back(std::move(child));
        }
        return node;
    }

    std::string EscapeJsonString(const std::string& input) {
        std::string output;
        output.reserve(input.size() + 8);
        for (char c : input) {
            switch (c) {
            case '"':  output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    output += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                } else {
                    output.push_back(c);
                }
                break;
            }
        }
        return output;
    }

    std::vector<uint32_t> SplitAppIds(const std::string& str) {
        std::vector<uint32_t> result;
        std::stringstream ss(str);
        std::string item;
        while (std::getline(ss, item, ',')) {
            while (!item.empty() && isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
            while (!item.empty() && isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
            if (!item.empty()) {
                uint32_t id = 0;
                auto [ptr, ec] = std::from_chars(item.data(), item.data() + item.size(), id);
                if (ec == std::errc{} && id > 0) {
                    result.push_back(id);
                }
            }
        }
        return result;
    }

} // anonymous namespace

std::string GetDefaultAppInfoPath() {
    if (SteamInstallPath[0] != '\0') {
        std::string path = std::string(SteamInstallPath) + "\\appcache\\appinfo.vdf";
        DWORD attr = GetFileAttributesA(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) return path;
    }

    // Fallback: Registry HKCU\Software\Valve\Steam\SteamPath
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buf[MAX_PATH] = {};
        DWORD bufSize = sizeof(buf);
        if (RegQueryValueExA(hKey, "SteamPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(buf), &bufSize) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            std::string path = std::string(buf) + "\\appcache\\appinfo.vdf";
            std::replace(path.begin(), path.end(), '/', '\\');
            DWORD attr = GetFileAttributesA(path.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES) return path;
        } else {
            RegCloseKey(hKey);
        }
    }

    return "C:\\Program Files (x86)\\Steam\\appcache\\appinfo.vdf";
}

std::string AppMetadataResult::ToJson() const {
    std::string out = "{\n";
    out += std::format("  \"code\": {},\n", code);
    out += std::format("  \"message\": \"{}\",\n", EscapeJsonString(message));
    out += std::format("  \"appId\": {},\n", appId);
    out += std::format("  \"name\": \"{}\",\n", EscapeJsonString(name));
    out += std::format("  \"dlcCount\": {},\n", dlcs.size());

    // DLCs array
    out += "  \"dlcs\": [\n";
    for (size_t i = 0; i < dlcs.size(); ++i) {
        const auto& dlc = dlcs[i];
        out += "    {\n";
        out += std::format("      \"dlcAppId\": {},\n", dlc.dlcAppId);
        out += std::format("      \"name\": \"{}\",\n", EscapeJsonString(dlc.name));
        out += "      \"depots\": [\n";
        for (size_t j = 0; j < dlc.depots.size(); ++j) {
            const auto& depot = dlc.depots[j];
            out += "        {\n";
            out += std::format("          \"depotId\": {},\n", depot.depotId);
            out += std::format("          \"manifestGid\": \"{}\",\n", depot.manifestGid);
            out += std::format("          \"manifestSize\": {}\n", depot.size);
            out += (j + 1 < dlc.depots.size()) ? "        },\n" : "        }\n";
        }
        out += "      ]\n";
        out += (i + 1 < dlcs.size()) ? "    },\n" : "    }\n";
    }
    out += "  ],\n";

    // Base depots array
    out += "  \"baseDepots\": [\n";
    for (size_t i = 0; i < baseDepots.size(); ++i) {
        const auto& depot = baseDepots[i];
        out += "    {\n";
        out += std::format("      \"depotId\": {},\n", depot.depotId);
        out += std::format("      \"manifestGid\": \"{}\",\n", depot.manifestGid);
        out += std::format("      \"manifestSize\": {}\n", depot.size);
        out += (i + 1 < baseDepots.size()) ? "    },\n" : "    }\n";
    }
    out += "  ]\n";
    out += "}";
    return out;
}

AppMetadataResult QueryAppMetadata(uint32_t appId, const std::string& customVdfPath) {
    AppMetadataResult result;
    result.appId = appId;

    std::string vdfPath = customVdfPath.empty() ? GetDefaultAppInfoPath() : customVdfPath;
    HANDLE hFile = CreateFileA(vdfPath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        result.code = 1;
        result.message = std::format("Failed to open appinfo.vdf at {}", vdfPath);
        return result;
    }

    LARGE_INTEGER fileSizeLi;
    if (!GetFileSizeEx(hFile, &fileSizeLi)) {
        CloseHandle(hFile);
        result.code = 2;
        result.message = "Failed to get appinfo.vdf file size";
        return result;
    }
    const uint64_t fileSize = static_cast<uint64_t>(fileSizeLi.QuadPart);

    // Read header (16 bytes)
    uint8_t hdr[16] = {};
    DWORD readBytes = 0;
    if (!ReadFile(hFile, hdr, sizeof(hdr), &readBytes, nullptr) || readBytes < 16) {
        CloseHandle(hFile);
        result.code = 3;
        result.message = "Failed to read appinfo.vdf header";
        return result;
    }

    uint32_t magic = 0;
    memcpy(&magic, hdr, 4);
    if (magic != kAppInfoMagicV29) {
        CloseHandle(hFile);
        result.code = 4;
        result.message = std::format("Unsupported appinfo.vdf magic: 0x{:08X} (expected v29)", magic);
        return result;
    }

    int64_t tableOffset = 0;
    memcpy(&tableOffset, hdr + 8, 8);
    if (tableOffset <= 16 || static_cast<uint64_t>(tableOffset) >= fileSize) {
        CloseHandle(hFile);
        result.code = 5;
        result.message = "Invalid string table offset in appinfo.vdf";
        return result;
    }

    // Read string table
    LARGE_INTEGER seekTable;
    seekTable.QuadPart = tableOffset;
    if (!SetFilePointerEx(hFile, seekTable, nullptr, FILE_BEGIN)) {
        CloseHandle(hFile);
        result.code = 6;
        result.message = "Failed to seek to string table";
        return result;
    }

    size_t stringTableBytes = static_cast<size_t>(fileSize - tableOffset);
    std::vector<uint8_t> strData(stringTableBytes);
    if (!ReadFile(hFile, strData.data(), static_cast<DWORD>(stringTableBytes), &readBytes, nullptr)) {
        CloseHandle(hFile);
        result.code = 7;
        result.message = "Failed to read string table";
        return result;
    }

    std::vector<std::string> strings;
    if (strData.size() >= 4) {
        uint32_t numStrings = 0;
        memcpy(&numStrings, strData.data(), 4);
        strings.reserve(numStrings);
        size_t pos = 4;
        for (uint32_t i = 0; i < numStrings && pos < strData.size(); ++i) {
            size_t start = pos;
            while (pos < strData.size() && strData[pos] != 0) pos++;
            strings.emplace_back(reinterpret_cast<const char*>(strData.data() + start), pos - start);
            if (pos < strData.size() && strData[pos] == 0) pos++;
        }
    }

    // Seek back to apps (offset 16) and index all apps
    LARGE_INTEGER seekApps;
    seekApps.QuadPart = 16;
    SetFilePointerEx(hFile, seekApps, nullptr, FILE_BEGIN);

    std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> appOffsets; // appId -> (fileOffset, size)
    uint64_t curPos = 16;
    while (curPos < static_cast<uint64_t>(tableOffset)) {
        uint32_t entry[2] = {}; // [0] = curAppId, [1] = size
        if (!ReadFile(hFile, entry, sizeof(entry), &readBytes, nullptr) || readBytes < sizeof(entry)) {
            break;
        }
        if (entry[0] == 0) break;

        appOffsets[entry[0]] = {curPos, entry[1]};
        curPos += 8 + entry[1];

        LARGE_INTEGER nextPos;
        nextPos.QuadPart = curPos;
        if (!SetFilePointerEx(hFile, nextPos, nullptr, FILE_BEGIN)) break;
    }

    auto targetIt = appOffsets.find(appId);
    if (targetIt == appOffsets.end()) {
        CloseHandle(hFile);
        result.code = 404;
        result.message = std::format("AppId {} not found in local appinfo cache", appId);
        return result;
    }

    // Helper to read and parse an app's KVNode
    auto ReadAppKV = [&](uint64_t offset, uint32_t size) -> KVNode {
        if (size <= kAppRecordHeaderSize) return KVNode{};
        LARGE_INTEGER appKvSeek;
        appKvSeek.QuadPart = offset + 8 + kAppRecordHeaderSize;
        if (!SetFilePointerEx(hFile, appKvSeek, nullptr, FILE_BEGIN)) return KVNode{};

        uint32_t kvBytes = size - kAppRecordHeaderSize;
        std::vector<uint8_t> kvBuffer(kvBytes);
        DWORD read = 0;
        if (!ReadFile(hFile, kvBuffer.data(), kvBytes, &read, nullptr) || read < kvBytes) {
            return KVNode{};
        }
        BufferReader reader(kvBuffer.data(), kvBuffer.size());
        return ParseKVNode(reader, strings);
    };

    // Parse the target game
    KVNode root = ReadAppKV(targetIt->second.first, targetIt->second.second);

    // 1. Game Name
    if (const auto* nameNode = root.Find("appinfo/common/name")) {
        result.name = nameNode->strVal;
    }

    // 2. DLC List
    std::vector<uint32_t> dlcAppIds;
    if (const auto* dlcNode = root.Find("appinfo/extended/listofdlc")) {
        dlcAppIds = SplitAppIds(dlcNode->strVal);
    }

    // 3. Collect depots defined in the main App record
    std::unordered_map<uint32_t, std::vector<DepotInfo>> dlcDepotsMap; // dlcAppId -> depots
    std::unordered_set<uint32_t> seenDepotIds;

    auto ProcessDepotsNode = [&](const KVNode* depotsNode, uint32_t defaultDlcId) {
        if (!depotsNode) return;
        for (const auto& depotNode : depotsNode->children) {
            uint32_t depotId = 0;
            auto [p, ec] = std::from_chars(depotNode.name.data(), depotNode.name.data() + depotNode.name.size(), depotId);
            if (ec != std::errc{} || depotId == 0) continue;

            if (seenDepotIds.count(depotId)) continue;
            seenDepotIds.insert(depotId);

            DepotInfo depot;
            depot.depotId = depotId;

            // Manifest GID
            if (const auto* gidNode = depotNode.Find("manifests/public/gid")) {
                depot.manifestGid = gidNode->AsUInt64();
            }

            // Size
            if (const auto* sizeNode = depotNode.Find("manifests/public/size")) {
                depot.size = sizeNode->AsUInt64();
            } else if (const auto* maxsizeNode = depotNode.Find("maxsize")) {
                depot.size = maxsizeNode->AsUInt64();
            }

            // Associated DLC AppId
            uint32_t dlcId = defaultDlcId;
            if (const auto* dlcAppIdNode = depotNode.Find("dlcappid")) {
                dlcId = dlcAppIdNode->AsUInt32();
            }
            depot.dlcAppId = dlcId;

            if (dlcId != 0 && dlcId != appId) {
                dlcDepotsMap[dlcId].push_back(depot);
            } else {
                result.baseDepots.push_back(depot);
            }
        }
    };

    ProcessDepotsNode(root.Find("appinfo/depots"), 0);

    // 4. Resolve DLC details (names and any DLC-specific depots)
    for (uint32_t dlcId : dlcAppIds) {
        DlcInfo dlc;
        dlc.dlcAppId = dlcId;

        auto dlcIt = appOffsets.find(dlcId);
        if (dlcIt != appOffsets.end()) {
            KVNode dlcRoot = ReadAppKV(dlcIt->second.first, dlcIt->second.second);
            if (const auto* nameNode = dlcRoot.Find("appinfo/common/name")) {
                dlc.name = nameNode->strVal;
            }
            // If the DLC has its own depots section
            ProcessDepotsNode(dlcRoot.Find("appinfo/depots"), dlcId);
        }

        // Attach all depots matching this DLC
        auto mapIt = dlcDepotsMap.find(dlcId);
        if (mapIt != dlcDepotsMap.end()) {
            dlc.depots = std::move(mapIt->second);
            dlcDepotsMap.erase(mapIt);
        }

        result.dlcs.push_back(std::move(dlc));
    }

    // Attach any leftover DLC depots that weren't in listofdlc
    for (auto& [leftoverDlcId, depots] : dlcDepotsMap) {
        DlcInfo dlc;
        dlc.dlcAppId = leftoverDlcId;
        auto dlcIt = appOffsets.find(leftoverDlcId);
        if (dlcIt != appOffsets.end()) {
            KVNode dlcRoot = ReadAppKV(dlcIt->second.first, dlcIt->second.second);
            if (const auto* nameNode = dlcRoot.Find("appinfo/common/name")) {
                dlc.name = nameNode->strVal;
            }
        }
        dlc.depots = std::move(depots);
        result.dlcs.push_back(std::move(dlc));
    }

    CloseHandle(hFile);

    result.code = 0;
    result.message = "success";
    return result;
}

} // namespace VdfParser
