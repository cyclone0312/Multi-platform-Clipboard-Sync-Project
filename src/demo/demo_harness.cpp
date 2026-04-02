#include "demo/demo_harness.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#endif

namespace demo
{

    using file_transfer::Action;
    using file_transfer::ActionType;
    using file_transfer::FileMeta;
    using file_transfer::FileOffer;
    using file_transfer::IncomingFileAbort;
    using file_transfer::IncomingFileChunk;
    using file_transfer::IncomingFileComplete;
    using file_transfer::IncomingFileRequest;

    DemoHarness::DemoHarness()
        : m_sender(&m_senderSource, m_config), m_receiver(&m_receiverSource, m_config)
    {
    }

    bool DemoHarness::run(const std::string &inputPath, const std::string &outputRoot)
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
        // 这三行把传输流水线启动了起来：
        // 先把文件清单登记到状态机里，模拟发送端和接收端分别拿到这个“目录”，然后接收端选最新的目录开始下载。
        //  发送端先把“库存表”登记好 把可提供的文件清单记到发送端状态里（m_localOffers）。
        processSender(m_sender.registerLocalOffer(offer));
        // 把同一份 offer 记到接收端状态里（m_remoteOffers）。你可以理解为：接收端拿到“可下载目录”。
        processReceiver(m_receiver.onRemoteFileOffer(offer));
        // 它会选最新 offer，进入 beginDownload，再立刻调度第一窗请求。  点火
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

    std::string DemoHarness::joinPath(const std::string &left, const std::string &right)
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

    std::string DemoHarness::baseName(const std::string &path)
    {
        const std::size_t pos = path.find_last_of("/\\");
        return pos == std::string::npos ? path : path.substr(pos + 1);
    }

    bool DemoHarness::pathExists(const std::string &path)
    {
        struct stat info;
        return stat(path.c_str(), &info) == 0;
    }

    void DemoHarness::ensureDirectory(const std::string &path)
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

    std::int64_t DemoHarness::fileSize(const std::string &path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        return input ? static_cast<std::int64_t>(input.tellg()) : -1;
    }

    bool DemoHarness::copyFile(const std::string &source, const std::string &target)
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

    void DemoHarness::createSampleFile(const std::string &path) const
    {
        std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 8192; ++i)
        {
            const unsigned char byte = static_cast<unsigned char>((i * 37) & 0xFF);
            output.write(reinterpret_cast<const char *>(&byte), 1);
        }
    }

    void DemoHarness::processSender(file_transfer::StepResult result)
    {
        processActions(result, true);
    }

    void DemoHarness::processReceiver(file_transfer::StepResult result)
    {
        processActions(result, false);
    }

    void DemoHarness::processActions(const file_transfer::StepResult &result, const bool fromSender)
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
                IncomingFileRequest request;
                request.sessionId = action.sessionId;
                request.fileId = action.fileId;
                request.requestId = action.requestId;
                request.offset = action.offset;
                request.length = action.length;
                // 发送端收到请求后，直接调用状态机接口处理这个请求，状态机会产出响应这个请求的动作（SendFileChunk 或 SendFileAbort）。
                processSender(m_sender.onRemoteFileRequest(request));
                break;
            }
            case ActionType::SendFileChunk:
            {
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

    std::int64_t DemoHarness::tick()
    {
        m_nowMs += 5;
        return m_nowMs;
    }

    std::string DemoHarness::parentDir(const std::string &path)
    {
        const std::size_t pos = path.find_last_of("/\\");
        return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
    }

} // namespace demo
