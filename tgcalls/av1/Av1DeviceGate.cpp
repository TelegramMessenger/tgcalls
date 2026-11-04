#include "av1/Av1DeviceGate.h"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace tgcalls {

namespace {

// Apple CPU families before the A18 / M4 generation, values as in mach/machine.h (CPUFAMILY_ARM_*).
constexpr uint32_t kFamiliesBeforeAv1Encoding[] = {
    0x1e2d6381, // SWIFT
    0x37a09642, // CYCLONE
    0x2c91a47e, // TYPHOON
    0x92fb37c8, // TWISTER
    0x67ceee93, // HURRICANE
    0xe81e7ef6, // MONSOON_MISTRAL
    0x07d34b9f, // VORTEX_TEMPEST
    0x462504d2, // LIGHTNING_THUNDER
    0x1b588bb3, // FIRESTORM_ICESTORM
    0xda33d83d, // BLIZZARD_AVALANCHE
    0x8765edea, // EVEREST_SAWTOOTH
    0xfa33415e, // IBIZA (M3)
    0x72015832, // PALMA (M3 Max)
    0x2876f5b5, // COLL (A17 Pro)
    0x5f4dea93, // LOBOS (M3 Pro)
    // 32-bit and non-Apple ARM families.
    0xe73283ae, // ARM_9
    0x8ff620d8, // ARM_11
    0x53b005f5, // XSCALE
    0xbd1b0ae9, // ARM_12
    0x0cc90e64, // ARM_13
    0x96077ef1, // ARM_14
    0xa8511bca, // ARM_15
};

} // namespace

bool IsAv1EncodeCapableCpuFamily(uint32_t cpuFamily) {
    for (uint32_t family : kFamiliesBeforeAv1Encoding) {
        if (family == cpuFamily) {
            return false;
        }
    }
    return true;
}

bool CurrentCpuCanEncodeAv1Realtime() {
#if defined(__APPLE__)
    uint32_t family = 0;
    size_t size = sizeof(family);
    if (sysctlbyname("hw.cpufamily", &family, &size, nullptr, 0) != 0) {
        return false;
    }
    return IsAv1EncodeCapableCpuFamily(family);
#else
    return false;
#endif
}

} // namespace tgcalls
