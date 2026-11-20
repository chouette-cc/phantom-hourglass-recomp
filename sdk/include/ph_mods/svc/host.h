#ifndef PH_MODS_SVC_HOST_H
#define PH_MODS_SVC_HOST_H

#include "ph_mods/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_HOST_SERVICE_ID "ph.core.host"
#define PH_HOST_SERVICE_MAJOR 1u
#define PH_HOST_SERVICE_MINOR 0u

typedef enum ModLifecycleEvent {
    MOD_LIFECYCLE_ATTACHED = 0,
    MOD_LIFECYCLE_DETACHED = 1
} ModLifecycleEvent;

typedef void (*ModLifecycleCallback)(ModContext* ctx, ModContext* subject, const char* subject_id,
                                     ModLifecycleEvent event, void* user_data);

typedef struct HostService {
    ServiceHeader header;
    const char* (*host_version)(void);
    const char* (*game_code)(void);
    const char* (*platform)(void);
    const char* (*mod_id)(ModContext* ctx);
    const char* (*mod_dir)(ModContext* ctx);     /* extracted bundle or folder path (read-only) */
    const char* (*data_dir)(ModContext* ctx);    /* persistent writable data directory for this mod */
    const char* (*native_dir)(ModContext* ctx);  /* directory where native libraries were extracted */
    uint64_t (*frame_count)(void);
    float (*fps)(void);
    double (*dt)(void);
    void (*notify)(ModContext* ctx, const char* message, uint64_t duration_ms);
    void (*fail)(ModContext* ctx, ModResult code, const char* message);
    const void* (*get_service)(ModContext* ctx, const char* id, uint16_t major_version, uint16_t min_minor_version);
    ModResult (*publish_service)(ModContext* ctx, const char* id, uint16_t major_version, uint16_t minor_version,
                                 const void* service);
    ModResult (*watch_mod_lifecycle)(ModContext* ctx, ModLifecycleCallback callback, void* user_data,
                                     uint64_t* out_watch_id);
    ModResult (*unwatch_mod_lifecycle)(ModContext* ctx, uint64_t watch_id);
} HostService;

#ifdef __cplusplus
}
#include "ph_mods/service.hpp"
template <>
struct ph_mods::ServiceTraits<HostService> {
    static constexpr const char* id = PH_HOST_SERVICE_ID;
    static constexpr uint16_t major_version = PH_HOST_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = PH_HOST_SERVICE_MINOR;
};
#endif

#endif /* PH_MODS_SVC_HOST_H */
