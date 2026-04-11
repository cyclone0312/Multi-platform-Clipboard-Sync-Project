#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace file_transfer
{

// 文件的元数据信息，通常作为 FileOffer (握手信令) 的一部分，
// 在传输通道建立之初告知对方。
struct FileMeta
{
    std::string fileId;       // 该文件在会话中的唯一字符串标识 (例如: 基于路径计算的值或 UUID)
    std::string path;         // 发送端侧存放该文件的绝对或相对路径 (接收端不需要用，且往往收不到它)
    std::string name;         // 文件对外的展示名称或相对路径名 (如 "photo.jpg")
    std::int64_t size = 0;    // 文件的总字节大小
    std::int64_t mtimeMs = 0; // 文件的最后修改时间戳 (毫秒)
    std::string sha256;       // 整个文件的 SHA-256 哈希值文本，接收端据此执行安全落盘校验
};

// 发送方向接收方通过带外信道或握手信令展示的“可供下载文件明细清单”。
struct FileOffer
{
    std::uint64_t sessionId = 0;       // 唯一标识这批次传输意图的会话 ID
    std::int64_t receivedAtMs = 0;     // 接收端本地在何年何时接收到这笔 Offer，用于挑选最新会话
    std::vector<FileMeta> files;       // 该批次一共包含哪些文件，以及文件特征
};

// 接收端用于跟踪下载进度的有状态运行时模型。
// 该类的生命周期仅在一次具体的 Session 会话下载进行中。
struct DownloadState
{
    std::uint64_t sessionId = 0;       // 当前关联的、正处于下载状态的会话 ID
    std::size_t fileIndex = 0;         // 当前下载到清单中的第几个文件
    
    // --- 滑动窗口通讯状态 ---
    std::int64_t nextOffset = 0;       // 接收端当前极度期盼收到的严密递增字节边界 (如果不匹配即被视为旧包丢弃)
    std::int64_t requestedLength = 0;  // 当前向发送端发起的那一轮窗口请求的字节目标总量
    std::int64_t receivedInWindow = 0; // 当前一轮窗口内，已经收复落实的字节数量
    std::string requestId;             // 用于这轮防串包验证及响应关系匹配的唯一随机 ID
    
    // --- 附属状态 ---
    std::string localPath;             // 下载即将落脚的本端目标物理路径
    int retryCount = 0;                // 在这个窗口因意外发生超时的累加重试次数
    bool pausedByDisconnect = false;   // 标志目前的任务是否归因于链路中断处于暂时休眠挂起状态
    std::int64_t deadlineMs = 0;       // 时钟扫描判断超时的过期死线 (毫秒)
};

// 微调传输行为的配置与策略。
struct TransferConfig
{
    std::size_t chunkSizeBytes = 512 * 1024; // 发送端每次向网络塞入发信的最长单切片限制 (默认值 512KB)
    std::size_t windowChunks = 16;           // 一次请求由几个切片窗口聚合而成 (总窗长 = chunks * 16 ，即默认 8MB)
    int requestTimeoutMs = 8000;             // 单段窗口允许沉默的最大毫秒数，若超过此时间没有新分段抵达则判断超时
    int maxRequestWindowRetries = 3;         // 当持续超时时允许重新要求重发的阈值容错次数
};

// ====== 以下为双方网络信道上传递的内容包体结构 (Protocol Payloads) ======

// 接收端主动发起的请求包：“麻烦将这份数据发送给我”。
struct IncomingFileRequest
{
    std::uint64_t sessionId = 0; // 会话标记
    std::string fileId;          // 目标对象 ID
    std::string requestId;       // 这轮拉取防串包和建立应答映射的 RequestId
    std::int64_t offset = 0;     // 希望索求对象的物理起始索点
    std::int64_t length = 0;     // 从上面起点开始一共渴望收到多长的切片群
};

// 真正承载原版文件实体的单块分包。
struct IncomingFileChunk
{
    std::uint64_t sessionId = 0; 
    std::string fileId;
    std::string requestId;         // 承载了它是为了回应哪一次的请求
    std::int64_t offset = 0;       // 本包起步相较于该源文件的绝对偏移边界
    std::uint32_t chunkCrc32 = 0;  // 这批纯粹流数据的网络传递完整性 CRC (不用于取代全文件 SHA256)
    std::vector<std::uint8_t> bytes; // 这块分片的真实二进制内容载荷
};

// 单个文件结束下发时的收尾确认通知包。
struct IncomingFileComplete
{
    std::uint64_t sessionId = 0;
    std::string fileId;
    std::string requestId;
    std::int64_t size = 0;         // 源文件的总长度保证 
    std::string sha256;            // 用来防止篡改的全文件总哈希特征符
};

// 一旦遭遇对端不可逆中断（如读不出文件、权限受封、或由于超时等自行断联抛出的错误包）的差错帧。
struct IncomingFileAbort
{
    std::uint64_t sessionId = 0;
    std::string fileId;
    std::string requestId;
    std::string reason;            // 记录下差错产生的原因诊断明细文本
};

} // namespace file_transfer
