#include <arch/x86_64/spinlock.h>
#include <kernel/cpu.h>
#include <kernel/drivers/ata.h>
#include <kernel/drivers/pci.h>
#include <kernel/init.h>
#include <kernel/scheduler.h>
#include <kernel/shell/shell.h>

#include <algorithm>
#include <atomic>
#include <stddef.h>

namespace Kernel::Drivers {

constexpr uint16_t ATA_DATA = 0x1F0;
constexpr uint16_t ATA_FEATURE = 0x1F1;
constexpr uint16_t ATA_SECTOR_COUNT = 0x1F2;
constexpr uint16_t ATA_LBA_LOW = 0x1F3;
constexpr uint16_t ATA_LBA_MID = 0x1F4;
constexpr uint16_t ATA_LBA_HIGH = 0x1F5;
constexpr uint16_t ATA_DEVICE = 0x1F6;
constexpr uint16_t ATA_COMMAND = 0x1F7;
constexpr uint16_t ATA_STATUS = 0x1F7;
constexpr uint16_t ATA_ALT_STATUS = 0x3F6;
constexpr uint16_t ATA_CONTROL = 0x3F6;

constexpr uint8_t ATA_SR_BSY = 0x80;
constexpr uint8_t ATA_SR_DRDY = 0x40;
constexpr uint8_t ATA_SR_DF = 0x20;
constexpr uint8_t ATA_SR_DSC = 0x10;
constexpr uint8_t ATA_SR_DRQ = 0x08;
constexpr uint8_t ATA_SR_ERR = 0x01;

constexpr uint8_t ATA_CMD_READ_SECTORS = 0x20;
constexpr uint8_t ATA_CMD_WRITE_SECTORS = 0x30;
constexpr uint8_t ATA_CMD_IDENTIFY = 0xEC;
constexpr uint8_t ATA_CMD_READ_DMA = 0xC8;
constexpr uint8_t ATA_CMD_WRITE_DMA = 0xCA;

constexpr uint32_t ATA_SECTOR_SIZE = 512;
constexpr size_t ATA_MAX_SECTORS = 255;
constexpr size_t ATA_MAX_TRANSFER = ATA_MAX_SECTORS * ATA_SECTOR_SIZE;
constexpr uint16_t ATA_BM_PRIMARY_OFFSET = 0;
constexpr uint16_t ATA_BM_COMMAND = 0;
constexpr uint16_t ATA_BM_STATUS = 2;
constexpr uint16_t ATA_BM_PRDT = 4;
constexpr uint8_t ATA_BM_START = 1 << 0;
constexpr uint8_t ATA_BM_INTERRUPT_ENABLE = 1 << 1;
constexpr uint8_t ATA_BM_READ_FROM_DEVICE = 1 << 3;
constexpr uint8_t ATA_BM_ERROR = 1 << 1;
constexpr uint8_t ATA_BM_INTERRUPT = 1 << 2;

struct DmaPrd {
  uint32_t physical_address;
  uint16_t byte_count;
  uint16_t flags;
} __attribute__((packed));

static_assert(sizeof(DmaPrd) == 8);

alignas(65536) static uint8_t dma_buffer[ATA_MAX_TRANSFER];
alignas(65536) static DmaPrd dma_prdt[2];
static uint16_t bus_master_base = 0;
static bool dma_enabled = false;
static std::atomic<bool> dma_active{false};
static std::atomic<bool> dma_completed{false};
static std::atomic<uint8_t> dma_completion_status{0};
static std::atomic<uint8_t> dma_completion_ata_status{0};
static Spinlock ata_lock;

class ATALockGuard {
private:
  Spinlock &lock;

public:
  explicit ATALockGuard(Spinlock &spinlock) : lock(spinlock) { lock.lock(); }
  ~ATALockGuard() { lock.unlock(); }
};

class KernelIoSchedulingGuard {
private:
  Kernel::Scheduler::KernelIoSchedulingHint previous;

public:
  KernelIoSchedulingGuard()
      : previous(Kernel::Scheduler::prioritize_current_for_kernel_io()) {}
  ~KernelIoSchedulingGuard() {
    Kernel::Scheduler::restore_current_scheduling(previous);
  }
};

static inline void ata_delay() {
  for (int i = 0; i < 4; i++) {
    inb(ATA_ALT_STATUS);
  }
}

static inline bool ata_wait_not_busy(int timeout = 100000) {
  while ((inb(ATA_STATUS) & ATA_SR_BSY) && --timeout > 0) {
  }
  return timeout > 0;
}

static inline bool ata_wait_drq(int timeout = 100000) {
  while (--timeout > 0) {
    uint8_t status = inb(ATA_STATUS);
    if (status & ATA_SR_ERR)
      return false;
    if (status & ATA_SR_DF)
      return false;
    if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ))
      return true;
  }
  return false;
}

