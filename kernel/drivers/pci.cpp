#include <kernel/drivers/pci.h>
#include <kernel/cpu.h>
#include <kernel/shell/shell.h>

static uint32_t pci_config_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | ((uint32_t)offset & 0xFCu);
}

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}
uint8_t pci_read_config8(uint8_t b, uint8_t s, uint8_t f, uint8_t o) {
    uint32_t v = pci_read_config(b, s, f, o);
    return (uint8_t)(v >> ((o & 3u) * 8u));
}
uint16_t pci_read_config16(uint8_t b, uint8_t s, uint8_t f, uint8_t o) {
    uint32_t v = pci_read_config(b, s, f, o);
    return (uint16_t)(v >> ((o & 2u) * 8u));
}

const char* pci_class_name(uint8_t c) {
    switch (c) {
    case 0x00: return "Unclassified / legacy"; case 0x01: return "Mass storage";
    case 0x02: return "Network"; case 0x03: return "Display / GPU";
    case 0x04: return "Multimedia / audio"; case 0x05: return "Memory controller";
    case 0x06: return "Bridge"; case 0x07: return "Communication controller";
    case 0x08: return "System peripheral"; case 0x09: return "Input controller";
    case 0x0A: return "Docking station"; case 0x0B: return "Processor";
    case 0x0C: return "Serial bus controller"; case 0x0D: return "Wireless controller";
    case 0x0E: return "Intelligent I/O controller"; case 0x0F: return "Satellite communication";
    case 0x10: return "Encryption / cryptography"; case 0x11: return "Signal processing";
    case 0x12: return "Processing accelerator"; case 0x13: return "Instrumentation";
    case 0x40: return "Coprocessor"; case 0xFF: return "Unassigned / vendor-specific";
    default: return "Reserved / vendor-defined class";
    }
}

const char* pci_subclass_name(uint8_t c, uint8_t s, uint8_t p) {
    if (c == 0x01) switch (s) {
    case 0x00: return "SCSI"; case 0x01: return "IDE"; case 0x02: return "Floppy";
    case 0x03: return "IPI"; case 0x04: return "RAID"; case 0x05: return "ATA";
    case 0x06: return p == 0x01 ? "SATA (AHCI)" : "SATA"; case 0x07: return "SAS";
    case 0x08: return p == 0x02 ? "NVMe" : "Non-volatile memory controller";
    case 0x09: return "Universal Flash Storage"; case 0x80: return "Other storage";
    }
    if (c == 0x02) switch (s) { case 0x00: return "Ethernet"; case 0x01: return "Token ring"; case 0x02: return "FDDI"; case 0x03: return "ATM"; case 0x04: return "ISDN"; case 0x05: return "WorldFip"; case 0x06: return "PICMG multi-computing"; case 0x07: return "InfiniBand"; case 0x08: return "Fabric controller"; case 0x80: return "Other network"; }
    if (c == 0x03) switch (s) { case 0x00: return "VGA-compatible"; case 0x01: return "XGA"; case 0x02: return "3D controller"; case 0x80: return "Other display"; }
    if (c == 0x04) switch (s) { case 0x00: return "Video"; case 0x01: return "Audio"; case 0x02: return "Computer telephony"; case 0x03: return "HD audio"; case 0x80: return "Other multimedia"; }
    if (c == 0x05) switch (s) { case 0x00: return "RAM"; case 0x01: return "Flash"; case 0x02: return "CXL memory"; case 0x80: return "Other memory"; }
    if (c == 0x06) switch (s) { case 0x00: return "Host bridge"; case 0x01: return "ISA bridge"; case 0x02: return "EISA bridge"; case 0x03: return "MCA bridge"; case 0x04: return "PCI-to-PCI bridge"; case 0x05: return "PCMCIA bridge"; case 0x06: return "NuBus bridge"; case 0x07: return "CardBus bridge"; case 0x08: return "RACEway bridge"; case 0x09: return "Semi-transparent PCI bridge"; case 0x0A: return "InfiniBand-to-PCI host bridge"; case 0x80: return "Other bridge"; }
    if (c == 0x07) switch (s) { case 0x00: return "Serial controller"; case 0x01: return "Parallel controller"; case 0x02: return "Multiport serial"; case 0x03: return "Modem"; case 0x04: return "GPIB"; case 0x05: return "Smart card"; case 0x80: return "Other communication"; }
    if (c == 0x08) switch (s) { case 0x00: return "Interrupt controller"; case 0x01: return "DMA controller"; case 0x02: return "System timer"; case 0x03: return "RTC"; case 0x04: return "PCI hot-plug controller"; case 0x05: return "SD host controller"; case 0x06: return "IOMMU"; case 0x07: return "Root-complex event collector"; case 0x80: return "Other system peripheral"; }
    if (c == 0x09) switch (s) { case 0x00: return "Keyboard"; case 0x01: return "Digitizer / pen"; case 0x02: return "Mouse"; case 0x03: return "Scanner"; case 0x04: return "Gameport"; case 0x80: return "Other input"; }
    if (c == 0x0B) switch (s) { case 0x00: return "386"; case 0x01: return "486"; case 0x02: return "Pentium"; case 0x10: return "Alpha"; case 0x20: return "PowerPC"; case 0x30: return "MIPS"; case 0x40: return "Co-processor"; }
    if (c == 0x0C) switch (s) { case 0x00: return "FireWire (IEEE 1394)"; case 0x01: return "ACCESS.bus"; case 0x02: return "SSA"; case 0x03: return "USB controller"; case 0x04: return "Fibre Channel"; case 0x05: return "SMBus"; case 0x06: return "InfiniBand"; case 0x07: return "IPMI"; case 0x08: return "SERCOS"; case 0x09: return "CANbus"; case 0x0A: return "MIPI controller"; case 0x80: return "Other serial bus"; }
    if (c == 0x0D) switch (s) { case 0x00: return "iRDA"; case 0x01: return "Consumer IR"; case 0x10: return "RF controller"; case 0x11: return "Bluetooth"; case 0x12: return "Broadband"; case 0x20: return "802.11 Wi-Fi"; case 0x21: return "802.11a Wi-Fi"; case 0x80: return "Other wireless"; }
    if (c == 0x10) return "Encryption / security";
    if (c == 0x11) return "Data acquisition / signal processing";
    if (c == 0x12) return "Processing accelerator";
    return "Other / unrecognized subclass";
}

