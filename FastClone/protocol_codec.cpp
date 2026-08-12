#include "protocol_codec.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace fc {

std::vector<uint8_t> EncodeAuthOk(const AuthOkInfo& info) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(info.role));
    AppendString(payload, info.sessionId);
    if (info.serverAddrs.size() > UINT16_MAX) {
        throw std::runtime_error("AuthOk server address list too long");
    }
    AppendU16(payload, static_cast<uint16_t>(info.serverAddrs.size()));
    for (const AdvertisedEndpoint& addr : info.serverAddrs) {
        AppendString(payload, addr.endpoint);
        AppendU16(payload, addr.nicGroup);
    }
    // FC7 connection-level capability bits, appended last (binary-delta section 8.1).
    payload.push_back(info.capabilities);
    // Extension string appended after the capability byte (T-largefile-block-multinic).
    AppendString(payload, info.extensionString);
    return payload;
}

AuthOkInfo DecodeAuthOk(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    if (payload.empty()) {
        throw std::runtime_error("AuthOk payload too short");
    }
    AuthOkInfo info;
    const uint8_t roleByte = payload[cursor++];
    info.role = (roleByte == static_cast<uint8_t>(AuthOkRole::JoinAck))
                    ? AuthOkRole::JoinAck
                    : AuthOkRole::NewSession;
    info.sessionId = ReadString(payload, cursor);
    const uint16_t addrCount = ReadU16(payload, cursor);
    info.serverAddrs.reserve(addrCount);
    for (uint16_t i = 0; i < addrCount; ++i) {
        AdvertisedEndpoint adv;
        adv.endpoint = ReadString(payload, cursor);
        adv.nicGroup = ReadU16(payload, cursor);
        info.serverAddrs.push_back(std::move(adv));
    }
    // FC7 capability byte trails the address list; absent => no capabilities (back-compat
    // with fixtures / probe payloads that predate the field, binary-delta section 8.1).
    if (cursor < payload.size()) {
        info.capabilities = payload[cursor++];
    }
    // Extension string trails the capability byte; absent => "" (same tolerant style).
    if (cursor < payload.size()) {
        info.extensionString = ReadString(payload, cursor);
    }
    return info;
}

std::vector<uint8_t> EncodeAuthClaim(const std::string& password, uint8_t capabilities,
                                     const std::string& extensionString) {
    // Bare password bytes first (legacy shape), then the trailing capability byte and
    // extension string (T-largefile-block-multinic).
    std::vector<uint8_t> payload(password.begin(), password.end());
    payload.push_back(capabilities);
    AppendString(payload, extensionString);
    return payload;
}

bool DecodeAuthClaim(const std::vector<uint8_t>& payload, const std::string& expectedPassword,
                     AuthClaimInfo& out) {
    out = AuthClaimInfo{};
    if (payload.size() < expectedPassword.size() ||
        !std::equal(expectedPassword.begin(), expectedPassword.end(),
                    reinterpret_cast<const char*>(payload.data()),
                    reinterpret_cast<const char*>(payload.data()) + expectedPassword.size())) {
        return false;  // password prefix mismatch -> authentication failure
    }
    size_t cursor = expectedPassword.size();
    // Trailing fields are read only while bytes remain (tolerant): a legacy bare-password
    // payload decodes with capabilities=0 / extensionString="".
    if (cursor < payload.size()) {
        out.capabilities = payload[cursor++];
    }
    if (cursor + 2 <= payload.size()) {
        const uint16_t extLen = static_cast<uint16_t>(payload[cursor]) |
                                static_cast<uint16_t>(static_cast<uint16_t>(payload[cursor + 1]) << 8);
        if (cursor + 2 + extLen <= payload.size()) {
            out.extensionString.assign(reinterpret_cast<const char*>(payload.data() + cursor + 2),
                                       extLen);
        }
    }
    return true;
}

std::vector<uint8_t> EncodeSessionJoin(const SessionJoinInfo& info) {
    std::vector<uint8_t> payload;
    AppendString(payload, info.sessionId);
    AppendString(payload, info.password);
    // Client capability byte + extension string trail the password (T-largefile-block-multinic).
    payload.push_back(info.capabilities);
    AppendString(payload, info.extensionString);
    return payload;
}

SessionJoinInfo DecodeSessionJoin(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    SessionJoinInfo info;
    info.sessionId = ReadString(payload, cursor);
    info.password = ReadString(payload, cursor);
    // Trailing capability byte / extension string: read only while bytes remain, so a
    // legacy payload (sessionId + password only) decodes with defaults.
    if (cursor < payload.size()) {
        info.capabilities = payload[cursor++];
    }
    if (cursor < payload.size()) {
        info.extensionString = ReadString(payload, cursor);
    }
    return info;
}

