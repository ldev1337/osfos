#pragma once

#include <cstdint>

inline constexpr uint16_t PCI_CONFIG_ADDRESS = 0xCF8;
inline constexpr uint16_t PCI_CONFIG_DATA = 0xCFC;
inline constexpr uint16_t PCI_MAX_BUSES = 256;
inline constexpr uint8_t PCI_MAX_SLOTS = 32;
inline constexpr uint8_t PCI_MAX_FUNCTIONS = 8;

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t function,
                         uint8_t offset);
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t function,
                      uint8_t offset, uint32_t value);
uint8_t pci_read_config8(uint8_t bus, uint8_t slot, uint8_t function,
                         uint8_t offset);
uint16_t pci_read_config16(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset);
