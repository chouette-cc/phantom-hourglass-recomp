#ifndef PH_MODS_SVC_SAVE_H
#define PH_MODS_SVC_SAVE_H

#include "ph_mods/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_SAVE_SERVICE_ID "ph.core.save"
#define PH_SAVE_SERVICE_MAJOR 1u
#define PH_SAVE_SERVICE_MINOR 0u

#define PH_SAVE_BLOB_MAX_BYTES (1024u * 1024u) /* 1 MiB per blob */

typedef uint64_t SaveObserverHandle;

typedef enum PhSaveEvent {
    PH_SAVE_EVENT_CARTRIDGE_WRITTEN = 0,
    PH_SAVE_EVENT_STATE_SAVED = 1,
    PH_SAVE_EVENT_STATE_LOADED = 2
} PhSaveEvent;

typedef void (*PhSaveObserverCallback)(ModContext* ctx, PhSaveEvent event, int slot, void* user_data);

typedef struct SaveService {
    ServiceHeader header;
    /* Mod-scoped persistent binary blobs stored alongside the active save file (never mutates the 512 KiB flash). */
    ModResult (*set_blob)(ModContext* ctx, const char* key, const void* data, size_t size);
    /* Pass out_buf == NULL to query the blob size in *io_size. */
    ModResult (*get_blob)(ModContext* ctx, const char* key, void* out_buf, size_t* io_size);
    ModResult (*delete_blob)(ModContext* ctx, const char* key);
    ModResult (*observe_saves)(ModContext* ctx, PhSaveObserverCallback cb, void* user_data,
                               SaveObserverHandle* out_handle);
    ModResult (*unobserve_saves)(ModContext* ctx, SaveObserverHandle handle);
} SaveService;

#ifdef __cplusplus
}
#include "ph_mods/service.hpp"
template <>
struct ph_mods::ServiceTraits<SaveService> {
    static constexpr const char* id = PH_SAVE_SERVICE_ID;
    static constexpr uint16_t major_version = PH_SAVE_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = PH_SAVE_SERVICE_MINOR;
};
#endif

#endif /* PH_MODS_SVC_SAVE_H */
