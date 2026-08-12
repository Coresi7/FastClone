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
    // Multipath (FC6): a follow-up connection claims an existing logical session by
    // sessionId instead of starting a new one. Sent by the client in place of Auth.
    SessionJoin = 5,
    // FastCheck (fastcheck, FC7 additive): the read-only comparison client claims a session with CheckAuth, replacing Auth.
    // Occupies the empty slot between SessionJoin=5 and ManifestRequest=10, without changing any existing frame layout.
    CheckAuth = 6,
    ManifestRequest = 10,
    ManifestEntry = 11,
    ManifestEnd = 12,
    ManifestProgress = 13,
    HashRequest = 20,
    HashResponse = 21,
    // Binary delta (FC7, binary-delta section 8.2). Independent message types (not reusing
    // FileChunk) so the delta state machine stays isolated from the regular download path.
    BlockSigRequest = 22,   // C->S: relPath
    BlockSigResponse = 23,  // S->C: relPath + signature set
    DeltaRangeOpen = 24,    // C->S: relPath + offset + length (stream id in frame header)
    DeltaRangeChunk = 25,   // S->C: raw range bytes (appended in send order to the stream)
    DeltaRangeEnd = 26,     // S->C: empty (range complete)
    DeltaError = 27,        // S->C: relPath (signature/range failure -> client full fallback)
    FileOpen = 30,
    FileChunk = 31,
    FileEnd = 32,
    FileError = 33,
    FileBatchOpen = 34,
    FileBatchChunk = 35,
    FileBatchEnd = 36,
    // Large-file block (file-range) transfer (FC7 additive, T-largefile-block-multinic).
    // Independent message types (not reusing DeltaRange*) so the large-file block state
    // machine stays isolated from the delta state machine. Sent only after both ends
    // negotiated the kCapFileRange capability bit (client_handshake.h).
    FileRangeOpen = 37,   // C->S: relPath + offset + length (stream id in frame header)
    FileRangeData = 38,   // S->C: raw range bytes (appended in send order to the stream)
    FileRangeEnd = 39,    // S->C: empty (range complete)
    SyncDone = 40,
    FileRangeError = 41,  // S->C: relPath (range open/read failure -> client single-stream fallback)
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

// Cumulative wire bytes sent/received across all sockets in this process (relaxed atomics,
// ~nanosecond cost). For release-level real-time rate diagnostics in progress lines.
uint64_t NetBytesSent();
uint64_t NetBytesRecv();

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
