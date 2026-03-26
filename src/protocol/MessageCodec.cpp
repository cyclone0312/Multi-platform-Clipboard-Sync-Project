#include "protocol/MessageCodec.h"

#include <QDataStream>

QByteArray MessageCodec::encode(const protocol::ClipboardMessage &message)
{
    QByteArray bytes;
    bytes.reserve(protocol::kHeaderLength + message.payload.size());

    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << protocol::kMagic;
    stream << protocol::kVersion;
    stream << protocol::kHeaderLength;
    stream << static_cast<quint16>(message.type);
    stream << message.flags;
    stream << message.sessionId;
    stream << message.sequence;
    stream << static_cast<quint64>(message.payload.size());
    stream << checksum(message.payload);
    stream << static_cast<quint32>(0);

    bytes.append(message.payload);
    return bytes;
}

bool MessageCodec::decode(const QByteArray &packet, protocol::ClipboardMessage *outMessage, QString *error)
{
    if (!outMessage)
    {
        if (error)
        {
            *error = QStringLiteral("output message pointer is null");
        }
        return false;
    }

    if (packet.size() < protocol::kHeaderLength)
    {
        if (error)
        {
            *error = QStringLiteral("packet shorter than header");
        }
        return false;
    }

    QDataStream stream(packet);
    stream.setByteOrder(QDataStream::LittleEndian);

    quint32 magic = 0;
    quint16 version = 0;
    quint16 headerLength = 0;
    quint16 typeRaw = 0;
    quint16 flags = 0;
    quint64 sessionId = 0;
    quint64 sequence = 0;
    quint64 payloadLength = 0;
    quint32 payloadChecksum = 0;
    quint32 reserved = 0;

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

    Q_UNUSED(reserved)

    if (magic != protocol::kMagic)
    {
        if (error)
        {
            *error = QStringLiteral("invalid magic");
        }
        return false;
    }

    if (version != protocol::kVersion)
    {
        if (error)
        {
            *error = QStringLiteral("unsupported version");
        }
        return false;
    }

    if (headerLength < protocol::kHeaderLength || packet.size() < headerLength)
    {
        if (error)
        {
            *error = QStringLiteral("invalid header length");
        }
        return false;
    }

    const qint64 expectedSize = static_cast<qint64>(headerLength + payloadLength);
    if (expectedSize != packet.size())
    {
        if (error)
        {
            *error = QStringLiteral("payload length mismatch");
        }
        return false;
    }

    const QByteArray payload = packet.mid(headerLength, static_cast<int>(payloadLength));
    if (checksum(payload) != payloadChecksum)
    {
        if (error)
        {
            *error = QStringLiteral("checksum mismatch");
        }
        return false;
    }

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