std::vector<uint8_t> EncodeCheckAuth(const CheckAuthInfo& info) {
    std::vector<uint8_t> payload;
    AppendString(payload, info.password);
    payload.push_back(info.flags);
    return payload;
}

CheckAuthInfo DecodeCheckAuth(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    CheckAuthInfo info;
    info.password = ReadString(payload, cursor);
    // The flags byte follows password; missing flags decode as 0 (compatible with future evolution and old payloads).
    if (cursor < payload.size()) {
        info.flags = payload[cursor++];
    }
    return info;
}

std::vector<uint8_t> EncodeManifestEntry(const FileEntry& entry) {
    std::vector<uint8_t> payload;
    payload.push_back(entry.isDirectory ? 1 : 0);
    AppendString(payload, entry.relativePath);
    AppendU64(payload, entry.fileSize);
    AppendI64(payload, entry.mtimeNs);
    return payload;
}

FileEntry DecodeManifestEntry(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    if (payload.empty()) {
        throw std::runtime_error("Manifest payload too short");
    }
    FileEntry entry;
    entry.isDirectory = payload[cursor++] != 0;
    entry.relativePath = ReadString(payload, cursor);
    entry.fileSize = ReadU64(payload, cursor);
    entry.mtimeNs = ReadI64(payload, cursor);
    return entry;
}

std::vector<uint8_t> EncodeHashRequest(const std::string& relPath) {
    std::vector<uint8_t> payload;
    AppendString(payload, relPath);
    return payload;
}

std::string DecodeHashRequest(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    return ReadString(payload, cursor);
}

std::vector<uint8_t> EncodeHashResponse(const std::string& relPath, const Hash256& hash) {
    std::vector<uint8_t> payload;
    AppendString(payload, relPath);
    payload.insert(payload.end(), hash.begin(), hash.end());
    return payload;
}

std::pair<std::string, Hash256> DecodeHashResponse(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    std::pair<std::string, Hash256> value;
    value.first = ReadString(payload, cursor);
    if (cursor + value.second.size() > payload.size()) {
        throw std::runtime_error("Hash response payload invalid");
    }
    std::copy(payload.begin() + static_cast<std::ptrdiff_t>(cursor), payload.begin() + static_cast<std::ptrdiff_t>(cursor + value.second.size()), value.second.begin());
    return value;
}

std::vector<uint8_t> EncodeFileOpen(const std::string& relPath) {
    std::vector<uint8_t> payload;
    AppendString(payload, relPath);
    return payload;
}

std::string DecodeFileOpen(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    return ReadString(payload, cursor);
}

std::vector<uint8_t> EncodeFileBatchRequest(const std::vector<std::string>& relPaths) {
    if (relPaths.size() > UINT16_MAX) {
        throw std::runtime_error("Batch request too large");
    }
    std::vector<uint8_t> payload;
    AppendU16(payload, static_cast<uint16_t>(relPaths.size()));
    for (const std::string& rel : relPaths) {
        AppendString(payload, rel);
    }
    return payload;
}

std::vector<std::string> DecodeFileBatchRequest(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    const uint16_t count = ReadU16(payload, cursor);
    std::vector<std::string> relPaths;
    relPaths.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        relPaths.push_back(ReadString(payload, cursor));
    }
    return relPaths;
}

std::vector<uint8_t> EncodeFileBatchOpenResponse(const std::vector<BatchFileRecord>& files) {
    if (files.size() > UINT16_MAX) {
        throw std::runtime_error("Batch response too large");
    }
    std::vector<uint8_t> payload;
    AppendU16(payload, static_cast<uint16_t>(files.size()));
    for (const auto& file : files) {
        payload.push_back(file.ok ? 1 : 0);
        AppendString(payload, file.relativePath);
        AppendU64(payload, file.fileSize);
        AppendI64(payload, file.mtimeNs);
    }
    return payload;
}

std::vector<BatchFileRecord> DecodeFileBatchOpenResponse(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    const uint16_t count = ReadU16(payload, cursor);
    std::vector<BatchFileRecord> files;
    files.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        if (cursor >= payload.size()) {
            throw std::runtime_error("Batch open response invalid");
        }
        BatchFileRecord file;
        file.ok = payload[cursor++] != 0;
        file.relativePath = ReadString(payload, cursor);
        file.fileSize = ReadU64(payload, cursor);
        file.mtimeNs = ReadI64(payload, cursor);
        files.push_back(std::move(file));
    }
    return files;
}

std::vector<uint8_t> EncodeBlockSigRequest(const std::string& relPath) {
    std::vector<uint8_t> payload;
    AppendString(payload, relPath);
    return payload;
}

