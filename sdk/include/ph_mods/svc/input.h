#ifndef PH_MODS_SVC_INPUT_H
#define PH_MODS_SVC_INPUT_H

#include "ph_mods/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_INPUT_SERVICE_ID "ph.core.input"
#define PH_INPUT_SERVICE_MAJOR 1u
#define PH_INPUT_SERVICE_MINOR 0u

enum PhKeyBits {
    PH_KEY_A      = 1u << 0,
    PH_KEY_B      = 1u << 1,
    PH_KEY_SELECT = 1u << 2,
    PH_KEY_START  = 1u << 3,
    PH_KEY_RIGHT  = 1u << 4,
    PH_KEY_LEFT   = 1u << 5,
    PH_KEY_UP     = 1u << 6,
    PH_KEY_DOWN   = 1u << 7,
    PH_KEY_R      = 1u << 8,
    PH_KEY_L      = 1u << 9,
    PH_KEY_X      = 1u << 10,
    PH_KEY_Y      = 1u << 11
};

struct PhInputState {
    uint32_t struct_size;
    uint32_t held;
    uint32_t pressed;
    bool touching;
    int tx, ty;
    bool blow;
    bool blow_sample;
    bool lid;
    bool stick_on;
    float stick_x, stick_y;
    float pointer_x, pointer_y;
    bool pointer_down;
    bool pointer_just_pressed;
    bool pointer_just_released;
    bool right_down;
    bool right_just_pressed;
    int win_w, win_h;
};

typedef struct InputService {
    ServiceHeader header;
    void (*get_state)(ModContext* ctx, PhInputState* out_state);
    bool (*is_key_down)(ModContext* ctx, const char* key_name);
    bool (*was_key_pressed)(ModContext* ctx, const char* key_name);
} InputService;

#ifdef __cplusplus
}
#include "ph_mods/service.hpp"
template <>
struct ph_mods::ServiceTraits<InputService> {
    static constexpr const char* id = PH_INPUT_SERVICE_ID;
    static constexpr uint16_t major_version = PH_INPUT_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = PH_INPUT_SERVICE_MINOR;
};
#endif

#endif /* PH_MODS_SVC_INPUT_H */
