#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::Memory {

constexpr bool dma32_range(uintptr_t address, size_t length) {
  constexpr uintptr_t DMA32_LIMIT = static_cast<uintptr_t>(0xFFFFFFFFu);
  return length != 0 && address <= DMA32_LIMIT &&
         length - 1 <= DMA32_LIMIT - address;
}

} // namespace Kernel::Memory