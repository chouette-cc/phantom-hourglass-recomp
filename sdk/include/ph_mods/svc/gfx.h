#ifndef PH_MODS_SVC_GFX_H
#define PH_MODS_SVC_GFX_H

#include "ph_mods/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_GFX_SERVICE_ID "ph.core.gfx"
#define PH_GFX_SERVICE_MAJOR 1u
#define PH_GFX_SERVICE_MINOR 0u

typedef struct PhRect {
    float x, y, w, h;
} PhRect;

typedef struct PhViews {
    bool top_shown;
    bool bottom_shown;
    PhRect top;
    PhRect bottom;
} PhViews;

struct PhDrawContext {
    uint32_t struct_size;
    int win_w;
    int win_h;
    float ui_scale;
    bool smooth;
    bool wide;
    bool stretch;
    int layout;      /* 0 bottom, 1 top, 2 both */
    int wide_extra;
    int wide_layer;  /* 0 top, 1 bottom */
    PhViews default_views;
};

typedef struct PhScreenDrawOpts {
    uint32_t struct_size;
    float u0, v0, u1, v1; /* normalized [0..1] source UV; ignored if use_src_ds is true */
    bool use_src_ds;      /* if true, sample src_ds in 256x192 DS pixel coordinates */
    PhRect src_ds;
    bool smooth;
    bool wide;
    float alpha;          /* 0..1 */
    uint32_t tint_argb;   /* 0xAARRGGBB */
} PhScreenDrawOpts;

#define PH_SCREEN_DRAW_OPTS_INIT { (uint32_t)sizeof(PhScreenDrawOpts), 0.0f, 0.0f, 1.0f, 1.0f, false, {0,0,256,192}, false, false, 1.0f, 0xFFFFFFFFu }

typedef struct PhTouchRectOpts {
    uint32_t struct_size;
    bool use_src_ds;
    PhRect src_ds;
    bool wide;
    bool block;
    int priority;
} PhTouchRectOpts;

#define PH_TOUCH_RECT_OPTS_INIT { (uint32_t)sizeof(PhTouchRectOpts), false, {0,0,256,192}, false, false, 0 }

typedef struct GfxService {
    ServiceHeader header;
    void (*win_size)(ModContext* ctx, int* out_w, int* out_h, float* out_ui_scale);
    PhViews (*default_views)(ModContext* ctx, int layout);
    void (*draw_screen)(ModContext* ctx, int layer, PhRect dst, const PhScreenDrawOpts* opts);
    void (*fill)(ModContext* ctx, PhRect dst, uint32_t argb);
    void (*outline)(ModContext* ctx, PhRect dst, float thickness, uint32_t argb);
    float (*text)(ModContext* ctx, float x, float y, float size, const char* str, uint32_t argb);
    void (*set_clip)(ModContext* ctx, PhRect clip);
    void (*clear_clip)(ModContext* ctx);
    /* Loads PNG/BMP relative to mod's `res/` (or mod root). Returns image handle > 0 or 0 on failure. */
    int (*load_image)(ModContext* ctx, const char* rel_path, bool smooth, int* out_w, int* out_h);
    int (*create_image)(ModContext* ctx, const uint32_t* rgba_pixels, int w, int h, bool smooth);
    void (*destroy_image)(ModContext* ctx, int image_id);
    void (*draw_image)(ModContext* ctx, int image_id, PhRect dst, float u0, float v0, float u1, float v1,
                       float alpha, uint32_t tint_argb);
    void (*set_touch_rect)(ModContext* ctx, PhRect rect, const PhTouchRectOpts* opts);
    void (*clear_touch_rects)(ModContext* ctx);
    void (*to_ds)(ModContext* ctx, PhRect rect, float px, float py, bool use_wide, int* out_x, int* out_y);
} GfxService;

#ifdef __cplusplus
}
#include "ph_mods/service.hpp"
template <>
struct ph_mods::ServiceTraits<GfxService> {
    static constexpr const char* id = PH_GFX_SERVICE_ID;
    static constexpr uint16_t major_version = PH_GFX_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = PH_GFX_SERVICE_MINOR;
};
#endif

#endif /* PH_MODS_SVC_GFX_H */
