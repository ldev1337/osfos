#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Kernel::Drivers::USB {

struct TRB {
  uint32_t parameter[2];
  uint32_t status;
  uint32_t control;
} __attribute__((packed));

class XHCIController {
public:
  explicit XHCIController(uintptr_t mmio_base);
  bool initialize();

private:
  uintptr_t base_;
  uintptr_t op_base_;

  volatile uint32_t *usbcmd_;
  volatile uint32_t *usbsts_;
  volatile uint32_t *pagesize_reg_;
  volatile uint64_t *crcr_;
  volatile uint64_t *dcbaap_;
  volatile uint32_t *config_;

  uint64_t *dcbaa_virtual_;
  TRB *command_ring_virtual_;
  size_t command_ring_enqueue_index_;
  bool ring_cycle_state_;

  bool allocate_dma_structures();
};

} // namespace Kernel::Drivers::USB