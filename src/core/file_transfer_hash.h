#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace file_transfer
{

// 计算给定字节数组的简单 CRC32 校验和（主要用于网络传输单包数据校验）
std::uint32_t crc32(const std::vector<std::uint8_t> &bytes);

// 读取本地文件并计算完整 SHA-256 哈希，返回十六进制字符串（主要用于发件前或落盘后的安全校验）
std::string sha256FileHex(const std::string &path);

// 渐进式 (Streaming) 计算 SHA256 算法的类封装。
// 允许在收到一个个流式的 FileChunk 分块时增量累加计算，
// 从而避免需要将高达 GB 级别的文件全量加载进内存再一次性计算哈希。
class Sha256
{
public:
    Sha256();

    void reset();                                            // 重置内部状态机到初始点
    void update(const std::uint8_t *data, std::size_t size); // 并入一段新的裸内存数据
    void update(const std::vector<std::uint8_t> &bytes);     // 并入一段新的向量块数据
    std::string finalHex();                                  // 结束计算并直接返回十六进制结果字符串

private:
    void transform(const std::uint8_t block[64]);

    std::uint64_t m_bitCount = 0;
    std::uint32_t m_state[8] = {};
    std::uint8_t m_buffer[64] = {};
    std::size_t m_bufferSize = 0;
};

} // namespace file_transfer
