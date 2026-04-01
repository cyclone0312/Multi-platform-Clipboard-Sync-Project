#include "core/file_transfer_hash.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace file_transfer
{
namespace
{

constexpr std::array<std::uint32_t, 64> kSha256Rounds = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u};

inline std::uint32_t rotr(const std::uint32_t value, const std::uint32_t bits)
{
    return (value >> bits) | (value << (32u - bits));
}

inline std::uint32_t readBigEndian32(const std::uint8_t *data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24u) |
           (static_cast<std::uint32_t>(data[1]) << 16u) |
           (static_cast<std::uint32_t>(data[2]) << 8u) |
           static_cast<std::uint32_t>(data[3]);
}

inline void writeBigEndian32(std::uint8_t *data, const std::uint32_t value)
{
    data[0] = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);
    data[1] = static_cast<std::uint8_t>((value >> 16u) & 0xFFu);
    data[2] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
    data[3] = static_cast<std::uint8_t>(value & 0xFFu);
}

inline void writeBigEndian64(std::uint8_t *data, const std::uint64_t value)
{
    for (int i = 0; i < 8; ++i)
    {
        data[i] = static_cast<std::uint8_t>((value >> ((7 - i) * 8)) & 0xFFu);
    }
}

std::string bytesToHex(const std::uint8_t *data, const std::size_t size)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i)
    {
        stream << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return stream.str();
}

} // namespace

std::uint32_t crc32(const std::vector<std::uint8_t> &bytes)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t byte : bytes)
    {
        crc ^= byte;
        for (int i = 0; i < 8; ++i)
        {
            const std::uint32_t mask = static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1u)));
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

Sha256::Sha256()
{
    reset();
}

void Sha256::reset()
{
    m_bitCount = 0;
    m_bufferSize = 0;
    m_state[0] = 0x6A09E667u;
    m_state[1] = 0xBB67AE85u;
    m_state[2] = 0x3C6EF372u;
    m_state[3] = 0xA54FF53Au;
    m_state[4] = 0x510E527Fu;
    m_state[5] = 0x9B05688Cu;
    m_state[6] = 0x1F83D9ABu;
    m_state[7] = 0x5BE0CD19u;
    std::memset(m_buffer, 0, sizeof(m_buffer));
}

void Sha256::update(const std::uint8_t *data, std::size_t size)
{
    if (data == nullptr || size == 0)
    {
        return;
    }

    m_bitCount += static_cast<std::uint64_t>(size) * 8u;

    std::size_t offset = 0;
    while (offset < size)
    {
        const std::size_t copySize = std::min<std::size_t>(sizeof(m_buffer) - m_bufferSize, size - offset);
        std::memcpy(m_buffer + m_bufferSize, data + offset, copySize);
        m_bufferSize += copySize;
        offset += copySize;

        if (m_bufferSize == sizeof(m_buffer))
        {
            transform(m_buffer);
            m_bufferSize = 0;
        }
    }
}

void Sha256::update(const std::vector<std::uint8_t> &bytes)
{
    update(bytes.data(), bytes.size());
}

std::string Sha256::finalHex()
{
    std::uint8_t block[64] = {};
    std::memcpy(block, m_buffer, m_bufferSize);
    block[m_bufferSize] = 0x80u;

    if (m_bufferSize >= 56)
    {
        transform(block);
        std::memset(block, 0, sizeof(block));
    }

    writeBigEndian64(block + 56, m_bitCount);
    transform(block);

    std::uint8_t digest[32] = {};
    for (int i = 0; i < 8; ++i)
    {
        writeBigEndian32(digest + (i * 4), m_state[i]);
    }

    const std::string hex = bytesToHex(digest, sizeof(digest));
    reset();
    return hex;
}

void Sha256::transform(const std::uint8_t block[64])
{
    std::uint32_t w[64] = {};
    for (int i = 0; i < 16; ++i)
    {
        w[i] = readBigEndian32(block + (i * 4));
    }
    for (int i = 16; i < 64; ++i)
    {
        const std::uint32_t s0 = rotr(w[i - 15], 7u) ^ rotr(w[i - 15], 18u) ^ (w[i - 15] >> 3u);
        const std::uint32_t s1 = rotr(w[i - 2], 17u) ^ rotr(w[i - 2], 19u) ^ (w[i - 2] >> 10u);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = m_state[0];
    std::uint32_t b = m_state[1];
    std::uint32_t c = m_state[2];
    std::uint32_t d = m_state[3];
    std::uint32_t e = m_state[4];
    std::uint32_t f = m_state[5];
    std::uint32_t g = m_state[6];
    std::uint32_t h = m_state[7];

    for (int i = 0; i < 64; ++i)
    {
        const std::uint32_t s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + kSha256Rounds[static_cast<std::size_t>(i)] + w[i];
        const std::uint32_t s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

std::string sha256FileHex(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }

    Sha256 hash;
    std::vector<std::uint8_t> buffer(1024 * 1024);
    while (input)
    {
        input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0)
        {
            hash.update(buffer.data(), static_cast<std::size_t>(count));
        }
    }

    if (!input.eof())
    {
        return {};
    }

    return hash.finalHex();
}

} // namespace file_transfer
