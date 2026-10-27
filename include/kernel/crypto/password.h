#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Kernel::Crypto {
    uint64_t hash_password(const char* password, size_t length, uint64_t salt);
    uint64_t generate_hardware_salt();
}
