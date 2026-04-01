#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace file_transfer
{

std::uint32_t crc32(const std::vector<std::uint8_t> &bytes);
std::string sha256FileHex(const std::string &path);

class Sha256
{
public:
    Sha256();

    void reset();
    void update(const std::uint8_t *data, std::size_t size);
    void update(const std::vector<std::uint8_t> &bytes);
    std::string finalHex();

private:
    void transform(const std::uint8_t block[64]);

    std::uint64_t m_bitCount = 0;
    std::uint32_t m_state[8] = {};
    std::uint8_t m_buffer[64] = {};
    std::size_t m_bufferSize = 0;
};

} // namespace file_transfer
