// Native C++ test mod #2: imports TestMathService from provider_mod + installs ARM9 function hooks via HookService!
#include "ph_mods/service.hpp"
#include "ph_mods/svc/hook.h"
#include "ph_mods/svc/log.h"
#include "ph_mods/svc/memory.h"
#include "test_math_service.h"

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(MemoryService, svc_mem);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(TestMathService, svc_math);

static PhHookHandle g_replace_handle = 0;

static PhHookAction on_target_pre(ModContext*, PhArm9Cpu* cpu, uint32_t, void*) {
    // Double argument r0 before the target function runs!
    cpu->r[0] *= 2u;
    return PH_HOOK_CONTINUE;
}

static PhHookAction on_target_post(ModContext*, PhArm9Cpu* cpu, uint32_t, void*) {
    // Add 7 to return value r0 after the target function finishes!
    cpu->r[0] += 7u;
    return PH_HOOK_CONTINUE;
}

static PhHookAction on_replace_fn(ModContext* ctx, PhArm9Cpu* cpu, uint32_t, void*) {
    // Call original via HookService::call_original, then add 1000!
    svc_hook->call_original(ctx, g_replace_handle, cpu);
    cpu->r[0] += 1000u;
    return PH_HOOK_CONTINUE;
}

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* err) {
    svc_log->info(mod_ctx, "consumer_mod initializing");
    // Call provider's custom exported service (guarantees provider initialized first!)
    const int val = svc_math->add_offset(mod_ctx, 23);
    svc_mem->poke32(mod_ctx, 0x02000410u, uint32_t(val));

    // Resolve and hook two mock ARM9 symbols!
    const uint32_t fn1 = svc_hook->resolve_symbol(mod_ctx, "TestTargetFn");
    const uint32_t fn2 = svc_hook->resolve_symbol(mod_ctx, "TestReplaceFn");
    if (!fn1 || !fn2) return ph_mods::set_error(err, MOD_UNAVAILABLE, "failed to resolve test symbols");

    if (svc_hook->add_pre(mod_ctx, fn1, on_target_pre, nullptr, 0, nullptr) != MOD_OK)
        return ph_mods::set_error(err, MOD_ERROR, "add_pre failed");
    if (svc_hook->add_post(mod_ctx, fn1, on_target_post, nullptr, 0, nullptr) != MOD_OK)
        return ph_mods::set_error(err, MOD_ERROR, "add_post failed");
    if (svc_hook->replace(mod_ctx, fn2, on_replace_fn, nullptr, 0, &g_replace_handle) != MOD_OK)
        return ph_mods::set_error(err, MOD_ERROR, "replace failed");

    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    const int val = svc_math->add_offset(mod_ctx, 5);
    svc_mem->poke32(mod_ctx, 0x02000414u, uint32_t(val));
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(void) { return MOD_OK; }

}  // extern "C"
