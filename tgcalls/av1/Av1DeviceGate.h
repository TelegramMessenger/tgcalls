#ifndef TGCALLS_AV1_AV1DEVICEGATE_H
#define TGCALLS_AV1_AV1DEVICEGATE_H

#include <cstdint>

namespace tgcalls {

// Who may encode AV1 in software, decided by chip: the A18 / M4 generation and later. A deny-list of the Apple CPU
// families before it (hw.cpufamily, mach/machine.h), so a family this list does not know, a later chip, counts as
// capable.
bool IsAv1EncodeCapableCpuFamily(uint32_t cpuFamily);

// This machine's CPU family through IsAv1EncodeCapableCpuFamily on Apple platforms (a simulator and an iOS app on a
// Mac report the host's); false elsewhere.
bool CurrentCpuCanEncodeAv1Realtime();

} // namespace tgcalls

#endif
