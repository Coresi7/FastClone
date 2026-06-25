#pragma once

#include "protocol.h"
#include "protocol_codec.h"
#include "win_socket.h"

#include <string>

namespace fc {

// 协议版本号（内联变量，跨 TU 唯一，避免 ODR 冲突）。FC6 -> FC7：引入二进制 delta。
// FC6 与 FC7 在 Hello 阶段严格相等检查处干净互拒（沿用既有版本不符语义，无未知帧风险，
// binary-delta D-04 候选 C）。
inline constexpr const char* kProtocolVersion = "FC7";

// 连接级能力位（FC7，AuthOk.capabilities 载荷，binary-delta §8.1）。bit0 = 服务端具备
// 二进制 delta 能力。客户端仅当「自身 --delta-min-size>0 且服务端通告 bit0」时才发送
// delta 专用消息（AC-17）。
inline constexpr uint8_t kCapDelta = 0x01;

// 发送一个仅含文本载荷的简单帧（streamId=0）。
void SendSimple(const SocketHandle& socket, MsgType type, const std::string& text = {});

// 客户端 Hello 协商（失败/版本不符抛 std::runtime_error）。
void NegotiateHelloAsClient(const SocketHandle& socket);

// 服务端 Hello 协商（版本不符回 Error 帧并抛）。仅版本协商，不含会话状态。
void NegotiateHelloAsServer(const SocketHandle& socket);

// 客户端首连：Hello -> Auth -> AuthOk(NewSession)，返回会话身份+广播端点。
AuthOkInfo HandshakeClientNew(const SocketHandle& socket, const std::string& password);

// 客户端辅链路：Hello -> SessionJoin -> AuthOk(JoinAck)。
void HandshakeClientJoin(const SocketHandle& socket, const std::string& password,
                         const std::string& sessionId);

}  // namespace fc
