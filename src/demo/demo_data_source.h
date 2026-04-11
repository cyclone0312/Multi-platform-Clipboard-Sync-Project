#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/file_transfer_state_machine.h"

namespace demo
{

    class DemoDataSource : public file_transfer::IFileTransferDataSource
    {
    public:
        bool readSourceBytes(const file_transfer::FileMeta &meta,
                             std::int64_t offset,
                             std::size_t length,
                             std::vector<std::uint8_t> *outBytes,
                             std::string *error) override;

        std::string makeRequestId() override;

    private:
        std::uint64_t m_counter = 0;
    };

} // namespace demo
