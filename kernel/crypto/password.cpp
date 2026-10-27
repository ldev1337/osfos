#include "kernel/crypto/password.h"

namespace Kernel::Crypto {
    static inline uint64_t fnv1a_64(const uint8_t* data, size_t length) {
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < length; i++) {
            hash ^= data[i];
            hash *= 0x100000001b3ULL;
        }
        return hash;
    }

    uint64_t hash_password(const char* password, size_t length, uint64_t salt) {
        uint64_t intermediate = fnv1a_64((const uint8_t*)&salt, sizeof(salt));
        intermediate ^= fnv1a_64((const uint8_t*)password, length);
        for (int i = 0; i < 100000; i++) {
            intermediate = fnv1a_64((const uint8_t*)&intermediate, sizeof(intermediate));
        }
        return intermediate;
    }

    uint64_t generate_hardware_salt() {
        uint64_t rand_val;
        asm volatile(
            "1: rdrand %0;\n"
            "jnc 1b;\n"
            : "=r"(rand_val)
            :
            : "cc"
        );
        return rand_val;
    }
}
