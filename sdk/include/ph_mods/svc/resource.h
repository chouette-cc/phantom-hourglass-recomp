#ifndef PH_MODS_SVC_RESOURCE_H
#define PH_MODS_SVC_RESOURCE_H

#include "ph_mods/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_RESOURCE_SERVICE_ID "ph.core.resource"
#define PH_RESOURCE_SERVICE_MAJOR 1u
#define PH_RESOURCE_SERVICE_MINOR 0u

typedef struct ResourceBuffer {
    uint8_t* data;
    size_t size;
} ResourceBuffer;

#define RESOURCE_BUFFER_INIT { NULL, 0 }

typedef struct ResourceService {
    ServiceHeader header;
    /* Loads a file relative to the mod's `res/` directory. Rejects absolute paths and `..`. */
    ModResult (*load)(ModContext* ctx, const char* rel_path, ResourceBuffer* out_buf);
    void (*free)(ModContext* ctx, ResourceBuffer* buf);
    bool (*exists)(ModContext* ctx, const char* rel_path);
} ResourceService;

#ifdef __cplusplus
}
#include "ph_mods/service.hpp"
template <>
struct ph_mods::ServiceTraits<ResourceService> {
    static constexpr const char* id = PH_RESOURCE_SERVICE_ID;
    static constexpr uint16_t major_version = PH_RESOURCE_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = PH_RESOURCE_SERVICE_MINOR;
};
#endif

#endif /* PH_MODS_SVC_RESOURCE_H */
