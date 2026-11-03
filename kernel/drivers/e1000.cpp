#include <kernel/cpu.h>
#include <kernel/drivers/e1000.h>

namespace Kernel::Drivers {

uintptr_t E1000::s_mmio_base = 0;
uint8_t E1000::s_mac[6] = {0};
uint8_t E1000::s_irq = 0;

E1000::RxDesc E1000::s_rx_descs[RX_DESC_COUNT];
E1000::TxDesc E1000::s_tx_descs[TX_DESC_COUNT];
uint8_t E1000::s_rx_buffers[RX_DESC_COUNT][PKT_BUFFER_SIZE];
uint8_t E1000::s_tx_buffers[RX_DESC_COUNT][PKT_BUFFER_SIZE];

uint32_t E1000::s_rx_cur = 0;
uint32_t E1000::s_tx_cur = 0;

// Software packet queue for interrupt-driven buffering
struct RxPacketNode {
  uint8_t data[1518];
  uint16_t len;
};
static constexpr size_t RX_QUEUE_SIZE = 16;
static RxPacketNode s_rx_queue[RX_QUEUE_SIZE];
static volatile size_t s_rx_queue_head = 0;
static volatile size_t s_rx_queue_tail = 0;

// Register Offsets
constexpr uint32_t REG_CTRL = 0x0000;
constexpr uint32_t REG_STATUS = 0x0008;
constexpr uint32_t REG_EERD = 0x0014;
constexpr uint32_t REG_IMS = 0x00D0;
constexpr uint32_t REG_RCTL = 0x0100;
constexpr uint32_t REG_TCTL = 0x0400;
constexpr uint32_t REG_RDBAL = 0x2800;
constexpr uint32_t REG_RDBAH = 0x2804;
constexpr uint32_t REG_RDLEN = 0x2808;
constexpr uint32_t REG_RDH = 0x2810;
constexpr uint32_t REG_RDT = 0x2818;
constexpr uint32_t REG_TDBAL = 0x3800;
constexpr uint32_t REG_TDBAH = 0x3804;
constexpr uint32_t REG_TDLEN = 0x3808;
constexpr uint32_t REG_TDH = 0x3810;
constexpr uint32_t REG_TDT = 0x3818;
constexpr uint32_t REG_RAL = 0x5400;
constexpr uint32_t REG_RAH = 0x5404;

static bool fits_dma32(const void *ptr, size_t len) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
  return len != 0 && address <= UINT32_MAX &&
         len - 1 <= UINT32_MAX - address;
}

void E1000::write_reg(uint32_t offset, uint32_t value) {
  *reinterpret_cast<volatile uint32_t *>(s_mmio_base + offset) = value;
}

uint32_t E1000::read_reg(uint32_t offset) {
  return *reinterpret_cast<volatile uint32_t *>(s_mmio_base + offset);
}

uint32_t E1000::read_eeprom(uint8_t addr) {
  write_reg(REG_EERD, (1 | (static_cast<uint32_t>(addr) << 8)));
  uint32_t temp = 0;
  while (!((temp = read_reg(REG_EERD)) & (1 << 4))) {
    // Wait for EEPROM read complete
  }
  return (temp >> 16);
}

void E1000::get_mac_address(uint8_t mac[6]) {
  for (int i = 0; i < 6; i++) {
    mac[i] = s_mac[i];
  }
}

uint8_t E1000::get_irq() { return s_irq; }

bool E1000::detect_and_init() {
  for (uint8_t bus = 0; bus < 4; bus++) {
    for (uint8_t slot = 0; slot < 32; slot++) {
      uint32_t address = (1U << 31) | (bus << 16) | (slot << 11);
      outl(0xCF8, address);
      uint32_t id = inl(0xCFC);

      // Intel Vendor ID = 0x8086, Device ID for 82540EM = 0x100E
      if ((id & 0xFFFF) == 0x8086 && ((id >> 16) & 0xFFFF) == 0x100E) {
        // Read BAR0 (Offset 0x10)
        outl(0xCF8, address | 0x10);
        uint32_t bar0 = inl(0xCFC);
        if ((bar0 & 1) != 0 || (bar0 & 0x6) != 0)
          return false;
        uintptr_t mmio = static_cast<uintptr_t>(bar0 & ~0xF);
        if (mmio == 0 || mmio > UINT32_MAX - REG_RAH)
          return false;

        // Enable MMIO and bus mastering (Offset 0x04)
        outl(0xCF8, address | 0x04);
        uint32_t cmd = inl(0xCFC);
        outl(0xCF8, address | 0x04);
        outl(0xCFC, (cmd & 0xFFFF) | (1 << 1) | (1 << 2));

        // Read Interrupt Line (Offset 0x3C)
        outl(0xCF8, address | 0x3C);
        s_irq = static_cast<uint8_t>(inl(0xCFC) & 0xFF);

        return init(mmio);
      }
    }
  }
  return false;
}

