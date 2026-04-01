#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#endif

#include "core/file_transfer_state_machine.h"

namespace
{

using Clock = std::chrono::system_clock;
using file_transfer::Action;
using file_transfer::ActionType;
using file_transfer::FileMeta;
using file_transfer::FileOffer;
using file_transfer::FileTransferStateMachine;
using file_transfer::IFileTransferDataSource;
using file_transfer::IncomingFileAbort;
using file_transfer::IncomingFileChunk;
using file_transfer::IncomingFileComplete;
using file_transfer::IncomingFileRequest;
using file_transfer::StepResult;
using file_transfer::TransferConfig;

class DemoDataSource : public IFileTransferDataSource
{
public:
    // Sender (发送端) 侧通过这个接口，根据请求窗口给出的 offset(起始位置) 和 length(长度) 
    // 直接从源文件读取要发送的数据块。这样状态机本身就不需要直接管理文件流的开启和关闭。
    bool readSourceBytes(const FileMeta &meta,
                         const std::int64_t offset,
                         const std::size_t length,
                         std::vector<std::uint8_t> *outBytes,
                         std::string *error) override
    {
        if (outBytes == nullptr)
        {
            if (error != nullptr)
            {
                *error = "output buffer is null";
            }
            return false;
        }

        std::ifstream input(meta.path, std::ios::binary);
        if (!input)
        {
            if (error != nullptr)
            {
                *error = "failed to open source file";
            }
            return false;
        }

        input.seekg(offset);
        if (!input)
        {
            if (error != nullptr)
            {
                *error = "failed to seek source file";
            }
            return false;
        }

        outBytes->assign(length, 0);
        input.read(reinterpret_cast<char *>(outBytes->data()), static_cast<std::streamsize>(length));
        const std::streamsize count = input.gcount();
        if (count < 0)
        {
            if (error != nullptr)
            {
                *error = "failed to read source file";
            }
            return false;
        }

        outBytes->resize(static_cast<std::size_t>(count));
        return true;
    }

    // demo 里用递增字符串模拟 requestId，方便观察请求窗口的推进。
    std::string makeRequestId() override
    {
        ++m_counter;
        return "req-" + std::to_string(m_counter);
    }

private:
    std::uint64_t m_counter = 0;
};

class DemoHarness
{
public:
    DemoHarness()
        : m_sender(&m_senderSource, m_config), m_receiver(&m_receiverSource, m_config)
    {
    }

    bool run(const std::string &inputPath = {}, const std::string &outputRoot = {})
    {
        const std::string root = outputRoot.empty() ? "build/pure_cpp_demo_runtime/" + std::to_string(m_nowMs) : outputRoot;
        const std::string senderDir = joinPath(root, "sender");
        const std::string receiverDir = joinPath(root, "receiver");
        ensureDirectory(senderDir);
        ensureDirectory(receiverDir);
        m_publishedPaths.clear();
        m_trace.clear();

        std::string sourcePath;
        if (inputPath.empty())
        {
            sourcePath = joinPath(senderDir, "sample_payload.bin");
            createSampleFile(sourcePath);
        }
        else
        {
            if (!pathExists(inputPath))
            {
                std::cerr << "Input file does not exist: " << inputPath << '\n';
                return false;
            }

            sourcePath = inputPath;
        }

        const std::uint64_t sessionId = 0x20260401ull;
        FileOffer offer;
        offer.sessionId = sessionId;
        offer.receivedAtMs = m_nowMs;

        // offer 只描述“我要传什么文件”，真正的文件字节会在后续 FileRequest/FileChunk 阶段传输。
        FileMeta meta;
        meta.fileId = "0";
        meta.path = sourcePath;
        meta.name = baseName(sourcePath);
        meta.size = fileSize(sourcePath);
        if (meta.size < 0)
        {
            std::cerr << "Failed to read input file size: " << sourcePath << '\n';
            return false;
        }
        meta.mtimeMs = m_nowMs;
        meta.sha256 = file_transfer::sha256FileHex(meta.path);
        if (meta.sha256.empty())
        {
            std::cerr << "Failed to compute input file SHA256: " << sourcePath << '\n';
            return false;
        }
        offer.files.push_back(meta);

        // ==== 文件传输核心流程 ====
        // 1. 发送端 (Sender)：登记本地可发送文件（告诉它自己的状态机“我准备好被拉取了”）。
        // 2. 接收端 (Receiver)：收到远端发来的 FileOffer 元数据信令（通常通过外部信道传输）。
        // 3. 接收端 (Receiver)：主动发起文件拉取任务，状态机开始规划滑动窗口请求。
        processSender(m_sender.registerLocalOffer(offer));
        processReceiver(m_receiver.onRemoteFileOffer(offer));
        // 开始下载后，状态机会产出 ActionType::SendFileRequest，通过 processActions 喂给 Sender，从而启动传输流水线。
        processReceiver(m_receiver.beginLatestDownload(m_nowMs, receiverDir));

        if (m_publishedPaths.size() != 1)
        {
            std::cerr << "Expected 1 completed file, got " << m_publishedPaths.size() << '\n';
            return false;
        }

        const std::string outputPath = m_publishedPaths.front();
        if (!pathExists(outputPath))
        {
            std::cerr << "Receiver output file does not exist: " << outputPath << '\n';
            return false;
        }

        const std::string sourceSha = file_transfer::sha256FileHex(sourcePath);
        const std::string outputSha = file_transfer::sha256FileHex(outputPath);
        if (sourceSha.empty() || outputSha.empty() || sourceSha != outputSha)
        {
            std::cerr << "SHA256 mismatch after transfer\n";
            return false;
        }

        std::cout << "Pure C++ file transfer demo succeeded\n";
        std::cout << "Source : " << sourcePath << '\n';
        std::cout << "Output : " << outputPath << '\n';
        std::cout << "SHA256 : " << outputSha << '\n';
        std::cout << "Trace:\n";
        for (const std::string &line : m_trace)
        {
            std::cout << "  " << line << '\n';
        }

        return true;
    }

private:
    static std::string joinPath(const std::string &left, const std::string &right)
    {
        if (left.empty())
        {
            return right;
        }
        const char tail = left.back();
        if (tail == '/' || tail == '\\')
        {
            return left + right;
        }
        return left + "/" + right;
    }

