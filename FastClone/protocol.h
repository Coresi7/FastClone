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
void SendFrameBatch(const SocketHandle& socket, const std::vector<Frame>& frames);
void AppendEncodedFrame(std::vector<uint8_t>& out, const Frame& frame);
Frame RecvFrame(const SocketHandle& socket);

// Diagnostics for framing desync investigation.
// Human-readable name for a wire type byte (or "?(<n>)" for out-of-enum values).
const char* MsgTypeName(uint8_t typeByte);
// True only for byte values that map to a defined MsgType.
bool IsKnownMsgType(uint8_t typeByte);
// Formats the last few frames RECEIVED on the CALLING thread (wire order), i.e. the
// per-thread history maintained by RecvFrame. Used to dump context when a desync or
// wrong-direction frame is detected. Returns "<none>" if the calling thread has not
// received any frames.
std::string DescribeRecentFrames();

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
