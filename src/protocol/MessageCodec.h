#pragma once

#include <QString>

#include "protocol/ProtocolHeader.h"

class MessageCodec
{
public:
    // 序列化协议头和负载（不含 TCP 长度前缀）。
    static QByteArray encode(const protocol::ClipboardMessage &message);
    // 解析并校验单个协议包。
    static bool decode(const QByteArray &packet, protocol::ClipboardMessage *outMessage, QString *error = nullptr);

private:
    // 轻量级负载校验，用于检测数据损坏。
    static quint32 checksum(const QByteArray &bytes);
};
