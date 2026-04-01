#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace file_transfer
{
    // 它定义状态机不会直接做事，而是输出动作让外层执行。
    enum class ActionType
    {
        None,
        SendFileRequest,
        SendFileChunk,
        SendFileComplete,
        SendFileAbort,
        OpenTempFile,
        AppendTempFile,
        CommitTempFile,
        AbortTempFile,
        StartTimer,
        StopTimer,
        LogStatus,
        PublishCompletedFiles
    };

    struct Action
    {
        ActionType type = ActionType::None;
        std::uint64_t sessionId = 0;
        std::size_t fileIndex = 0;
        std::string fileId;
        std::string requestId;
        std::string path;
        std::string targetPath;
        std::string text;
        std::string reason;
        std::string sha256;
        std::int64_t offset = 0;
        std::int64_t length = 0;
        std::int64_t timeoutMs = 0;
        std::uint32_t chunkCrc32 = 0;
        std::vector<std::uint8_t> bytes;
        std::vector<std::string> paths;
    };

    struct StepResult
    {
        bool ok = true;
        std::string error;
        std::vector<Action> actions;

        void add(Action action)
        {
            actions.push_back(std::move(action));
        }

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
