#include "protocol.h"

#include <array>
#include <stdexcept>

namespace fc {

namespace {

constexpr size_t kHeaderSize = 9;

void Ensure(bool cond, const char* message) {
    if (!cond) {
        throw std::runtime_error(message);
    }
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
    std::vector<uint8_t> encoded;
    AppendEncodedFrame(encoded, frame);
    SendAll(socket, encoded.data(), encoded.size());
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
    Frame frame;
    frame.type = static_cast<MsgType>(header[0]);
    frame.streamId = streamId;
    frame.payload.resize(payloadLen);
    if (payloadLen > 0) {
        RecvAll(socket, frame.payload.data(), frame.payload.size());
    }
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
