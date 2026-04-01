#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace file_transfer
{

struct FileMeta
{
    std::string fileId;
    std::string path;
    std::string name;
    std::int64_t size = 0;
    std::int64_t mtimeMs = 0;
    std::string sha256;
};

struct FileOffer
{
    std::uint64_t sessionId = 0;
    std::int64_t receivedAtMs = 0;
    std::vector<FileMeta> files;
};

struct DownloadState
{
    std::uint64_t sessionId = 0;
    std::size_t fileIndex = 0;
    std::int64_t nextOffset = 0;
    std::int64_t requestedLength = 0;
    std::int64_t receivedInWindow = 0;
    std::string requestId;
    std::string localPath;
    int retryCount = 0;
    bool pausedByDisconnect = false;
    std::int64_t deadlineMs = 0;
};

struct TransferConfig
{
    std::size_t chunkSizeBytes = 512 * 1024;
    std::size_t windowChunks = 16;
    int requestTimeoutMs = 8000;
    int maxRequestWindowRetries = 3;
};

struct IncomingFileRequest
{
    std::uint64_t sessionId = 0;
    std::string fileId;
    std::string requestId;
    std::int64_t offset = 0;
    std::int64_t length = 0;
};

struct IncomingFileChunk
{
    std::uint64_t sessionId = 0;
    std::string fileId;
    std::string requestId;
    std::int64_t offset = 0;
    std::uint32_t chunkCrc32 = 0;
    std::vector<std::uint8_t> bytes;
};

struct IncomingFileComplete
{
    std::uint64_t sessionId = 0;
    std::string fileId;
    std::string requestId;
    std::int64_t size = 0;
    std::string sha256;
};

struct IncomingFileAbort
{
    std::uint64_t sessionId = 0;
    std::string fileId;
    std::string requestId;
    std::string reason;
};

} // namespace file_transfer