static __init bool find_bus_master_ide(uint16_t *base) {
  for (uint16_t bus = 0; bus < PCI_MAX_BUSES; ++bus) {
    for (uint8_t slot = 0; slot < PCI_MAX_SLOTS; ++slot) {
      for (uint8_t function = 0; function < PCI_MAX_FUNCTIONS; ++function) {
        const uint32_t id = pci_read_config(bus, slot, function, 0x00);
        if ((id & 0xFFFF) == 0xFFFF)
          break;

        if (pci_read_config8(bus, slot, function, 0x0B) != 0x01 ||
            pci_read_config8(bus, slot, function, 0x0A) != 0x01)
          continue;

        const uint8_t programming_interface =
            pci_read_config8(bus, slot, function, 0x09);
        if (!(programming_interface & 0x80) || (programming_interface & 0x01))
          continue;

        const uint32_t bar4 = pci_read_config(bus, slot, function, 0x20);
        if (!(bar4 & 1))
          continue;

        const uint32_t io_base = bar4 & ~0x0FU;
        if (io_base == 0 || io_base > 0xFFF8)
          continue;

        const uint32_t command_status =
            pci_read_config(bus, slot, function, 0x04);
        pci_write_config(bus, slot, function, 0x04,
                         (command_status & 0xFFFF) | (1U << 2));
        *base = static_cast<uint16_t>(io_base + ATA_BM_PRIMARY_OFFSET);
        return true;
      }
    }
  }
  return false;
}

static bool __init drive_supports_enabled_dma() {
  uint16_t identify[256];

  outb(ATA_CONTROL, 1 << 2);
  outb(ATA_DEVICE, 0xA0);
  ata_delay();
  outb(ATA_SECTOR_COUNT, 0);
  outb(ATA_LBA_LOW, 0);
  outb(ATA_LBA_MID, 0);
  outb(ATA_LBA_HIGH, 0);
  outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

  if (!ata_wait_drq()) {
    outb(ATA_CONTROL, 0);
    return false;
  }

  insw(ATA_DATA, identify, 256);
  outb(ATA_CONTROL, 0);

  const bool dma_supported = (identify[49] & (1 << 8)) != 0;
  const bool dma_mode_enabled =
      (identify[63] & 0x0700) != 0 || (identify[88] & 0x7F00) != 0;
  return dma_supported && dma_mode_enabled;
}

static bool fits_dma32(const void *ptr, size_t length) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
  return length != 0 && address <= UINT32_MAX &&
         length - 1 <= UINT32_MAX - address;
}

static size_t __init prepare_prdt(size_t length) {
  const uintptr_t buffer_address = reinterpret_cast<uintptr_t>(dma_buffer);
  size_t entries = 0;
  size_t offset = 0;

  while (offset < length &&
         entries < (sizeof(dma_prdt) / sizeof(dma_prdt[0]))) {
    const uintptr_t address = buffer_address + offset;
    size_t chunk = 0x10000 - (address & 0xFFFF);
    chunk = std::min(chunk, length - offset);

    dma_prdt[entries].physical_address = static_cast<uint32_t>(address);
    dma_prdt[entries].byte_count =
        chunk == 0x10000 ? 0 : static_cast<uint16_t>(chunk);
    dma_prdt[entries].flags = 0;
    offset += chunk;
    ++entries;
  }

  if (offset != length || entries == 0)
    return 0;

  dma_prdt[entries - 1].flags = 1 << 15;
  return entries;
}

static bool wait_for_dma_irq() {
  uint64_t rflags;
  __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
  const bool interrupts_enabled = (rflags & (1ull << 9)) != 0;

  for (uint32_t timeout = 0; timeout < 10000; ++timeout) {
    if (dma_completed.load(std::memory_order_acquire))
      return true;

    if (interrupts_enabled) {
      __asm__ volatile("cli" : : : "memory");
      if (dma_completed.load(std::memory_order_acquire)) {
        __asm__ volatile("sti" : : : "memory");
        return true;
      }
      __asm__ volatile("sti; hlt" : : : "memory");
    } else {
      __asm__ volatile("pause" : : : "memory");
    }
  }
  return dma_completed.load(std::memory_order_acquire);
}

