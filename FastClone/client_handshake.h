#pragma once

#include "protocol.h"
#include "protocol_codec.h"
#include "win_socket.h"

#include <string>

namespace fc {

// FC6 协议版本号（内联变量，跨 TU 唯一，避免 ODR 冲突）。
inline constexpr const char* kProtocolVersion = "FC6";

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
