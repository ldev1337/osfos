#include <kernel/drivers/net/rtl8168.h>

#include <kernel/cpu.h>
#include <kernel/drivers/net/realtek.h>

namespace Kernel::Drivers::Net::RTL8168 {
namespace {
constexpr size_t RING_SIZE = 16;
constexpr size_t BUFFER_SIZE = 2048;
constexpr uint32_t DESC_OWN = 1u << 31;
constexpr uint32_t DESC_EOR = 1u << 30;
constexpr uint32_t DESC_FS = 1u << 29;
constexpr uint32_t DESC_LS = 1u << 28;
constexpr uint32_t DESC_LENGTH = 0x3FFF;
constexpr uint32_t REG_MAC0 = 0x00;
constexpr uint32_t REG_TX_DESC = 0x20;
constexpr uint32_t REG_TX_DESC_HIGH = 0x24;
constexpr uint32_t REG_CHIP_CMD = 0x37;
constexpr uint32_t REG_TX_POLL = 0x38;
constexpr uint32_t REG_INTR_MASK = 0x3C;
constexpr uint32_t REG_INTR_STATUS = 0x3E;
constexpr uint32_t REG_TX_CONFIG = 0x40;
constexpr uint32_t REG_RX_CONFIG = 0x44;
constexpr uint32_t REG_RX_DESC = 0xE4;
constexpr uint32_t REG_RX_DESC_HIGH = 0xE8;
constexpr uint32_t REG_CPLUS_CMD = 0xE0;
constexpr uint32_t REG_CONFIG1 = 0x52;

struct Descriptor {
  volatile uint32_t options1;
  volatile uint32_t options2;
  uint64_t address;
} __attribute__((packed, aligned(16)));

Descriptor rx_desc[RING_SIZE] __attribute__((aligned(256)));
Descriptor tx_desc[RING_SIZE] __attribute__((aligned(256)));
uint8_t rx_buffers[RING_SIZE][BUFFER_SIZE] __attribute__((aligned(16)));
uint8_t tx_buffers[RING_SIZE][BUFFER_SIZE] __attribute__((aligned(16)));
uintptr_t io_base;
uint8_t mac[6];
uint32_t rx_index;
uint32_t tx_index;
bool initialized;

uint8_t read8(uint32_t offset) { return inb(io_base + offset); }
uint16_t read16(uint32_t offset) { return inw(io_base + offset); }
uint32_t read32(uint32_t offset) { return inl(io_base + offset); }
void write8(uint32_t offset, uint8_t value) { outb(io_base + offset, value); }
void write16(uint32_t offset, uint16_t value) { outw(io_base + offset, value); }
void write32(uint32_t offset, uint32_t value) { outl(io_base + offset, value); }
bool dma32(const void *ptr, size_t length) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
  return length && address <= UINT32_MAX && length - 1 <= UINT32_MAX - address;
}
} // namespace

bool detect_and_init() {
  static constexpr uint16_t IDS[] = {0x8168, 0x8161, 0x8169, 0x8167,
                                     0x8162, 0x8165, 0x8136};
  if (initialized)
    return true;
  Realtek::PciDevice device{};
  if (!Realtek::find_io_device(IDS, sizeof(IDS) / sizeof(IDS[0]), &device))
    return false;
  io_base = device.io_base;

  if (!dma32(rx_desc, sizeof(rx_desc)) || !dma32(tx_desc, sizeof(tx_desc)) ||
      !dma32(rx_buffers, sizeof(rx_buffers)) ||
      !dma32(tx_buffers, sizeof(tx_buffers)))
    return false;

  write8(REG_CHIP_CMD, 0x10);
  uint32_t timeout = 1000000;
  while ((read8(REG_CHIP_CMD) & 0x10) && --timeout) {
  }
  if (!timeout)
    return false;
  write8(REG_CONFIG1, read8(REG_CONFIG1) | 1);
  for (size_t i = 0; i < 6; ++i)
    mac[i] = read8(REG_MAC0 + i);

  for (size_t i = 0; i < RING_SIZE; ++i) {
    rx_desc[i].address = reinterpret_cast<uintptr_t>(rx_buffers[i]);
    rx_desc[i].options2 = 0;
    rx_desc[i].options1 =
        DESC_OWN | (i == RING_SIZE - 1 ? DESC_EOR : 0) | BUFFER_SIZE;
    tx_desc[i].address = reinterpret_cast<uintptr_t>(tx_buffers[i]);
    tx_desc[i].options1 = i == RING_SIZE - 1 ? DESC_EOR : 0;
    tx_desc[i].options2 = 0;
  }
  write8(REG_CHIP_CMD, 0x0C);
  write32(REG_RX_DESC,
          static_cast<uint32_t>(reinterpret_cast<uintptr_t>(rx_desc)));
  write32(REG_RX_DESC_HIGH, 0);
  write32(REG_TX_DESC,
          static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tx_desc)));
  write32(REG_TX_DESC_HIGH, 0);
  write32(REG_RX_CONFIG, 0x0000E70F);
  write32(REG_TX_CONFIG, 0x03000700);
  write16(REG_CPLUS_CMD, read16(REG_CPLUS_CMD) | 0x2000);
  write16(REG_INTR_STATUS, 0xFFFF);
  write16(REG_INTR_MASK, 0);
  rx_index = tx_index = 0;
  initialized = true;
  return true;
}

bool start() { return initialized; }

bool send(const void *data, size_t length) {
  if (!initialized || !data || !length || length > BUFFER_SIZE)
    return false;
  Descriptor &desc = tx_desc[tx_index];
  uint32_t timeout = 1000000;
  while ((desc.options1 & DESC_OWN) && --timeout) {
  }
  if (!timeout)
    return false;
  const auto *src = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < length; ++i)
    tx_buffers[tx_index][i] = src[i];
  desc.options1 = (desc.options1 & DESC_EOR) | DESC_FS | DESC_LS |
                  static_cast<uint32_t>(length) | DESC_OWN;
  write8(REG_TX_POLL, 0x40);
  tx_index = (tx_index + 1) % RING_SIZE;
  return true;
}

int receive(void *buffer, size_t capacity) {
  if (!initialized || !buffer || !capacity)
    return 0;
  Descriptor &desc = rx_desc[rx_index];
  const uint32_t status = desc.options1;
  if (status & DESC_OWN)
    return 0;
  const size_t length = status & DESC_LENGTH;
  const bool valid = (status & (DESC_FS | DESC_LS)) == (DESC_FS | DESC_LS) &&
                     length >= 4 && length <= BUFFER_SIZE;
  const size_t copy = valid && length - 4 < capacity ? length - 4 : capacity;
  if (valid)
    for (size_t i = 0; i < copy; ++i)
      static_cast<uint8_t *>(buffer)[i] = rx_buffers[rx_index][i];
  desc.options1 = (status & DESC_EOR) | DESC_OWN | BUFFER_SIZE;
  rx_index = (rx_index + 1) % RING_SIZE;
  return valid ? static_cast<int>(copy) : 0;
}

void get_mac_address(uint8_t output[6]) {
  if (!output)
    return;
  for (size_t i = 0; i < 6; ++i)
    output[i] = mac[i];
}
} // namespace Kernel::Drivers::Net::RTL8168
