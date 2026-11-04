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
    // Intel Macs never encode AV1 (CPUFAMILY_INTEL_*; Comet Lake is the last Intel Mac).
    0xaa33392b, // INTEL_6_13
    0x78ea4fbc, // INTEL_PENRYN
    0x6b5a4cd2, // INTEL_NEHALEM
    0x573b5eec, // INTEL_WESTMERE
    0x5490b78c, // INTEL_SANDYBRIDGE
    0x1f65e835, // INTEL_IVYBRIDGE
    0x10b282dc, // INTEL_HASWELL
    0x582ed09c, // INTEL_BROADWELL
    0x37fc219f, // INTEL_SKYLAKE
    0x0f817246, // INTEL_KABYLAKE
    0x38435547, // INTEL_ICELAKE
    0x1cf8a03e, // INTEL_COMETLAKE
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
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    // Apple silicon only: an x86 build never encodes AV1, on an Intel Mac or translated by Rosetta.
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
