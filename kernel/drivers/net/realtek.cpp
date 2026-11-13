#include <kernel/drivers/net/realtek.h>

#include <kernel/drivers/pci.h>

namespace Kernel::Drivers::Net::Realtek {
bool find_io_device(const uint16_t *device_ids, unsigned count,
                    PciDevice *result) {
  if (!device_ids || !count || !result)
    return false;
  for (uint16_t bus = 0; bus < PCI_MAX_BUSES; ++bus) {
    for (uint8_t slot = 0; slot < PCI_MAX_SLOTS; ++slot) {
      for (uint8_t function = 0; function < PCI_MAX_FUNCTIONS; ++function) {
        const uint32_t id =
            pci_read_config(static_cast<uint8_t>(bus), slot, function, 0);
        if ((id & 0xFFFF) != 0x10EC)
          continue;
        const uint16_t device_id = static_cast<uint16_t>(id >> 16);
        bool supported = false;
        for (unsigned i = 0; i < count; ++i)
          supported |= device_ids[i] == device_id;
        if (!supported)
          continue;

        const uint32_t bar =
            pci_read_config(static_cast<uint8_t>(bus), slot, function, 0x10);
        if (!(bar & 1) || !(bar & ~3u))
          continue;
        uint16_t command =
            pci_read_config16(static_cast<uint8_t>(bus), slot, function, 4);
        pci_write_config(static_cast<uint8_t>(bus), slot, function, 4,
                         (command | 5u) & 0xFFFFu);
        result->io_base = static_cast<uint16_t>(bar & ~3u);
        result->bus = static_cast<uint8_t>(bus);
        result->slot = slot;
        result->function = function;
        result->device_id = device_id;
        return true;
      }
    }
  }
  return false;
}
} // namespace Kernel::Drivers::Net::Realtek
