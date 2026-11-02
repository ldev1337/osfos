#include <kernel/cpu.h>
#include <kernel/drivers/ata.h>
#include <kernel/shell/shell.h>

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

constexpr uint8_t ATA_SR_BSY = 0x80;
constexpr uint8_t ATA_SR_DRDY = 0x40;
constexpr uint8_t ATA_SR_DF = 0x20;
constexpr uint8_t ATA_SR_DSC = 0x10;
constexpr uint8_t ATA_SR_DRQ = 0x08;
constexpr uint8_t ATA_SR_ERR = 0x01;

constexpr uint8_t ATA_CMD_READ_SECTORS = 0x20;
constexpr uint8_t ATA_CMD_WRITE_SECTORS = 0x30;

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

void ATADriver::init() { outb(ATA_DEVICE, 0xA0); }

bool ATADriver::read_sectors(uint32_t lba, uint8_t sector_count, void *buf) {
  if (sector_count == 0)
    return true;

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

  if (!ata_wait_not_busy()) {
    kernel_printf("[ATA] Write timeout: BSY stuck high before command\n");
    return false;
  }

  outb(ATA_DEVICE, 0xE0 | ((lba >> 24) & 0x0F));
  ata_delay();

  outb(ATA_FEATURE, 0x00);
  outb(ATA_SECTOR_COUNT, sector_count);
  outb(ATA_LBA_LOW, (uint8_t)lba);
  outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
  outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
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