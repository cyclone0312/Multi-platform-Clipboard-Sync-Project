#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace file_transfer
{
    // 定义状态机对外输出的动作类型 (ActionType)。
    // 状态机本身不包含执行能力，它的所有决策都以 Action 形式抛出，交由外层的执行器 (Actuator) 来引发真正的副作用。
    enum class ActionType
    {
        None,                 // 无操作
        SendFileRequest,      // 向远端发送数据请求窗口 (请求对方发来特定范围的 Chunk)
        SendFileChunk,        // 向远端发送文件切片 (本端作为 Sender 时响应对方的 Request)
        SendFileComplete,     // 向远端发送文件结束标志
        SendFileAbort,        // 向远端发送传输中止错误
        OpenTempFile,         // 在本地磁盘开启临时后缀 (.part) 文件，准备写入
        AppendTempFile,       // 往本地磁盘的临时文件中追加数据块
        CommitTempFile,       // 将本地临时文件重命名/移动到目标正式路径，完成下载
        AbortTempFile,        // 丢弃并删除本地的临时破损文件
        StartTimer,           // 启动重试或超时计时器
        StopTimer,            // 停止当前所有有效计时器
        LogStatus,            // 状态机希望打印的上下文日志或错误信息
        PublishCompletedFiles // 通知外层本次 Session 的所有文件已经全部下载并验证落盘完毕
    };

    // 状态机抛出的通用动作结构体。
    // 为了简化设计，各种可能用到的属性全被平铺到了这个 struct 中，不同 ActionType 根据自身需要读取或写入对应的字段。
    struct Action
    {
        ActionType type = ActionType::None; // 具体的动作类型

        // --- 上下文标识 ---
        std::uint64_t sessionId = 0; // 会话 ID (标识本次批量传输任务)
        std::size_t fileIndex = 0;   // 文件在当前批次中的序号索引
        std::string fileId;          // 文件的全局标识字符串
        std::string requestId;       // 窗口请求 ID，用于防串包和乱序校验

        // --- 本地 IO 属性 ---
        std::string path;       // 文件操作源路径，或临时文件路径
        std::string targetPath; // 文件重命名或拷贝的目标正式路径

        // --- 诊断与元信息 ---
        std::string text;   // 状态日志文本
        std::string reason; // 失败、中断原因等描述文字
        std::string sha256; // 需要被校验或已计算出的 SHA256 哈希值

        // --- 数据与滑动窗口 ---
        std::int64_t offset = 0;         // 当前操作的字节流绝对偏移量起始点
        std::int64_t length = 0;         // 本次需要读取或发送的长度
        std::int64_t timeoutMs = 0;      // 计时器触发的超时判定时间 (毫秒)
        std::uint32_t chunkCrc32 = 0;    // 分块的网络传输 CRC 校验码
        std::vector<std::uint8_t> bytes; // 要被写入磁盘或发送到网络的具体二进制片段

        // --- 批量成果 ---
        std::vector<std::string> paths; // 下载完全收官后，本次交付的所有最终文件路径
    };

    // 此结构体作为状态机主要接口的返回值。
    // 它封装了一次状态转移计算中所产生的“所有副作用指令”。
    struct StepResult
    {
        bool ok = true;              // 这一步计算是否成功，如果产生内部逻辑无法包容的异常则为 false
        std::string error;           // 当 ok 为 false 时的错误简要说明
        std::vector<Action> actions; // 需要外部 Actuator 去线性执行的动作序列

        void add(Action action)
        {
            actions.push_back(std::move(action));
        }

        // merge 的作用就是把调用merge的StepResult和merge函数里面的StepResult other两个 StepResult 拼起来：状态拼、错误拼、动作列表拼。
        void merge(StepResult other)
        {
            ok = ok && other.ok;
            if (error.empty() && !other.error.empty())
            {
                error = std::move(other.error);
            }
            for (Action &action : other.actions)
            {
                actions.push_back(std::move(action));
            }
        }
    };

} // namespace file_transfer