bool E1000::init(uintptr_t mmio_base) {
  if (mmio_base == 0 || mmio_base > UINT32_MAX - REG_RAH ||
      !fits_dma32(s_rx_descs, sizeof(s_rx_descs)) ||
      !fits_dma32(s_tx_descs, sizeof(s_tx_descs)) ||
      !fits_dma32(s_rx_buffers, sizeof(s_rx_buffers)) ||
      !fits_dma32(s_tx_buffers, sizeof(s_tx_buffers)))
    return false;

  s_mmio_base = mmio_base;

  uint32_t ctrl = read_reg(REG_CTRL);
  write_reg(REG_CTRL, ctrl | (1 << 6)); // Set Link Up

  uint32_t ral = read_reg(REG_RAL);
  uint32_t rah = read_reg(REG_RAH);

  s_mac[0] = ral & 0xFF;
  s_mac[1] = (ral >> 8) & 0xFF;
  s_mac[2] = (ral >> 16) & 0xFF;
  s_mac[3] = (ral >> 24) & 0xFF;
  s_mac[4] = rah & 0xFF;
  s_mac[5] = (rah >> 8) & 0xFF;

  if (s_mac[0] == 0 && s_mac[1] == 0) {
    uint32_t eeprom_val = read_eeprom(0);
    s_mac[0] = eeprom_val & 0xFF;
    s_mac[1] = (eeprom_val >> 8) & 0xFF;
    eeprom_val = read_eeprom(1);
    s_mac[2] = eeprom_val & 0xFF;
    s_mac[3] = (eeprom_val >> 8) & 0xFF;
    eeprom_val = read_eeprom(2);
    s_mac[4] = eeprom_val & 0xFF;
    s_mac[5] = (eeprom_val >> 8) & 0xFF;
  }

  init_rx();
  init_tx();

  // Enable Interrupts (Link Status Change, RX/TX)
  write_reg(REG_IMS, 0x1F6DC);

  return true;
}

void E1000::init_rx() {
  write_reg(REG_RDBAL, reinterpret_cast<uintptr_t>(s_rx_descs));
  write_reg(REG_RDBAH, 0);
  write_reg(REG_RDLEN, RX_DESC_COUNT * sizeof(RxDesc));
  write_reg(REG_RDH, 0);
  write_reg(REG_RDT, RX_DESC_COUNT - 1);

  for (size_t i = 0; i < RX_DESC_COUNT; i++) {
    s_rx_descs[i].address = reinterpret_cast<uintptr_t>(s_rx_buffers[i]);
    s_rx_descs[i].status = 0;
  }

  uint32_t rctl = read_reg(REG_RCTL);
  rctl |= (1 << 1)     // RE: Receiver Enable
          | (1 << 2)   // SPE: Strict Priority Ethernet
          | (1 << 4)   // BAM: Broadcast Accept Mode
          | (1 << 15)  // BCE: Broadcast Channel Enable
          | (0 << 16); // BSIZE: 2048 bytes
  write_reg(REG_RCTL, rctl);
}

void E1000::init_tx() {
  write_reg(REG_TDBAL, reinterpret_cast<uintptr_t>(s_tx_descs));
  write_reg(REG_TDBAH, 0);
  write_reg(REG_TDLEN, TX_DESC_COUNT * sizeof(TxDesc));
  write_reg(REG_TDH, 0);
  write_reg(REG_TDT, 0);

  for (size_t i = 0; i < TX_DESC_COUNT; i++) {
    s_tx_descs[i].address = reinterpret_cast<uintptr_t>(s_tx_buffers[i]);
    s_tx_descs[i].status = 0x1;
    s_tx_descs[i].cmd = 0;
  }

  uint32_t tctl = read_reg(REG_TCTL);
  tctl |= (1 << 1)        // EN: Transmitter Enable
          | (1 << 3)      // PSP: Pad Short Packets
          | (0x0F << 4)   // CT: Collision Threshold
          | (0x40 << 12); // Cold Collision Limit
  write_reg(REG_TCTL, tctl);
}

