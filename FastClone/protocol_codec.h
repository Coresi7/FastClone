#pragma once

#include "file_index.h"
#include "protocol.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fc {

// Wire record for a single file in a batch transfer. relativePath/fileSize/mtimeNs/ok
// travel on the wire; absPath is a server-side convenience that is never serialized.
struct BatchFileRecord {
    std::string relativePath;
    uint64_t fileSize = 0;
    int64_t mtimeNs = 0;
    bool ok = false;
    std::filesystem::path absPath;
};

// Multipath (FC6) handshake codecs (design §3.2 / r6-route-quality §2).
// AuthOk payload: role(u8) + sessionId(str) + addrCount(u16) + (addr[str], nicGroup[u16])*.
// role 0 = NewSession (first connection), role 1 = JoinAck (follow-up connection).
// Each advertised endpoint carries a dense server-side physical-NIC group number so the
// client can dedup auxiliaries by server NIC rather than by host:port (L-r6-01). The
// nicGroup is opaque to the client: it only identifies which endpoints share one server
// physical NIC. FC6-internal wire evolution (no version bump, see gate decision).
enum class AuthOkRole : uint8_t {
    NewSession = 0,
    JoinAck = 1
};

// One server endpoint advertised in AuthOk: the wire endpoint string ("ip:port" /
// "[ipv6]:port", D-01 encoding) plus the dense per-physical-NIC group it belongs to.
struct AdvertisedEndpoint {
    std::string endpoint;   // "ip:port" or "[ipv6]:port"
    uint16_t nicGroup = 0;  // server physical-NIC group (same value for one NIC's addrs)
};

struct AuthOkInfo {
    AuthOkRole role = AuthOkRole::NewSession;
    std::string sessionId;
    std::vector<AdvertisedEndpoint> serverAddrs;  // advertised endpoints + NIC group
};

std::vector<uint8_t> EncodeAuthOk(const AuthOkInfo& info);
AuthOkInfo DecodeAuthOk(const std::vector<uint8_t>& payload);

// SessionJoin payload: sessionId(str) + password(str).
struct SessionJoinInfo {
    std::string sessionId;
    std::string password;
};

std::vector<uint8_t> EncodeSessionJoin(const SessionJoinInfo& info);
SessionJoinInfo DecodeSessionJoin(const std::vector<uint8_t>& payload);

std::vector<uint8_t> EncodeManifestEntry(const FileEntry& entry);
FileEntry DecodeManifestEntry(const std::vector<uint8_t>& payload);

std::vector<uint8_t> EncodeHashRequest(const std::string& relPath);
std::string DecodeHashRequest(const std::vector<uint8_t>& payload);

std::vector<uint8_t> EncodeHashResponse(const std::string& relPath, const Hash256& hash);
std::pair<std::string, Hash256> DecodeHashResponse(const std::vector<uint8_t>& payload);

std::vector<uint8_t> EncodeFileOpen(const std::string& relPath);
std::string DecodeFileOpen(const std::vector<uint8_t>& payload);

std::vector<uint8_t> EncodeFileBatchRequest(const std::vector<std::string>& relPaths);
std::vector<std::string> DecodeFileBatchRequest(const std::vector<uint8_t>& payload);

std::vector<uint8_t> EncodeFileBatchOpenResponse(const std::vector<BatchFileRecord>& files);
std::vector<BatchFileRecord> DecodeFileBatchOpenResponse(const std::vector<uint8_t>& payload);

}  // namespace fc
