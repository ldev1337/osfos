#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::Drivers {

class E1000 {
public:
  struct RxDesc {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
  } __attribute__((packed));

  struct TxDesc {
    uint64_t address;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
  } __attribute__((packed));

  static bool init(uintptr_t mmio_base);
  static bool detect_and_init();
  static bool start();

  static bool send(const void *data, size_t len);
  static int receive(void *buffer, size_t max_len);
  static int poll_packet(void *buffer, size_t max_len);
  static void get_mac_address(uint8_t mac[6]);
  static void handle_interrupt();
  static uint8_t get_irq();

private:
  static uint32_t read_reg(uint32_t offset);
  static void write_reg(uint32_t offset, uint32_t value);
  static uint32_t read_eeprom(uint8_t addr);
  static void init_rx();
  static void init_tx();
  static void rx_worker();
  static bool rx_work_should_block();
  static bool rx_descriptor_ready();
  static size_t drain_rx(size_t budget);

  static constexpr size_t RX_DESC_COUNT = 32;
  static constexpr size_t TX_DESC_COUNT = 32;
  static constexpr size_t PKT_BUFFER_SIZE = 2048;

  static uintptr_t s_mmio_base;
  static uint8_t s_mac[6];
  static uint8_t s_irq;

  static RxDesc s_rx_descs[RX_DESC_COUNT] __attribute__((aligned(16)));
  static TxDesc s_tx_descs[TX_DESC_COUNT] __attribute__((aligned(16)));
  static uint8_t s_rx_buffers[RX_DESC_COUNT][PKT_BUFFER_SIZE]
      __attribute__((aligned(16)));
  static uint8_t s_tx_buffers[TX_DESC_COUNT][PKT_BUFFER_SIZE]
      __attribute__((aligned(16)));

  static uint32_t s_rx_cur;
  static uint32_t s_tx_cur;
};

} // namespace Kernel::Drivers