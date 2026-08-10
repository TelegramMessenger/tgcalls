#include "napi/native_api.h"
#include "tgcall_native.h"

static napi_value InitTgCallModule(napi_env env, napi_value exports) {
  return tgcall::Init(env, exports);
}

static napi_module g_tgCallModule = {
    1,
    0,
    nullptr,
    InitTgCallModule,
    "tgcall",
    nullptr,
    {0},
};

extern "C" __attribute__((constructor)) void RegisterTgCallModule(void) {
  napi_module_register(&g_tgCallModule);
}
