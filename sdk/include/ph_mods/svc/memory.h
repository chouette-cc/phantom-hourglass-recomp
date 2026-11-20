#ifndef PH_MODS_SVC_MEMORY_H
#define PH_MODS_SVC_MEMORY_H

#include "ph_mods/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_MEMORY_SERVICE_ID "ph.core.memory"
#define PH_MEMORY_SERVICE_MAJOR 1u
#define PH_MEMORY_SERVICE_MINOR 0u

typedef struct MemoryService {
    ServiceHeader header;
    uint8_t (*peek8)(ModContext* ctx, uint32_t addr);
    uint16_t (*peek16)(ModContext* ctx, uint32_t addr);
    uint32_t (*peek32)(ModContext* ctx, uint32_t addr);
    bool (*poke8)(ModContext* ctx, uint32_t addr, uint8_t value);
    bool (*poke16)(ModContext* ctx, uint32_t addr, uint16_t value);
    bool (*poke32)(ModContext* ctx, uint32_t addr, uint32_t value);
    size_t (*read)(ModContext* ctx, uint32_t addr, uint8_t* out_buf, size_t size);
    size_t (*write)(ModContext* ctx, uint32_t addr, const uint8_t* data, size_t size);
    /* Pattern scan using hex bytes with optional "??" wildcards (e.g. "10 40 ?? E9"). Returns 0 if not found. */
    uint32_t (*find_pattern)(ModContext* ctx, uint32_t start_addr, size_t length, const char* pattern);
} MemoryService;

#ifdef __cplusplus
}
#include "ph_mods/service.hpp"
template <>
struct ph_mods::ServiceTraits<MemoryService> {
    static constexpr const char* id = PH_MEMORY_SERVICE_ID;
    static constexpr uint16_t major_version = PH_MEMORY_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = PH_MEMORY_SERVICE_MINOR;
};
#endif

#endif /* PH_MODS_SVC_MEMORY_H */
