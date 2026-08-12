#pragma once

#include "delta.h"
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

// Multipath (FC6) handshake codecs (design section 3.2 / r6-route-quality section 2).
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
    // FC7 connection-level capability bits (binary-delta section 8.1). Appended at the END of the
    // AuthOk payload; DecodeAuthOk reads it only when bytes remain (default 0), so older
    // fixtures / probe code without the field decode as "no capabilities".
    uint8_t capabilities = 0;
    // FC7 additive (T-largefile-block-multinic): trailing free-form extension string.
    // Read only when bytes remain after `capabilities` (same tolerant style); absent => "".
    std::string extensionString;
};

std::vector<uint8_t> EncodeAuthOk(const AuthOkInfo& info);
AuthOkInfo DecodeAuthOk(const std::vector<uint8_t>& payload);

// Auth payload (client -> server session claim, T-largefile-block-multinic): the bare
// password bytes, optionally followed by capabilities(u8) + extensionString(str). The
// server parses tolerantly: the payload must start with the expected password; the
// capability byte and extension string are read only while bytes remain, so a legacy
// client sending a bare password decodes as capabilities=0 / extensionString="".
struct AuthClaimInfo {
    uint8_t capabilities = 0;
    std::string extensionString;
};

std::vector<uint8_t> EncodeAuthClaim(const std::string& password, uint8_t capabilities,
                                     const std::string& extensionString);
// Returns false only when the password prefix mismatches (authentication failure). A
// well-formed password prefix with a missing/truncated trailing extension decodes with
// defaults (tolerant), never throws.
bool DecodeAuthClaim(const std::vector<uint8_t>& payload, const std::string& expectedPassword,
                     AuthClaimInfo& out);

// SessionJoin payload: sessionId(str) + password(str), optionally followed by
// capabilities(u8) + extensionString(str) (client capability advertisement,
// T-largefile-block-multinic). Trailing fields are read only while bytes remain, so a
// legacy payload decodes with capabilities=0 / extensionString="".
struct SessionJoinInfo {
    std::string sessionId;
    std::string password;
    uint8_t capabilities = 0;
    std::string extensionString;
};

std::vector<uint8_t> EncodeSessionJoin(const SessionJoinInfo& info);
SessionJoinInfo DecodeSessionJoin(const std::vector<uint8_t>& payload);

// CheckAuth payload (fastcheck): password(str) + flags(u8). Deliberately structured (AppendString+flags)
// rather than Auth's bare password bytes, to ease round-trip unit tests and future evolution. flags is
// currently always 0; DecodeCheckAuth decodes it as 0 when the flags byte is absent, following DecodeAuthOk's
// "read only when bytes remain" tolerance style.
struct CheckAuthInfo {
    std::string password;
    uint8_t flags = 0;
};

std::vector<uint8_t> EncodeCheckAuth(const CheckAuthInfo& info);
CheckAuthInfo DecodeCheckAuth(const std::vector<uint8_t>& payload);

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

// --- Binary delta (FC7, binary-delta section 8.2) ---

// BlockSigRequest / DeltaError / BlockSigRequest share the bare-relPath payload shape.
std::vector<uint8_t> EncodeBlockSigRequest(const std::string& relPath);
std::string DecodeBlockSigRequest(const std::vector<uint8_t>& payload);

std::vector<uint8_t> EncodeDeltaError(const std::string& relPath);
std::string DecodeDeltaError(const std::vector<uint8_t>& payload);

// BlockSigResponse: relPath + fileHash(16, XXH3-128 raw layout matching ComputeFileHash) +
// fileSize(u64) + blockSize(u32) + blockCount(u32) + strongLen(u8) +
// blockCount * (weak(u32) + strong[strongLen]). The full-file hash rides along so the
// client can run the FR-23 reconstruction check WITHOUT a separate HashRequest (which would
// entangle the delta flow with the FallbackHash machinery). Block offsets are NOT on the
// wire; the client derives them from index*blockSize (design section 8.2).
struct BlockSigResponseInfo {
    std::string relPath;
    Hash256 fileHash{};
    delta::SignatureSet sig;
};
std::vector<uint8_t> EncodeBlockSigResponse(const std::string& relPath, const Hash256& fileHash, const delta::SignatureSet& sig);
BlockSigResponseInfo DecodeBlockSigResponse(const std::vector<uint8_t>& payload);

// DeltaRangeOpen: relPath + offset(u64) + length(u64). The stream id travels in the frame
// header; DeltaRangeChunk payloads are raw bytes and DeltaRangeEnd is empty (no codec).
struct DeltaRangeRequest {
    std::string relPath;
    uint64_t offset = 0;
    uint64_t length = 0;
};
std::vector<uint8_t> EncodeDeltaRangeOpen(const DeltaRangeRequest& req);
DeltaRangeRequest DecodeDeltaRangeOpen(const std::vector<uint8_t>& payload);

// --- Large-file block (file-range) transfer (FC7 additive, T-largefile-block-multinic) ---

// FileRangeOpen: relPath + offset(u64) + length(u64) (wire-identical shape to
// DeltaRangeOpen, but a separate MsgType family so the two state machines stay isolated).
// FileRangeData payloads are raw bytes and FileRangeEnd is empty (no codec).
struct FileRangeOpenReq {
    std::string relPath;
    uint64_t offset = 0;
    uint64_t length = 0;
};
std::vector<uint8_t> EncodeFileRangeOpen(const FileRangeOpenReq& req);
FileRangeOpenReq DecodeFileRangeOpen(const std::vector<uint8_t>& payload);

// FileRangeError: relPath (same bare-string shape as DeltaError, separate codec names for
// state-machine isolation).
std::vector<uint8_t> EncodeFileRangeError(const std::string& relPath);
std::string DecodeFileRangeError(const std::vector<uint8_t>& payload);

}  // namespace fc
