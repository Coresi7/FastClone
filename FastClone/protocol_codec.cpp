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
    return info;
}

std::vector<uint8_t> EncodeSessionJoin(const SessionJoinInfo& info) {
    std::vector<uint8_t> payload;
    AppendString(payload, info.sessionId);
    AppendString(payload, info.password);
    return payload;
}

SessionJoinInfo DecodeSessionJoin(const std::vector<uint8_t>& payload) {
    size_t cursor = 0;
    SessionJoinInfo info;
    info.sessionId = ReadString(payload, cursor);
    info.password = ReadString(payload, cursor);
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

}  // namespace fc
