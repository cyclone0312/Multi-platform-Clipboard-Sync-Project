#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace protocol
{

    // 协议魔数：ASCII "CSYN" 的小端整数表示。
    constexpr quint32 kMagic = 0x4353594E; // CSYN
    // 头部结构版本号。
    constexpr quint16 kVersion = 1;
    // 当前固定头部长度（字节）。
    constexpr quint16 kHeaderLength = 44;

    enum class MessageType : quint16
    {
        // UTF-8 纯文本剪贴板内容。
        TextPlain = 0x0001,
        // 预留：HTML 剪贴板内容。
        TextHtml = 0x0002,
        // 预留：RTF 剪贴板内容。
        TextRtf = 0x0003,
        // 预留：位图/图片负载。
        ImageBitmap = 0x0004,
        // 预留：流式传输开始帧。
        StreamStart = 0x0010,
        // 预留：流式传输分片帧。
        StreamChunk = 0x0011,
        // 预留：流式传输结束帧。
        StreamEnd = 0x0012,
        // 预留：手动注入剪贴板命令。
        ClipboardInject = 0x0020,
        // 文件复制声明（仅元信息，不含文件字节）。
        FileOffer = 0x0030,
        // 文件按需读取请求（支持 offset + length）。
        FileRequest = 0x0031,
        // 文件分块数据帧。
        FileChunk = 0x0032,
        // 文件传输结束通知。
        FileComplete = 0x0033,
        // 文件传输中止通知。
        FileAbort = 0x0034,
        // 正向确认。
        Ack = 0x00F0,
        // 负向确认。
        Nack = 0x00F1,
        // 错误通知。
        Error = 0x00FF
    };

    struct ClipboardMessage
    {
        // 消息类型。
        MessageType type = MessageType::TextPlain;
        // 位标志，预留给后续协议选项。
        quint16 flags = 0;
        // 一次同步动作的会话标识。
        quint64 sessionId = 0;
        // 会话内单调递增序号。
        quint64 sequence = 0;
        // 原始负载字节。
        QByteArray payload;
    };

} // namespace protocol
