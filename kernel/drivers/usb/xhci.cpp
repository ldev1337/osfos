#include <kernel/drivers/usb/xhci.h>
#include <kernel/memory/pmm.h>
#include <arch/x86_64/string.h>

extern "C" void kernel_printf(const char* fmt, ...);

namespace Kernel::Drivers::USB {

constexpr size_t TRB_RING_SIZE = 256;

XHCIController::XHCIController(uintptr_t mmio_base)
    : base_(mmio_base), op_base_(0), usbcmd_(nullptr), usbsts_(nullptr),
      pagesize_reg_(nullptr), crcr_(nullptr), dcbaap_(nullptr), config_(nullptr),
      dcbaa_virtual_(nullptr), command_ring_virtual_(nullptr),
      command_ring_enqueue_index_(0), ring_cycle_state_(true) {}

bool XHCIController::allocate_dma_structures() {
    void* dcbaa_phys = Kernel::Memory::g_pmm.allocate_page_block(1);
    if (!dcbaa_phys) {
        kernel_printf("[xHCI] Failed to allocate PMM block for DCBAA\n");
        return false;
    }
    dcbaa_virtual_ = reinterpret_cast<uint64_t*>(dcbaa_phys);
    memset(dcbaa_virtual_, 0, 4096);

    size_t ring_bytes = TRB_RING_SIZE * sizeof(TRB);
    size_t ring_pages = (ring_bytes + 4095) / 4096;
    void* ring_phys = Kernel::Memory::g_pmm.allocate_page_block(ring_pages);
    if (!ring_phys) {
        kernel_printf("[xHCI] Failed to allocate PMM block for Command Ring\n");
        return false;
    }
    command_ring_virtual_ = reinterpret_cast<TRB*>(ring_phys);
    memset(command_ring_virtual_, 0, ring_pages * 4096);

    return true;
}

bool XHCIController::initialize() {
    kernel_printf("[xHCI] Initializing Controller at MMIO: 0x%lX\n", base_);

    uint8_t cap_length = *reinterpret_cast<volatile uint8_t*>(base_);
    op_base_ = base_ + cap_length;

    usbcmd_       = reinterpret_cast<volatile uint32_t*>(op_base_ + 0x00);
    usbsts_       = reinterpret_cast<volatile uint32_t*>(op_base_ + 0x04);
    pagesize_reg_ = reinterpret_cast<volatile uint32_t*>(op_base_ + 0x08);
    crcr_         = reinterpret_cast<volatile uint64_t*>(op_base_ + 0x18);
    dcbaap_       = reinterpret_cast<volatile uint64_t*>(op_base_ + 0x30);
    config_       = reinterpret_cast<volatile uint32_t*>(op_base_ + 0x38);

    int timeout = 1000000;
    while ((*usbsts_ & (1 << 11)) && --timeout > 0);
    if (timeout <= 0) {
        kernel_printf("[xHCI] Error: Controller timed out waiting for CNR clear.\n");
        return false;
    }

    *usbcmd_ &= ~1u;
    while (!(*usbsts_ & (1 << 0)));

    *usbcmd_ |= (1 << 1);
    while (*usbcmd_ & (1 << 1));

    timeout = 1000000;
    while ((*usbsts_ & (1 << 11)) && --timeout > 0);

    if (!allocate_dma_structures()) {
        return false;
    }

    *dcbaap_ = reinterpret_cast<uint64_t>(dcbaa_virtual_);

    uint64_t cmd_ring_phys = reinterpret_cast<uint64_t>(command_ring_virtual_);
    *crcr_ = cmd_ring_phys | 1u;

    *config_ = (*config_ & ~0xFFu) | 0x01u;

    *usbcmd_ |= 1u;
    while (*usbsts_ & (1 << 0));

    kernel_printf("[xHCI] Controller successfully started and running!\n");
    return true;
}

}