static const char* pci_capability_name(uint8_t id) {
    switch (id) {
    case 0x01: return "Power management"; case 0x02: return "AGP"; case 0x03: return "Vital product data";
    case 0x05: return "MSI"; case 0x07: return "PCI-X"; case 0x09: return "Vendor-specific";
    case 0x0A: return "Debug port"; case 0x0D: return "Bridge subsystem"; case 0x10: return "PCI Express";
    case 0x11: return "MSI-X"; case 0x12: return "SATA configuration"; case 0x13: return "Advanced features";
    default: return "Other capability";
    }
}

static void print_capabilities(uint8_t b, uint8_t s, uint8_t f, uint8_t ptr) {
    
    for (unsigned n = 0; ptr >= 0x40 && ptr <= 0xFC && n < 48; ++n) {
        uint32_t item = pci_read_config(b, s, f, ptr);
        uint8_t id = (uint8_t)item, next = (uint8_t)(item >> 8);
        kernel_printf("    Capability 0x%02X: %s\n", id, pci_capability_name(id));
        if (!next || next == ptr) break;
        ptr = next;
    }
}

static void print_bars(const PCIDevice* d) {
    unsigned count = d->header_type == PCI_HEADER_BRIDGE ? 2 : (d->header_type == PCI_HEADER_CARDBUS ? 1 : 6);
    for (unsigned i = 0; i < count; ++i) {
        uint32_t bar = d->bars[i];
        if (!bar) continue;
        if (bar & 1u) {
            kernel_printf("    BAR%u: I/O port 0x%08X\n", i, bar & ~3u);
        } else {
            uint32_t type = (bar >> 1) & 3u;
            if (type == 2u && i + 1 < count) {
                uint64_t addr = ((uint64_t)d->bars[i + 1] << 32) | (bar & 0xFFFFFFF0u);
                kernel_printf("    BAR%u: 64-bit MMIO 0x%08X%08X%s\n", i, (uint32_t)(addr >> 32), (uint32_t)addr, (bar & 8u) ? " prefetchable" : "");
                ++i;
            } else {
                kernel_printf("    BAR%u: MMIO 0x%08X%s\n", i, bar & 0xFFFFFFF0u, (bar & 8u) ? " prefetchable" : "");
            }
        }
    }
}

