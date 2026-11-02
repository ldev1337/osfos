#ifndef PCI_H
#define PCI_H

#include <cstdint>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_MAX_BUSES 256
#define PCI_MAX_SLOTS 32
#define PCI_MAX_FUNCTIONS 8

#define PCI_CLASS_UNCLASSIFIED 0x00
#define PCI_CLASS_STORAGE 0x01
#define PCI_CLASS_NETWORK 0x02
#define PCI_CLASS_DISPLAY 0x03
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_CLASS_MEMORY 0x05
#define PCI_CLASS_BRIDGE 0x06
#define PCI_CLASS_COMM 0x07
#define PCI_CLASS_SYSTEM 0x08
#define PCI_CLASS_INPUT 0x09
#define PCI_CLASS_DOCKING 0x0A
#define PCI_CLASS_PROCESSOR 0x0B
#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_CLASS_WIRELESS 0x0D
#define PCI_CLASS_INTELLIGENT_IO 0x0E
#define PCI_CLASS_SATELLITE 0x0F
#define PCI_CLASS_ENCRYPTION 0x10
#define PCI_CLASS_SIGNAL 0x11
#define PCI_CLASS_ACCELERATOR 0x12
#define PCI_CLASS_INSTRUMENTATION 0x13
#define PCI_CLASS_COPROCESSOR 0x40
#define PCI_CLASS_UNASSIGNED 0xFF

#define PCI_SUBCLASS_SCSI 0x00
#define PCI_SUBCLASS_IDE 0x01
#define PCI_SUBCLASS_FLOPPY 0x02
#define PCI_SUBCLASS_IPI 0x03
#define PCI_SUBCLASS_RAID 0x04
#define PCI_SUBCLASS_ATA 0x05
#define PCI_SUBCLASS_SATA 0x06
#define PCI_SUBCLASS_SAS 0x07
#define PCI_SUBCLASS_NVME 0x08
#define PCI_PROGIF_AHCI 0x01
#define PCI_PROGIF_NVME 0x02
#define PCI_HEADER_MULTIFUNCTION 0x80
#define PCI_HEADER_TYPE_MASK 0x7F
#define PCI_HEADER_ENDPOINT 0x00
#define PCI_HEADER_BRIDGE 0x01
#define PCI_HEADER_CARDBUS 0x02
#define PCI_STATUS_CAPABILITIES 0x00100000u
#define PCI_CAP_PM 0x01
#define PCI_CAP_AGP 0x02
#define PCI_CAP_VPD 0x03
#define PCI_CAP_SLOT_ID 0x04
#define PCI_CAP_MSI 0x05
#define PCI_CAP_PCIX 0x07
#define PCI_CAP_HT 0x08
#define PCI_CAP_VENDOR 0x09
#define PCI_CAP_DEBUG 0x0A
#define PCI_CAP_CPCI 0x0B
#define PCI_CAP_PCI_HOTPLUG 0x0C
#define PCI_CAP_BRIDGE_SUBSYS 0x0D
#define PCI_CAP_AGP8X 0x0E
#define PCI_CAP_SECURE 0x0F
#define PCI_CAP_PCIEXP 0x10
#define PCI_CAP_MSIX 0x11
#define PCI_CAP_SATA 0x12
#define PCI_CAP_PCIE_ADV 0x13

struct PCIDevice {
  uint8_t bus, slot, func;
  uint16_t vendor_id, device_id;
  uint16_t command, status;
  uint8_t revision, prog_if, sub_class, base_class;
  uint8_t header_type, cache_line_size, latency_timer;
  uint8_t interrupt_line, interrupt_pin, capabilities_ptr;
  uint16_t subsystem_vendor_id, subsystem_id;
  uint32_t bars[6];
};

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func,
                         uint8_t offset);
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                      uint32_t value);
uint16_t pci_read_config16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset);
uint8_t pci_read_config8(uint8_t bus, uint8_t slot, uint8_t func,
                         uint8_t offset);
const char *pci_class_name(uint8_t base_class);
const char *pci_subclass_name(uint8_t base_class, uint8_t sub_class,
                              uint8_t prog_if);
void detect_pci_devices();
#endif