static bool transfer_dma(uint32_t lba, uint8_t sector_count, void *buffer,
                         bool read) {
  const size_t byte_count = static_cast<size_t>(sector_count) * ATA_SECTOR_SIZE;
  if (!dma_enabled || lba > 0x0FFFFFFF || byte_count > sizeof(dma_buffer) ||
      !fits_dma32(dma_buffer, byte_count) ||
      !fits_dma32(dma_prdt, sizeof(dma_prdt)) || prepare_prdt(byte_count) == 0)
    return false;

  if (!read) {
    const auto *source = static_cast<const uint8_t *>(buffer);
    for (size_t i = 0; i < byte_count; ++i)
      dma_buffer[i] = source[i];
  }

  if (!ata_wait_not_busy())
    return false;

  outb(ATA_DEVICE, 0xE0 | ((lba >> 24) & 0x0F));
  ata_delay();
  outb(ATA_FEATURE, 0);
  outb(ATA_SECTOR_COUNT, sector_count);
  outb(ATA_LBA_LOW, static_cast<uint8_t>(lba));
  outb(ATA_LBA_MID, static_cast<uint8_t>(lba >> 8));
  outb(ATA_LBA_HIGH, static_cast<uint8_t>(lba >> 16));

  outb(bus_master_base + ATA_BM_COMMAND, 0);
  outb(bus_master_base + ATA_BM_STATUS, ATA_BM_ERROR | ATA_BM_INTERRUPT);
  outl(bus_master_base + ATA_BM_PRDT,
       static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dma_prdt)));

  uint64_t rflags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) : : "memory");
  dma_completed.store(false, std::memory_order_relaxed);
  dma_completion_status.store(0, std::memory_order_relaxed);
  dma_completion_ata_status.store(0, std::memory_order_relaxed);
  dma_active.store(true, std::memory_order_release);

  const uint8_t bm_command =
      ATA_BM_INTERRUPT_ENABLE | (read ? ATA_BM_READ_FROM_DEVICE : 0);
  outb(bus_master_base + ATA_BM_COMMAND, bm_command);
  outb(ATA_COMMAND, read ? ATA_CMD_READ_DMA : ATA_CMD_WRITE_DMA);
  outb(bus_master_base + ATA_BM_COMMAND, bm_command | ATA_BM_START);
  if (rflags & (1ULL << 9))
    __asm__ volatile("sti" : : : "memory");

  const bool completed = wait_for_dma_irq();

  __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) : : "memory");
  outb(bus_master_base + ATA_BM_COMMAND, bm_command);
  const uint8_t bm_status = inb(bus_master_base + ATA_BM_STATUS);
  const uint8_t ata_status = inb(ATA_STATUS);
  outb(bus_master_base + ATA_BM_STATUS, ATA_BM_ERROR | ATA_BM_INTERRUPT);
  dma_active.store(false, std::memory_order_release);
  const uint8_t irq_bm_status =
      dma_completion_status.load(std::memory_order_acquire);
  const uint8_t irq_ata_status =
      dma_completion_ata_status.load(std::memory_order_acquire);
  if ((rflags & (1ULL << 9)) != 0U)
    __asm__ volatile("sti" : : : "memory");

  if (!completed || (bm_status & ATA_BM_ERROR) ||
      !(irq_bm_status & ATA_BM_INTERRUPT) ||
      (ata_status & (ATA_SR_ERR | ATA_SR_DF | ATA_SR_BSY)) ||
      (irq_ata_status & (ATA_SR_ERR | ATA_SR_DF | ATA_SR_BSY)))
    return false;

  if (read) {
    auto *destination = static_cast<uint8_t *>(buffer);
    for (size_t i = 0; i < byte_count; ++i)
      destination[i] = dma_buffer[i];
  }
  return true;
}

void __init ATADriver::init() {
  outb(ATA_DEVICE, 0xA0);
  dma_enabled = find_bus_master_ide(&bus_master_base) &&
                drive_supports_enabled_dma() &&
                fits_dma32(dma_buffer, sizeof(dma_buffer)) &&
                fits_dma32(dma_prdt, sizeof(dma_prdt));

  if (dma_enabled) {
    outb(bus_master_base + ATA_BM_COMMAND, ATA_BM_INTERRUPT_ENABLE);
    outb(ATA_BM_STATUS + bus_master_base, ATA_BM_ERROR | ATA_BM_INTERRUPT);
    outb(ATA_CONTROL, 0);
    const uint8_t slave_mask = inb(0xA1);
    outb(0xA1, slave_mask & ~(1 << 6));
    kernel_printf("[ATA] Bus-master DMA enabled (IRQ14)\n");
  } else {
    bus_master_base = 0;
    outb(ATA_CONTROL, 1 << 2);
    kernel_printf("[ATA] DMA unavailable; using PIO\n");
  }
}

