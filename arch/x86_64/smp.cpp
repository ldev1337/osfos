#include <arch/x86_64/string.h>
#include <atomic>
#include <kernel/cpu.h>
#include <kernel/memory/pmm.h>
#include <kernel/scheduler.h>
#include <kernel/smp.h>

namespace Kernel::SMP {
namespace {

constexpr uintptr_t TRAMPOLINE_ADDRESS = 0x8000;
constexpr uint32_t LAPIC_TIMER_VECTOR = 0x31;
constexpr size_t AP_STACK_SIZE = 16 * 1024;
alignas(16) static uint8_t ap_stacks[MAX_CPUS][AP_STACK_SIZE];
static volatile uint32_t *local_apic = nullptr;
static std::atomic<uint32_t> online_processors{1};

extern "C" uint8_t ap_trampoline_start[];
extern "C" uint8_t ap_trampoline_end[];
extern "C" uint8_t ap_trampoline_cr3[];
extern "C" uint8_t ap_trampoline_stack[];
extern "C" uint8_t ap_trampoline_entry[];

static uint64_t rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return (uint64_t(hi) << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t value) {
  __asm__ volatile("wrmsr"
                   :
                   : "c"(msr), "a"(uint32_t(value)), "d"(uint32_t(value >> 32))
                   : "memory");
}

static bool lapic_init(bool start_timer) {
  uint32_t eax = 1, ebx, ecx, edx;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
  if (!(edx & (1u << 9)))
    return false;
  uint64_t apic_base = rdmsr(0x1B);
  if (apic_base & (1ull << 10))
    return false; // x2APIC needs the MSR-based IPI path.
  apic_base |= 1ull << 11;
  wrmsr(0x1B, apic_base);
  if (!local_apic)
    local_apic = reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>(apic_base & 0xFFFFF000ull));
  local_apic[0x80 / 4] = 0;
  local_apic[0xF0 / 4] = (1u << 8) | 0xFF;
  local_apic[0x280 / 4] = 0;
  if (start_timer) {
    local_apic[0x3E0 / 4] = 0x3; // Divide the APIC clock by 16.
    local_apic[0x320 / 4] = (1u << 17) | LAPIC_TIMER_VECTOR;
    local_apic[0x380 / 4] = 1000000;
  }
  return true;
}

static void delay_cycles(uint64_t cycles) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  const uint64_t start = (uint64_t(hi) << 32) | lo;
  do {
    __asm__ volatile("pause");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  } while (((uint64_t(hi) << 32) | lo) - start < cycles);
}

static bool wait_icr_idle() {
  for (uint32_t tries = 0; tries < 1000000; ++tries) {
    if (!(local_apic[0x300 / 4] & (1u << 12)))
      return true;
    __asm__ volatile("pause");
  }
  return false;
}

static bool send_ipi(uint8_t apic_id, uint32_t command) {
  if (!wait_icr_idle())
    return false;
  local_apic[0x310 / 4] = uint32_t(apic_id) << 24;
  local_apic[0x300 / 4] = command;
  return wait_icr_idle();
}

static void patch_trampoline(uint64_t cr3, uintptr_t stack, uintptr_t entry) {
  const size_t image_size = ap_trampoline_end - ap_trampoline_start;
  memcpy(reinterpret_cast<void *>(TRAMPOLINE_ADDRESS), ap_trampoline_start,
         image_size);
  auto patch_address = [](uint8_t *symbol) {
    return TRAMPOLINE_ADDRESS + (symbol - ap_trampoline_start);
  };
  *reinterpret_cast<volatile uint32_t *>(patch_address(ap_trampoline_cr3)) =
      static_cast<uint32_t>(cr3);
  *reinterpret_cast<volatile uint64_t *>(patch_address(ap_trampoline_stack)) =
      stack;
  *reinterpret_cast<volatile uint64_t *>(patch_address(ap_trampoline_entry)) =
      entry;
  __asm__ volatile("mfence; wbinvd" ::: "memory");
}

