#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace VdfParser {

    struct DepotInfo {
        uint32_t depotId = 0;
        uint64_t manifestGid = 0;
        uint64_t size = 0;
        uint32_t dlcAppId = 0;
    };

    struct DlcInfo {
        uint32_t dlcAppId = 0;
        std::string name;
        std::vector<DepotInfo> depots;
    };

    struct AppMetadataResult {
        int code = 0;              // 0: success, non-0: error
        std::string message;
        uint32_t appId = 0;
        std::string name;
        std::vector<DlcInfo> dlcs;
        std::vector<DepotInfo> baseDepots;

        std::string ToJson() const;
    };

    // Finds the default Steam appinfo.vdf path on the local machine
    std::string GetDefaultAppInfoPath();

    // Queries DLCs and Depot Manifest GIDs for the given appId from appinfo.vdf
    AppMetadataResult QueryAppMetadata(uint32_t appId, const std::string& customVdfPath = "");

} // namespace VdfParser
