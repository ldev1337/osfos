#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::SMP {

constexpr size_t MAX_CPUS = 64;

struct CpuInfo {
  uint32_t apic_id;
  bool enabled;
  bool online;
};

// Read firmware CPU topology from the Multiboot2 ACPI tags.
bool discover(uintptr_t multiboot_info, CpuInfo *cpus, size_t capacity,
              size_t *count);
size_t launch_application_processors(const CpuInfo *cpus, size_t count);
size_t online_cpu_count(void);
void local_apic_eoi(void);
[[noreturn]] void application_processor_main(void);

} // namespace Kernel::SMP
