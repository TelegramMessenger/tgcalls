#include "real_tgcall_factory_registry.h"

#include "InstanceV13_0_0Impl.h"
#include "InstanceV4_0_0Impl.h"
#include "tgcall_logging.h"
#include "tgcalls/Instance.h"
#if defined(TGCALL_USE_INSTANCE_V2_IMPL)
#include "tgcalls/v2/InstanceV2Impl.h"
#endif
#if defined(TGCALL_USE_INSTANCE_V2_REFERENCE_IMPL)
#include "tgcalls/v2/InstanceV2ReferenceImpl.h"
#endif
#if defined(TGCALL_USE_INSTANCE_IMPL)
#include "tgcalls/InstanceImpl.h"
#endif

#include <hilog/log.h>

#include <mutex>

namespace {
std::once_flag g_registrationFlag;
bool g_registered = false;
}  // namespace

namespace tgcall {

// Per ADR-0001 rev 5: registry registers each implementation independently via
// tgcalls::Register<>() — order doesn't influence dispatch (Meta::Create picks
// by protocolVersion string), so the simplest layout just calls each Register
// where the corresponding compile-unit is in the build. InstanceV4_0_0Impl is
// always compiled (it lives in our tree); the upstream implementations are
// gated by the CMake-selected TGCALL_USE_* defines.
bool EnsureRealTgCallFactoryRegistered() {
  std::call_once(g_registrationFlag, []() {
    bool aggregated = false;
    aggregated |= tgcalls::Register<tgcalls::InstanceV4_0_0Impl>();
    // ADR-0009 / Task 5.1: tgcalls v2 signaling V3 — restores Web-K interop after the
    // 2026-05-24 protocol switch (#65). 4.0.0 stays registered as legacy by product decision.
    aggregated |= tgcalls::Register<tgcalls::InstanceV13_0_0Impl>();
#if defined(TGCALL_USE_INSTANCE_V2_REFERENCE_IMPL)
    aggregated |= tgcalls::Register<tgcalls::InstanceV2ReferenceImpl>();
#endif
#if defined(TGCALL_USE_INSTANCE_V2_IMPL)
    aggregated |= tgcalls::Register<tgcalls::InstanceV2Impl>();
#endif
#if defined(TGCALL_USE_INSTANCE_IMPL)
    aggregated |= tgcalls::Register<tgcalls::InstanceImpl>();
#endif
    g_registered = aggregated;
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "TgCall real factory registration result=%{public}d",
                 g_registered ? 1 : 0);
  });
  return g_registered;
}

}  // namespace tgcall
