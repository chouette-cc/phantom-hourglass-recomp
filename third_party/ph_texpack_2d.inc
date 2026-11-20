// ph patch: Custom HD Texture Pack helpers for 2D OBJ sprites and 2D BG layers (runtime/src/texpack.cpp).
extern "C" {
uint32_t ntr_texpack_generation();
int ntr_texpack_obj_active();
int ntr_texpack_bg_active();
int ntr_texpack_dump_active();
uint64_t ntr_texpack_hash(const void* data, uint32_t bytes);
void ntr_texpack_dump_obj(uint64_t hash, uint32_t w, uint32_t h, const uint32_t* rgba8);
void ntr_texpack_dump_bg(uint64_t hash, uint32_t w, uint32_t h, const uint32_t* rgba8);
unsigned ntr_texpack_get_obj(uint64_t hash);
unsigned ntr_texpack_get_bg(uint64_t hash);
}

static GLuint ph_custom_bg_tex[2][4] = {};
static GLuint ph_custom_spr_tex[2][128] = {};
static GLint ph_use_custom_spr_uloc = -1;
static uint32_t ph_tp_gen[2] = {0, 0};
static bool ph_tp_render_trans = false;

static inline uint32_t ph_rgb555_to_rgba8(uint16_t c15)
{
    const uint32_t r5 = c15 & 0x1Fu;
    const uint32_t g5 = (c15 >> 5) & 0x1Fu;
    const uint32_t b5 = (c15 >> 10) & 0x1Fu;
    const uint32_t r = (r5 << 3) | (r5 >> 2);
    const uint32_t g = (g5 << 3) | (g5 >> 2);
    const uint32_t b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;  // little-endian RGBA8888 bytes
}

// Decodes a single OAM sprite to RGBA8888 on CPU (matches 2DSpritePreFS.glsl bit-for-bit, zero GPU stall).
// Returns false if the sprite is completely transparent.
template <typename OamT>
static bool ph_decode_oam_sprite(const OamT& sp, const u8* vram, u32 vrammask, const u16* palbuf, uint32_t* out)
{
    const int w = sp.Size[0], h = sp.Size[1];
    if (w <= 0 || h <= 0 || w > 64 || h > 64) return false;
    bool non_empty = false;
    if (sp.Type == 0)
    {
        for (int y = 0; y < h; y++)
        {
            const u32 yrow = sp.TileOffset + ((y >> 3) * sp.TileStride) + ((y & 7) << 2);
            for (int x = 0; x < w; x++)
            {
                const u32 off = yrow + ((x >> 3) * 32) + ((x & 7) >> 1);
                u8 b = vram[off & vrammask];
                u8 idx = (x & 1) ? (b >> 4) : (b & 0xF);
                if (idx == 0) out[y * w + x] = 0;
                else { out[y * w + x] = ph_rgb555_to_rgba8(palbuf[(sp.PalOffset + idx) & 0xFF]); non_empty = true; }
            }
        }
    }
    else if (sp.Type == 1)
    {
        const u32 palbase = (sp.PalOffset < 17) ? (sp.PalOffset * 256u) : 0u;
        for (int y = 0; y < h; y++)
        {
            const u32 yrow = sp.TileOffset + ((y >> 3) * sp.TileStride) + ((y & 7) << 3);
            for (int x = 0; x < w; x++)
            {
                const u32 off = yrow + ((x >> 3) * 64) + (x & 7);
                const u8 idx = vram[off & vrammask];
                if (idx == 0) out[y * w + x] = 0;
                else { out[y * w + x] = ph_rgb555_to_rgba8(palbuf[palbase + idx]); non_empty = true; }
            }
        }
    }
    else if (sp.Type == 2)
    {
        for (int y = 0; y < h; y++)
        {
            const u32 yrow = sp.TileOffset + (y * sp.TileStride);
            for (int x = 0; x < w; x++)
            {
                const u32 off = yrow + (x * 2);
                const u16 c15 = u16(vram[off & vrammask]) | (u16(vram[(off + 1) & vrammask]) << 8);
                if ((c15 & 0x8000u) == 0) out[y * w + x] = 0;
                else { out[y * w + x] = ph_rgb555_to_rgba8(c15); non_empty = true; }
            }
        }
    }
    return non_empty;
}
