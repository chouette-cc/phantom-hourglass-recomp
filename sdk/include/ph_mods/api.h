/*
 * Phantom Hourglass Recomp — Native Mod SDK (C11 ABI)
 *
 * Versioned service architecture inspired by Dusklight:
 *   - Native mods export `ph_mod_descriptor` (provided automatically by DEFINE_MOD() in service.hpp)
 *     plus lifecycle functions (`mod_initialize`, `mod_update`, `mod_input`, `mod_draw`,
 *     `mod_post_draw`, `mod_shutdown`).
 *   - Host capabilities and cross-mod APIs are exposed as individually versioned C structs
 *     beginning with `ServiceHeader`.
 */
#ifndef PH_MODS_API_H
#define PH_MODS_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define MOD_EXPORT __declspec(dllexport)
#else
#define MOD_EXPORT __attribute__((visibility("default")))
#endif

#define PH_MOD_SDK_ABI_VERSION 1u

typedef enum ModResult {
    MOD_OK = 0,
    MOD_ERROR = 1,
    MOD_INVALID_ARGUMENT = 2,
    MOD_UNAVAILABLE = 3,
    MOD_CONFLICT = 4,
    MOD_UNSUPPORTED = 5,
    MOD_BUFFER_TOO_SMALL = 6
} ModResult;

typedef struct ModError {
    ModResult code;
    char message[256];
} ModError;

typedef struct ModContext ModContext;

typedef struct ServiceHeader {
    uint32_t struct_size;
    uint16_t major_version;
    uint16_t minor_version;
} ServiceHeader;

#define SERVICE_HEADER(Type, major_ver, minor_ver) \
    { (uint32_t)sizeof(Type), (uint16_t)(major_ver), (uint16_t)(minor_ver) }

#define SERVICE_HAS(svc_ptr, field) \
    ((svc_ptr) != NULL && ((svc_ptr)->header.struct_size >= (uint32_t)(offsetof(__typeof__(*(svc_ptr)), field) + sizeof((svc_ptr)->field))))

typedef struct ModServiceImport {
    const char* id;
    uint16_t major_version;
    uint16_t min_minor_version;
    bool optional;
    const void** target_slot; /* loader writes the resolved service pointer here */
} ModServiceImport;

typedef struct ModServiceExport {
    const char* id;
    uint16_t major_version;
    uint16_t minor_version;
    const void* service; /* NULL for deferred services published during mod_initialize */
} ModServiceExport;

/* Forward declarations for optional frame/input/draw callbacks. */
typedef struct PhInputState PhInputState;
typedef struct PhDrawContext PhDrawContext;

typedef struct ModDescriptor {
    uint32_t abi_version; /* PH_MOD_SDK_ABI_VERSION */
    ModContext** ctx_slot;
    const ModServiceImport* imports;
    size_t import_count;
    const ModServiceExport* exports;
    size_t export_count;

    /* Lifecycle callbacks (any may be NULL). */
    ModResult (*initialize)(ModError* error);
    ModResult (*update)(ModError* error);
    ModResult (*on_input)(PhInputState* input, ModError* error);
    ModResult (*on_draw)(const PhDrawContext* ctx, bool* handled, ModError* error);
    ModResult (*on_post_draw)(const PhDrawContext* ctx, ModError* error);
    ModResult (*shutdown)(void);
} ModDescriptor;

typedef const ModDescriptor* (*PhModDescriptorFn)(void);
#define PH_MOD_DESCRIPTOR_SYMBOL "ph_mod_descriptor"

#ifdef __cplusplus
}
#endif

#endif /* PH_MODS_API_H */
