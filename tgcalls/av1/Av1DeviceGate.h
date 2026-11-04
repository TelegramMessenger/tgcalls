#ifndef TGCALLS_AV1_AV1DEVICEGATE_H
#define TGCALLS_AV1_AV1DEVICEGATE_H

#include <cstdint>

namespace tgcalls {

// Who may encode AV1 in software, decided by chip: Apple silicon of the A18 / M4 generation and later. A deny-list of
// the Apple CPU families before it and of every Intel Mac family (hw.cpufamily, mach/machine.h), so a family this list
// does not know, a later Apple chip, counts as capable.
bool IsAv1EncodeCapableCpuFamily(uint32_t cpuFamily);

// This machine's CPU family through IsAv1EncodeCapableCpuFamily in an arm64 build on Apple platforms (a simulator and
// an iOS app on a Mac report the host's); false in an x86 build (an Intel Mac, or Rosetta) and on other platforms.
bool CurrentCpuCanEncodeAv1Realtime();

} // namespace tgcalls

#endif