struct MultibootTag {
  uint32_t type, size;
};
struct RsdpV1 {
  char signature[8];
  uint8_t checksum;
  char oem_id[6];
  uint8_t revision;
  uint32_t rsdt_address;
} __attribute__((packed));
struct RsdpV2 {
  RsdpV1 v1;
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t extended_checksum;
  uint8_t reserved[3];
} __attribute__((packed));
struct SdtHeader {
  char signature[4];
  uint32_t length;
  uint8_t revision, checksum;
  char oem_id[6], oem_table_id[8];
  uint32_t oem_revision, creator_id, creator_revision;
} __attribute__((packed));
struct Madt {
  SdtHeader header;
  uint32_t local_apic_address, flags;
} __attribute__((packed));
struct MadtEntry {
  uint8_t type, length;
} __attribute__((packed));
struct MadtLocalApic {
  MadtEntry entry;
  uint8_t processor_uid, apic_id;
  uint32_t flags;
} __attribute__((packed));
struct MadtLocalX2Apic {
  MadtEntry entry;
  uint16_t reserved;
  uint32_t apic_id, flags, processor_uid;
} __attribute__((packed));

bool checksum_ok(const void *data, size_t length) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  uint8_t sum = 0;
  for (size_t i = 0; i < length; ++i)
    sum = uint8_t(sum + bytes[i]);
  return sum == 0;
}

bool sig(const SdtHeader *h, const char *s) {
  for (size_t i = 0; i < 4; ++i)
    if (h->signature[i] != s[i])
      return false;
  return true;
}

const Madt *find_madt(const RsdpV2 *rsdp) {
  const bool xsdt = rsdp->v1.revision >= 2 && rsdp->xsdt_address;
  auto *root = reinterpret_cast<const SdtHeader *>(static_cast<uintptr_t>(
      xsdt ? rsdp->xsdt_address : rsdp->v1.rsdt_address));
  if (!root || root->length < sizeof(SdtHeader) ||
      !checksum_ok(root, root->length))
    return nullptr;
  const size_t width = xsdt ? 8 : 4;
  const size_t bytes = root->length - sizeof(SdtHeader);
  if (bytes % width)
    return nullptr;
  const auto *entries = reinterpret_cast<const uint8_t *>(root + 1);
  for (size_t i = 0; i < bytes; i += width) {
    uint64_t address = xsdt ? *reinterpret_cast<const uint64_t *>(entries + i)
                            : *reinterpret_cast<const uint32_t *>(entries + i);
    auto *table =
        reinterpret_cast<const SdtHeader *>(static_cast<uintptr_t>(address));
    if (table && table->length >= sizeof(SdtHeader) && sig(table, "APIC") &&
        checksum_ok(table, table->length))
      return reinterpret_cast<const Madt *>(table);
  }
  return nullptr;
}

} // namespace

bool discover(uintptr_t info_addr, CpuInfo *cpus, size_t capacity,
              size_t *count) {
  if (!info_addr || !cpus || !capacity || !count)
    return false;
  *count = 0;
  const uint32_t total_size = *reinterpret_cast<const uint32_t *>(info_addr);
  if (total_size < 16 || info_addr > UINTPTR_MAX - total_size)
    return false;
  const auto *cursor = reinterpret_cast<const uint8_t *>(info_addr + 8);
  const auto *end = reinterpret_cast<const uint8_t *>(info_addr + total_size);
  const RsdpV2 *rsdp = nullptr;
  static RsdpV2 legacy_rsdp{};
  while (cursor + sizeof(MultibootTag) <= end) {
    auto *tag = reinterpret_cast<const MultibootTag *>(cursor);
    if (tag->size < sizeof(MultibootTag) || cursor + tag->size > end)
      return false;
    if (tag->type == 0)
      break;
    if (tag->type == 15 && tag->size >= 8 + sizeof(RsdpV2)) {
      rsdp = reinterpret_cast<const RsdpV2 *>(cursor + 8);
      break;
    }
    if (tag->type == 14 && tag->size >= 8 + sizeof(RsdpV1)) {
      auto *v1 = reinterpret_cast<const RsdpV1 *>(cursor + 8);
      if (v1->revision == 0) {
        legacy_rsdp.v1 = *v1;
        rsdp = &legacy_rsdp;
        break;
      }
    }
    cursor += (tag->size + 7u) & ~size_t(7u);
  }
  if (!rsdp || !checksum_ok(&rsdp->v1, sizeof(RsdpV1)))
    return false;
  if (rsdp->v1.revision >= 2 &&
      (rsdp->length < sizeof(RsdpV2) || !checksum_ok(rsdp, rsdp->length)))
    return false;
  const Madt *madt = find_madt(rsdp);
  if (!madt || madt->header.length < sizeof(Madt))
    return false;
  uint32_t bsp_apic_id = 0;
  uint32_t eax = 1, ebx, ecx, edx;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
  bsp_apic_id = (ebx >> 24) & 0xffu;
  const auto *entry = reinterpret_cast<const uint8_t *>(madt + 1);
  const auto *madt_end =
      reinterpret_cast<const uint8_t *>(madt) + madt->header.length;
  while (entry + sizeof(MadtEntry) <= madt_end) {
    auto *h = reinterpret_cast<const MadtEntry *>(entry);
    if (h->length < sizeof(MadtEntry) || entry + h->length > madt_end)
      return false;
    uint32_t apic_id = 0, flags = 0;
    bool processor = false;
    if (h->type == 0 && h->length >= sizeof(MadtLocalApic)) {
      auto *cpu = reinterpret_cast<const MadtLocalApic *>(entry);
      apic_id = cpu->apic_id;
      flags = cpu->flags;
      processor = true;
    } else if (h->type == 9 && h->length >= sizeof(MadtLocalX2Apic)) {
      auto *cpu = reinterpret_cast<const MadtLocalX2Apic *>(entry);
      apic_id = cpu->apic_id;
      flags = cpu->flags;
      processor = true;
    }
    if (processor && (flags & 3u) && *count < capacity) {
      cpus[*count] = {apic_id, true, apic_id == bsp_apic_id};
      ++*count;
    }
    entry += h->length;
  }
  return *count != 0;
}