    static std::string baseName(const std::string &path)
    {
        const std::size_t pos = path.find_last_of("/\\");
        return pos == std::string::npos ? path : path.substr(pos + 1);
    }

    static bool pathExists(const std::string &path)
    {
        struct stat info;
        return stat(path.c_str(), &info) == 0;
    }

    static void ensureDirectory(const std::string &path)
    {
        if (path.empty() || path == "." || pathExists(path))
        {
            return;
        }

        const std::size_t split = path.find_last_of("/\\");
        if (split != std::string::npos)
        {
            ensureDirectory(path.substr(0, split));
        }

#if defined(_WIN32)
        _mkdir(path.c_str());
#else
        mkdir(path.c_str(), 0755);
#endif
    }

    static std::int64_t fileSize(const std::string &path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        return input ? static_cast<std::int64_t>(input.tellg()) : -1;
    }

    static bool copyFile(const std::string &source, const std::string &target)
    {
        std::ifstream input(source.c_str(), std::ios::binary);
        if (!input)
        {
            return false;
        }

        std::ofstream output(target.c_str(), std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return false;
        }

        output << input.rdbuf();
        return static_cast<bool>(output);
    }

    void createSampleFile(const std::string &path) const
    {
        std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 8192; ++i)
        {
            const unsigned char byte = static_cast<unsigned char>((i * 37) & 0xFF);
            output.write(reinterpret_cast<const char *>(&byte), 1);
        }
    }

    void processSender(StepResult result)
    {
        processActions(result, true);
    }

    void processReceiver(StepResult result)
    {
        processActions(result, false);
    }

