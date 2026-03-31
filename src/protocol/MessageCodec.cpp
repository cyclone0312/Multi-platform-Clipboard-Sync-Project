#include "protocol/MessageCodec.h"

#include <QDataStream>

QByteArray MessageCodec::encode(const protocol::ClipboardMessage &message)
{
    QByteArray bytes;
    // 预留完整包容量（固定头 + 负载），减少追加时的重分配。
    bytes.reserve(protocol::kHeaderLength + message.payload.size());

    QDataStream stream(&bytes, QIODevice::WriteOnly);
    // 协议统一使用小端字节序，保证跨端解析一致。
    stream.setByteOrder(QDataStream::LittleEndian);

    // 协议魔数：快速识别“这是不是本协议的数据包”。
    stream << protocol::kMagic;
    // 协议版本：用于后续兼容和升级。
    stream << protocol::kVersion;
    // 头部长度：当前为固定值，预留未来扩展头字段能力。
    stream << protocol::kHeaderLength;
    // 消息类型：告诉接收端 payload 应按哪种业务语义解析。
    stream << static_cast<quint16>(message.type);
    // 标志位：预留给压缩、分片、优先级等控制选项。
    stream << message.flags;
    // 会话 ID：标识一次同步动作，便于关联同批消息。
    stream << message.sessionId;
    // 会话内序号：用于排序、去重或重传判断。
    stream << message.sequence;
    // 负载长度：接收端据此从包中精确切出 payload。
    stream << static_cast<quint64>(message.payload.size());
    // 负载校验值：用于检测传输损坏或数据错位。
    stream << checksum(message.payload);
    // 保留字段：当前写 0，后续协议升级可复用。
    stream << static_cast<quint32>(0);

    // 头部之后紧跟原始负载字节。
    bytes.append(message.payload);
    return bytes;
}

bool MessageCodec::decode(const QByteArray &packet, protocol::ClipboardMessage *outMessage, QString *error)
{
    // 模块1：输出参数校验。调用方必须提供可写的输出对象。
    if (!outMessage)
    {
        if (error)
        {
            *error = QStringLiteral("output message pointer is null");
        }
        return false;
    }

    // 模块2：最小长度校验。连固定头都不够，直接判为无效包。
    if (packet.size() < protocol::kHeaderLength)
    {
        if (error)
        {
            *error = QStringLiteral("packet shorter than header");
        }
        return false;
    }

    // 基于收到的二进制 packet 创建一个“读数据流” 指定按小端序解析字节
    QDataStream stream(packet);
    stream.setByteOrder(QDataStream::LittleEndian);

    // 模块3：定义头字段容器，按协议顺序读取。
    quint32 magic = 0;
    quint16 version = 0;
    quint16 headerLength = 0;
    quint16 typeRaw = 0; // 接收消息类型原始值（后续再转成 MessageType 枚举）
    quint16 flags = 0;
    quint64 sessionId = 0; // 接收会话 ID，用来关联同一次同步
    quint64 sequence = 0;
    quint64 payloadLength = 0;
    quint32 payloadChecksum = 0;
    quint32 reserved = 0; // 接收保留字段（当前一般没业务用途，给未来扩展）

    // 读取包中对应内容
    stream >> magic;
    stream >> version;
    stream >> headerLength;
    stream >> typeRaw;
    stream >> flags;
    stream >> sessionId;
    stream >> sequence;
    stream >> payloadLength;
    stream >> payloadChecksum;
    stream >> reserved;

    // 预留字段当前不参与业务逻辑。
    Q_UNUSED(reserved)

    // 模块4：协议身份校验。magic 不匹配说明不是本协议数据。
    if (magic != protocol::kMagic)
    {
        if (error)
        {
            *error = QStringLiteral("invalid magic");
        }
        return false;
    }

    // 模块5：协议版本校验。版本不兼容时拒绝解析。
    if (version != protocol::kVersion)
    {
        if (error)
        {
            *error = QStringLiteral("unsupported version");
        }
        return false;
    }

    // 模块6：头长度校验。必须不小于当前最小头，且不能超出实际包长。
    if (headerLength < protocol::kHeaderLength || packet.size() < headerLength)
    {
        if (error)
        {
            *error = QStringLiteral("invalid header length");
        }
        return false;
    }

    // 模块7：总长一致性校验。实际包长应等于头长 + 负载长。
    const qint64 expectedSize = static_cast<qint64>(headerLength + payloadLength);
    if (expectedSize != packet.size())
    {
        if (error)
        {
            *error = QStringLiteral("payload length mismatch");
        }
        return false;
    }

    // 模块8：提取负载并做校验和比对，检测数据损坏或错位。
    const QByteArray payload = packet.mid(headerLength, static_cast<int>(payloadLength));
    if (checksum(payload) != payloadChecksum)
    {
        if (error)
        {
            *error = QStringLiteral("checksum mismatch");
        }
        return false;
    }

    // 模块9：所有校验通过后回填输出对象，完成解码。
    outMessage->type = static_cast<protocol::MessageType>(typeRaw);
    outMessage->flags = flags;
    outMessage->sessionId = sessionId;
    outMessage->sequence = sequence;
    outMessage->payload = payload;
    return true;
}

quint32 MessageCodec::checksum(const QByteArray &bytes)
{
    quint32 value = 0;
    for (char byte : bytes)
    {
        value = (value * 131) + static_cast<quint8>(byte);
    }
    return value;
}
