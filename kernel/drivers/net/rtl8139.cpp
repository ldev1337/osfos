#include <kernel/drivers/net/rtl8139.h>

#include <kernel/cpu.h>
#include <kernel/memory/dma.h>
#include <kernel/drivers/net/realtek.h>

namespace Kernel::Drivers::Net::RTL8139 {
namespace {
constexpr size_t RX_SIZE = 8192;
constexpr size_t TX_COUNT = 4;
constexpr size_t TX_SIZE = 2048;
constexpr size_t BUFFER_SIZE = RX_SIZE + 16 + 1500;
constexpr uint32_t RING_SIZE = RX_SIZE + 16;
constexpr uint8_t REG_CMD = 0x37;
constexpr uint8_t REG_CAPR = 0x38;
constexpr uint8_t REG_CBR = 0x3A;
constexpr uint8_t REG_TSD = 0x10;
constexpr uint8_t REG_TSAD = 0x20;
constexpr uint8_t REG_RBSTART = 0x30;
constexpr uint8_t REG_RCR = 0x44;
constexpr uint8_t REG_TCR = 0x40;

uint8_t rx_buffer[BUFFER_SIZE] __attribute__((aligned(16)));
uint8_t tx_buffers[TX_COUNT][TX_SIZE] __attribute__((aligned(16)));
uint8_t mac[6];
uintptr_t io_base;
uint32_t rx_offset;
uint8_t tx_index;
bool initialized;

uint8_t read8(uint8_t reg) { return inb(io_base + reg); }
uint16_t read16(uint8_t reg) { return inw(io_base + reg); }
uint32_t read32(uint8_t reg) { return inl(io_base + reg); }
void write8(uint8_t reg, uint8_t value) { outb(io_base + reg, value); }
void write16(uint8_t reg, uint16_t value) { outw(io_base + reg, value); }
void write32(uint8_t reg, uint32_t value) { outl(io_base + reg, value); }
uint8_t ring_byte(uint32_t offset) { return rx_buffer[offset % RING_SIZE]; }
} // namespace

bool detect_and_init() {
  static constexpr uint16_t IDS[] = {0x8129, 0x8139, 0x8138, 0x1300};
  if (initialized)
    return true;
  Realtek::PciDevice device{};
  if (!Realtek::find_io_device(IDS, sizeof(IDS) / sizeof(IDS[0]), &device))
    return false;
  const uintptr_t rx_addr = reinterpret_cast<uintptr_t>(rx_buffer);
  if (!Kernel::Memory::dma32_range(rx_addr, BUFFER_SIZE))
    return false;
  for (size_t i = 0; i < TX_COUNT; ++i) {
    const uintptr_t tx_addr = reinterpret_cast<uintptr_t>(tx_buffers[i]);
    if (!Kernel::Memory::dma32_range(tx_addr, TX_SIZE))
      return false;
  }
  io_base = device.io_base;
  write8(REG_CMD, 0x10);
  uint32_t timeout = 1000000;
  while ((read8(REG_CMD) & 0x10) && --timeout) {
  }
  if (!timeout)
    return false;
  for (size_t i = 0; i < 6; ++i)
    mac[i] = read8(static_cast<uint8_t>(i));

  rx_offset = 0;
  tx_index = 0;
  write32(REG_RBSTART, static_cast<uint32_t>(rx_addr));
  for (size_t i = 0; i < TX_COUNT; ++i)
    write32(static_cast<uint8_t>(REG_TSAD + i * 4),
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tx_buffers[i])));
  write32(REG_TCR, 0x03000700);
  write32(REG_RCR, 0x0000008F);
  write16(REG_CAPR, 0xFFF0);
  write8(REG_CMD, 0x0C);
  initialized = true;
  return true;
}

bool start() { return initialized; }

bool send(const void *data, size_t length) {
  if (!initialized || !data || !length || length > TX_SIZE)
    return false;
  const uint8_t slot = tx_index;
  const uint16_t tsd_port = static_cast<uint16_t>(io_base + REG_TSD + slot * 4);
  auto *destination = tx_buffers[slot];
  const auto *source = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < length; ++i)
    destination[i] = source[i];
  write32(static_cast<uint8_t>(REG_TSD + slot * 4),
          static_cast<uint32_t>(length));
  tx_index = (tx_index + 1) % TX_COUNT;
  uint32_t timeout = 1000000;
  uint32_t status = 0;
  while (((status = inl(tsd_port)) & ((1u << 15) | (1u << 14))) == 0 &&
         --timeout) {
  }
  return timeout != 0 && (status & (1u << 15)) != 0;
}

int receive(void *buffer, size_t capacity) {
  if (!initialized || !buffer || !capacity)
    return 0;
  if (read8(REG_CMD) & 1)
    return 0;
  const uint32_t available = read16(REG_CBR) % RING_SIZE;
  if (rx_offset == available)
    return 0;
  const uint16_t status = ring_byte(rx_offset);
  const uint16_t length =
      ring_byte(rx_offset + 2) |
      (static_cast<uint16_t>(ring_byte(rx_offset + 3)) << 8);
  if (!(status & 1) || length < 4 || length > 1518) {
    rx_offset = available;
    write16(REG_CAPR, static_cast<uint16_t>(rx_offset - 16));
    return 0;
  }
  const size_t frame_length = length - 4;
  const size_t copy = frame_length < capacity ? frame_length : capacity;
  for (size_t i = 0; i < copy; ++i)
    static_cast<uint8_t *>(buffer)[i] = ring_byte(rx_offset + 4 + i);
  rx_offset = (rx_offset + length + 7) & ~3u;
  rx_offset %= RING_SIZE;
  write16(REG_CAPR, static_cast<uint16_t>(rx_offset - 16));
  return static_cast<int>(copy);
}

void get_mac_address(uint8_t output[6]) {
  if (!output)
    return;
  for (size_t i = 0; i < 6; ++i)
    output[i] = mac[i];
}
} // namespace Kernel::Drivers::Net::RTL8139