    // 核心执行器 (Actuator / Side-Effect Handler)：
    // 状态机（FileTransferStateMachine）被设计为纯逻辑的无副作用（Side-Effect Free）组件。
    // 它只产出明确的 Action（动作指令），比如打开文件、写入数据、发送网络包等，它本身不调用操作系统 API。
    // processActions 负责统一解释并“执行”这些动作，从而完成如磁盘 IO 和网络传输等实际产生副作用的操作。
    // 这种设计使得状态机的单元测试可以做到完全脱离底层依赖。
    void processActions(const StepResult &result, const bool fromSender)
    {
        for (const Action &action : result.actions)
        {
            switch (action.type)
            {
            case ActionType::LogStatus:
                m_trace.push_back(action.text);
                break;
            case ActionType::OpenTempFile:
                ensureDirectory(parentDir(action.path));
                {
                    std::ofstream output(action.path.c_str(), std::ios::binary | std::ios::trunc);
                }
                break;
            case ActionType::AppendTempFile:
            {
                std::ofstream output(action.path.c_str(), std::ios::binary | std::ios::app);
                output.write(reinterpret_cast<const char *>(action.bytes.data()),
                             static_cast<std::streamsize>(action.bytes.size()));
                break;
            }
            case ActionType::CommitTempFile:
            {
                ensureDirectory(parentDir(action.targetPath));
                std::remove(action.targetPath.c_str());
                if (!copyFile(action.path, action.targetPath))
                {
                    m_trace.push_back("failed to commit temp file: " + action.path);
                    break;
                }
                std::remove(action.path.c_str());
                break;
            }
            case ActionType::AbortTempFile:
                std::remove(action.path.c_str());
                break;
            case ActionType::SendFileRequest:
            {
                // receiver 发出的“请求窗口”在 demo 里不经过网络，
                // 而是直接投递给 sender 状态机。
                IncomingFileRequest request;
                request.sessionId = action.sessionId;
                request.fileId = action.fileId;
                request.requestId = action.requestId;
                request.offset = action.offset;
                request.length = action.length;
                processSender(m_sender.onRemoteFileRequest(request));
                break;
            }
            case ActionType::SendFileChunk:
            {
                // sender 产生的 chunk 同样直接喂给 receiver，
                // 这就是“假网络闭环”的核心。
                IncomingFileChunk chunk;
                chunk.sessionId = action.sessionId;
                chunk.fileId = action.fileId;
                chunk.requestId = action.requestId;
                chunk.offset = action.offset;
                chunk.chunkCrc32 = action.chunkCrc32;
                chunk.bytes = action.bytes;
                processReceiver(m_receiver.onRemoteFileChunk(chunk, tick()));
                break;
            }
            case ActionType::SendFileComplete:
            {
                IncomingFileComplete complete;
                complete.sessionId = action.sessionId;
                complete.fileId = action.fileId;
                complete.requestId = action.requestId;
                complete.size = action.length;
                complete.sha256 = action.sha256;
                processReceiver(m_receiver.onRemoteFileComplete(complete));
                break;
            }
            case ActionType::SendFileAbort:
            {
                IncomingFileAbort abortMessage;
                abortMessage.sessionId = action.sessionId;
                abortMessage.fileId = action.fileId;
                abortMessage.requestId = action.requestId;
                abortMessage.reason = action.reason;
                processReceiver(m_receiver.onRemoteFileAbort(abortMessage));
                break;
            }
            case ActionType::PublishCompletedFiles:
                m_publishedPaths = action.paths;
                break;
            case ActionType::StartTimer:
            case ActionType::StopTimer:
            case ActionType::None:
                break;
            }
        }

        if (!result.ok)
        {
            const char *side = fromSender ? "sender" : "receiver";
            std::ostringstream stream;
            stream << side << " step failed: " << result.error;
            m_trace.push_back(stream.str());
        }
    }

    std::int64_t tick()
    {
        // demo 用单调递增的假时间驱动超时语义，不依赖真实定时器。
        m_nowMs += 5;
        return m_nowMs;
    }

    static std::string parentDir(const std::string &path)
    {
        const std::size_t pos = path.find_last_of("/\\");
        return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
    }

    TransferConfig m_config = []()
    {
        TransferConfig config;
        // 故意把 chunk/window 配小，这样传一个小文件也能看到多次请求窗口推进。
        config.chunkSizeBytes = 1024;
        config.windowChunks = 3;
        config.requestTimeoutMs = 500;
        config.maxRequestWindowRetries = 2;
        return config;
    }();
    // m_sender 和 m_receiver 模拟两端，但都运行在同一个进程里。
    DemoDataSource m_senderSource;
    DemoDataSource m_receiverSource;
    FileTransferStateMachine m_sender;
    FileTransferStateMachine m_receiver;
    std::int64_t m_nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    std::vector<std::string> m_publishedPaths;
    std::vector<std::string> m_trace;
};

} // namespace

int main(int argc, char **argv)
{
    if (argc >= 2)
    {
        const std::string arg1 = argv[1];
        if (arg1 == "-h" || arg1 == "--help")
        {
            std::cout << "Usage: pure_cpp_file_transfer_demo.exe [input-file] [output-root]\n";
            std::cout << "  input-file  : optional source file path. If omitted, a sample file is generated.\n";
            std::cout << "  output-root : optional output root directory. Receiver files are written under <output-root>/receiver/<sessionId>/.\n";
            return 0;
        }
    }

    std::string inputPath;
    std::string outputRoot;
    if (argc >= 2)
    {
        inputPath = argv[1];
    }
    if (argc >= 3)
    {
        outputRoot = argv[2];
    }

    DemoHarness harness;
    return harness.run(inputPath, outputRoot) ? 0 : 1;
}
