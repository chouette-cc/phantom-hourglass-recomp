#ifndef PH_MODS_SVC_OVERLAY_H
#define PH_MODS_SVC_OVERLAY_H

#include "ph_mods/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_OVERLAY_SERVICE_ID "ph.core.overlay"
#define PH_OVERLAY_SERVICE_MAJOR 1u
#define PH_OVERLAY_SERVICE_MINOR 0u

typedef struct OverlayService {
    ServiceHeader header;
    /* Inspect the active (modded) ROM's NitroFS filesystem. */
    size_t (*rom_file_count)(ModContext* ctx);
    const char* (*rom_file_path)(ModContext* ctx, size_t index);
    bool (*rom_file_size)(ModContext* ctx, const char* nitro_path, uint32_t* out_size);
    size_t (*rom_read)(ModContext* ctx, const char* nitro_path, uint32_t offset, uint8_t* out_buf, size_t max_len);
} OverlayService;

#ifdef __cplusplus
}
#include "ph_mods/service.hpp"
template <>
struct ph_mods::ServiceTraits<OverlayService> {
    static constexpr const char* id = PH_OVERLAY_SERVICE_ID;
    static constexpr uint16_t major_version = PH_OVERLAY_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = PH_OVERLAY_SERVICE_MINOR;
};
#endif

#endif /* PH_MODS_SVC_OVERLAY_H */
