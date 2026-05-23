#pragma once

#include "win_socket.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

enum class MsgType : uint8_t {
    Hello = 1,
    Auth = 2,
    AuthOk = 3,
    AuthFail = 4,
    ManifestRequest = 10,
    ManifestEntry = 11,
    ManifestEnd = 12,
    ManifestProgress = 13,
    HashRequest = 20,
    HashResponse = 21,
    FileOpen = 30,
    FileChunk = 31,
    FileEnd = 32,
    FileError = 33,
    FileBatchOpen = 34,
    FileBatchChunk = 35,
    FileBatchEnd = 36,
    SyncDone = 40,
    Error = 255
};

struct Frame {
    MsgType type = MsgType::Error;
    uint32_t streamId = 0;
    std::vector<uint8_t> payload;
};

void SendFrame(const SocketHandle& socket, const Frame& frame);
void AppendEncodedFrame(std::vector<uint8_t>& out, const Frame& frame);
Frame RecvFrame(const SocketHandle& socket);

void AppendU16(std::vector<uint8_t>& out, uint16_t value);
void AppendU32(std::vector<uint8_t>& out, uint32_t value);
void AppendU64(std::vector<uint8_t>& out, uint64_t value);
void AppendI64(std::vector<uint8_t>& out, int64_t value);
uint16_t ReadU16(const std::vector<uint8_t>& buf, size_t& cursor);
uint32_t ReadU32(const std::vector<uint8_t>& buf, size_t& cursor);
uint64_t ReadU64(const std::vector<uint8_t>& buf, size_t& cursor);
int64_t ReadI64(const std::vector<uint8_t>& buf, size_t& cursor);
void AppendString(std::vector<uint8_t>& out, const std::string& value);
std::string ReadString(const std::vector<uint8_t>& buf, size_t& cursor);

}  // namespace fc
