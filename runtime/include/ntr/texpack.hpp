// Custom HD Texture Pack engine (2D OBJ sprites, 2D BG layers, and 3D textures).
// Zero game assets are bundled: players dump textures from their own ROM with --dump-textures into
// <data-dir>/textures/dump/{obj,bg,3d}/ and place replacement PNGs under <data-dir>/textures/load/.
#pragma once

#include <cstdint>
#include <string>

namespace ntr::texpack {

struct Stats {
    size_t obj_count = 0;
    size_t bg_count = 0;
    size_t tex3d_count = 0;
    size_t any_count = 0;
    size_t dumped_count = 0;
};

// Base directory containing dump/ and load/ (typically <data-dir>/textures).
void set_root(const std::string& dir);
std::string root();

// Enable/disable replacement texture loading at runtime (safe from any thread; invalidates caches on next frame).
void set_enabled(bool enabled);
bool enabled();

// Enable/disable automatic PNG dumping of seen OBJ, BG, and 3D textures into <root>/dump/{obj,bg,3d}/.
void set_dump(bool dump);
bool dump_enabled();

// Rescan <root>/load/ recursively and bump the generation counter so active renderers refresh.
void reload();

// Delete all cached OpenGL texture handles on the current GL context (call before destroying GL context).
void clear_gl();
// Drop cached OpenGL texture names without calling glDeleteTextures (call after Android EGL context loss).
void abandon_gl();

// Flush pending asynchronous PNG dumps (call on clean shutdown).
void flush_dumps();

Stats stats();

}  // namespace ntr::texpack

// C hooks called by melonDS's patched 2D/3D OpenGL & Compute renderers (third_party/CMakeLists.txt).
extern "C" {
uint32_t ntr_texpack_generation();
int ntr_texpack_obj_active();
int ntr_texpack_bg_active();
int ntr_texpack_3d_active();
int ntr_texpack_dump_active();

uint64_t ntr_texpack_hash(const void* data, uint32_t bytes);

void ntr_texpack_dump_obj(uint64_t hash, uint32_t w, uint32_t h, const uint32_t* rgba8);
void ntr_texpack_dump_bg(uint64_t hash, uint32_t w, uint32_t h, const uint32_t* rgba8);
void ntr_texpack_dump_3d(uint64_t hash, uint32_t w, uint32_t h, const uint32_t* rgb6a5);

// Must be called on the OpenGL context thread. Returns a GL texture handle (or 0 if no replacement exists).
unsigned ntr_texpack_get_obj(uint64_t hash);
unsigned ntr_texpack_get_bg(uint64_t hash);
unsigned ntr_texpack_get_3d(uint64_t hash, int use_tex_storage);

void ntr_texpack_clear_gl();
}
