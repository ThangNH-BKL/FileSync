#pragma once
// Read-only, UI-only session metadata. Persistent storage keys never change.
#include <string>
namespace fsyn {
struct ClientConnectionInfo {
    std::string hostKey;   // Existing server layout: <ipFolder>\\<exeFolder>
    std::string ip;        // From accepted socket, not client-supplied HELLO.ip
    std::string watchRoot; // Authenticated HELLO.watchRoot (for display/browse only)
};
}
