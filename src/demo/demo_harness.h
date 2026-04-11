#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "core/file_transfer_state_machine.h"
#include "demo/demo_data_source.h"

namespace demo
{

    class DemoHarness
    {
    public:
        DemoHarness();

        bool run(const std::string &inputPath = {}, const std::string &outputRoot = {});

    private:
        using Clock = std::chrono::system_clock;

        static std::string joinPath(const std::string &left, const std::string &right);
        static std::string baseName(const std::string &path);
        static bool pathExists(const std::string &path);
        static void ensureDirectory(const std::string &path);
        static std::int64_t fileSize(const std::string &path);
        static bool copyFile(const std::string &source, const std::string &target);
        static std::string parentDir(const std::string &path);

        void createSampleFile(const std::string &path) const;
        void processSender(file_transfer::StepResult result);
        void processReceiver(file_transfer::StepResult result);
        void processActions(const file_transfer::StepResult &result, bool fromSender);
        std::int64_t tick();

        file_transfer::TransferConfig m_config = []()
        {
            file_transfer::TransferConfig config;
            config.chunkSizeBytes = 1024;
            config.windowChunks = 3;
            config.requestTimeoutMs = 500;
            config.maxRequestWindowRetries = 2;
            return config;
        }();

        DemoDataSource m_senderSource;
        DemoDataSource m_receiverSource;
        file_transfer::FileTransferStateMachine m_sender;
        file_transfer::FileTransferStateMachine m_receiver;
        std::int64_t m_nowMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        std::vector<std::string> m_publishedPaths;
        std::vector<std::string> m_trace;
    };

} // namespace demo