extern "C" void ata_irq_handler() {
  if (bus_master_base == 0)
    return;

  const uint8_t bm_status = inb(bus_master_base + ATA_BM_STATUS);
  const uint8_t ata_status = inb(ATA_STATUS);
  outb(bus_master_base + ATA_BM_STATUS, ATA_BM_ERROR | ATA_BM_INTERRUPT);

  if (dma_active.load(std::memory_order_acquire)) {
    dma_completion_status.store(bm_status, std::memory_order_relaxed);
    dma_completion_ata_status.store(ata_status, std::memory_order_relaxed);
    dma_completed.store(true, std::memory_order_release);
  }
}

bool ATADriver::read_sectors(uint32_t lba, uint8_t sector_count, void *buf) {
  if (sector_count == 0)
    return true;

  KernelIoSchedulingGuard service_hint;
  ATALockGuard guard(ata_lock);
  if (dma_enabled) {
    const bool success = transfer_dma(lba, sector_count, buf, true);
    return success;
  }

  if (!ata_wait_not_busy()) {
    kernel_printf("[ATA] Read timeout: BSY stuck high before command\n");
    return false;
  }

  outb(ATA_DEVICE, 0xE0 | ((lba >> 24) & 0x0F));
  ata_delay();

  outb(ATA_SECTOR_COUNT, sector_count);
  outb(ATA_LBA_LOW, (uint8_t)lba);
  outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
  outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
  outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);

  uint16_t *ptr = static_cast<uint16_t *>(buf);

  for (int s = 0; s < sector_count; s++) {
    ata_delay();

    if (!ata_wait_not_busy()) {
      kernel_printf("[ATA] Read timeout: BSY stuck high during sector %d\n", s);
      return false;
    }

    uint8_t status = inb(ATA_STATUS);
    if (status & ATA_SR_ERR) {
      kernel_printf("[ATA] Read error: ERR set (status=0x%x)\n", status);
      return false;
    }
    if (status & ATA_SR_DF) {
      kernel_printf("[ATA] Read error: Drive Fault\n");
      return false;
    }

    if (!ata_wait_drq()) {
      kernel_printf("[ATA] Read timeout: DRQ never set or error occurred\n");
      return false;
    }

    insw(ATA_DATA, ptr, 256);
    ptr += 256;
  }

  return true;
}

bool ATADriver::write_sectors(uint32_t lba, uint8_t sector_count,
                              const void *buffer) {
  if (sector_count == 0)
    return true;

  KernelIoSchedulingGuard service_hint;
  ATALockGuard guard(ata_lock);
  if (dma_enabled) {
    const bool success =
        transfer_dma(lba, sector_count, const_cast<void *>(buffer), false);
    return success;
  }

  if (!ata_wait_not_busy()) {
    kernel_printf("[ATA] Write timeout: BSY stuck high before command\n");
    return false;
  }

  outb(ATA_DEVICE, 0xE0 | ((lba >> 24) & 0x0F));
  ata_delay();

  outb(ATA_FEATURE, 0x00);
  outb(ATA_SECTOR_COUNT, sector_count);
  outb(ATA_LBA_LOW, static_cast<uint8_t>(lba));
  outb(ATA_LBA_MID, static_cast<uint8_t>(lba >> 8));
  outb(ATA_LBA_HIGH, static_cast<uint8_t>(lba >> 16));
  outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);

  const uint16_t *ptr = static_cast<const uint16_t *>(buffer);

  for (int s = 0; s < sector_count; s++) {
    ata_delay();

    if (!ata_wait_not_busy()) {
      kernel_printf("[ATA] Write timeout: BSY stuck high during sector %d\n",
                    s);
      return false;
    }

    if (!ata_wait_drq()) {
      kernel_printf("[ATA] Write timeout: DRQ never set or error occurred\n");
      return false;
    }

    outsw(ATA_DATA, ptr, 256);
    ptr += 256;
  }

  return true;
}

} // namespace Kernel::Drivers
