#ifndef PH_MODS_SVC_HOOK_H
#define PH_MODS_SVC_HOOK_H

#include "ph_mods/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_HOOK_SERVICE_ID "ph.core.hook"
#define PH_HOOK_SERVICE_MAJOR 1u
#define PH_HOOK_SERVICE_MINOR 0u

typedef uint64_t PhHookHandle;

typedef enum PhHookAction {
    PH_HOOK_CONTINUE = 0,
    PH_HOOK_SKIP_ORIGINAL = 1
} PhHookAction;

/* Guest ARM9 CPU register view passed into function hooks (matches NtrCpu layout). */
typedef struct PhArm9Cpu {
    uint32_t r[16];
    uint32_t n, z, c, v, q;
    uint32_t cpsr_ctrl;
} PhArm9Cpu;

typedef PhHookAction (*PhHookCallback)(ModContext* ctx, PhArm9Cpu* cpu, uint32_t addr, void* user_data);

typedef struct HookService {
    ServiceHeader header;
    /* Resolves a function name from functions.txt to its ARM9 address (bit 0 = Thumb), or 0 if not found. */
    uint32_t (*resolve_symbol)(ModContext* ctx, const char* name);
    /* Returns the symbol name for an ARM9 function address, or NULL if unnamed. */
    const char* (*symbol_name)(ModContext* ctx, uint32_t addr);
    /* Install pre, post, or replace hooks on any recompiled ARM9 function (by address). */
    ModResult (*add_pre)(ModContext* ctx, uint32_t addr, PhHookCallback cb, void* user_data, int priority,
                         PhHookHandle* out_handle);
    ModResult (*add_post)(ModContext* ctx, uint32_t addr, PhHookCallback cb, void* user_data, int priority,
                          PhHookHandle* out_handle);
    ModResult (*replace)(ModContext* ctx, uint32_t addr, PhHookCallback cb, void* user_data, int priority,
                         PhHookHandle* out_handle);
    ModResult (*remove)(ModContext* ctx, PhHookHandle handle);
    /* From inside a replace hook, calls the original ARM9 function with `cpu`. */
    ModResult (*call_original)(ModContext* ctx, PhHookHandle handle, PhArm9Cpu* cpu);
    /* Invokes a guest ARM9 function safely from mod_update / hooks. */
    ModResult (*call_guest)(ModContext* ctx, uint32_t addr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                            uint32_t* out_r0);
} HookService;

#ifdef __cplusplus
}
#include "ph_mods/service.hpp"
template <>
struct ph_mods::ServiceTraits<HookService> {
    static constexpr const char* id = PH_HOOK_SERVICE_ID;
    static constexpr uint16_t major_version = PH_HOOK_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = PH_HOOK_SERVICE_MINOR;
};
#endif

#endif /* PH_MODS_SVC_HOOK_H */
