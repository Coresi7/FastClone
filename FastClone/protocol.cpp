#include "protocol.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace fc {

namespace {

constexpr size_t kHeaderSize = 9;

// Sanity ceiling for a single frame's payload. The largest legitimate frame is a file
// chunk bounded by the tuned chunk size (clamped to <= 64 MiB) and batch-open responses
// of a few MiB. A desynced read interprets payload bytes as a 32-bit length, which is
// almost always far larger than this, so it is a cheap, false-positive-safe desync probe.
constexpr uint32_t kMaxSaneFramePayload = 1024u * 1024u * 1024u;  // 1 GiB

// Per-thread ring of recently RECEIVED frames (wire order), for desync post-mortem.
// Each receiving thread (one client recv thread / one server session receiver) owns its
// own history; no locking needed.
struct RecvHistoryEntry {
    uint8_t type = 0;
    uint32_t streamId = 0;
    uint32_t payloadLen = 0;
    bool used = false;
};
constexpr size_t kRecvHistorySize = 32;
thread_local std::array<RecvHistoryEntry, kRecvHistorySize> tlRecvHistory{};
thread_local size_t tlRecvHistoryPos = 0;
thread_local uint64_t tlRecvFrameIndex = 0;   // frames fully received so far on this thread
thread_local uint64_t tlRecvByteOffset = 0;   // wire bytes consumed so far on this thread

void RecordRecvHistory(uint8_t type, uint32_t streamId, uint32_t payloadLen) {
    tlRecvHistory[tlRecvHistoryPos % kRecvHistorySize] = RecvHistoryEntry{type, streamId, payloadLen, true};
    ++tlRecvHistoryPos;
}

void Ensure(bool cond, const char* message) {
    if (!cond) {
        throw std::runtime_error(message);
    }
}

std::array<uint8_t, kHeaderSize> EncodeHeader(const Frame& frame) {
    // Send-side guard (desync diagnostics): every frame in the codebase is built with a
    // literal MsgType, so an unknown type here means the Frame object was corrupted (e.g.
    // heap corruption / use-after-free) BEFORE it reached the wire. Catching it at the
    // source distinguishes "peer emitted a bad frame" from "transport stream misaligned".
    if (!IsKnownMsgType(static_cast<uint8_t>(frame.type))) {
        std::ostringstream os;
        os << "EncodeHeader refusing to send corrupted frame: type_byte="
           << static_cast<int>(static_cast<uint8_t>(frame.type))
           << " streamId=" << frame.streamId << " payloadLen=" << frame.payload.size();
        throw std::runtime_error(os.str());
    }
    const uint32_t payloadLen = static_cast<uint32_t>(frame.payload.size());
    std::array<uint8_t, kHeaderSize> header{};
    header[0] = static_cast<uint8_t>(frame.type);
    header[1] = static_cast<uint8_t>(frame.streamId & 0xFF);
    header[2] = static_cast<uint8_t>((frame.streamId >> 8) & 0xFF);
    header[3] = static_cast<uint8_t>((frame.streamId >> 16) & 0xFF);
    header[4] = static_cast<uint8_t>((frame.streamId >> 24) & 0xFF);
    header[5] = static_cast<uint8_t>(payloadLen & 0xFF);
    header[6] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
    header[7] = static_cast<uint8_t>((payloadLen >> 16) & 0xFF);
    header[8] = static_cast<uint8_t>((payloadLen >> 24) & 0xFF);
    return header;
}

}  // namespace

