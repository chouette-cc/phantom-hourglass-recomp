#include "ntr/texpack.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "frontend/glad/glad.h"
#include "xxhash/xxhash.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <fstream>

namespace fs = std::filesystem;

namespace ntr::texpack {
namespace {

enum class Kind { Obj, Bg, Tex3D };

struct DumpJob {
    std::string path;
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint8_t> rgba;
};

// Parse a 16-hex-digit hash prefix from a filename stem (e.g. "0123456789abcdef" or "0123456789abcdef_start").
bool parse_hash_stem(const std::string& stem, uint64_t& out) {
    if (stem.size() < 16) return false;
    if (stem.size() > 16 && stem[16] != '_' && stem[16] != '-' && stem[16] != '.') return false;
    uint64_t v = 0;
    for (int i = 0; i < 16; ++i) {
        const unsigned char ch = static_cast<unsigned char>(stem[size_t(i)]);
        uint64_t nib = 0;
        if (ch >= '0' && ch <= '9') nib = ch - '0';
        else if (ch >= 'a' && ch <= 'f') nib = 10 + (ch - 'a');
        else if (ch >= 'A' && ch <= 'F') nib = 10 + (ch - 'A');
        else return false;
        v = (v << 4) | nib;
    }
    out = v;
    return true;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(fs::path(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto sz = f.tellg();
    if (sz <= 0 || sz > 64 * 1024 * 1024) return false;
    f.seekg(0, std::ios::beg);
    out.resize(size_t(sz));
    return bool(f.read(reinterpret_cast<char*>(out.data()), sz));
}

struct State {
    std::atomic<bool> enabled{false};
    std::atomic<bool> dump{false};
    std::atomic<uint32_t> generation{1};
    std::atomic<bool> has_obj{false};
    std::atomic<bool> has_bg{false};
    std::atomic<bool> has_3d{false};
    std::atomic<size_t> dumped_count{0};

    std::mutex index_mutex;
    std::string root_dir;
    std::unordered_map<uint64_t, std::string> obj_paths;
    std::unordered_map<uint64_t, std::string> bg_paths;
    std::unordered_map<uint64_t, std::string> tex3d_paths;
    std::unordered_map<uint64_t, std::string> any_paths;

    // GL texture caches (accessed only on the GL context thread).
    std::mutex gl_mutex;
    uint32_t gl_gen = 0;
    std::unordered_map<uint64_t, GLuint> gl_obj;
    std::unordered_map<uint64_t, GLuint> gl_bg;
    std::unordered_map<uint64_t, GLuint> gl_3d;

    // Asynchronous PNG dump queue (prevents disk I/O from stalling frames).
    std::mutex dump_mutex;
    std::condition_variable dump_cv;
    std::unordered_set<uint64_t> dumped_obj;
    std::unordered_set<uint64_t> dumped_bg;
    std::unordered_set<uint64_t> dumped_3d;
    std::deque<DumpJob> dump_queue;
    std::thread dump_worker;
    bool dump_stop = false;

    ~State() {
        stop_dump_worker();
    }

    void start_dump_worker_locked() {
        if (dump_worker.joinable()) return;
        dump_stop = false;
        dump_worker = std::thread([this] {
            for (;;) {
                DumpJob job;
                {
                    std::unique_lock<std::mutex> lock(dump_mutex);
                    dump_cv.wait(lock, [this] { return dump_stop || !dump_queue.empty(); });
                    if (dump_stop && dump_queue.empty()) break;
                    job = std::move(dump_queue.front());
                    dump_queue.pop_front();
                }
                std::error_code ec;
                const fs::path p = fs::path(job.path);
                fs::create_directories(p.parent_path(), ec);
                if (!fs::exists(p, ec)) {
                    if (stbi_write_png(job.path.c_str(), int(job.w), int(job.h), 4, job.rgba.data(), int(job.w * 4))) {
                        dumped_count.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    void stop_dump_worker() {
        {
            std::lock_guard<std::mutex> lock(dump_mutex);
            if (!dump_worker.joinable()) return;
            dump_stop = true;
        }
        dump_cv.notify_all();
        if (dump_worker.joinable()) dump_worker.join();
    }

    void flush_dumps() {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(dump_mutex);
                if (dump_queue.empty() || !dump_worker.joinable()) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void scan_load_dir() {
        std::unordered_map<uint64_t, std::string> n_obj, n_bg, n_3d, n_any;
        std::string root_copy;
        {
            std::lock_guard<std::mutex> lock(index_mutex);
            root_copy = root_dir;
        }
        if (!root_copy.empty()) {
            const fs::path load_root = fs::path(root_copy) / "load";
            std::error_code ec;
            if (fs::is_directory(load_root, ec)) {
                for (fs::recursive_directory_iterator it(load_root, fs::directory_options::skip_permission_denied, ec), end;
                     it != end && !ec; it.increment(ec)) {
                    if (!it->is_regular_file(ec)) continue;
                    const fs::path p = it->path();
                    const std::string ext = to_lower(p.extension().string());
                    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp" && ext != ".tga") continue;
                    uint64_t hash = 0;
                    if (!parse_hash_stem(p.stem().string(), hash)) continue;

                    const fs::path rel = fs::relative(p, load_root, ec);
                    std::string first_dir;
                    if (!ec && rel.begin() != rel.end()) {
                        auto seg = rel.begin();
                        auto next = seg;
                        ++next;
                        if (next != rel.end()) first_dir = to_lower(seg->string());
                    }
                    const std::string full = p.string();
                    if (first_dir == "obj" || first_dir == "sprites" || first_dir == "ui") {
                        n_obj[hash] = full;
                    } else if (first_dir == "bg" || first_dir == "backgrounds") {
                        n_bg[hash] = full;
                    } else if (first_dir == "3d" || first_dir == "textures3d") {
                        n_3d[hash] = full;
                    } else {
                        n_any[hash] = full;
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(index_mutex);
            obj_paths = std::move(n_obj);
            bg_paths = std::move(n_bg);
            tex3d_paths = std::move(n_3d);
            any_paths = std::move(n_any);
            const bool any = !any_paths.empty();
            has_obj.store(any || !obj_paths.empty(), std::memory_order_relaxed);
            has_bg.store(any || !bg_paths.empty(), std::memory_order_relaxed);
            has_3d.store(any || !tex3d_paths.empty(), std::memory_order_relaxed);
            if (any || !obj_paths.empty() || !bg_paths.empty() || !tex3d_paths.empty()) {
                std::fprintf(stderr, "texpack: indexed %zu obj, %zu bg, %zu 3d, %zu shared textures from %s/load\n",
                             obj_paths.size(), bg_paths.size(), tex3d_paths.size(), any_paths.size(), root_copy.c_str());
            }
        }
        generation.fetch_add(1, std::memory_order_release);
    }

    std::string find_path(Kind kind, uint64_t hash) {
        std::lock_guard<std::mutex> lock(index_mutex);
        const auto& specific = (kind == Kind::Obj) ? obj_paths : (kind == Kind::Bg) ? bg_paths : tex3d_paths;
        if (auto it = specific.find(hash); it != specific.end()) return it->second;
        if (auto it = any_paths.find(hash); it != any_paths.end()) return it->second;
        return {};
    }

    void clear_gl_locked() {
        for (const auto& [h, id] : gl_obj) if (id) glDeleteTextures(1, &id);
        for (const auto& [h, id] : gl_bg) if (id) glDeleteTextures(1, &id);
        for (const auto& [h, id] : gl_3d) if (id) glDeleteTextures(1, &id);
        gl_obj.clear();
        gl_bg.clear();
        gl_3d.clear();
    }

    void sync_gl_generation_locked() {
        const uint32_t g = generation.load(std::memory_order_acquire);
        if (gl_gen != g) {
            clear_gl_locked();
            gl_gen = g;
        }
    }

    void enqueue_dump(Kind kind, uint64_t hash, uint32_t w, uint32_t h, std::vector<uint8_t> rgba) {
        if (!dump.load(std::memory_order_relaxed) || w == 0 || h == 0) return;
        std::string root_copy;
        {
            std::lock_guard<std::mutex> lock(index_mutex);
            root_copy = root_dir;
        }
        if (root_copy.empty()) return;

        std::lock_guard<std::mutex> lock(dump_mutex);
        auto& seen = (kind == Kind::Obj) ? dumped_obj : (kind == Kind::Bg) ? dumped_bg : dumped_3d;
        if (!seen.insert(hash).second) return;
        if (dump_queue.size() >= 256) return;  // cap memory if game streams rapidly

        const char* sub = (kind == Kind::Obj) ? "obj" : (kind == Kind::Bg) ? "bg" : "3d";
        char fname[64];
        std::snprintf(fname, sizeof fname, "%016llx_%ux%u.png",
                      static_cast<unsigned long long>(hash), unsigned(w), unsigned(h));
        const fs::path full = fs::path(root_copy) / "dump" / sub / fname;
        start_dump_worker_locked();
        dump_queue.push_back({full.string(), w, h, std::move(rgba)});
        dump_cv.notify_one();
    }
};

State& state() {
    static State s;
    return s;
}

GLuint upload_2d_rgba(const uint8_t* pixels, int w, int h, bool clamp_edges) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    const GLint wrap = clamp_edges ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return tex;
}

GLuint upload_3d_rgb6a5_array(const uint8_t* rgba, int w, int h, bool use_tex_storage) {
    std::vector<uint8_t> conv(size_t(w) * size_t(h) * 4u);
    const size_t n = size_t(w) * size_t(h);
    for (size_t i = 0; i < n; ++i) {
        const size_t o = i * 4u;
        conv[o + 0] = static_cast<uint8_t>(rgba[o + 0] >> 2);  // 0..63
        conv[o + 1] = static_cast<uint8_t>(rgba[o + 1] >> 2);  // 0..63
        conv[o + 2] = static_cast<uint8_t>(rgba[o + 2] >> 2);  // 0..63
        conv[o + 3] = static_cast<uint8_t>(rgba[o + 3] >> 3);  // 0..31
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (use_tex_storage) {
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8UI, w, h, 1);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, w, h, 1, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, conv.data());
    } else {
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8UI, w, h, 1, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, conv.data());
    }
    return tex;
}

}  // namespace

void set_root(const std::string& dir) {
    {
        std::lock_guard<std::mutex> lock(state().index_mutex);
        state().root_dir = dir;
    }
    state().scan_load_dir();
}

std::string root() {
    std::lock_guard<std::mutex> lock(state().index_mutex);
    return state().root_dir;
}

void set_enabled(bool enabled) {
    const bool prev = state().enabled.exchange(enabled, std::memory_order_acq_rel);
    if (prev != enabled) {
        if (enabled) state().scan_load_dir();
        else state().generation.fetch_add(1, std::memory_order_release);
    }
}

bool enabled() {
    return state().enabled.load(std::memory_order_relaxed);
}

void set_dump(bool dump) {
    state().dump.store(dump, std::memory_order_relaxed);
    if (dump) state().generation.fetch_add(1, std::memory_order_release);
}

bool dump_enabled() {
    return state().dump.load(std::memory_order_relaxed);
}

void reload() {
    state().scan_load_dir();
}

void clear_gl() {
    std::lock_guard<std::mutex> lock(state().gl_mutex);
    state().clear_gl_locked();
}

void abandon_gl() {
    std::lock_guard<std::mutex> lock(state().gl_mutex);
    state().gl_obj.clear();
    state().gl_bg.clear();
    state().gl_3d.clear();
}

void flush_dumps() {
    state().flush_dumps();
}

Stats stats() {
    Stats out;
    std::lock_guard<std::mutex> lock(state().index_mutex);
    out.obj_count = state().obj_paths.size();
    out.bg_count = state().bg_paths.size();
    out.tex3d_count = state().tex3d_paths.size();
    out.any_count = state().any_paths.size();
    out.dumped_count = state().dumped_count.load(std::memory_order_relaxed);
    return out;
}

}  // namespace ntr::texpack

extern "C" {

uint32_t ntr_texpack_generation() {
    return ntr::texpack::state().generation.load(std::memory_order_acquire);
}

int ntr_texpack_obj_active() {
    auto& s = ntr::texpack::state();
    return (s.dump.load(std::memory_order_relaxed) ||
            (s.enabled.load(std::memory_order_relaxed) && s.has_obj.load(std::memory_order_relaxed))) ? 1 : 0;
}

int ntr_texpack_bg_active() {
    auto& s = ntr::texpack::state();
    return (s.dump.load(std::memory_order_relaxed) ||
            (s.enabled.load(std::memory_order_relaxed) && s.has_bg.load(std::memory_order_relaxed))) ? 1 : 0;
}

int ntr_texpack_3d_active() {
    auto& s = ntr::texpack::state();
    return (s.dump.load(std::memory_order_relaxed) ||
            (s.enabled.load(std::memory_order_relaxed) && s.has_3d.load(std::memory_order_relaxed))) ? 1 : 0;
}

int ntr_texpack_dump_active() {
    return ntr::texpack::state().dump.load(std::memory_order_relaxed) ? 1 : 0;
}

uint64_t ntr_texpack_hash(const void* data, uint32_t bytes) {
    return XXH64(data, bytes, 0);
}

void ntr_texpack_dump_obj(uint64_t hash, uint32_t w, uint32_t h, const uint32_t* rgba8) {
    if (!rgba8 || w == 0 || h == 0) return;
    const size_t bytes = size_t(w) * size_t(h) * 4u;
    std::vector<uint8_t> buf(bytes);
    std::memcpy(buf.data(), rgba8, bytes);
    ntr::texpack::state().enqueue_dump(ntr::texpack::Kind::Obj, hash, w, h, std::move(buf));
}

void ntr_texpack_dump_bg(uint64_t hash, uint32_t w, uint32_t h, const uint32_t* rgba8) {
    if (!rgba8 || w == 0 || h == 0) return;
    const size_t bytes = size_t(w) * size_t(h) * 4u;
    std::vector<uint8_t> buf(bytes);
    std::memcpy(buf.data(), rgba8, bytes);
    ntr::texpack::state().enqueue_dump(ntr::texpack::Kind::Bg, hash, w, h, std::move(buf));
}

void ntr_texpack_dump_3d(uint64_t hash, uint32_t w, uint32_t h, const uint32_t* rgb6a5) {
    if (!rgb6a5 || w == 0 || h == 0) return;
    const size_t n = size_t(w) * size_t(h);
    std::vector<uint8_t> rgba(n * 4u);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(rgb6a5);
    for (size_t i = 0; i < n; ++i) {
        const size_t o = i * 4u;
        const uint8_t r6 = src[o + 0] & 0x3Fu;
        const uint8_t g6 = src[o + 1] & 0x3Fu;
        const uint8_t b6 = src[o + 2] & 0x3Fu;
        const uint8_t a5 = src[o + 3] & 0x1Fu;
        rgba[o + 0] = static_cast<uint8_t>((r6 << 2) | (r6 >> 4));
        rgba[o + 1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
        rgba[o + 2] = static_cast<uint8_t>((b6 << 2) | (b6 >> 4));
        rgba[o + 3] = static_cast<uint8_t>((a5 << 3) | (a5 >> 2));
    }
    ntr::texpack::state().enqueue_dump(ntr::texpack::Kind::Tex3D, hash, w, h, std::move(rgba));
}

unsigned ntr_texpack_get_obj(uint64_t hash) {
    auto& s = ntr::texpack::state();
    if (!s.enabled.load(std::memory_order_relaxed) || !s.has_obj.load(std::memory_order_relaxed)) return 0;
    std::lock_guard<std::mutex> lock(s.gl_mutex);
    s.sync_gl_generation_locked();
    if (auto it = s.gl_obj.find(hash); it != s.gl_obj.end()) return it->second;

    const std::string path = s.find_path(ntr::texpack::Kind::Obj, hash);
    if (path.empty()) {
        s.gl_obj.emplace(hash, 0u);
        return 0;
    }
    std::vector<uint8_t> bytes;
    int w = 0, h = 0, comp = 0;
    GLuint tex = 0;
    if (ntr::texpack::read_file_bytes(path, bytes)) {
        stbi_uc* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comp, 4);
        if (pixels && w > 0 && h > 0) {
            tex = ntr::texpack::upload_2d_rgba(pixels, w, h, true);
        }
        if (pixels) stbi_image_free(pixels);
    }
    s.gl_obj.emplace(hash, tex);
    return tex;
}

unsigned ntr_texpack_get_bg(uint64_t hash) {
    auto& s = ntr::texpack::state();
    if (!s.enabled.load(std::memory_order_relaxed) || !s.has_bg.load(std::memory_order_relaxed)) return 0;
    std::lock_guard<std::mutex> lock(s.gl_mutex);
    s.sync_gl_generation_locked();
    if (auto it = s.gl_bg.find(hash); it != s.gl_bg.end()) return it->second;

    const std::string path = s.find_path(ntr::texpack::Kind::Bg, hash);
    if (path.empty()) {
        s.gl_bg.emplace(hash, 0u);
        return 0;
    }
    std::vector<uint8_t> bytes;
    int w = 0, h = 0, comp = 0;
    GLuint tex = 0;
    if (ntr::texpack::read_file_bytes(path, bytes)) {
        stbi_uc* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comp, 4);
        if (pixels && w > 0 && h > 0) {
            tex = ntr::texpack::upload_2d_rgba(pixels, w, h, false);
        }
        if (pixels) stbi_image_free(pixels);
    }
    s.gl_bg.emplace(hash, tex);
    return tex;
}

unsigned ntr_texpack_get_3d(uint64_t hash, int use_tex_storage) {
    auto& s = ntr::texpack::state();
    if (!s.enabled.load(std::memory_order_relaxed) || !s.has_3d.load(std::memory_order_relaxed)) return 0;
    std::lock_guard<std::mutex> lock(s.gl_mutex);
    s.sync_gl_generation_locked();
    if (auto it = s.gl_3d.find(hash); it != s.gl_3d.end()) return it->second;

    const std::string path = s.find_path(ntr::texpack::Kind::Tex3D, hash);
    if (path.empty()) {
        s.gl_3d.emplace(hash, 0u);
        return 0;
    }
    std::vector<uint8_t> bytes;
    int w = 0, h = 0, comp = 0;
    GLuint tex = 0;
    if (ntr::texpack::read_file_bytes(path, bytes)) {
        stbi_uc* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comp, 4);
        if (pixels && w > 0 && h > 0) {
            tex = ntr::texpack::upload_3d_rgb6a5_array(pixels, w, h, use_tex_storage != 0);
        }
        if (pixels) stbi_image_free(pixels);
    }
    s.gl_3d.emplace(hash, tex);
    return tex;
}

void ntr_texpack_clear_gl() {
    ntr::texpack::clear_gl();
}

}  // extern "C"