size_t launch_application_processors(const CpuInfo *cpus, size_t count) {
  if (!cpus || count < 2 || count > MAX_CPUS || !lapic_init(false))
    return 0;

  const size_t image_size = ap_trampoline_end - ap_trampoline_start;
  if (image_size == 0 || image_size > Kernel::Memory::PMM::PAGE_SIZE)
    return 0;

  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  size_t launched = 0;
  size_t stack_index = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!cpus[i].enabled || cpus[i].online || cpus[i].apic_id > 0xFF)
      continue;
    const uint8_t apic_id = static_cast<uint8_t>(cpus[i].apic_id);
    const uintptr_t stack_top =
        reinterpret_cast<uintptr_t>(ap_stacks[stack_index] + AP_STACK_SIZE);
    patch_trampoline(cr3, stack_top,
                     reinterpret_cast<uintptr_t>(application_processor_main));

    // Snapshot before sending INIT/SIPI. An AP can reach its entry point
    // before the startup sequence returns, so sampling afterward can miss
    // its increment and make us wait forever for an extra CPU.
    const uint32_t expected_online =
        online_processors.load(std::memory_order_acquire) + 1;

    // INIT assert, INIT deassert, then the SIPI vector for physical 0x8000.
    if (!send_ipi(apic_id, 0x0000C500))
      continue;
    delay_cycles(10000000);
    if (!send_ipi(apic_id, 0x00008500))
      continue;
    delay_cycles(200000);
    if (!send_ipi(apic_id, 0x00004608))
      continue;
    delay_cycles(200000);
    (void)send_ipi(apic_id, 0x00004608);

    bool came_online = false;
    for (uint32_t tries = 0; tries < 100000000; ++tries) {
      if (online_processors.load(std::memory_order_acquire) >=
          expected_online) {
        came_online = true;
        break;
      }
      __asm__ volatile("pause");
    }
    if (came_online) {
      ++launched;
      ++stack_index;
    } else {
      // Do not overwrite the shared low-memory trampoline while an AP may
      // still be executing it after a delayed startup.
      break;
    }
  }
  return launched;
}

void local_apic_eoi(void) {
  if (local_apic)
    local_apic[0xB0 / 4] = 0;
}

size_t online_cpu_count(void) {
  return online_processors.load(std::memory_order_acquire);
}

[[noreturn]] void application_processor_main(void) {
  cpu_ap_init();
  Kernel::Scheduler::scheduler_ap_init();
  if (!lapic_init(true)) {
    for (;;)
      __asm__ volatile("cli; hlt");
  }
  online_processors.fetch_add(1, std::memory_order_release);
  for (;;)
    __asm__ volatile("sti; hlt" ::: "memory");
}

} // namespace Kernel::SMP
