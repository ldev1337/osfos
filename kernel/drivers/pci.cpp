#include <kernel/cpu.h>
#include <kernel/drivers/pci.h>

namespace {
uint32_t config_address(uint8_t bus, uint8_t slot, uint8_t function,
                        uint8_t offset) {
  return 0x80000000u | (static_cast<uint32_t>(bus) << 16) |
         (static_cast<uint32_t>(slot) << 11) |
         (static_cast<uint32_t>(function) << 8) | (offset & 0xFCu);
}
} // namespace

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t function,
                         uint8_t offset) {
  outl(PCI_CONFIG_ADDRESS,
       config_address(bus, slot, function, offset));
  return inl(PCI_CONFIG_DATA);
}

void pci_write_config(uint8_t bus, uint8_t slot, uint8_t function,
                      uint8_t offset, uint32_t value) {
  outl(PCI_CONFIG_ADDRESS,
       config_address(bus, slot, function, offset));
  outl(PCI_CONFIG_DATA, value);
}

uint8_t pci_read_config8(uint8_t bus, uint8_t slot, uint8_t function,
                         uint8_t offset) {
  const uint32_t value = pci_read_config(bus, slot, function, offset);
  return static_cast<uint8_t>(value >> ((offset & 3u) * 8u));
}

uint16_t pci_read_config16(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset) {
  const uint32_t value = pci_read_config(bus, slot, function, offset);
  return static_cast<uint16_t>(value >> ((offset & 2u) * 8u));
}