bool E1000::send(const void *data, size_t len) {
  if (len > PKT_BUFFER_SIZE)
    return false;

  uint32_t tail = read_reg(REG_TDT);

  for (size_t i = 0; i < len; i++) {
    s_tx_buffers[tail][i] = static_cast<const uint8_t *>(data)[i];
  }

  s_tx_descs[tail].length = static_cast<uint16_t>(len);
  s_tx_descs[tail].cmd = (1 << 0) | (1 << 1) | (1 << 3);
  s_tx_descs[tail].status = 0;

  tail = (tail + 1) % TX_DESC_COUNT;
  write_reg(REG_TDT, tail);

  volatile uint8_t *status =
      &s_tx_descs[(tail == 0 ? TX_DESC_COUNT - 1 : tail - 1)].status;
  int timeout = 100000;
  while (!(*status & 0x1) && timeout > 0) {
    timeout--;
  }

  return timeout > 0;
}

int E1000::receive(void *buffer, size_t max_len) {
  const int queued = poll_packet(buffer, max_len);
  if (queued > 0)
    return queued;

  uint32_t tail = read_reg(REG_RDT);
  uint32_t head = (tail + 1) % RX_DESC_COUNT;

  if ((s_rx_descs[head].status & 0x1) != 0) {
    uint16_t len = s_rx_descs[head].length;
    size_t copy_len = len < max_len ? len : max_len;

    for (size_t i = 0; i < copy_len; i++) {
      static_cast<uint8_t *>(buffer)[i] = s_rx_buffers[head][i];
    }

    s_rx_descs[head].status = 0;
    write_reg(REG_RDT, head);
    return static_cast<int>(copy_len);
  }
  return 0;
}

int E1000::poll_packet(void *buffer, size_t max_len) {
  if (s_rx_queue_head == s_rx_queue_tail) {
    return 0;
  }

  const auto &pkt = s_rx_queue[s_rx_queue_head];
  size_t copy_len = pkt.len < max_len ? pkt.len : max_len;
  for (size_t i = 0; i < copy_len; i++) {
    static_cast<uint8_t *>(buffer)[i] = pkt.data[i];
  }

  s_rx_queue_head = (s_rx_queue_head + 1) % RX_QUEUE_SIZE;
  return static_cast<int>(copy_len);
}

void E1000::handle_interrupt() {
  uint32_t icr =
      read_reg(0x00C0); // Read ICR (Automatically clears pending cause bits)
  if (icr == 0)
    return;

  // 1. Link Status Change (LSC - Bit 2)
  if (icr & (1 << 2)) {
    uint32_t status = read_reg(REG_STATUS);
    bool link_up = (status & (1 << 1)) != 0;
    (void)link_up;
  }

  // 2. Packet Received Interrupt (RXT/RXDMT0/Overrun - Bits 4, 6, 7)
  if ((icr & (1 << 6)) || (icr & (1 << 7)) || (icr & (1 << 4))) {
    while (true) {
      uint32_t tail = read_reg(REG_RDT);
      uint32_t head = (tail + 1) % RX_DESC_COUNT;

      if ((s_rx_descs[head].status & 0x1) != 0) { // Descriptor Done
        uint16_t len = s_rx_descs[head].length;

        size_t next_tail = (s_rx_queue_tail + 1) % RX_QUEUE_SIZE;
        if (next_tail != s_rx_queue_head) { // Queue not full
          size_t copy_len = len < 1518 ? len : 1518;
          for (size_t i = 0; i < copy_len; i++) {
            s_rx_queue[s_rx_queue_tail].data[i] = s_rx_buffers[head][i];
          }
          s_rx_queue[s_rx_queue_tail].len = static_cast<uint16_t>(copy_len);
          s_rx_queue_tail = next_tail;
        }

        s_rx_descs[head].status = 0;
        write_reg(REG_RDT, head);
      } else {
        break;
      }
    }
  }
}

} // namespace Kernel::Drivers