void AppendEncodedFrame(std::vector<uint8_t>& out, const Frame& frame) {
    const uint32_t payloadLen = static_cast<uint32_t>(frame.payload.size());
    out.reserve(out.size() + kHeaderSize + frame.payload.size());
    out.push_back(static_cast<uint8_t>(frame.type));
    out.push_back(static_cast<uint8_t>(frame.streamId & 0xFF));
    out.push_back(static_cast<uint8_t>((frame.streamId >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((frame.streamId >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((frame.streamId >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>(payloadLen & 0xFF));
    out.push_back(static_cast<uint8_t>((payloadLen >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((payloadLen >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((payloadLen >> 24) & 0xFF));
    if (!frame.payload.empty()) {
        out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    }
}

void SendFrame(const SocketHandle& socket, const Frame& frame) {
    std::array<uint8_t, kHeaderSize> header = EncodeHeader(frame);
    if (frame.payload.empty()) {
        SendAll(socket, header.data(), header.size());
        return;
    }
    std::array<SocketBuffer, 2> buffers{};
    buffers[0].data = header.data();
    buffers[0].len = header.size();
    buffers[1].data = frame.payload.data();
    buffers[1].len = frame.payload.size();
    SendBuffersAll(socket, buffers.data(), buffers.size());
}

void SendFrameBatch(const SocketHandle& socket, const std::vector<Frame>& frames) {
    if (frames.empty()) {
        return;
    }
    constexpr size_t kFrameChunk = 256;
    for (size_t offset = 0; offset < frames.size(); offset += kFrameChunk) {
        const size_t count = std::min<size_t>(kFrameChunk, frames.size() - offset);
        std::vector<std::array<uint8_t, kHeaderSize>> headers;
        headers.reserve(count);
        std::vector<SocketBuffer> buffers;
        buffers.reserve(count * 2);
        for (size_t i = 0; i < count; ++i) {
            const Frame& frame = frames[offset + i];
            headers.push_back(EncodeHeader(frame));
            buffers.push_back(SocketBuffer{headers.back().data(), kHeaderSize});
            if (!frame.payload.empty()) {
                buffers.push_back(SocketBuffer{frame.payload.data(), frame.payload.size()});
            }
        }
        SendBuffersAll(socket, buffers.data(), buffers.size());
    }
}

const char* MsgTypeName(uint8_t typeByte) {
    switch (static_cast<MsgType>(typeByte)) {
        case MsgType::Hello: return "Hello";
        case MsgType::Auth: return "Auth";
        case MsgType::AuthOk: return "AuthOk";
        case MsgType::AuthFail: return "AuthFail";
        case MsgType::ManifestRequest: return "ManifestRequest";
        case MsgType::ManifestEntry: return "ManifestEntry";
        case MsgType::ManifestEnd: return "ManifestEnd";
        case MsgType::ManifestProgress: return "ManifestProgress";
        case MsgType::HashRequest: return "HashRequest";
        case MsgType::HashResponse: return "HashResponse";
        case MsgType::FileOpen: return "FileOpen";
        case MsgType::FileChunk: return "FileChunk";
        case MsgType::FileEnd: return "FileEnd";
        case MsgType::FileError: return "FileError";
        case MsgType::FileBatchOpen: return "FileBatchOpen";
        case MsgType::FileBatchChunk: return "FileBatchChunk";
        case MsgType::FileBatchEnd: return "FileBatchEnd";
        case MsgType::SyncDone: return "SyncDone";
        case MsgType::Error: return "Error";
    }
    return "?";
}

bool IsKnownMsgType(uint8_t typeByte) {
    switch (static_cast<MsgType>(typeByte)) {
        case MsgType::Hello:
        case MsgType::Auth:
        case MsgType::AuthOk:
        case MsgType::AuthFail:
        case MsgType::ManifestRequest:
        case MsgType::ManifestEntry:
        case MsgType::ManifestEnd:
        case MsgType::ManifestProgress:
        case MsgType::HashRequest:
        case MsgType::HashResponse:
        case MsgType::FileOpen:
        case MsgType::FileChunk:
        case MsgType::FileEnd:
        case MsgType::FileError:
        case MsgType::FileBatchOpen:
        case MsgType::FileBatchChunk:
        case MsgType::FileBatchEnd:
        case MsgType::SyncDone:
        case MsgType::Error:
            return true;
    }
    return false;
}

std::string DescribeRecentFrames() {
    std::ostringstream os;
    const size_t total = std::min<size_t>(tlRecvHistoryPos, kRecvHistorySize);
    if (total == 0) {
        return "<none>";
    }
    // Walk oldest -> newest of the retained window.
    const size_t start = (tlRecvHistoryPos >= kRecvHistorySize) ? (tlRecvHistoryPos - kRecvHistorySize) : 0;
    os << "[recv_frame_index=" << tlRecvFrameIndex << " recv_byte_offset=" << tlRecvByteOffset
       << " last " << total << " frames (oldest->newest): ";
    for (size_t i = 0; i < total; ++i) {
        const RecvHistoryEntry& e = tlRecvHistory[(start + i) % kRecvHistorySize];
        if (!e.used) {
            continue;
        }
        os << MsgTypeName(e.type) << "(t=" << static_cast<int>(e.type)
           << ",sid=" << e.streamId << ",len=" << e.payloadLen << ") ";
    }
    os << "]";
    return os.str();
}

Frame RecvFrame(const SocketHandle& socket) {
    std::array<uint8_t, kHeaderSize> header{};
    RecvAll(socket, header.data(), header.size());
    const uint32_t streamId = static_cast<uint32_t>(header[1]) |
                              (static_cast<uint32_t>(header[2]) << 8) |
                              (static_cast<uint32_t>(header[3]) << 16) |
                              (static_cast<uint32_t>(header[4]) << 24);
    const uint32_t payloadLen = static_cast<uint32_t>(header[5]) |
                                (static_cast<uint32_t>(header[6]) << 8) |
                                (static_cast<uint32_t>(header[7]) << 16) |
                                (static_cast<uint32_t>(header[8]) << 24);
    // Desync probe: if the header byte is not a known type, or the declared payload is
    // absurd, the stream is misaligned. Fail LOUDLY here (wire order) with the raw header
    // bytes + per-thread recent-frame history, which pinpoints exactly where alignment
    // was lost instead of surfacing as a confusing higher-level "unexpected frame".
    if (!IsKnownMsgType(header[0]) || payloadLen > kMaxSaneFramePayload) {
        char hex[3 * kHeaderSize + 1] = {0};
        for (size_t i = 0; i < kHeaderSize; ++i) {
            std::snprintf(hex + i * 3, 4, "%02X ", header[i]);
        }
        std::ostringstream os;
        os << "RecvFrame desync: bad header type_byte=" << static_cast<int>(header[0])
           << " (" << MsgTypeName(header[0]) << ") streamId=" << streamId
           << " payloadLen=" << payloadLen
           << " known_type=" << (IsKnownMsgType(header[0]) ? 1 : 0)
           << " header_hex=[" << hex << "] " << DescribeRecentFrames();
        throw std::runtime_error(os.str());
    }
    Frame frame;
    frame.type = static_cast<MsgType>(header[0]);
    frame.streamId = streamId;
    frame.payload.resize(payloadLen);
    if (payloadLen > 0) {
        RecvAll(socket, frame.payload.data(), frame.payload.size());
    }
    RecordRecvHistory(header[0], streamId, payloadLen);
    ++tlRecvFrameIndex;
    tlRecvByteOffset += static_cast<uint64_t>(kHeaderSize) + payloadLen;
    return frame;
}

void AppendU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void AppendU64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendI64(std::vector<uint8_t>& out, int64_t value) {
    AppendU64(out, static_cast<uint64_t>(value));
}

uint16_t ReadU16(const std::vector<uint8_t>& buf, size_t& cursor) {
    Ensure(cursor + 2 <= buf.size(), "ReadU16 out of range");
    const uint16_t value = static_cast<uint16_t>(buf[cursor]) |
                           (static_cast<uint16_t>(buf[cursor + 1]) << 8);
    cursor += 2;
    return value;
}

uint32_t ReadU32(const std::vector<uint8_t>& buf, size_t& cursor) {
    Ensure(cursor + 4 <= buf.size(), "ReadU32 out of range");
    const uint32_t value = static_cast<uint32_t>(buf[cursor]) |
                           (static_cast<uint32_t>(buf[cursor + 1]) << 8) |
                           (static_cast<uint32_t>(buf[cursor + 2]) << 16) |
                           (static_cast<uint32_t>(buf[cursor + 3]) << 24);
    cursor += 4;
    return value;
}

uint64_t ReadU64(const std::vector<uint8_t>& buf, size_t& cursor) {
    Ensure(cursor + 8 <= buf.size(), "ReadU64 out of range");
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(buf[cursor + i]) << (i * 8);
    }
    cursor += 8;
    return value;
}

int64_t ReadI64(const std::vector<uint8_t>& buf, size_t& cursor) {
    return static_cast<int64_t>(ReadU64(buf, cursor));
}

void AppendString(std::vector<uint8_t>& out, const std::string& value) {
    if (value.size() > UINT16_MAX) {
        throw std::runtime_error("String too long");
    }
    AppendU16(out, static_cast<uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

std::string ReadString(const std::vector<uint8_t>& buf, size_t& cursor) {
    const uint16_t len = ReadU16(buf, cursor);
    Ensure(cursor + len <= buf.size(), "ReadString out of range");
    std::string value(reinterpret_cast<const char*>(buf.data() + cursor), len);
    cursor += len;
    return value;
}

}  // namespace fc
