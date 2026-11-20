#ifndef PH_MODS_SVC_DS_H
#define PH_MODS_SVC_DS_H

#include "ph_mods/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_DS_SERVICE_ID "ph.core.ds"
#define PH_DS_SERVICE_MAJOR 1u
#define PH_DS_SERVICE_MINOR 0u

typedef struct PhDisplayInfo {
    uint32_t struct_size;
    uint32_t powcnt1;
    uint32_t dispcnt_a;
    uint32_t dispcnt_b;
    uint16_t master_bright_a;
    uint16_t master_bright_b;
    uint32_t disp3dcnt;
    bool screen_swap;
    bool screens_enabled;
    bool lid_closed;
    bool new_3d_frame;
} PhDisplayInfo;

typedef struct DsService {
    ServiceHeader header;
    void (*display_info)(ModContext* ctx, PhDisplayInfo* out_info);
    int (*wide_extra)(ModContext* ctx);
    int (*wide_layer)(ModContext* ctx);
    void (*set_layout)(ModContext* ctx, int layout); /* 0 bottom, 1 top, 2 both */
    int (*get_layout)(ModContext* ctx);
    void (*save_state)(ModContext* ctx, int slot);
    void (*load_state)(ModContext* ctx, int slot);
    void (*screenshot)(ModContext* ctx, const char* filename);
    void (*set_mic_blow)(ModContext* ctx, bool on);
    void (*set_mic_sample)(ModContext* ctx, bool on);
    void (*set_lid_closed)(ModContext* ctx, bool closed);
    void (*set_cpu_speed)(ModContext* ctx, int multiplier);
    void (*set_fast_cart)(ModContext* ctx, bool on);
} DsService;

#ifdef __cplusplus
}
#include "ph_mods/service.hpp"
template <>
struct ph_mods::ServiceTraits<DsService> {
    static constexpr const char* id = PH_DS_SERVICE_ID;
    static constexpr uint16_t major_version = PH_DS_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = PH_DS_SERVICE_MINOR;
};
#endif

#endif /* PH_MODS_SVC_DS_H */
