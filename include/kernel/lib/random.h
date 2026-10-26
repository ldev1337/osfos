#ifndef KERNEL_LIB_RANDOM_H
#define KERNEL_LIB_RANDOM_H

#include <stdint.h>

namespace Kernel {
namespace Lib {

class Random {
private:
    uint64_t state;

    static inline bool has_rdrand() {
        uint32_t eax, ebx, ecx, edx;
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        return (ecx & (1 << 30)) != 0;
    }

public:
    explicit Random(uint64_t seed = 0x853c49e6748fea9bULL) : state(seed) {
        if (state == 0) {
            state = rdtsc();
        }
    }

    static inline uint64_t rdtsc() {
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
    }

    inline uint64_t next_u64() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1DULL;
    }

    inline uint32_t next_u32() {
        return static_cast<uint32_t>(next_u64() >> 32);
    }

    inline uint32_t next() {
        uint32_t val;
        unsigned char success;
        __asm__ volatile (
            "rdrand %1\n\t"
            "setc %0"
            : "=qm"(success), "=r"(val)
            :
            : "cc"
        );
        if (success) {
            return val;
        }
        return next_u32();
    }

    inline uint32_t next_range(uint32_t min, uint32_t max) {
        if (min >= max) return min;
        uint64_t range = static_cast<uint64_t>(max) - min + 1;
        uint64_t mult = static_cast<uint64_t>(next()) * range;
        return min + static_cast<uint32_t>(mult >> 32);
    }
};

} 
} 

#endif 