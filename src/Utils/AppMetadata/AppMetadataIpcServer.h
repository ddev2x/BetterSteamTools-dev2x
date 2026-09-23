#pragma once

#include <cstdint>

namespace AppMetadataIpcServer {

    // Starts the IPC Named Pipe server thread inside Steam
    void Start();

    // Stops the IPC Named Pipe server thread
    void Stop();

    // Checks if the server is running
    bool IsRunning();

} // namespace AppMetadataIpcServer

// ── C-ABI Exported Functions for External Applications ──────────────────────────
// Any external application (C#, Python, C++, Go, etc.) can load OpenSteamTool.dll
// and invoke this method to query DLCs and Depot Manifest IDs.
extern "C" {
    // Queries DLCs and Depot Manifests for an AppId.
    // Returns a heap-allocated JSON string (caller must free with OST_FreeString).
    __declspec(dllexport) const char* __cdecl OST_QueryAppDlcAndManifests(
        uint32_t appId,
        const char* token,
        bool forceRefresh
    );

    // Frees a string returned by OST_QueryAppDlcAndManifests.
    __declspec(dllexport) void __cdecl OST_FreeString(const char* p);
}
