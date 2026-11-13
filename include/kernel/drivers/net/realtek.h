#pragma once

#include <cstdint>

namespace Kernel::Drivers::Net::Realtek {
struct PciDevice {
  uint16_t io_base;
  uint8_t bus;
  uint8_t slot;
  uint8_t function;
  uint16_t device_id;
};

bool find_io_device(const uint16_t *device_ids, unsigned count,
                    PciDevice *result);
} // namespace Kernel::Drivers::Net::Realtek
