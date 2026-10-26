#pragma once
#include <stdint.h>

namespace Kernel::Drivers {
    class ATADriver {
    public:
        static void init();
        static bool read_sectors(uint32_t lba, uint8_t sector_count, void* buffer);
        static bool write_sectors(uint32_t lba, uint8_t sector_count, const void* buffer);
    };
}