std::string DecodeBlockSigRequest(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    return ReadString(payload, cursor);
}

std::vector<uint8_t> EncodeDeltaError(const std::string& relPath) {
    std::vector<uint8_t> payload;
    AppendString(payload, relPath);
    return payload;
}

std::string DecodeDeltaError(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    return ReadString(payload, cursor);
}

std::vector<uint8_t> EncodeBlockSigResponse(const std::string& relPath, const Hash256& fileHash, const delta::SignatureSet& sig) {
    if (sig.strongLen > 16) {
        throw std::runtime_error("BlockSigResponse strongLen out of range");
    }
    if (sig.blocks.size() != sig.blockCount) {
        throw std::runtime_error("BlockSigResponse block count mismatch");
    }
    std::vector<uint8_t> payload;
    payload.reserve(2 + relPath.size() + fileHash.size() + 17 +
                    static_cast<size_t>(sig.blockCount) * (4 + sig.strongLen));
    AppendString(payload, relPath);
    payload.insert(payload.end(), fileHash.begin(), fileHash.end());
    AppendU64(payload, sig.fileSize);
    AppendU32(payload, sig.blockSize);
    AppendU32(payload, sig.blockCount);
    payload.push_back(sig.strongLen);
    for (const delta::BlockSig& bs : sig.blocks) {
        AppendU32(payload, bs.weak);
        payload.insert(payload.end(), bs.strong.begin(), bs.strong.begin() + sig.strongLen);
    }
    return payload;
}

BlockSigResponseInfo DecodeBlockSigResponse(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    BlockSigResponseInfo info;
    info.relPath = ReadString(payload, cursor);
    if (cursor + info.fileHash.size() > payload.size()) {
        throw std::runtime_error("BlockSigResponse file hash truncated");
    }
    std::copy(payload.begin() + static_cast<std::ptrdiff_t>(cursor),
              payload.begin() + static_cast<std::ptrdiff_t>(cursor + info.fileHash.size()),
              info.fileHash.begin());
    cursor += info.fileHash.size();
    delta::SignatureSet& sig = info.sig;
    sig.fileSize = ReadU64(payload, cursor);
    sig.blockSize = ReadU32(payload, cursor);
    sig.blockCount = ReadU32(payload, cursor);
    if (cursor >= payload.size()) {
        throw std::runtime_error("BlockSigResponse payload truncated");
    }
    sig.strongLen = payload[cursor++];
    if (sig.strongLen > 16) {
        throw std::runtime_error("BlockSigResponse strongLen out of range");
    }
    sig.blocks.reserve(sig.blockCount);
    for (uint32_t i = 0; i < sig.blockCount; ++i) {
        delta::BlockSig bs;
        bs.weak = ReadU32(payload, cursor);
        if (cursor + sig.strongLen > payload.size()) {
            throw std::runtime_error("BlockSigResponse strong checksum truncated");
        }
        std::copy(payload.begin() + static_cast<std::ptrdiff_t>(cursor),
                  payload.begin() + static_cast<std::ptrdiff_t>(cursor + sig.strongLen),
                  bs.strong.begin());
        cursor += sig.strongLen;
        sig.blocks.push_back(bs);
    }
    return info;
}

std::vector<uint8_t> EncodeDeltaRangeOpen(const DeltaRangeRequest& req) {
    std::vector<uint8_t> payload;
    AppendString(payload, req.relPath);
    AppendU64(payload, req.offset);
    AppendU64(payload, req.length);
    return payload;
}

DeltaRangeRequest DecodeDeltaRangeOpen(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    DeltaRangeRequest req;
    req.relPath = ReadString(payload, cursor);
    req.offset = ReadU64(payload, cursor);
    req.length = ReadU64(payload, cursor);
    return req;
}

std::vector<uint8_t> EncodeFileRangeOpen(const FileRangeOpenReq& req) {
    std::vector<uint8_t> payload;
    AppendString(payload, req.relPath);
    AppendU64(payload, req.offset);
    AppendU64(payload, req.length);
    return payload;
}

FileRangeOpenReq DecodeFileRangeOpen(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    FileRangeOpenReq req;
    req.relPath = ReadString(payload, cursor);
    req.offset = ReadU64(payload, cursor);
    req.length = ReadU64(payload, cursor);
    return req;
}

std::vector<uint8_t> EncodeFileRangeError(const std::string& relPath) {
    std::vector<uint8_t> payload;
    AppendString(payload, relPath);
    return payload;
}

std::string DecodeFileRangeError(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    return ReadString(payload, cursor);
}

}  // namespace fc