static bool read_pci_device(uint8_t b, uint8_t s, uint8_t f, PCIDevice* d) {
    uint32_t id = pci_read_config(b, s, f, 0x00);
    d->vendor_id = (uint16_t)id; d->device_id = (uint16_t)(id >> 16);
    if (d->vendor_id == 0xFFFF || d->vendor_id == 0x0000) return false;
    uint32_t cmd = pci_read_config(b, s, f, 0x04);
    d->command = (uint16_t)cmd; d->status = (uint16_t)(cmd >> 16);
    uint32_t cls = pci_read_config(b, s, f, 0x08);
    d->revision = (uint8_t)cls; d->prog_if = (uint8_t)(cls >> 8);
    d->sub_class = (uint8_t)(cls >> 16); d->base_class = (uint8_t)(cls >> 24);
    uint32_t hdr = pci_read_config(b, s, f, 0x0C);
    d->cache_line_size = (uint8_t)hdr; d->latency_timer = (uint8_t)(hdr >> 8);
    d->header_type = (uint8_t)((hdr >> 16) & PCI_HEADER_TYPE_MASK);
    d->capabilities_ptr = (d->status & 0x10u) ? (pci_read_config(b, s, f, 0x34) & 0xFFu) : 0;
    d->subsystem_vendor_id = d->subsystem_id = 0;
    if (d->header_type == PCI_HEADER_ENDPOINT) {
        uint32_t sub = pci_read_config(b, s, f, 0x2C);
        d->subsystem_vendor_id = (uint16_t)sub; d->subsystem_id = (uint16_t)(sub >> 16);
    }
    uint32_t intr = pci_read_config(b, s, f, 0x3C);
    d->interrupt_line = (uint8_t)intr; d->interrupt_pin = (uint8_t)(intr >> 8);
    for (unsigned i = 0; i < 6; ++i) d->bars[i] = 0;
    unsigned bars = d->header_type == PCI_HEADER_BRIDGE ? 2 : (d->header_type == PCI_HEADER_CARDBUS ? 1 : 6);
    for (unsigned i = 0; i < bars; ++i) d->bars[i] = pci_read_config(b, s, f, (uint8_t)(0x10 + i * 4));
    d->bus = b; d->slot = s; d->func = f;
    return true;
}

static void print_device(const PCIDevice* d) {
    kernel_printf("PCI %02X:%02X.%u  %04X:%04X  %s\n", d->bus, d->slot, d->func,
                  d->vendor_id, d->device_id, pci_class_name(d->base_class));
    kernel_printf("    Type: %s (class %02X, subclass %02X, prog-if %02X), rev %02X\n",
                  pci_subclass_name(d->base_class, d->sub_class, d->prog_if),
                  d->base_class, d->sub_class, d->prog_if, d->revision);
    if (d->header_type == PCI_HEADER_ENDPOINT)
        kernel_printf("    Subsystem: %04X:%04X\n", d->subsystem_vendor_id, d->subsystem_id);
    kernel_printf("    Command: %04X  Status: %04X  IRQ line/pin: %u/%u\n",
                  d->command, d->status, d->interrupt_line, d->interrupt_pin);
    print_bars(d);
    if (d->capabilities_ptr) print_capabilities(d->bus, d->slot, d->func, d->capabilities_ptr);
}

void detect_pci_devices() {
    unsigned found = 0;
    kernel_printf("Scanning PCI/PCIe configuration space (legacy mechanism #1)...\n\n");
    for (uint16_t bus = 0; bus < PCI_MAX_BUSES; ++bus) {
        for (uint8_t slot = 0; slot < PCI_MAX_SLOTS; ++slot) {
            PCIDevice d;
            if (!read_pci_device((uint8_t)bus, slot, 0, &d)) continue;
            print_device(&d); ++found;
            if (d.header_type & PCI_HEADER_MULTIFUNCTION) {
                for (uint8_t f = 1; f < PCI_MAX_FUNCTIONS; ++f) {
                    if (read_pci_device((uint8_t)bus, slot, f, &d)) { print_device(&d); ++found; }
                }
            }
        }
    }
    kernel_printf("\nPCI scan complete: %u function(s) found.\n", found);
}