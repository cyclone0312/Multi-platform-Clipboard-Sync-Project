#include "demo/demo_data_source.h"

#include <fstream>

namespace demo
{
    // 给定文件 + 偏移量 + 长度”从源文件里读一段二进制数据，放进 outBytes，供发送端切 chunk 后发出去。
    bool DemoDataSource::readSourceBytes(const file_transfer::FileMeta &meta,
                                         const std::int64_t offset,
                                         const std::size_t length,
                                         std::vector<std::uint8_t> *outBytes,
                                         std::string *error)
    {
        if (outBytes == nullptr)
        {
            if (error != nullptr)
            {
                *error = "output buffer is null";
            }
            return false;
        }

        // 打开源文件（二进制模式）
        std::ifstream input(meta.path, std::ios::binary);
        if (!input)
        {
            if (error != nullptr)
            {
                *error = "failed to open source file";
            }
            return false;
        }

        // 跳到指定偏移量
        input.seekg(offset);
        if (!input)
        {
            if (error != nullptr)
            {
                *error = "failed to seek source file";
            }
            return false;
        }

        // 申请读取缓冲区并读取数据。这里先把 outBytes 扩容到请求的长度，
        // 然后直接把数据读到 outBytes 的内存里。读完后根据实际读取的字节数调整 outBytes 的大小。
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
        // 把 outBytes 缩到真实大小
        outBytes->resize(static_cast<std::size_t>(count));
        return true;
    }

    std::string DemoDataSource::makeRequestId()
    {
        ++m_counter;
        return "req-" + std::to_string(m_counter);
    }

} // namespace demo
