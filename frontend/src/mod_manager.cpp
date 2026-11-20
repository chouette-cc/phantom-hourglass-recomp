#include "mod_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "gl_present.hpp"
#include "mod_bundle.hpp"
#include "mod_json.hpp"

// Native Mod SDK (versioned C services)
#include "ph_mods/api.h"
#include "ph_mods/svc/config.h"
#include "ph_mods/svc/ds.h"
#include "ph_mods/svc/gfx.h"
#include "ph_mods/svc/hook.h"
#include "ph_mods/svc/host.h"
#include "ph_mods/svc/input.h"
#include "ph_mods/svc/log.h"
#include "ph_mods/svc/memory.h"
#include "ph_mods/svc/overlay.h"
#include "ph_mods/svc/resource.h"
#include "ph_mods/svc/save.h"

namespace ph {
namespace mod_internal {

namespace fs = std::filesystem;

constexpr int kApiVersion = 1;
constexpr const char* kHostVersion = "0.1.0";
constexpr int kLuaMaxInstructions = 2000000;

const char* platform_name() {
#ifdef __ANDROID__
    return "android";
#elif defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

const char* native_platform_dir() {
#if defined(_WIN32)
    return "windows-x64";
#elif defined(__ANDROID__)
    return "android-arm64";
#elif defined(__APPLE__)
    return "macos-arm64";
#else
    return "linux-x64";
#endif
}

const char* native_lib_ext() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

void* dynlib_open(const fs::path& p, std::string& err) {
#ifdef _WIN32
    const std::wstring wp = p.wstring();
    const std::wstring dir = p.parent_path().wstring();
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR lets sibling runtime libraries in lib/<platform>/ resolve cleanly!
    HMODULE h = LoadLibraryExW(wp.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!h) h = LoadLibraryW(wp.c_str());
    if (!h) err = "LoadLibrary failed (Win32 error " + std::to_string(GetLastError()) + ")";
    return reinterpret_cast<void*>(h);
#else
    const std::string s = p.string();
    void* h = dlopen(s.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* de = dlerror();
        err = de ? de : "dlopen failed";
    }
    return h;
#endif
}

void* dynlib_sym(void* lib, const char* name) {
    if (!lib || !name) return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

void dynlib_close(void* lib) {
    if (!lib) return;
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string sanitize_id(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') out.push_back(char(std::tolower(c)));
        else if (c == ' ') out.push_back('-');
    }
    return out;
}

std::string sanitize_filename_token(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '-' || c == '_') out.push_back(char(c));
        else out.push_back('_');
    }
    return out.empty() ? "blob" : out;
}

bool parse_hex_u32(const std::string& s, uint32_t& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    const unsigned long val = std::strtoul(t.c_str(), &end, 0);
    if (!end || *end != '\0') return false;
    out = uint32_t(val);
    return true;
}

bool parse_hex_bytes(const std::string& s, std::vector<uint8_t>& out, std::vector<uint8_t>* mask_out = nullptr) {
    out.clear();
    if (mask_out) mask_out->clear();
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) {
        if (tok == "??" || tok == "?") {
            if (!mask_out) return false;
            out.push_back(0);
            mask_out->push_back(0);
            continue;
        }
        if (tok.size() == 2 && std::isxdigit(static_cast<unsigned char>(tok[0])) &&
            std::isxdigit(static_cast<unsigned char>(tok[1]))) {
            out.push_back(uint8_t(std::strtoul(tok.c_str(), nullptr, 16)));
            if (mask_out) mask_out->push_back(0xFF);
        } else if (tok.size() > 2 && tok.size() % 2 == 0) {
            for (size_t i = 0; i < tok.size(); i += 2) {
                std::string pair = tok.substr(i, 2);
                if (pair == "??") {
                    if (!mask_out) return false;
                    out.push_back(0);
                    mask_out->push_back(0);
                } else if (std::isxdigit(static_cast<unsigned char>(pair[0])) &&
                           std::isxdigit(static_cast<unsigned char>(pair[1]))) {
                    out.push_back(uint8_t(std::strtoul(pair.c_str(), nullptr, 16)));
                    if (mask_out) mask_out->push_back(0xFF);
                } else {
                    return false;
                }
            }
        } else {
            return false;
        }
    }
    return !out.empty();
}

std::string read_text_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_text_file(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), std::streamsize(text.size()));
    return bool(f);
}

bool safe_subpath(const fs::path& root, const std::string& rel, fs::path& out) {
    if (rel.empty() || rel.find('\0') != std::string::npos) return false;
    fs::path p(rel);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
    for (const auto& part : p)
        if (part == "..") return false;
    std::error_code ec;
    const fs::path canon_root = fs::weakly_canonical(root, ec);
    if (ec) return false;
    const fs::path candidate = fs::weakly_canonical(canon_root / p, ec);
    if (ec) return false;
    const std::string rs = canon_root.generic_string();
    const std::string cs = candidate.generic_string();
    if (cs.size() < rs.size() || cs.compare(0, rs.size(), rs) != 0) return false;
    if (cs.size() > rs.size() && cs[rs.size()] != '/') return false;
    out = candidate;
    return true;
}

void push_json_to_lua(lua_State* L, const json::Value& v) {
    switch (v.type) {
        case json::Type::Null: lua_pushnil(L); break;
        case json::Type::Bool: lua_pushboolean(L, v.boolean ? 1 : 0); break;
        case json::Type::Number: {
            const double r = std::trunc(v.number);
            if (v.number == r && std::fabs(v.number) < 9007199254740992.0)
                lua_pushinteger(L, lua_Integer(r));
            else
                lua_pushnumber(L, v.number);
            break;
        }
        case json::Type::String: lua_pushlstring(L, v.str.data(), v.str.size()); break;
        case json::Type::Array:
            lua_createtable(L, int(v.arr.size()), 0);
            for (size_t i = 0; i < v.arr.size(); i++) {
                push_json_to_lua(L, v.arr[i]);
                lua_rawseti(L, -2, lua_Integer(i + 1));
            }
            break;
        case json::Type::Object:
            lua_createtable(L, 0, int(v.obj.size()));
            for (const auto& [k, val] : v.obj) {
                lua_pushlstring(L, k.data(), k.size());
                push_json_to_lua(L, val);
                lua_rawset(L, -3);
            }
            break;
    }
}

json::Value lua_to_json(lua_State* L, int idx, int depth = 0) {
    if (depth > 32) return json::Value::make_null();
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN: return json::Value::make_bool(lua_toboolean(L, idx) != 0);
        case LUA_TNUMBER: return json::Value::make_number(lua_tonumber(L, idx));
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            return json::Value::make_string(std::string(s ? s : "", len));
        }
        case LUA_TTABLE: {
            size_t count = 0;
            lua_Integer max_key = 0;
            bool is_array = true;
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
                    const lua_Integer k = lua_tointeger(L, -2);
                    if (k >= 1) { max_key = std::max(max_key, k); count++; }
                    else is_array = false;
                } else {
                    is_array = false;
                }
                lua_pop(L, 1);
            }
            if (is_array && count > 0 && size_t(max_key) == count) {
                json::Value arr = json::Value::make_array();
                arr.arr.reserve(count);
                for (lua_Integer i = 1; i <= max_key; i++) {
                    lua_rawgeti(L, idx, i);
                    arr.arr.push_back(lua_to_json(L, -1, depth + 1));
                    lua_pop(L, 1);
                }
                return arr;
            }
            json::Value obj = json::Value::make_object();
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                std::string key;
                if (lua_type(L, -2) == LUA_TSTRING) {
                    size_t len = 0;
                    const char* s = lua_tolstring(L, -2, &len);
                    key.assign(s, len);
                } else if (lua_type(L, -2) == LUA_TNUMBER) {
                    key = std::to_string(lua_tointeger(L, -2));
                }
                if (!key.empty()) obj.obj[key] = lua_to_json(L, -1, depth + 1);
                lua_pop(L, 1);
            }
            return obj;
        }
        default: return json::Value::make_null();
    }
}

void watchdog_hook(lua_State* L, lua_Debug*) {
    luaL_error(L, "mod callback exceeded instruction limit (possible infinite loop)");
}

void push_rect(lua_State* L, const Rect& r) {
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, r.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, r.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, r.w); lua_setfield(L, -2, "w");
    lua_pushnumber(L, r.h); lua_setfield(L, -2, "h");
}

void push_views(lua_State* L, const Views& v) {
    lua_createtable(L, 0, 4);
    lua_pushboolean(L, v.top_shown ? 1 : 0); lua_setfield(L, -2, "top_shown");
    lua_pushboolean(L, v.bottom_shown ? 1 : 0); lua_setfield(L, -2, "bottom_shown");
    push_rect(L, v.top); lua_setfield(L, -2, "top");
    push_rect(L, v.bottom); lua_setfield(L, -2, "bottom");
}

const char* layout_name(Layout l) {
    switch (l) {
        case Layout::Top: return "top";
        case Layout::Both: return "both";
        default: return "bottom";
    }
}

uint32_t parse_argb(lua_State* L, int idx, uint32_t def) {
    if (lua_isnoneornil(L, idx)) return def;
    return uint32_t(uint64_t(lua_tointeger(L, idx)) & 0xFFFFFFFFu);
}

struct RamPatchSpec {
    uint32_t addr = 0;
    std::vector<uint8_t> bytes;
    bool freeze = false;
    std::string region;
};

struct EventHandler {
    int id = 0;
    std::string event;
    int lua_ref = LUA_NOREF;
};

struct BusHandler {
    std::string topic;
    int lua_ref = LUA_NOREF;
};

struct ConfigSubEntry {
    uint64_t sub_id = 0;
    uint64_t var_handle = 0;
    ConfigChangeCallback cb = nullptr;
    void* user_data = nullptr;
    int lua_ref = LUA_NOREF;
};

struct HookRecord {
    uint64_t rt_handle = 0;
    struct LoadedMod* owner = nullptr;
    PhHookCallback native_cb = nullptr;
    void* user_data = nullptr;
    int lua_ref = LUA_NOREF;
};

struct LoadedMod {
    std::string id;
    std::string name;
    std::string version = "1.0.0";
    std::string author;
    std::string description;
    std::string runtime_spec;
    std::string entry;
    int priority = 0;
    std::vector<std::string> regions;
    std::vector<std::string> dependencies;
    bool enabled_by_default = true;
    bool enabled = true;
    bool compatible = true;
    bool is_archive = false;
    bool has_overlay = false;
    bool has_textures = false;
    ModKind kind = ModKind::AssetOnly;

    fs::path dir;          // extracted bundle or unpacked mod folder
    fs::path data_dir;     // persistent writable dir (<mods_dir>/.data/<id>/)
    fs::path native_lib;   // source DLL/.so inside bundle
    fs::path native_dir;   // staged runtime dir for DLL/.so
    std::string dir_str, data_dir_str, native_dir_str;

    std::vector<RamPatchSpec> ram_patches;
    std::vector<RomPatchSpec> rom_patches;

    // Native state:
    void* dynlib = nullptr;
    const ModDescriptor* desc = nullptr;
    ModResult (*fn_init)(ModError*) = nullptr;
    ModResult (*fn_update)(ModError*) = nullptr;
    ModResult (*fn_input)(PhInputState*, ModError*) = nullptr;
    ModResult (*fn_draw)(const PhDrawContext*, bool*, ModError*) = nullptr;
    ModResult (*fn_post_draw)(const PhDrawContext*, ModError*) = nullptr;
    ModResult (*fn_shutdown)(void) = nullptr;

    // Lua state:
    lua_State* L = nullptr;
    int next_handler_id = 1;
    std::vector<EventHandler> handlers;
    std::vector<BusHandler> bus_handlers;

    // Shared runtime state:
    bool loaded = false;
    bool initialized = false;
    bool faulted = false;
    std::string error;
    json::Value config_json = json::Value::make_object();
    std::vector<ModConfigItem> config_items;
    std::vector<ConfigSubEntry> config_subs;
    std::unordered_map<std::string, int> loaded_images;
    std::vector<int> owned_images;
    std::vector<uint64_t> owned_hooks;
    bool startup_patches_applied = false;
};

constexpr const char* kRegManagerKey = "ph_mod_manager_impl";
constexpr const char* kRegModKey = "ph_mod_instance";

struct ServiceKey {
    std::string id;
    uint16_t major = 0;
    bool operator<(const ServiceKey& o) const {
        if (id != o.id) return id < o.id;
        return major < o.major;
    }
};

struct RegisteredService {
    uint16_t minor = 0;
    const void* ptr = nullptr;
    LoadedMod* provider = nullptr;  // nullptr for built-in host services
    bool deferred = false;
};

struct LifecycleWatch {
    uint64_t id = 0;
    LoadedMod* owner = nullptr;
    ModLifecycleCallback cb = nullptr;
    void* user_data = nullptr;
};

struct SaveObserver {
    uint64_t id = 0;
    LoadedMod* owner = nullptr;
    PhSaveObserverCallback cb = nullptr;
    void* user_data = nullptr;
};

}  // namespace mod_internal
}  // namespace ph

// Global opaque ModContext matching sdk/include/ph_mods/api.h.
struct ModContext {
    ph::ModManager::Impl* impl = nullptr;
    ph::mod_internal::LoadedMod* mod = nullptr;
};

namespace ph {
using namespace mod_internal;

struct ModManager::Impl {
    fs::path mods_dir;
    fs::path save_blob_dir;
    std::string game_code = "AZEE";
    bool dev_mode = false;
    ntr::Runtime* rt = nullptr;
    ModHostCallbacks cb;
    const std::vector<uint8_t>* rom_bytes = nullptr;
    std::vector<NitroFileEntry> nitro_files;
    std::unordered_map<std::string, size_t> nitro_by_path;
    std::unordered_map<std::string, size_t> nitro_by_lower;

    std::vector<std::unique_ptr<LoadedMod>> mods;
    std::vector<std::unique_ptr<ModContext>> mod_ctxs;
    std::vector<LoadedMod*> init_order;  // topological order for update & reverse shutdown
    std::map<std::string, bool> saved_enabled;
    std::map<ServiceKey, RegisteredService> services;

    uint64_t reload_generation = 0;
    uint64_t next_sub_id = 1;
    uint64_t next_watch_id = 1;
    uint64_t next_save_obs_id = 1;
    std::vector<LifecycleWatch> lifecycle_watches;
    std::vector<SaveObserver> save_observers;
    std::unordered_map<uint64_t, std::unique_ptr<HookRecord>> hook_records;

    // Built-in C service tables:
    LogService svc_log{};
    HostService svc_host{};
    ResourceService svc_resource{};
    ConfigService svc_config{};
    MemoryService svc_memory{};
    HookService svc_hook{};
    GfxService svc_gfx{};
    InputService svc_input{};
    DsService svc_ds{};
    OverlayService svc_overlay{};
    SaveService svc_save{};

    // Active frame/draw/input transient state (game thread only).
    GlPresenter* gl_presenter = nullptr;
    uint64_t frame_num = 0;
    float fps = 60.0f;
    double dt = 1.0 / 60.0;
    const ModDrawContext* draw_ctx = nullptr;
    ModInputState cached_input{};
    const ModInputState* input_ctx = nullptr;
    std::vector<ModTouchRect> frame_touch_rects;

    mutable std::mutex snapshot_mutex;
    std::vector<ModTouchRect> published_touch_rects;
    std::vector<ModSummary> published_summaries;

    Impl() { init_builtin_services(); }

    static Impl* get_impl(lua_State* L) {
        lua_getfield(L, LUA_REGISTRYINDEX, kRegManagerKey);
        auto* p = static_cast<Impl*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return p;
    }

    static LoadedMod* get_mod(lua_State* L) {
        lua_getfield(L, LUA_REGISTRYINDEX, kRegModKey);
        auto* p = static_cast<LoadedMod*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return p;
    }

    ModContext* context_for(LoadedMod* m) {
        for (auto& c : mod_ctxs)
            if (c->mod == m) return c.get();
        auto c = std::make_unique<ModContext>();
        c->impl = this;
        c->mod = m;
        ModContext* raw = c.get();
        mod_ctxs.push_back(std::move(c));
        return raw;
    }

    void publish_snapshots() {
        std::vector<ModSummary> sums;
        sums.reserve(mods.size());
        for (const auto& m : mods) {
            ModSummary s;
            s.id = m->id;
            s.name = m->name;
            s.version = m->version;
            s.author = m->author;
            s.description = m->description;
            s.kind = m->kind;
            s.is_archive = m->is_archive;
            s.enabled = m->enabled;
            s.loaded = m->loaded;
            s.compatible = m->compatible;
            s.has_overlay = m->has_overlay;
            s.has_textures = m->has_textures;
            s.error = m->error;
            s.config = m->config_items;
            sums.push_back(std::move(s));
        }
        std::lock_guard<std::mutex> lock(snapshot_mutex);
        published_summaries = std::move(sums);
    }

    void publish_touch_rects() {
        auto sorted = frame_touch_rects;
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const ModTouchRect& a, const ModTouchRect& b) { return a.priority > b.priority; });
        std::lock_guard<std::mutex> lock(snapshot_mutex);
        published_touch_rects = std::move(sorted);
    }

    void load_mods_ini() {
        saved_enabled.clear();
        std::ifstream f(mods_dir / "mods.ini");
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = trim(line.substr(0, eq));
            const std::string v = trim(line.substr(eq + 1));
            if (!k.empty()) saved_enabled[k] = (v == "1" || v == "true" || v == "on");
        }
    }

    void save_mods_ini() const {
        if (mods_dir.empty()) return;
        std::ostringstream ss;
        ss << "# Phantom Hourglass Recomp mod enable states (1 = enabled, 0 = disabled)\n";
        for (const auto& [k, on] : saved_enabled) ss << k << "=" << (on ? 1 : 0) << "\n";
        write_text_file(mods_dir / "mods.ini", ss.str());
    }

    fs::path config_path_for(const LoadedMod& m) const {
        // Persistent data_dir for archives (so updating .phmod never wipes config!), or mod folder for unpacked mods.
        return m.is_archive ? (m.data_dir / "config.json") : (m.dir / "config.json");
    }

    void load_mod_config(LoadedMod& m) {
        m.config_json = json::Value::make_object();
        const std::string text = read_text_file(config_path_for(m));
        if (text.empty()) return;
        json::Value parsed;
        std::string err;
        if (json::parse(text, parsed, err) && parsed.is_object()) m.config_json = std::move(parsed);
    }

    void save_mod_config(const LoadedMod& m) const {
        write_text_file(config_path_for(m), json::dump(m.config_json, true));
    }

    fs::path save_blob_path(const LoadedMod& m, const std::string& key) const {
        const fs::path base = !save_blob_dir.empty() ? save_blob_dir : (m.data_dir / "save_blobs");
        return base / sanitize_filename_token(m.id) / (sanitize_filename_token(key) + ".bin");
    }

    void fire_lifecycle(LoadedMod* subject, ModLifecycleEvent ev) {
        ModContext* subj_ctx = context_for(subject);
        const auto copy = lifecycle_watches;
        for (const LifecycleWatch& w : copy) {
            if (!w.cb || !w.owner || w.owner == subject || !w.owner->loaded || w.owner->faulted) continue;
            w.cb(context_for(w.owner), subj_ctx, subject->id.c_str(), ev, w.user_data);
        }
    }

    void cleanup_mod_resources(LoadedMod& m) {
        // Remove all ARM9 function hooks owned by this mod.
        for (uint64_t h : m.owned_hooks) {
            if (rt) rt->remove_function_hook(h);
            hook_records.erase(h);
        }
        m.owned_hooks.clear();

        // Remove lifecycle watches and save observers owned by this mod.
        lifecycle_watches.erase(
            std::remove_if(lifecycle_watches.begin(), lifecycle_watches.end(),
                           [&](const LifecycleWatch& w) { return w.owner == &m; }),
            lifecycle_watches.end());
        save_observers.erase(
            std::remove_if(save_observers.begin(), save_observers.end(),
                           [&](const SaveObserver& o) { return o.owner == &m; }),
            save_observers.end());

        // Remove services provided by this mod.
        for (auto it = services.begin(); it != services.end();) {
            if (it->second.provider == &m) it = services.erase(it);
            else ++it;
        }

        // Destroy GL images created by this mod.
        if (gl_presenter) {
            for (int img : m.owned_images) gl_presenter->destroy_image(img);
        }
        m.owned_images.clear();
        m.loaded_images.clear();
        m.config_subs.clear();
        m.config_items.clear();
    }

    void fault_mod(LoadedMod& m, const std::string& msg) {
        if (m.faulted) return;
        m.faulted = true;
        m.loaded = false;
        m.error = msg;
        std::fprintf(stderr, "mod [%s] error: %s\n", m.id.c_str(), msg.c_str());
        if (cb.notify) cb.notify("Mod '" + m.name + "' error (disabled)", 4000);
        cleanup_mod_resources(m);
        publish_snapshots();
    }

    bool call_lua(LoadedMod& m, int nargs, int nresults) {
        if (!m.L || m.faulted) return false;
        lua_sethook(m.L, &watchdog_hook, LUA_MASKCOUNT, kLuaMaxInstructions);
        const int rc = lua_pcall(m.L, nargs, nresults, 0);
        lua_sethook(m.L, nullptr, 0, 0);
        if (rc != LUA_OK) {
            const char* err = lua_tostring(m.L, -1);
            const std::string msg = err ? err : "unknown Lua error";
            lua_pop(m.L, 1);
            fault_mod(m, msg);
            return false;
        }
        return true;
    }

    void update_ph_globals(LoadedMod& m) {
        if (!m.L) return;
        lua_getglobal(m.L, "ph");
        if (lua_istable(m.L, -1)) {
            lua_pushinteger(m.L, lua_Integer(frame_num)); lua_setfield(m.L, -2, "frame");
            lua_pushnumber(m.L, double(fps)); lua_setfield(m.L, -2, "fps");
            lua_pushnumber(m.L, dt); lua_setfield(m.L, -2, "dt");
        }
        lua_pop(m.L, 1);
    }

    const NitroFileEntry* find_nitro(const std::string& path) const {
        if (auto it = nitro_by_path.find(path); it != nitro_by_path.end()) return &nitro_files[it->second];
        std::string lo = path;
        for (char& c : lo) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (auto it = nitro_by_lower.find(lo); it != nitro_by_lower.end()) return &nitro_files[it->second];
        return nullptr;
    }

    uint32_t find_mem_pattern(uint32_t start, size_t len, const char* pat_str) const {
        if (!rt || !pat_str) return 0;
        len = std::min<size_t>(len, 4u * 1024u * 1024u);
        std::vector<uint8_t> pat, mask;
        if (!parse_hex_bytes(pat_str, pat, &mask) || pat.size() > len) return 0;
        std::vector<uint8_t> buf(len);
        const size_t got = rt->read_mem(start, buf.data(), len);
        if (got < pat.size()) return 0;
        for (size_t i = 0; i + pat.size() <= got; i++) {
            bool ok = true;
            for (size_t j = 0; j < pat.size(); j++) {
                if (mask[j] && buf[i + j] != pat[j]) { ok = false; break; }
            }
            if (ok) return start + uint32_t(i);
        }
        return 0;
    }

    void notify_config_change(LoadedMod& m, size_t item_idx, const ConfigVarValue& prev, const ConfigVarValue& cur) {
        const uint64_t handle = uint64_t(item_idx + 1);
        const auto subs = m.config_subs;
        for (const ConfigSubEntry& sub : subs) {
            if (sub.var_handle != handle) continue;
            if (sub.cb) {
                sub.cb(context_for(&m), handle, &cur, &prev, sub.user_data);
            } else if (sub.lua_ref != LUA_NOREF && m.L && !m.faulted) {
                lua_rawgeti(m.L, LUA_REGISTRYINDEX, sub.lua_ref);
                auto push_val = [&](const ConfigVarValue& v) {
                    if (v.type == CONFIG_VAR_BOOL) lua_pushboolean(m.L, v.bool_value ? 1 : 0);
                    else if (v.type == CONFIG_VAR_INT) lua_pushinteger(m.L, lua_Integer(v.int_value));
                    else if (v.type == CONFIG_VAR_FLOAT) lua_pushnumber(m.L, v.float_value);
                    else lua_pushstring(m.L, v.string_value ? v.string_value : "");
                };
                push_val(cur);
                push_val(prev);
                call_lua(m, 2, 0);
            }
        }
    }

    static ConfigVarValue item_to_value(const ModConfigItem& it) {
        ConfigVarValue v{};
        switch (it.kind) {
            case ModConfigItem::Kind::Bool: v.type = CONFIG_VAR_BOOL; v.bool_value = it.bool_val; break;
            case ModConfigItem::Kind::Int: v.type = CONFIG_VAR_INT; v.int_value = it.int_val; break;
            case ModConfigItem::Kind::Float: v.type = CONFIG_VAR_FLOAT; v.float_value = it.float_val; break;
            case ModConfigItem::Kind::String:
            case ModConfigItem::Kind::Hotkey:
                v.type = CONFIG_VAR_STRING;
                v.string_value = it.string_val.c_str();
                break;
            case ModConfigItem::Kind::Choice:
                v.type = CONFIG_VAR_CHOICE;
                v.int_value = it.int_val;
                v.string_value = (!it.choices.empty() && size_t(it.int_val) < it.choices.size())
                                     ? it.choices[size_t(it.int_val)].c_str()
                                     : "";
                break;
        }
        return v;
    }

    // ---- ARM9 Function Hook Trampoline (shared by Native C++ and Lua hooks) ----
    static ntr::Runtime::HookAction hook_trampoline(NtrCpu* cpu, uint32_t addr, void* ud) {
        auto* rec = static_cast<HookRecord*>(ud);
        if (!rec || !rec->owner || !rec->owner->loaded || rec->owner->faulted)
            return ntr::Runtime::HookAction::Continue;
        LoadedMod& m = *rec->owner;
        if (rec->native_cb) {
            static_assert(offsetof(NtrCpu, r) == offsetof(PhArm9Cpu, r));
            static_assert(offsetof(NtrCpu, cpsr_ctrl) == offsetof(PhArm9Cpu, cpsr_ctrl));
            ModContext* ctx = nullptr;
            if (m.desc && m.desc->ctx_slot) ctx = *m.desc->ctx_slot;
            const PhHookAction act =
                rec->native_cb(ctx, reinterpret_cast<PhArm9Cpu*>(cpu), addr, rec->user_data);
            return act == PH_HOOK_SKIP_ORIGINAL ? ntr::Runtime::HookAction::SkipOriginal
                                                : ntr::Runtime::HookAction::Continue;
        }
        if (rec->lua_ref != LUA_NOREF && m.L) {
            Impl* s = get_impl(m.L);
            lua_State* L = m.L;
            lua_rawgeti(L, LUA_REGISTRYINDEX, rec->lua_ref);
            // Push CPU register table { r0..r15, addr } and read back mutations after call!
            lua_createtable(L, 0, 17);
            for (int i = 0; i < 16; i++) {
                char rn[4];
                std::snprintf(rn, sizeof rn, "r%d", i);
                lua_pushinteger(L, lua_Integer(cpu->r[i]));
                lua_setfield(L, -2, rn);
            }
            lua_pushinteger(L, lua_Integer(addr));
            lua_setfield(L, -2, "addr");
            lua_pushvalue(L, -1);
            const int tbl_ref = luaL_ref(L, LUA_REGISTRYINDEX);
            ntr::Runtime::HookAction action = ntr::Runtime::HookAction::Continue;
            if (s->call_lua(m, 1, 1)) {
                if (lua_toboolean(L, -1)) action = ntr::Runtime::HookAction::SkipOriginal;
                lua_pop(L, 1);
                lua_rawgeti(L, LUA_REGISTRYINDEX, tbl_ref);
                if (lua_istable(L, -1)) {
                    for (int i = 0; i < 16; i++) {
                        char rn[4];
                        std::snprintf(rn, sizeof rn, "r%d", i);
                        lua_getfield(L, -1, rn);
                        if (lua_isinteger(L, -1)) cpu->r[i] = uint32_t(lua_tointeger(L, -1));
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            }
            luaL_unref(L, LUA_REGISTRYINDEX, tbl_ref);
            return action;
        }
        return ntr::Runtime::HookAction::Continue;
    }

    // =========================================================================================================
    // Built-in C Services Implementation (Dusklight-style versioned C ABI)
    // =========================================================================================================

    static void c_log_write(ModContext* ctx, PhLogLevel lvl, const char* msg) {
        const char* tag = (lvl == PH_LOG_ERROR) ? "ERR" : (lvl == PH_LOG_WARN) ? "WARN"
                          : (lvl == PH_LOG_DEBUG) ? "DBG" : (lvl == PH_LOG_TRACE) ? "TRC" : "INFO";
        const char* id = (ctx && ctx->mod) ? ctx->mod->id.c_str() : "?";
        std::fprintf(stderr, "[mod:%s][%s] %s\n", id, tag, msg ? msg : "");
    }
    static void c_log_trace(ModContext* c, const char* m) { c_log_write(c, PH_LOG_TRACE, m); }
    static void c_log_debug(ModContext* c, const char* m) { c_log_write(c, PH_LOG_DEBUG, m); }
    static void c_log_info(ModContext* c, const char* m) { c_log_write(c, PH_LOG_INFO, m); }
    static void c_log_warn(ModContext* c, const char* m) { c_log_write(c, PH_LOG_WARN, m); }
    static void c_log_error(ModContext* c, const char* m) { c_log_write(c, PH_LOG_ERROR, m); }

    static const char* c_host_version(void) { return kHostVersion; }
    static const char* c_host_game_code(void) { return s_active_impl ? s_active_impl->game_code.c_str() : "AZEE"; }
    static const char* c_host_platform(void) { return platform_name(); }
    static const char* c_host_mod_id(ModContext* c) { return (c && c->mod) ? c->mod->id.c_str() : ""; }
    static const char* c_host_mod_dir(ModContext* c) { return (c && c->mod) ? c->mod->dir_str.c_str() : ""; }
    static const char* c_host_data_dir(ModContext* c) {
        if (!c || !c->mod) return "";
        std::error_code ec;
        fs::create_directories(c->mod->data_dir, ec);
        return c->mod->data_dir_str.c_str();
    }
    static const char* c_host_native_dir(ModContext* c) { return (c && c->mod) ? c->mod->native_dir_str.c_str() : ""; }
    static uint64_t c_host_frame_count(void) { return s_active_impl ? s_active_impl->frame_num : 0; }
    static float c_host_fps(void) { return s_active_impl ? s_active_impl->fps : 60.0f; }
    static double c_host_dt(void) { return s_active_impl ? s_active_impl->dt : (1.0 / 60.0); }
    static void c_host_notify(ModContext* c, const char* msg, uint64_t ms) {
        if (c && c->impl && c->impl->cb.notify && msg) c->impl->cb.notify(msg, ms ? ms : 2500);
    }
    static void c_host_fail(ModContext* c, ModResult, const char* msg) {
        if (c && c->impl && c->mod) c->impl->fault_mod(*c->mod, msg ? msg : "mod reported failure");
    }
    static const void* c_host_get_service(ModContext* c, const char* id, uint16_t maj, uint16_t min_minor) {
        if (!c || !c->impl || !id) return nullptr;
        const auto it = c->impl->services.find({id, maj});
        if (it == c->impl->services.end() || !it->second.ptr || it->second.minor < min_minor) return nullptr;
        return it->second.ptr;
    }
    static ModResult c_host_publish_service(ModContext* c, const char* id, uint16_t maj, uint16_t minor,
                                            const void* svc) {
        if (!c || !c->impl || !c->mod || !id || !svc) return MOD_INVALID_ARGUMENT;
        ServiceKey key{id, maj};
        auto it = c->impl->services.find(key);
        if (it != c->impl->services.end() && it->second.provider != c->mod && it->second.ptr != nullptr)
            return MOD_CONFLICT;
        c->impl->services[key] = {minor, svc, c->mod, false};
        return MOD_OK;
    }
    static ModResult c_host_watch_lifecycle(ModContext* c, ModLifecycleCallback cb, void* ud, uint64_t* out_id) {
        if (!c || !c->impl || !c->mod || !cb) return MOD_INVALID_ARGUMENT;
        const uint64_t id = c->impl->next_watch_id++;
        c->impl->lifecycle_watches.push_back({id, c->mod, cb, ud});
        if (out_id) *out_id = id;
        return MOD_OK;
    }
    static ModResult c_host_unwatch_lifecycle(ModContext* c, uint64_t id) {
        if (!c || !c->impl || !c->mod) return MOD_INVALID_ARGUMENT;
        auto& v = c->impl->lifecycle_watches;
        const auto it = std::find_if(v.begin(), v.end(), [&](const LifecycleWatch& w) {
            return w.id == id && w.owner == c->mod;
        });
        if (it == v.end()) return MOD_UNAVAILABLE;
        v.erase(it);
        return MOD_OK;
    }

    static ModResult c_res_load(ModContext* c, const char* rel, ResourceBuffer* out) {
        if (!c || !c->mod || !rel || !out) return MOD_INVALID_ARGUMENT;
        fs::path p;
        const fs::path res_root = c->mod->dir / "res";
        if (!safe_subpath(res_root, rel, p) || !fs::is_regular_file(p)) return MOD_UNAVAILABLE;
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) return MOD_UNAVAILABLE;
        const auto sz = f.tellg();
        if (sz < 0) return MOD_ERROR;
        f.seekg(0);
        const size_t n = static_cast<size_t>(sz);
        auto* buf = static_cast<uint8_t*>(std::malloc(std::max<size_t>(1, n)));
        if (!buf) return MOD_ERROR;
        if (n > 0) f.read(reinterpret_cast<char*>(buf), sz);
        out->data = buf;
        out->size = n;
        return MOD_OK;
    }
    static void c_res_free(ModContext*, ResourceBuffer* buf) {
        if (!buf) return;
        std::free(buf->data);
        buf->data = nullptr;
        buf->size = 0;
    }
    static bool c_res_exists(ModContext* c, const char* rel) {
        if (!c || !c->mod || !rel) return false;
        fs::path p;
        std::error_code ec;
        return safe_subpath(c->mod->dir / "res", rel, p) && fs::is_regular_file(p, ec);
    }

    static ModResult c_cfg_register(ModContext* c, const ConfigVarDesc* d, ConfigVarHandle* out_var) {
        if (!c || !c->impl || !c->mod || !d || !d->name || !d->name[0]) return MOD_INVALID_ARGUMENT;
        LoadedMod& m = *c->mod;
        const std::string key = d->name;
        const std::string label = (d->label && d->label[0]) ? d->label : key;
        size_t idx = m.config_items.size();
        for (size_t i = 0; i < m.config_items.size(); i++)
            if (m.config_items[i].key == key) { idx = i; break; }
        if (idx == m.config_items.size()) m.config_items.push_back({});
        ModConfigItem& item = m.config_items[idx];
        item.key = key;
        item.label = label;
        switch (d->type) {
            case CONFIG_VAR_BOOL:
                item.kind = ModConfigItem::Kind::Bool;
                item.bool_val = m.config_json.bool_or(key, d->default_bool);
                m.config_json.obj[key] = json::Value::make_bool(item.bool_val);
                break;
            case CONFIG_VAR_INT:
                item.kind = ModConfigItem::Kind::Int;
                item.min_val = d->min_value;
                item.max_val = d->max_value > d->min_value ? d->max_value : double(INT64_MAX);
                item.step = d->step > 0 ? d->step : 1.0;
                item.int_val = m.config_json.int_or(key, d->default_int);
                if (d->max_value > d->min_value)
                    item.int_val = std::clamp<int64_t>(item.int_val, int64_t(d->min_value), int64_t(d->max_value));
                m.config_json.obj[key] = json::Value::make_number(double(item.int_val));
                break;
            case CONFIG_VAR_FLOAT:
                item.kind = ModConfigItem::Kind::Float;
                item.min_val = d->min_value;
                item.max_val = d->max_value;
                item.step = d->step > 0 ? d->step : 0.1;
                item.float_val = m.config_json.number_or(key, d->default_float);
                if (d->max_value > d->min_value)
                    item.float_val = std::clamp(item.float_val, d->min_value, d->max_value);
                m.config_json.obj[key] = json::Value::make_number(item.float_val);
                break;
            case CONFIG_VAR_STRING:
                item.kind = ModConfigItem::Kind::String;
                item.string_val = m.config_json.string_or(key, d->default_string ? d->default_string : "");
                m.config_json.obj[key] = json::Value::make_string(item.string_val);
                break;
            case CONFIG_VAR_CHOICE:
                item.kind = ModConfigItem::Kind::Choice;
                item.choices.clear();
                for (size_t i = 0; i < d->choice_count && d->choices; i++)
                    item.choices.push_back(d->choices[i] ? d->choices[i] : "");
                if (item.choices.empty()) item.choices.push_back("Default");
                item.int_val = std::clamp<int64_t>(d->default_int, 0, int64_t(item.choices.size() - 1));
                if (const json::Value* ex = m.config_json.get(key)) {
                    if (ex->is_string()) {
                        for (size_t i = 0; i < item.choices.size(); i++)
                            if (item.choices[i] == ex->str) item.int_val = int64_t(i);
                    } else if (ex->is_number()) {
                        item.int_val = std::clamp<int64_t>(int64_t(ex->number), 0, int64_t(item.choices.size() - 1));
                    }
                }
                m.config_json.obj[key] = json::Value::make_string(item.choices[size_t(item.int_val)]);
                break;
        }
        if (out_var) *out_var = uint64_t(idx + 1);
        c->impl->publish_snapshots();
        return MOD_OK;
    }
    static ModConfigItem* cfg_item(ModContext* c, ConfigVarHandle h) {
        if (!c || !c->mod || h == 0 || size_t(h) > c->mod->config_items.size()) return nullptr;
        return &c->mod->config_items[size_t(h - 1)];
    }
    static ModResult c_cfg_get_bool(ModContext* c, ConfigVarHandle h, bool* out) {
        auto* it = cfg_item(c, h);
        if (!it || !out || it->kind != ModConfigItem::Kind::Bool) return MOD_INVALID_ARGUMENT;
        *out = it->bool_val;
        return MOD_OK;
    }
    static ModResult c_cfg_set_bool(ModContext* c, ConfigVarHandle h, bool v) {
        auto* it = cfg_item(c, h);
        if (!it || it->kind != ModConfigItem::Kind::Bool) return MOD_INVALID_ARGUMENT;
        if (it->bool_val == v) return MOD_OK;
        const auto prev = item_to_value(*it);
        it->bool_val = v;
        c->mod->config_json.obj[it->key] = json::Value::make_bool(v);
        c->impl->notify_config_change(*c->mod, size_t(h - 1), prev, item_to_value(*it));
        c->impl->publish_snapshots();
        return MOD_OK;
    }
    static ModResult c_cfg_get_int(ModContext* c, ConfigVarHandle h, int64_t* out) {
        auto* it = cfg_item(c, h);
        if (!it || !out || (it->kind != ModConfigItem::Kind::Int && it->kind != ModConfigItem::Kind::Choice))
            return MOD_INVALID_ARGUMENT;
        *out = it->int_val;
        return MOD_OK;
    }
    static ModResult c_cfg_set_int(ModContext* c, ConfigVarHandle h, int64_t v) {
        auto* it = cfg_item(c, h);
        if (!it || (it->kind != ModConfigItem::Kind::Int && it->kind != ModConfigItem::Kind::Choice))
            return MOD_INVALID_ARGUMENT;
        if (it->int_val == v) return MOD_OK;
        const auto prev = item_to_value(*it);
        it->int_val = v;
        if (it->kind == ModConfigItem::Kind::Choice && !it->choices.empty()) {
            it->int_val = std::clamp<int64_t>(v, 0, int64_t(it->choices.size() - 1));
            c->mod->config_json.obj[it->key] = json::Value::make_string(it->choices[size_t(it->int_val)]);
        } else {
            c->mod->config_json.obj[it->key] = json::Value::make_number(double(v));
        }
        c->impl->notify_config_change(*c->mod, size_t(h - 1), prev, item_to_value(*it));
        c->impl->publish_snapshots();
        return MOD_OK;
    }
    static ModResult c_cfg_get_float(ModContext* c, ConfigVarHandle h, double* out) {
        auto* it = cfg_item(c, h);
        if (!it || !out || it->kind != ModConfigItem::Kind::Float) return MOD_INVALID_ARGUMENT;
        *out = it->float_val;
        return MOD_OK;
    }
    static ModResult c_cfg_set_float(ModContext* c, ConfigVarHandle h, double v) {
        auto* it = cfg_item(c, h);
        if (!it || it->kind != ModConfigItem::Kind::Float) return MOD_INVALID_ARGUMENT;
        if (it->float_val == v) return MOD_OK;
        const auto prev = item_to_value(*it);
        it->float_val = v;
        c->mod->config_json.obj[it->key] = json::Value::make_number(v);
        c->impl->notify_config_change(*c->mod, size_t(h - 1), prev, item_to_value(*it));
        c->impl->publish_snapshots();
        return MOD_OK;
    }
    static ModResult c_cfg_get_string(ModContext* c, ConfigVarHandle h, char* buf, size_t* io_sz) {
        auto* it = cfg_item(c, h);
        if (!it || !io_sz || it->kind != ModConfigItem::Kind::String) return MOD_INVALID_ARGUMENT;
        const size_t need = it->string_val.size() + 1;
        if (!buf || *io_sz < need) { *io_sz = need; return buf ? MOD_BUFFER_TOO_SMALL : MOD_OK; }
        std::memcpy(buf, it->string_val.c_str(), need);
        *io_sz = need;
        return MOD_OK;
    }
    static ModResult c_cfg_set_string(ModContext* c, ConfigVarHandle h, const char* v) {
        auto* it = cfg_item(c, h);
        if (!it || !v || it->kind != ModConfigItem::Kind::String) return MOD_INVALID_ARGUMENT;
        if (it->string_val == v) return MOD_OK;
        const std::string prev_str = it->string_val;
        ConfigVarValue prev{CONFIG_VAR_STRING, false, 0, 0.0, prev_str.c_str()};
        it->string_val = v;
        c->mod->config_json.obj[it->key] = json::Value::make_string(it->string_val);
        c->impl->notify_config_change(*c->mod, size_t(h - 1), prev, item_to_value(*it));
        c->impl->publish_snapshots();
        return MOD_OK;
    }
    static ModResult c_cfg_subscribe(ModContext* c, ConfigVarHandle h, ConfigChangeCallback cb, void* ud,
                                     ConfigSubHandle* out) {
        if (!c || !c->impl || !c->mod || !cb || !cfg_item(c, h)) return MOD_INVALID_ARGUMENT;
        const uint64_t sid = c->impl->next_sub_id++;
        c->mod->config_subs.push_back({sid, h, cb, ud, LUA_NOREF});
        if (out) *out = sid;
        return MOD_OK;
    }
    static ModResult c_cfg_unsubscribe(ModContext* c, ConfigSubHandle sid) {
        if (!c || !c->mod) return MOD_INVALID_ARGUMENT;
        auto& v = c->mod->config_subs;
        const auto it = std::find_if(v.begin(), v.end(), [&](const ConfigSubEntry& e) { return e.sub_id == sid; });
        if (it == v.end()) return MOD_UNAVAILABLE;
        v.erase(it);
        return MOD_OK;
    }
    static ModResult c_cfg_save(ModContext* c) {
        if (!c || !c->impl || !c->mod) return MOD_INVALID_ARGUMENT;
        c->impl->save_mod_config(*c->mod);
        return MOD_OK;
    }

    static uint8_t c_mem_peek8(ModContext* c, uint32_t a) { return (c && c->impl && c->impl->rt) ? c->impl->rt->peek8(a) : 0; }
    static uint16_t c_mem_peek16(ModContext* c, uint32_t a) { return (c && c->impl && c->impl->rt) ? c->impl->rt->peek16(a) : 0; }
    static uint32_t c_mem_peek32(ModContext* c, uint32_t a) { return (c && c->impl && c->impl->rt) ? c->impl->rt->peek32(a) : 0; }
    static bool c_mem_poke8(ModContext* c, uint32_t a, uint8_t v) { return c && c->impl && c->impl->rt && c->impl->rt->poke8(a, v); }
    static bool c_mem_poke16(ModContext* c, uint32_t a, uint16_t v) { return c && c->impl && c->impl->rt && c->impl->rt->poke16(a, v); }
    static bool c_mem_poke32(ModContext* c, uint32_t a, uint32_t v) { return c && c->impl && c->impl->rt && c->impl->rt->poke32(a, v); }
    static size_t c_mem_read(ModContext* c, uint32_t a, uint8_t* out, size_t n) {
        return (c && c->impl && c->impl->rt) ? c->impl->rt->read_mem(a, out, n) : 0;
    }
    static size_t c_mem_write(ModContext* c, uint32_t a, const uint8_t* data, size_t n) {
        return (c && c->impl && c->impl->rt) ? c->impl->rt->write_mem(a, data, n) : 0;
    }
    static uint32_t c_mem_find(ModContext* c, uint32_t start, size_t len, const char* pat) {
        return (c && c->impl) ? c->impl->find_mem_pattern(start, len, pat) : 0;
    }

    static uint32_t c_hook_resolve(ModContext* c, const char* name) {
        return (c && c->impl && c->impl->rt) ? c->impl->rt->resolve_symbol(name) : 0;
    }
    static const char* c_hook_sym_name(ModContext* c, uint32_t addr) {
        return (c && c->impl && c->impl->rt) ? c->impl->rt->symbol_name(addr) : nullptr;
    }
    static ModResult c_hook_install(ModContext* c, uint32_t addr, ntr::Runtime::HookKind kind, PhHookCallback cb,
                                    void* ud, int prio, PhHookHandle* out) {
        if (!c || !c->impl || !c->mod || !c->impl->rt || !addr || !cb) return MOD_INVALID_ARGUMENT;
        auto rec = std::make_unique<HookRecord>();
        rec->owner = c->mod;
        rec->native_cb = cb;
        rec->user_data = ud;
        HookRecord* raw = rec.get();
        const uint64_t h = c->impl->rt->add_function_hook(addr, kind, &hook_trampoline, raw, prio);
        if (!h) return MOD_UNAVAILABLE;
        raw->rt_handle = h;
        c->impl->hook_records[h] = std::move(rec);
        c->mod->owned_hooks.push_back(h);
        if (out) *out = h;
        return MOD_OK;
    }
    static ModResult c_hook_pre(ModContext* c, uint32_t a, PhHookCallback cb, void* ud, int p, PhHookHandle* out) {
        return c_hook_install(c, a, ntr::Runtime::HookKind::Pre, cb, ud, p, out);
    }
    static ModResult c_hook_post(ModContext* c, uint32_t a, PhHookCallback cb, void* ud, int p, PhHookHandle* out) {
        return c_hook_install(c, a, ntr::Runtime::HookKind::Post, cb, ud, p, out);
    }
    static ModResult c_hook_replace(ModContext* c, uint32_t a, PhHookCallback cb, void* ud, int p, PhHookHandle* out) {
        return c_hook_install(c, a, ntr::Runtime::HookKind::Replace, cb, ud, p, out);
    }
    static ModResult c_hook_remove(ModContext* c, PhHookHandle h) {
        if (!c || !c->impl || !c->mod || !c->impl->rt) return MOD_INVALID_ARGUMENT;
        auto it = c->impl->hook_records.find(h);
        if (it == c->impl->hook_records.end() || it->second->owner != c->mod) return MOD_UNAVAILABLE;
        c->impl->rt->remove_function_hook(h);
        c->impl->hook_records.erase(it);
        auto& v = c->mod->owned_hooks;
        v.erase(std::remove(v.begin(), v.end(), h), v.end());
        return MOD_OK;
    }
    static ModResult c_hook_call_orig(ModContext* c, PhHookHandle h, PhArm9Cpu* cpu) {
        if (!c || !c->impl || !c->impl->rt) return MOD_INVALID_ARGUMENT;
        return c->impl->rt->call_hook_original(h, reinterpret_cast<NtrCpu*>(cpu)) ? MOD_OK : MOD_UNAVAILABLE;
    }
    static ModResult c_hook_call_guest(ModContext* c, uint32_t addr, uint32_t r0, uint32_t r1, uint32_t r2,
                                       uint32_t r3, uint32_t* out_r0) {
        if (!c || !c->impl || !c->impl->rt) return MOD_INVALID_ARGUMENT;
        return c->impl->rt->call_guest_function(addr, r0, r1, r2, r3, out_r0) ? MOD_OK : MOD_UNAVAILABLE;
    }

    static void c_gfx_win_size(ModContext* c, int* w, int* h, float* ui) {
        Impl* s = c ? c->impl : nullptr;
        if (s && s->draw_ctx) {
            if (w) *w = s->draw_ctx->win_w;
            if (h) *h = s->draw_ctx->win_h;
            if (ui) *ui = s->draw_ctx->ui_scale;
        } else {
            if (w) *w = (s && s->input_ctx) ? s->input_ctx->win_w : 256;
            if (h) *h = (s && s->input_ctx) ? s->input_ctx->win_h : 192;
            if (ui) *ui = 1.0f;
        }
    }
    static PhViews c_gfx_default_views(ModContext* c, int layout) {
        PhViews out{};
        if (!c || !c->impl || !c->impl->draw_ctx) return out;
        const auto& dc = *c->impl->draw_ctx;
        const Layout l = layout == 1 ? Layout::Top : layout == 2 ? Layout::Both : Layout::Bottom;
        const Views v = compute_views(float(dc.win_w), float(dc.win_h), l, dc.stretch, dc.wide_extra, dc.wide_layer);
        out.top_shown = v.top_shown;
        out.bottom_shown = v.bottom_shown;
        out.top = {v.top.x, v.top.y, v.top.w, v.top.h};
        out.bottom = {v.bottom.x, v.bottom.y, v.bottom.w, v.bottom.h};
        return out;
    }
    static void c_gfx_draw_screen(ModContext* c, int layer, PhRect dst, const PhScreenDrawOpts* opts) {
        if (!c || !c->impl || !c->impl->draw_ctx || !c->impl->draw_ctx->gl || !c->impl->draw_ctx->frame) return;
        const auto& dc = *c->impl->draw_ctx;
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1, alpha = 1.0f;
        bool smooth = dc.smooth, wide = dc.wide && dc.wide_extra > 0;
        uint32_t tint = 0xFFFFFFFFu;
        if (opts) {
            smooth = opts->smooth;
            wide = opts->wide;
            alpha = opts->alpha;
            tint = opts->tint_argb;
            if (opts->use_src_ds) {
                u0 = std::clamp(opts->src_ds.x / 256.0f, 0.0f, 1.0f);
                v0 = std::clamp(opts->src_ds.y / 192.0f, 0.0f, 1.0f);
                u1 = std::clamp((opts->src_ds.x + opts->src_ds.w) / 256.0f, 0.0f, 1.0f);
                v1 = std::clamp((opts->src_ds.y + opts->src_ds.h) / 192.0f, 0.0f, 1.0f);
                wide = false;
            } else {
                u0 = opts->u0; v0 = opts->v0; u1 = opts->u1; v1 = opts->v1;
            }
        }
        dc.gl->screen_ex(*dc.frame, layer, {dst.x, dst.y, dst.w, dst.h}, u0, v0, u1, v1, smooth, wide, alpha, tint);
        if (u0 == 0.0f && v0 == 0.0f && u1 == 1.0f && v1 == 1.0f && alpha >= 0.5f) {
            const bool w_on = wide && dc.wide_extra > 0 && dc.wide_layer == layer;
            std::array<HudMove, 32> hm{};
            int hm_cnt = 0;
            if (w_on) {
                hm_cnt = std::min(dc.frame->hud_moves, 32);
                for (int i = 0; i < hm_cnt; i++) {
                    const auto& m = dc.frame->hud_move[i];
                    hm[size_t(i)] = {m.x0, m.y0, m.x1, m.y1, m.dx};
                }
            }
            dc.gl->draw_hd_text(layer, {dst.x, dst.y, dst.w, dst.h}, w_on ? dc.wide_extra : 0, w_on ? hm.data() : nullptr,
                                hm_cnt);
        }
    }
    static void c_gfx_fill(ModContext* c, PhRect r, uint32_t argb) {
        if (c && c->impl && c->impl->draw_ctx && c->impl->draw_ctx->gl)
            c->impl->draw_ctx->gl->fill({r.x, r.y, r.w, r.h}, argb);
    }
    static void c_gfx_outline(ModContext* c, PhRect r, float thick, uint32_t argb) {
        if (c && c->impl && c->impl->draw_ctx && c->impl->draw_ctx->gl)
            c->impl->draw_ctx->gl->outline({r.x, r.y, r.w, r.h}, thick, argb);
    }
    static float c_gfx_text(ModContext* c, float x, float y, float sz, const char* s, uint32_t argb) {
        if (c && c->impl && c->impl->draw_ctx && c->impl->draw_ctx->gl && s)
            return c->impl->draw_ctx->gl->text(x, y, sz, s, argb);
        return 0.0f;
    }
    static void c_gfx_set_clip(ModContext* c, PhRect r) {
        if (c && c->impl && c->impl->draw_ctx && c->impl->draw_ctx->gl)
            c->impl->draw_ctx->gl->set_clip({r.x, r.y, r.w, r.h});
    }
    static void c_gfx_clear_clip(ModContext* c) {
        if (c && c->impl && c->impl->draw_ctx && c->impl->draw_ctx->gl) c->impl->draw_ctx->gl->clear_clip();
    }
    static int c_gfx_load_image(ModContext* c, const char* rel, bool smooth, int* ow, int* oh) {
        if (!c || !c->impl || !c->mod || !rel || !c->impl->cb.decode_image) return 0;
        GlPresenter* gl = (c->impl->draw_ctx && c->impl->draw_ctx->gl) ? c->impl->draw_ctx->gl : c->impl->gl_presenter;
        if (!gl) return 0;
        fs::path resolved;
        if (!safe_subpath(c->mod->dir / "res", rel, resolved) || !fs::is_regular_file(resolved)) {
            if (!safe_subpath(c->mod->dir, rel, resolved) || !fs::is_regular_file(resolved)) return 0;
        }
        std::vector<uint32_t> rgba;
        int w = 0, h = 0;
        if (!c->impl->cb.decode_image(resolved, rgba, w, h)) return 0;
        const int id = gl->create_image(rgba.data(), w, h, smooth);
        if (id > 0) {
            c->mod->owned_images.push_back(id);
            if (ow) *ow = w;
            if (oh) *oh = h;
        }
        return id;
    }
    static int c_gfx_create_image(ModContext* c, const uint32_t* px, int w, int h, bool smooth) {
        if (!c || !c->impl || !c->mod) return 0;
        GlPresenter* gl = (c->impl->draw_ctx && c->impl->draw_ctx->gl) ? c->impl->draw_ctx->gl : c->impl->gl_presenter;
        if (!gl) return 0;
        const int id = gl->create_image(px, w, h, smooth);
        if (id > 0) c->mod->owned_images.push_back(id);
        return id;
    }
    static void c_gfx_destroy_image(ModContext* c, int id) {
        if (!c || !c->impl || !c->mod) return;
        GlPresenter* gl = (c->impl->draw_ctx && c->impl->draw_ctx->gl) ? c->impl->draw_ctx->gl : c->impl->gl_presenter;
        if (gl) gl->destroy_image(id);
        auto& v = c->mod->owned_images;
        v.erase(std::remove(v.begin(), v.end(), id), v.end());
    }
    static void c_gfx_draw_image(ModContext* c, int id, PhRect dst, float u0, float v0, float u1, float v1,
                                 float alpha, uint32_t tint) {
        if (c && c->impl && c->impl->draw_ctx && c->impl->draw_ctx->gl)
            c->impl->draw_ctx->gl->draw_image(id, {dst.x, dst.y, dst.w, dst.h}, u0, v0, u1, v1, alpha, tint);
    }
    static void c_gfx_set_touch_rect(ModContext* c, PhRect r, const PhTouchRectOpts* opts) {
        if (!c || !c->impl) return;
        ModTouchRect tr;
        tr.rect = {r.x, r.y, r.w, r.h};
        tr.src_ds = {0, 0, 256, 192};
        if (opts) {
            tr.has_src = opts->use_src_ds;
            if (opts->use_src_ds) tr.src_ds = {opts->src_ds.x, opts->src_ds.y, opts->src_ds.w, opts->src_ds.h};
            tr.wide = opts->wide;
            tr.block = opts->block;
            tr.priority = opts->priority;
        }
        c->impl->frame_touch_rects.push_back(tr);
    }
    static void c_gfx_clear_touch_rects(ModContext* c) {
        if (c && c->impl) c->impl->frame_touch_rects.clear();
    }
    static void c_gfx_to_ds(ModContext* c, PhRect r, float px, float py, bool use_wide, int* ox, int* oy) {
        int tx = 0, ty = 0;
        Impl* s = c ? c->impl : nullptr;
        const int extra = (use_wide && s && s->draw_ctx && s->draw_ctx->wide_layer == 1) ? s->draw_ctx->wide_extra : 0;
        if (r.w > 0 && r.h > 0) {
            std::array<HudMove, 32> moves{};
            int cnt = 0;
            if (extra > 0 && s && s->draw_ctx && s->draw_ctx->frame) {
                cnt = s->draw_ctx->frame->hud_moves;
                for (int i = 0; i < cnt; i++) {
                    const auto& hm = s->draw_ctx->frame->hud_move[i];
                    moves[size_t(i)] = {hm.x0, hm.y0, hm.x1, hm.y1, hm.dx};
                }
            }
            to_ds({r.x, r.y, r.w, r.h}, extra, px, py, tx, ty, moves.data(), cnt);
        }
        if (ox) *ox = tx;
        if (oy) *oy = ty;
    }

    static void c_input_get_state(ModContext* c, PhInputState* out) {
        if (!out) return;
        *out = {};
        out->struct_size = sizeof(PhInputState);
        if (!c || !c->impl || !c->impl->input_ctx) return;
        const ModInputState& i = *c->impl->input_ctx;
        out->held = i.held;
        out->pressed = i.pressed;
        out->touching = i.touching;
        out->tx = i.tx;
        out->ty = i.ty;
        out->blow = i.blow;
        out->blow_sample = i.blow_sample;
        out->lid = i.lid;
        out->stick_on = i.stick_on;
        out->stick_x = i.stick_x;
        out->stick_y = i.stick_y;
        out->pointer_x = i.pointer_x;
        out->pointer_y = i.pointer_y;
        out->pointer_down = i.pointer_down;
        out->pointer_just_pressed = i.pointer_just_pressed;
        out->pointer_just_released = i.pointer_just_released;
        out->right_down = i.right_down;
        out->right_just_pressed = i.right_just_pressed;
        out->win_w = i.win_w;
        out->win_h = i.win_h;
    }
    static bool c_input_key_down(ModContext* c, const char* k) {
        return c && c->impl && c->impl->cb.is_key_down && k && c->impl->cb.is_key_down(k);
    }
    static bool c_input_key_pressed(ModContext* c, const char* k) {
        return c && c->impl && c->impl->cb.was_key_pressed && k && c->impl->cb.was_key_pressed(k);
    }

    static void c_ds_display_info(ModContext* c, PhDisplayInfo* out) {
        if (!out) return;
        *out = {};
        out->struct_size = sizeof(PhDisplayInfo);
        if (!c || !c->impl || !c->impl->rt) return;
        const auto d = c->impl->rt->display_info();
        out->powcnt1 = d.powcnt1;
        out->dispcnt_a = d.dispcnt_a;
        out->dispcnt_b = d.dispcnt_b;
        out->master_bright_a = d.master_bright_a;
        out->master_bright_b = d.master_bright_b;
        out->disp3dcnt = d.disp3dcnt;
        out->screen_swap = d.screen_swap;
        out->screens_enabled = d.screens_enabled;
        out->lid_closed = d.lid_closed;
        out->new_3d_frame = d.new_3d_frame;
    }
    static int c_ds_wide_extra(ModContext* c) {
        return (c && c->impl && c->impl->draw_ctx) ? c->impl->draw_ctx->wide_extra
                                                   : ((c && c->impl && c->impl->rt) ? c->impl->rt->video().wide_extra : 0);
    }
    static int c_ds_wide_layer(ModContext* c) {
        return (c && c->impl && c->impl->draw_ctx) ? c->impl->draw_ctx->wide_layer : 1;
    }
    static void c_ds_set_layout(ModContext* c, int l) {
        if (c && c->impl && c->impl->cb.set_layout)
            c->impl->cb.set_layout(l == 1 ? Layout::Top : l == 2 ? Layout::Both : Layout::Bottom);
    }
    static int c_ds_get_layout(ModContext* c) {
        return (c && c->impl && c->impl->cb.get_layout) ? int(c->impl->cb.get_layout()) : 0;
    }
    static void c_ds_save_state(ModContext* c, int s) {
        if (c && c->impl && c->impl->cb.save_state && s >= 1 && s <= 12) c->impl->cb.save_state(s);
    }
    static void c_ds_load_state(ModContext* c, int s) {
        if (c && c->impl && c->impl->cb.load_state && s >= 1 && s <= 12) c->impl->cb.load_state(s);
    }
    static void c_ds_screenshot(ModContext* c, const char* name) {
        if (c && c->impl && c->impl->cb.screenshot) c->impl->cb.screenshot(name ? name : "");
    }
    static void c_ds_mic_blow(ModContext* c, bool on) {
        if (c && c->impl && c->impl->cb.set_mic_blow) c->impl->cb.set_mic_blow(on);
    }
    static void c_ds_mic_sample(ModContext* c, bool on) {
        if (c && c->impl && c->impl->cb.set_mic_sample) c->impl->cb.set_mic_sample(on);
    }
    static void c_ds_lid_closed(ModContext* c, bool on) {
        if (c && c->impl && c->impl->cb.set_lid_closed) c->impl->cb.set_lid_closed(on);
    }
    static void c_ds_cpu_speed(ModContext* c, int m) {
        if (c && c->impl && c->impl->rt) c->impl->rt->set_cpu_speed(std::clamp(m, 1, 4));
    }
    static void c_ds_fast_cart(ModContext* c, bool on) {
        if (c && c->impl && c->impl->rt) c->impl->rt->set_fast_cart(on);
    }

    static size_t c_ovl_count(ModContext* c) { return (c && c->impl) ? c->impl->nitro_files.size() : 0; }
    static const char* c_ovl_path(ModContext* c, size_t idx) {
        if (!c || !c->impl || idx >= c->impl->nitro_files.size()) return nullptr;
        return c->impl->nitro_files[idx].path.c_str();
    }
    static bool c_ovl_size(ModContext* c, const char* path, uint32_t* out) {
        if (!c || !c->impl || !path) return false;
        const auto* e = c->impl->find_nitro(path);
        if (!e) return false;
        if (out) *out = e->size;
        return true;
    }
    static size_t c_ovl_read(ModContext* c, const char* path, uint32_t off, uint8_t* out, size_t max_len) {
        if (!c || !c->impl || !path || !out || max_len == 0) return 0;
        const auto* e = c->impl->find_nitro(path);
        if (!e || off >= e->size) return 0;
        const size_t n = std::min<size_t>(max_len, size_t(e->size - off));
        const uint32_t rom_off = e->offset + off;
        if (c->impl->rom_bytes) {
            if (size_t(rom_off) + n > c->impl->rom_bytes->size()) return 0;
            std::memcpy(out, c->impl->rom_bytes->data() + rom_off, n);
            return n;
        }
        return c->impl->rt ? c->impl->rt->read_rom(rom_off, out, n) : 0;
    }

    static ModResult c_save_set_blob(ModContext* c, const char* key, const void* data, size_t size) {
        if (!c || !c->impl || !c->mod || !key || !key[0] || size > PH_SAVE_BLOB_MAX_BYTES) return MOD_INVALID_ARGUMENT;
        if (size > 0 && !data) return MOD_INVALID_ARGUMENT;
        const fs::path p = c->impl->save_blob_path(*c->mod, key);
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f) return MOD_ERROR;
        if (size > 0) f.write(static_cast<const char*>(data), std::streamsize(size));
        return f ? MOD_OK : MOD_ERROR;
    }
    static ModResult c_save_get_blob(ModContext* c, const char* key, void* out, size_t* io_sz) {
        if (!c || !c->impl || !c->mod || !key || !key[0] || !io_sz) return MOD_INVALID_ARGUMENT;
        const fs::path p = c->impl->save_blob_path(*c->mod, key);
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) return MOD_UNAVAILABLE;
        const auto sz = f.tellg();
        if (sz < 0) return MOD_ERROR;
        const size_t n = static_cast<size_t>(sz);
        if (!out || *io_sz < n) { *io_sz = n; return out ? MOD_BUFFER_TOO_SMALL : MOD_OK; }
        f.seekg(0);
        if (n > 0) f.read(static_cast<char*>(out), sz);
        *io_sz = n;
        return f ? MOD_OK : MOD_ERROR;
    }
    static ModResult c_save_delete_blob(ModContext* c, const char* key) {
        if (!c || !c->impl || !c->mod || !key || !key[0]) return MOD_INVALID_ARGUMENT;
        std::error_code ec;
        return fs::remove(c->impl->save_blob_path(*c->mod, key), ec) ? MOD_OK : MOD_UNAVAILABLE;
    }
    static ModResult c_save_observe(ModContext* c, PhSaveObserverCallback cb, void* ud, SaveObserverHandle* out) {
        if (!c || !c->impl || !c->mod || !cb) return MOD_INVALID_ARGUMENT;
        const uint64_t id = c->impl->next_save_obs_id++;
        c->impl->save_observers.push_back({id, c->mod, cb, ud});
        if (out) *out = id;
        return MOD_OK;
    }
    static ModResult c_save_unobserve(ModContext* c, SaveObserverHandle id) {
        if (!c || !c->impl || !c->mod) return MOD_INVALID_ARGUMENT;
        auto& v = c->impl->save_observers;
        const auto it = std::find_if(v.begin(), v.end(), [&](const SaveObserver& o) {
            return o.id == id && o.owner == c->mod;
        });
        if (it == v.end()) return MOD_UNAVAILABLE;
        v.erase(it);
        return MOD_OK;
    }

    static inline Impl* s_active_impl = nullptr;

    void init_builtin_services() {
        svc_log = {SERVICE_HEADER(LogService, PH_LOG_SERVICE_MAJOR, PH_LOG_SERVICE_MINOR),
                   c_log_write, c_log_trace, c_log_debug, c_log_info, c_log_warn, c_log_error};
        svc_host = {SERVICE_HEADER(HostService, PH_HOST_SERVICE_MAJOR, PH_HOST_SERVICE_MINOR),
                    c_host_version, c_host_game_code, c_host_platform, c_host_mod_id, c_host_mod_dir,
                    c_host_data_dir, c_host_native_dir, c_host_frame_count, c_host_fps, c_host_dt,
                    c_host_notify, c_host_fail, c_host_get_service, c_host_publish_service,
                    c_host_watch_lifecycle, c_host_unwatch_lifecycle};
        svc_resource = {SERVICE_HEADER(ResourceService, PH_RESOURCE_SERVICE_MAJOR, PH_RESOURCE_SERVICE_MINOR),
                        c_res_load, c_res_free, c_res_exists};
        svc_config = {SERVICE_HEADER(ConfigService, PH_CONFIG_SERVICE_MAJOR, PH_CONFIG_SERVICE_MINOR),
                      c_cfg_register, c_cfg_get_bool, c_cfg_set_bool, c_cfg_get_int, c_cfg_set_int,
                      c_cfg_get_float, c_cfg_set_float, c_cfg_get_string, c_cfg_set_string,
                      c_cfg_subscribe, c_cfg_unsubscribe, c_cfg_save};
        svc_memory = {SERVICE_HEADER(MemoryService, PH_MEMORY_SERVICE_MAJOR, PH_MEMORY_SERVICE_MINOR),
                      c_mem_peek8, c_mem_peek16, c_mem_peek32, c_mem_poke8, c_mem_poke16, c_mem_poke32,
                      c_mem_read, c_mem_write, c_mem_find};
        svc_hook = {SERVICE_HEADER(HookService, PH_HOOK_SERVICE_MAJOR, PH_HOOK_SERVICE_MINOR),
                    c_hook_resolve, c_hook_sym_name, c_hook_pre, c_hook_post, c_hook_replace,
                    c_hook_remove, c_hook_call_orig, c_hook_call_guest};
        svc_gfx = {SERVICE_HEADER(GfxService, PH_GFX_SERVICE_MAJOR, PH_GFX_SERVICE_MINOR),
                   c_gfx_win_size, c_gfx_default_views, c_gfx_draw_screen, c_gfx_fill, c_gfx_outline,
                   c_gfx_text, c_gfx_set_clip, c_gfx_clear_clip, c_gfx_load_image, c_gfx_create_image,
                   c_gfx_destroy_image, c_gfx_draw_image, c_gfx_set_touch_rect, c_gfx_clear_touch_rects, c_gfx_to_ds};
        svc_input = {SERVICE_HEADER(InputService, PH_INPUT_SERVICE_MAJOR, PH_INPUT_SERVICE_MINOR),
                     c_input_get_state, c_input_key_down, c_input_key_pressed};
        svc_ds = {SERVICE_HEADER(DsService, PH_DS_SERVICE_MAJOR, PH_DS_SERVICE_MINOR),
                  c_ds_display_info, c_ds_wide_extra, c_ds_wide_layer, c_ds_set_layout, c_ds_get_layout,
                  c_ds_save_state, c_ds_load_state, c_ds_screenshot, c_ds_mic_blow, c_ds_mic_sample,
                  c_ds_lid_closed, c_ds_cpu_speed, c_ds_fast_cart};
        svc_overlay = {SERVICE_HEADER(OverlayService, PH_OVERLAY_SERVICE_MAJOR, PH_OVERLAY_SERVICE_MINOR),
                       c_ovl_count, c_ovl_path, c_ovl_size, c_ovl_read};
        svc_save = {SERVICE_HEADER(SaveService, PH_SAVE_SERVICE_MAJOR, PH_SAVE_SERVICE_MINOR),
                    c_save_set_blob, c_save_get_blob, c_save_delete_blob, c_save_observe, c_save_unobserve};
    }

    void register_builtin_services() {
        services.clear();
        auto reg = [&](const char* id, uint16_t maj, uint16_t min_v, const void* ptr) {
            services[{id, maj}] = {min_v, ptr, nullptr, false};
        };
        reg(PH_LOG_SERVICE_ID, PH_LOG_SERVICE_MAJOR, PH_LOG_SERVICE_MINOR, &svc_log);
        reg(PH_HOST_SERVICE_ID, PH_HOST_SERVICE_MAJOR, PH_HOST_SERVICE_MINOR, &svc_host);
        reg(PH_RESOURCE_SERVICE_ID, PH_RESOURCE_SERVICE_MAJOR, PH_RESOURCE_SERVICE_MINOR, &svc_resource);
        reg(PH_CONFIG_SERVICE_ID, PH_CONFIG_SERVICE_MAJOR, PH_CONFIG_SERVICE_MINOR, &svc_config);
        reg(PH_MEMORY_SERVICE_ID, PH_MEMORY_SERVICE_MAJOR, PH_MEMORY_SERVICE_MINOR, &svc_memory);
        reg(PH_HOOK_SERVICE_ID, PH_HOOK_SERVICE_MAJOR, PH_HOOK_SERVICE_MINOR, &svc_hook);
        reg(PH_GFX_SERVICE_ID, PH_GFX_SERVICE_MAJOR, PH_GFX_SERVICE_MINOR, &svc_gfx);
        reg(PH_INPUT_SERVICE_ID, PH_INPUT_SERVICE_MAJOR, PH_INPUT_SERVICE_MINOR, &svc_input);
        reg(PH_DS_SERVICE_ID, PH_DS_SERVICE_MAJOR, PH_DS_SERVICE_MINOR, &svc_ds);
        reg(PH_OVERLAY_SERVICE_ID, PH_OVERLAY_SERVICE_MAJOR, PH_OVERLAY_SERVICE_MINOR, &svc_overlay);
        reg(PH_SAVE_SERVICE_ID, PH_SAVE_SERVICE_MAJOR, PH_SAVE_SERVICE_MINOR, &svc_save);
    }

    // =========================================================================================================
    // Lua 5.4 C Bindings (`ph.*` global AND Dusklight-style `require("ph.<module>")`)
    // =========================================================================================================

    static int l_log(lua_State* L) {
        LoadedMod* m = get_mod(L);
        const int n = lua_gettop(L);
        std::string msg;
        for (int i = 1; i <= n; i++) {
            if (i > 1) msg.push_back('\t');
            size_t len = 0;
            const char* s = luaL_tolstring(L, i, &len);
            if (s) msg.append(s, len);
            lua_pop(L, 1);
        }
        std::fprintf(stderr, "[mod:%s] %s\n", m ? m->id.c_str() : "?", msg.c_str());
        return 0;
    }
    static int l_log_level(lua_State* L, const char* tag) {
        LoadedMod* m = get_mod(L);
        const char* s = luaL_checkstring(L, 1);
        std::fprintf(stderr, "[mod:%s][%s] %s\n", m ? m->id.c_str() : "?", tag, s);
        return 0;
    }
    static int l_log_info(lua_State* L) { return l_log_level(L, "INFO"); }
    static int l_log_warn(lua_State* L) { return l_log_level(L, "WARN"); }
    static int l_log_error(lua_State* L) { return l_log_level(L, "ERR"); }
    static int l_log_debug(lua_State* L) { return l_log_level(L, "DBG"); }

    static int l_notify(lua_State* L) {
        Impl* self = get_impl(L);
        const char* msg = luaL_checkstring(L, 1);
        const uint64_t ms = uint64_t(std::clamp<lua_Integer>(luaL_optinteger(L, 2, 2500), 250, 15000));
        if (self->cb.notify) self->cb.notify(msg, ms);
        return 0;
    }
    static int l_fail(lua_State* L) {
        const char* msg = luaL_optstring(L, 1, "mod called fail()");
        return luaL_error(L, "%s", msg);
    }
    static int l_on(lua_State* L) {
        LoadedMod* m = get_mod(L);
        const char* ev = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_pushvalue(L, 2);
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        const int id = m->next_handler_id++;
        m->handlers.push_back({id, ev, ref});
        lua_pushinteger(L, id);
        return 1;
    }
    static int l_off(lua_State* L) {
        LoadedMod* m = get_mod(L);
        const int id = int(luaL_checkinteger(L, 1));
        for (auto it = m->handlers.begin(); it != m->handlers.end(); ++it) {
            if (it->id == id) {
                luaL_unref(L, LUA_REGISTRYINDEX, it->lua_ref);
                m->handlers.erase(it);
                lua_pushboolean(L, 1);
                return 1;
            }
        }
        lua_pushboolean(L, 0);
        return 1;
    }

    // Sandboxed require: supports `ph.<service>` built-in modules (Dusklight-style) AND mod-local `.lua` files!
    static int l_require(lua_State* L) {
        LoadedMod* m = get_mod(L);
        std::string name = luaL_checkstring(L, 1);
        lua_getfield(L, LUA_REGISTRYINDEX, "_PH_LOADED");
        lua_getfield(L, -1, name.c_str());
        if (!lua_isnil(L, -1)) return 1;
        lua_pop(L, 1);

        // Built-in `ph.<mod>` modules mirror the global `ph` sub-tables.
        if (name.rfind("ph.", 0) == 0) {
            const std::string sub = name.substr(3);
            lua_getglobal(L, "ph");
            if (sub == "host") {
                lua_pushvalue(L, -1);
            } else {
                lua_getfield(L, -1, sub.c_str());
            }
            if (!lua_isnil(L, -1)) {
                lua_pushvalue(L, -1);
                lua_setfield(L, -4, name.c_str());
                return 1;
            }
            lua_pop(L, 2);
        }

        std::string rel = name;
        if (rel.rfind("./", 0) == 0) rel.erase(0, 2);
        for (char& c : rel) if (c == '.') c = '/';
        if (rel.size() < 4 || rel.substr(rel.size() - 4) != ".lua") rel += ".lua";

        fs::path resolved;
        bool ok = safe_subpath(m->dir / "res", rel, resolved) && fs::is_regular_file(resolved);
        if (!ok) ok = safe_subpath(m->dir, rel, resolved) && fs::is_regular_file(resolved);
        if (!ok) return luaL_error(L, "module '%s' not found in mod directory", name.c_str());

        const std::string src = read_text_file(resolved);
        const std::string chunk = "@" + m->id + "/" + rel;
        if (luaL_loadbufferx(L, src.data(), src.size(), chunk.c_str(), "t") != LUA_OK) return lua_error(L);
        lua_call(L, 0, 1);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 1); }
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name.c_str());
        return 1;
    }

    // ph.mem
    static int l_mem_u8(lua_State* L) { auto* s = get_impl(L); lua_pushinteger(L, s->rt ? s->rt->peek8(uint32_t(luaL_checkinteger(L, 1))) : 0); return 1; }
    static int l_mem_s8(lua_State* L) { auto* s = get_impl(L); lua_pushinteger(L, s->rt ? int8_t(s->rt->peek8(uint32_t(luaL_checkinteger(L, 1)))) : 0); return 1; }
    static int l_mem_u16(lua_State* L) { auto* s = get_impl(L); lua_pushinteger(L, s->rt ? s->rt->peek16(uint32_t(luaL_checkinteger(L, 1))) : 0); return 1; }
    static int l_mem_s16(lua_State* L) { auto* s = get_impl(L); lua_pushinteger(L, s->rt ? int16_t(s->rt->peek16(uint32_t(luaL_checkinteger(L, 1)))) : 0); return 1; }
    static int l_mem_u32(lua_State* L) { auto* s = get_impl(L); lua_pushinteger(L, lua_Integer(s->rt ? s->rt->peek32(uint32_t(luaL_checkinteger(L, 1))) : 0u)); return 1; }
    static int l_mem_s32(lua_State* L) { auto* s = get_impl(L); lua_pushinteger(L, int32_t(s->rt ? s->rt->peek32(uint32_t(luaL_checkinteger(L, 1))) : 0u)); return 1; }
    static int l_mem_f32(lua_State* L) {
        auto* s = get_impl(L);
        const uint32_t raw = s->rt ? s->rt->peek32(uint32_t(luaL_checkinteger(L, 1))) : 0u;
        float f = 0.0f; std::memcpy(&f, &raw, 4);
        lua_pushnumber(L, double(f));
        return 1;
    }
    static int l_mem_q12(lua_State* L) {
        auto* s = get_impl(L);
        const int32_t v = int32_t(s->rt ? s->rt->peek32(uint32_t(luaL_checkinteger(L, 1))) : 0u);
        lua_pushnumber(L, double(v) / 4096.0);
        return 1;
    }
    static int l_mem_write_u8(lua_State* L) { auto* s = get_impl(L); lua_pushboolean(L, (s->rt && s->rt->poke8(uint32_t(luaL_checkinteger(L, 1)), uint8_t(luaL_checkinteger(L, 2)))) ? 1 : 0); return 1; }
    static int l_mem_write_u16(lua_State* L) { auto* s = get_impl(L); lua_pushboolean(L, (s->rt && s->rt->poke16(uint32_t(luaL_checkinteger(L, 1)), uint16_t(luaL_checkinteger(L, 2)))) ? 1 : 0); return 1; }
    static int l_mem_write_u32(lua_State* L) { auto* s = get_impl(L); lua_pushboolean(L, (s->rt && s->rt->poke32(uint32_t(luaL_checkinteger(L, 1)), uint32_t(luaL_checkinteger(L, 2)))) ? 1 : 0); return 1; }
    static int l_mem_read_bytes(lua_State* L) {
        auto* s = get_impl(L);
        const uint32_t a = uint32_t(luaL_checkinteger(L, 1));
        const size_t n = size_t(std::clamp<lua_Integer>(luaL_checkinteger(L, 2), 0, 1024 * 1024));
        std::string buf(n, '\0');
        const size_t got = (s->rt && n > 0) ? s->rt->read_mem(a, reinterpret_cast<uint8_t*>(buf.data()), n) : 0;
        lua_pushlstring(L, buf.data(), got);
        return 1;
    }
    static int l_mem_write_bytes(lua_State* L) {
        auto* s = get_impl(L);
        const uint32_t a = uint32_t(luaL_checkinteger(L, 1));
        size_t len = 0;
        const char* data = luaL_checklstring(L, 2, &len);
        const size_t wrote = (s->rt && len > 0) ? s->rt->write_mem(a, reinterpret_cast<const uint8_t*>(data), len) : 0;
        lua_pushinteger(L, lua_Integer(wrote));
        return 1;
    }
    static int l_mem_read_cstr(lua_State* L) {
        auto* s = get_impl(L);
        const uint32_t a = uint32_t(luaL_checkinteger(L, 1));
        const size_t max_len = size_t(std::clamp<lua_Integer>(luaL_optinteger(L, 2, 256), 1, 4096));
        std::string buf(max_len, '\0');
        const size_t got = s->rt ? s->rt->read_mem(a, reinterpret_cast<uint8_t*>(buf.data()), max_len) : 0;
        size_t len = 0; while (len < got && buf[len] != '\0') len++;
        lua_pushlstring(L, buf.data(), len);
        return 1;
    }
    static int l_mem_find(lua_State* L) {
        auto* s = get_impl(L);
        const uint32_t found = s->find_mem_pattern(uint32_t(luaL_checkinteger(L, 1)),
                                                   size_t(std::max<lua_Integer>(0, luaL_checkinteger(L, 2))),
                                                   luaL_checkstring(L, 3));
        if (!found) return lua_pushnil(L), 1;
        lua_pushinteger(L, lua_Integer(found));
        return 1;
    }

    // ph.hook (ARM9 game function hooking directly from Lua!)
    static uint32_t resolve_lua_hook_target(lua_State* L, int idx) {
        Impl* s = get_impl(L);
        if (lua_isstring(L, idx)) return s->rt ? s->rt->resolve_symbol(lua_tostring(L, idx)) : 0;
        return uint32_t(luaL_checkinteger(L, idx));
    }
    static int l_hook_install(lua_State* L, ntr::Runtime::HookKind kind) {
        Impl* s = get_impl(L);
        LoadedMod* m = get_mod(L);
        if (!s->rt) return lua_pushnil(L), 1;
        const uint32_t addr = resolve_lua_hook_target(L, 1);
        if (!addr) return lua_pushnil(L), 1;
        luaL_checktype(L, 2, LUA_TFUNCTION);
        const int prio = int(luaL_optinteger(L, 3, 0));
        lua_pushvalue(L, 2);
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        auto rec = std::make_unique<HookRecord>();
        rec->owner = m;
        rec->lua_ref = ref;
        HookRecord* raw = rec.get();
        const uint64_t h = s->rt->add_function_hook(addr, kind, &hook_trampoline, raw, prio);
        if (!h) { luaL_unref(L, LUA_REGISTRYINDEX, ref); return lua_pushnil(L), 1; }
        raw->rt_handle = h;
        s->hook_records[h] = std::move(rec);
        m->owned_hooks.push_back(h);
        lua_pushinteger(L, lua_Integer(h));
        return 1;
    }
    static int l_hook_resolve(lua_State* L) {
        Impl* s = get_impl(L);
        const uint32_t a = s->rt ? s->rt->resolve_symbol(luaL_checkstring(L, 1)) : 0;
        if (!a) return lua_pushnil(L), 1;
        lua_pushinteger(L, lua_Integer(a));
        return 1;
    }
    static int l_hook_symbol_name(lua_State* L) {
        Impl* s = get_impl(L);
        const char* n = s->rt ? s->rt->symbol_name(uint32_t(luaL_checkinteger(L, 1))) : nullptr;
        if (!n) return lua_pushnil(L), 1;
        lua_pushstring(L, n);
        return 1;
    }
    static int l_hook_pre(lua_State* L) { return l_hook_install(L, ntr::Runtime::HookKind::Pre); }
    static int l_hook_post(lua_State* L) { return l_hook_install(L, ntr::Runtime::HookKind::Post); }
    static int l_hook_replace(lua_State* L) { return l_hook_install(L, ntr::Runtime::HookKind::Replace); }
    static int l_hook_remove(lua_State* L) {
        Impl* s = get_impl(L);
        LoadedMod* m = get_mod(L);
        const uint64_t h = uint64_t(luaL_checkinteger(L, 1));
        auto it = s->hook_records.find(h);
        if (it == s->hook_records.end() || it->second->owner != m) return lua_pushboolean(L, 0), 1;
        if (s->rt) s->rt->remove_function_hook(h);
        if (it->second->lua_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, it->second->lua_ref);
        s->hook_records.erase(it);
        auto& v = m->owned_hooks;
        v.erase(std::remove(v.begin(), v.end(), h), v.end());
        lua_pushboolean(L, 1);
        return 1;
    }
    static int l_hook_call_original(lua_State* L) {
        Impl* s = get_impl(L);
        const uint64_t h = uint64_t(luaL_checkinteger(L, 1));
        lua_pushboolean(L, (s->rt && s->rt->call_hook_original(h, nullptr)) ? 1 : 0);
        return 1;
    }

    // ph.ds
    static int l_ds_display(lua_State* L) {
        Impl* s = get_impl(L);
        const ntr::Runtime::DisplayInfo d = s->rt ? s->rt->display_info() : ntr::Runtime::DisplayInfo{};
        lua_createtable(L, 0, 13);
        lua_pushinteger(L, d.powcnt1); lua_setfield(L, -2, "powcnt1");
        lua_pushinteger(L, d.dispcnt_a); lua_setfield(L, -2, "dispcnt_a");
        lua_pushinteger(L, d.dispcnt_b); lua_setfield(L, -2, "dispcnt_b");
        lua_pushinteger(L, d.master_bright_a); lua_setfield(L, -2, "master_bright_a");
        lua_pushinteger(L, d.master_bright_b); lua_setfield(L, -2, "master_bright_b");
        lua_pushinteger(L, d.disp3dcnt); lua_setfield(L, -2, "disp3dcnt");
        lua_pushboolean(L, d.screen_swap ? 1 : 0); lua_setfield(L, -2, "screen_swap");
        lua_pushboolean(L, d.screen_swap ? 1 : 0); lua_setfield(L, -2, "is_3d_on_top");
        lua_pushboolean(L, d.screens_enabled ? 1 : 0); lua_setfield(L, -2, "screens_enabled");
        lua_pushboolean(L, d.lid_closed ? 1 : 0); lua_setfield(L, -2, "lid_closed");
        lua_pushboolean(L, d.new_3d_frame ? 1 : 0); lua_setfield(L, -2, "new_3d_frame");
        const uint32_t top_cnt = d.screen_swap ? d.dispcnt_a : d.dispcnt_b;
        const uint32_t bot_cnt = d.screen_swap ? d.dispcnt_b : d.dispcnt_a;
        lua_pushboolean(L, ((top_cnt >> 16) & 3u) != 0 ? 1 : 0); lua_setfield(L, -2, "top_enabled");
        lua_pushboolean(L, ((bot_cnt >> 16) & 3u) != 0 ? 1 : 0); lua_setfield(L, -2, "bottom_enabled");
        return 1;
    }
    static int l_ds_wide_extra(lua_State* L) { auto* s = get_impl(L); lua_pushinteger(L, s->draw_ctx ? s->draw_ctx->wide_extra : (s->rt ? s->rt->video().wide_extra : 0)); return 1; }
    static int l_ds_wide_layer(lua_State* L) { auto* s = get_impl(L); lua_pushinteger(L, s->draw_ctx ? s->draw_ctx->wide_layer : 1); return 1; }
    static int l_ds_hud_moves(lua_State* L) {
        Impl* s = get_impl(L);
        lua_newtable(L);
        if (s->draw_ctx && s->draw_ctx->frame) {
            const ntr::Frame& f = *s->draw_ctx->frame;
            for (int i = 0; i < f.hud_moves; i++) {
                lua_createtable(L, 0, 5);
                lua_pushinteger(L, f.hud_move[i].x0); lua_setfield(L, -2, "x0");
                lua_pushinteger(L, f.hud_move[i].y0); lua_setfield(L, -2, "y0");
                lua_pushinteger(L, f.hud_move[i].x1); lua_setfield(L, -2, "x1");
                lua_pushinteger(L, f.hud_move[i].y1); lua_setfield(L, -2, "y1");
                lua_pushinteger(L, f.hud_move[i].dx); lua_setfield(L, -2, "dx");
                lua_rawseti(L, -2, i + 1);
            }
        }
        return 1;
    }
    static int l_ds_set_mic_blow(lua_State* L) { auto* s = get_impl(L); if (s->cb.set_mic_blow) s->cb.set_mic_blow(lua_toboolean(L, 1) != 0); return 0; }
    static int l_ds_set_mic_sample(lua_State* L) { auto* s = get_impl(L); if (s->cb.set_mic_sample) s->cb.set_mic_sample(lua_toboolean(L, 1) != 0); return 0; }
    static int l_ds_set_lid_closed(lua_State* L) { auto* s = get_impl(L); if (s->cb.set_lid_closed) s->cb.set_lid_closed(lua_toboolean(L, 1) != 0); return 0; }
    static int l_ds_set_cpu_speed(lua_State* L) { auto* s = get_impl(L); if (s->rt) s->rt->set_cpu_speed(int(std::clamp<lua_Integer>(luaL_checkinteger(L, 1), 1, 4))); return 0; }
    static int l_ds_set_fast_cart(lua_State* L) { auto* s = get_impl(L); if (s->rt) s->rt->set_fast_cart(lua_toboolean(L, 1) != 0); return 0; }
    static int l_ds_save_state(lua_State* L) { auto* s = get_impl(L); const int sl = int(luaL_checkinteger(L, 1)); if (sl >= 1 && sl <= 12 && s->cb.save_state) s->cb.save_state(sl); return 0; }
    static int l_ds_load_state(lua_State* L) { auto* s = get_impl(L); const int sl = int(luaL_checkinteger(L, 1)); if (sl >= 1 && sl <= 12 && s->cb.load_state) s->cb.load_state(sl); return 0; }
    static int l_ds_set_layout(lua_State* L) {
        auto* s = get_impl(L);
        const std::string mode = luaL_checkstring(L, 1);
        if (s->cb.set_layout) s->cb.set_layout(mode == "top" ? Layout::Top : mode == "both" ? Layout::Both : Layout::Bottom);
        return 0;
    }
    static int l_ds_get_layout(lua_State* L) { auto* s = get_impl(L); lua_pushstring(L, layout_name(s->cb.get_layout ? s->cb.get_layout() : Layout::Bottom)); return 1; }
    static int l_ds_screenshot(lua_State* L) { auto* s = get_impl(L); if (s->cb.screenshot) s->cb.screenshot(luaL_optstring(L, 1, "")); return 0; }

    // ph.input
    static int l_input_held(lua_State* L) { auto* s = get_impl(L); lua_pushinteger(L, s->input_ctx ? s->input_ctx->held : 0); return 1; }
    static int l_input_pressed(lua_State* L) { auto* s = get_impl(L); lua_pushinteger(L, s->input_ctx ? s->input_ctx->pressed : 0); return 1; }
    static int l_input_is_down(lua_State* L) { auto* s = get_impl(L); const uint32_t m = uint32_t(luaL_checkinteger(L, 1)); lua_pushboolean(L, (s->input_ctx && (s->input_ctx->held & m) == m) ? 1 : 0); return 1; }
    static int l_input_touch(lua_State* L) {
        auto* s = get_impl(L);
        lua_createtable(L, 0, 3);
        lua_pushboolean(L, (s->input_ctx && s->input_ctx->touching) ? 1 : 0); lua_setfield(L, -2, "touching");
        lua_pushinteger(L, s->input_ctx ? s->input_ctx->tx : 0); lua_setfield(L, -2, "x");
        lua_pushinteger(L, s->input_ctx ? s->input_ctx->ty : 0); lua_setfield(L, -2, "y");
        return 1;
    }
    static int l_input_pointer(lua_State* L) {
        auto* s = get_impl(L);
        lua_createtable(L, 0, 7);
        lua_pushnumber(L, s->input_ctx ? s->input_ctx->pointer_x : 0.0f); lua_setfield(L, -2, "x");
        lua_pushnumber(L, s->input_ctx ? s->input_ctx->pointer_y : 0.0f); lua_setfield(L, -2, "y");
        lua_pushboolean(L, (s->input_ctx && s->input_ctx->pointer_down) ? 1 : 0); lua_setfield(L, -2, "down");
        lua_pushboolean(L, (s->input_ctx && s->input_ctx->pointer_just_pressed) ? 1 : 0); lua_setfield(L, -2, "just_pressed");
        lua_pushboolean(L, (s->input_ctx && s->input_ctx->pointer_just_released) ? 1 : 0); lua_setfield(L, -2, "just_released");
        lua_pushboolean(L, (s->input_ctx && s->input_ctx->right_down) ? 1 : 0); lua_setfield(L, -2, "right_down");
        lua_pushboolean(L, (s->input_ctx && s->input_ctx->right_just_pressed) ? 1 : 0); lua_setfield(L, -2, "right_just_pressed");
        return 1;
    }
    static int l_input_stick(lua_State* L) {
        auto* s = get_impl(L);
        lua_createtable(L, 0, 3);
        lua_pushboolean(L, (s->input_ctx && s->input_ctx->stick_on) ? 1 : 0); lua_setfield(L, -2, "enabled");
        lua_pushnumber(L, s->input_ctx ? s->input_ctx->stick_x : 0.0f); lua_setfield(L, -2, "x");
        lua_pushnumber(L, s->input_ctx ? s->input_ctx->stick_y : 0.0f); lua_setfield(L, -2, "y");
        return 1;
    }
    static int l_input_key_down(lua_State* L) { auto* s = get_impl(L); lua_pushboolean(L, (s->cb.is_key_down && s->cb.is_key_down(luaL_checkstring(L, 1))) ? 1 : 0); return 1; }
    static int l_input_key_pressed(lua_State* L) { auto* s = get_impl(L); lua_pushboolean(L, (s->cb.was_key_pressed && s->cb.was_key_pressed(luaL_checkstring(L, 1))) ? 1 : 0); return 1; }

    // ph.gfx
    static int l_gfx_win_size(lua_State* L) {
        Impl* s = get_impl(L);
        if (s->draw_ctx) { lua_pushinteger(L, s->draw_ctx->win_w); lua_pushinteger(L, s->draw_ctx->win_h); lua_pushnumber(L, s->draw_ctx->ui_scale); }
        else if (s->input_ctx) { lua_pushinteger(L, s->input_ctx->win_w); lua_pushinteger(L, s->input_ctx->win_h); lua_pushnumber(L, 1.0); }
        else { lua_pushinteger(L, 256); lua_pushinteger(L, 192); lua_pushnumber(L, 1.0); }
        return 3;
    }
    static int l_gfx_default_views(lua_State* L) {
        Impl* s = get_impl(L);
        if (!s->draw_ctx) return lua_pushnil(L), 1;
        Layout layout = s->draw_ctx->layout;
        if (lua_isstring(L, 1)) {
            const std::string ls = lua_tostring(L, 1);
            if (ls == "top") layout = Layout::Top;
            else if (ls == "both") layout = Layout::Both;
            else if (ls == "bottom") layout = Layout::Bottom;
        }
        push_views(L, compute_views(float(s->draw_ctx->win_w), float(s->draw_ctx->win_h), layout,
                                    s->draw_ctx->stretch, s->draw_ctx->wide_extra, s->draw_ctx->wide_layer));
        return 1;
    }
    static int l_gfx_draw_screen(lua_State* L) {
        Impl* s = get_impl(L);
        if (!s->draw_ctx || !s->draw_ctx->gl || !s->draw_ctx->frame) return 0;
        int layer = 1;
        if (lua_isstring(L, 1)) { const std::string ls = lua_tostring(L, 1); layer = (ls == "top" || ls == "0") ? 0 : 1; }
        else layer = int(luaL_checkinteger(L, 1));
        const float x = float(luaL_checknumber(L, 2)), y = float(luaL_checknumber(L, 3));
        const float w = float(luaL_checknumber(L, 4)), h = float(luaL_checknumber(L, 5));
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1, alpha = 1.0f;
        bool smooth = s->draw_ctx->smooth, wide = s->draw_ctx->wide && s->draw_ctx->wide_extra > 0;
        uint32_t tint = 0xFFFFFFFFu;
        if (lua_istable(L, 6)) {
            lua_getfield(L, 6, "smooth"); if (lua_isboolean(L, -1)) smooth = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
            lua_getfield(L, 6, "wide"); if (lua_isboolean(L, -1)) wide = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
            lua_getfield(L, 6, "alpha"); if (lua_isnumber(L, -1)) alpha = float(lua_tonumber(L, -1)); lua_pop(L, 1);
            lua_getfield(L, 6, "tint");
            if (lua_isinteger(L, -1)) { const uint32_t t = uint32_t(lua_tointeger(L, -1)); tint = (t <= 0xFFFFFFu) ? (0xFF000000u | t) : t; }
            lua_pop(L, 1);
            lua_getfield(L, 6, "u0"); if (lua_isnumber(L, -1)) u0 = float(lua_tonumber(L, -1)); lua_pop(L, 1);
            lua_getfield(L, 6, "v0"); if (lua_isnumber(L, -1)) v0 = float(lua_tonumber(L, -1)); lua_pop(L, 1);
            lua_getfield(L, 6, "u1"); if (lua_isnumber(L, -1)) u1 = float(lua_tonumber(L, -1)); lua_pop(L, 1);
            lua_getfield(L, 6, "v1"); if (lua_isnumber(L, -1)) v1 = float(lua_tonumber(L, -1)); lua_pop(L, 1);
            lua_getfield(L, 6, "src");
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "x"); const float sx = float(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
                lua_getfield(L, -1, "y"); const float sy = float(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
                lua_getfield(L, -1, "w"); const float sw = float(luaL_optnumber(L, -1, 256.0)); lua_pop(L, 1);
                lua_getfield(L, -1, "h"); const float sh = float(luaL_optnumber(L, -1, 192.0)); lua_pop(L, 1);
                u0 = std::clamp(sx / 256.0f, 0.0f, 1.0f); v0 = std::clamp(sy / 192.0f, 0.0f, 1.0f);
                u1 = std::clamp((sx + sw) / 256.0f, 0.0f, 1.0f); v1 = std::clamp((sy + sh) / 192.0f, 0.0f, 1.0f);
                wide = false;
            }
            lua_pop(L, 1);
        }
        s->draw_ctx->gl->screen_ex(*s->draw_ctx->frame, layer, {x, y, w, h}, u0, v0, u1, v1, smooth, wide, alpha, tint);
        if (u0 == 0.0f && v0 == 0.0f && u1 == 1.0f && v1 == 1.0f && alpha >= 0.5f) {
            const auto& dc = *s->draw_ctx;
            const bool w_on = wide && dc.wide_extra > 0 && dc.wide_layer == layer;
            std::array<HudMove, 32> hm{};
            int hm_cnt = 0;
            if (w_on) {
                hm_cnt = std::min(dc.frame->hud_moves, 32);
                for (int i = 0; i < hm_cnt; i++) {
                    const auto& m = dc.frame->hud_move[i];
                    hm[size_t(i)] = {m.x0, m.y0, m.x1, m.y1, m.dx};
                }
            }
            dc.gl->draw_hd_text(layer, {x, y, w, h}, w_on ? dc.wide_extra : 0, w_on ? hm.data() : nullptr, hm_cnt);
        }
        return 0;
    }
    static int l_gfx_fill(lua_State* L) {
        Impl* s = get_impl(L);
        if (s->draw_ctx && s->draw_ctx->gl)
            s->draw_ctx->gl->fill({float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)),
                                   float(luaL_checknumber(L, 3)), float(luaL_checknumber(L, 4))},
                                  parse_argb(L, 5, 0xFFFFFFFFu));
        return 0;
    }
    static int l_gfx_outline(lua_State* L) {
        Impl* s = get_impl(L);
        if (s->draw_ctx && s->draw_ctx->gl)
            s->draw_ctx->gl->outline({float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)),
                                      float(luaL_checknumber(L, 3)), float(luaL_checknumber(L, 4))},
                                     float(luaL_checknumber(L, 5)), parse_argb(L, 6, 0xFFFFFFFFu));
        return 0;
    }
    static int l_gfx_text(lua_State* L) {
        Impl* s = get_impl(L);
        if (!s->draw_ctx || !s->draw_ctx->gl) return lua_pushnumber(L, 0), 1;
        lua_pushnumber(L, double(s->draw_ctx->gl->text(float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)),
                                                       float(luaL_checknumber(L, 3)), luaL_checkstring(L, 4),
                                                       parse_argb(L, 5, 0xFFFFFFFFu))));
        return 1;
    }
    static int l_gfx_set_clip(lua_State* L) {
        Impl* s = get_impl(L);
        if (s->draw_ctx && s->draw_ctx->gl)
            s->draw_ctx->gl->set_clip({float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)),
                                       float(luaL_checknumber(L, 3)), float(luaL_checknumber(L, 4))});
        return 0;
    }
    static int l_gfx_clear_clip(lua_State* L) {
        Impl* s = get_impl(L);
        if (s->draw_ctx && s->draw_ctx->gl) s->draw_ctx->gl->clear_clip();
        return 0;
    }
    static int l_gfx_load_image(lua_State* L) {
        Impl* s = get_impl(L);
        LoadedMod* m = get_mod(L);
        GlPresenter* gl = (s->draw_ctx && s->draw_ctx->gl) ? s->draw_ctx->gl : s->gl_presenter;
        if (!gl || !s->cb.decode_image) return lua_pushnil(L), 1;
        const std::string rel = luaL_checkstring(L, 1);
        const bool smooth = lua_isnoneornil(L, 2) ? true : (lua_toboolean(L, 2) != 0);
        if (auto it = m->loaded_images.find(rel); it != m->loaded_images.end()) {
            int w = 0, h = 0;
            if (gl->image_size(it->second, w, h)) {
                lua_createtable(L, 0, 3);
                lua_pushinteger(L, it->second); lua_setfield(L, -2, "id");
                lua_pushinteger(L, w); lua_setfield(L, -2, "w");
                lua_pushinteger(L, h); lua_setfield(L, -2, "h");
                return 1;
            }
        }
        fs::path resolved;
        if (!safe_subpath(m->dir / "res", rel, resolved) || !fs::is_regular_file(resolved)) {
            if (!safe_subpath(m->dir, rel, resolved) || !fs::is_regular_file(resolved)) return lua_pushnil(L), 1;
        }
        std::vector<uint32_t> rgba;
        int w = 0, h = 0;
        if (!s->cb.decode_image(resolved, rgba, w, h)) return lua_pushnil(L), 1;
        const int id = gl->create_image(rgba.data(), w, h, smooth);
        if (id <= 0) return lua_pushnil(L), 1;
        m->loaded_images[rel] = id;
        m->owned_images.push_back(id);
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, id); lua_setfield(L, -2, "id");
        lua_pushinteger(L, w); lua_setfield(L, -2, "w");
        lua_pushinteger(L, h); lua_setfield(L, -2, "h");
        return 1;
    }
    static int l_gfx_draw_image(lua_State* L) {
        Impl* s = get_impl(L);
        if (!s->draw_ctx || !s->draw_ctx->gl) return 0;
        int id = 0;
        if (lua_istable(L, 1)) { lua_getfield(L, 1, "id"); id = int(lua_tointeger(L, -1)); lua_pop(L, 1); }
        else id = int(luaL_checkinteger(L, 1));
        const Rect r{float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)),
                     float(luaL_checknumber(L, 4)), float(luaL_checknumber(L, 5))};
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1, alpha = 1.0f;
        uint32_t tint = 0xFFFFFFFFu;
        if (lua_istable(L, 6)) {
            lua_getfield(L, 6, "u0"); if (lua_isnumber(L, -1)) u0 = float(lua_tonumber(L, -1)); lua_pop(L, 1);
            lua_getfield(L, 6, "v0"); if (lua_isnumber(L, -1)) v0 = float(lua_tonumber(L, -1)); lua_pop(L, 1);
            lua_getfield(L, 6, "u1"); if (lua_isnumber(L, -1)) u1 = float(lua_tonumber(L, -1)); lua_pop(L, 1);
            lua_getfield(L, 6, "v1"); if (lua_isnumber(L, -1)) v1 = float(lua_tonumber(L, -1)); lua_pop(L, 1);
            lua_getfield(L, 6, "alpha"); if (lua_isnumber(L, -1)) alpha = float(lua_tonumber(L, -1)); lua_pop(L, 1);
            lua_getfield(L, 6, "tint");
            if (lua_isinteger(L, -1)) { const uint32_t t = uint32_t(lua_tointeger(L, -1)); tint = (t <= 0xFFFFFFu) ? (0xFF000000u | t) : t; }
            lua_pop(L, 1);
        }
        s->draw_ctx->gl->draw_image(id, r, u0, v0, u1, v1, alpha, tint);
        return 0;
    }
    static int l_gfx_to_ds(lua_State* L) {
        Impl* s = get_impl(L);
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_getfield(L, 1, "x"); const float rx = float(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 1, "y"); const float ry = float(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 1, "w"); const float rw = float(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 1, "h"); const float rh = float(lua_tonumber(L, -1)); lua_pop(L, 1);
        int tx = 0, ty = 0;
        c_gfx_to_ds(s->context_for(get_mod(L)), {rx, ry, rw, rh}, float(luaL_checknumber(L, 2)),
                    float(luaL_checknumber(L, 3)), lua_toboolean(L, 4) != 0, &tx, &ty);
        lua_pushinteger(L, tx); lua_pushinteger(L, ty);
        return 2;
    }
    static int l_gfx_set_touch_rect(lua_State* L) {
        Impl* s = get_impl(L);
        ModTouchRect tr;
        tr.rect = {float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)),
                   float(luaL_checknumber(L, 3)), float(luaL_checknumber(L, 4))};
        tr.src_ds = {0, 0, 256, 192};
        if (lua_istable(L, 5)) {
            lua_getfield(L, 5, "wide"); tr.wide = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
            lua_getfield(L, 5, "block"); tr.block = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
            lua_getfield(L, 5, "priority"); if (lua_isinteger(L, -1)) tr.priority = int(lua_tointeger(L, -1)); lua_pop(L, 1);
            lua_getfield(L, 5, "src");
            if (lua_istable(L, -1)) {
                tr.has_src = true;
                lua_getfield(L, -1, "x"); tr.src_ds.x = float(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
                lua_getfield(L, -1, "y"); tr.src_ds.y = float(luaL_optnumber(L, -1, 0.0)); lua_pop(L, 1);
                lua_getfield(L, -1, "w"); tr.src_ds.w = float(luaL_optnumber(L, -1, 256.0)); lua_pop(L, 1);
                lua_getfield(L, -1, "h"); tr.src_ds.h = float(luaL_optnumber(L, -1, 192.0)); lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        s->frame_touch_rects.push_back(tr);
        return 0;
    }
    static int l_gfx_clear_touch_rects(lua_State* L) { get_impl(L)->frame_touch_rects.clear(); return 0; }

    // ph.config
    static ModConfigItem* find_or_add_cfg(LoadedMod* m, const std::string& key) {
        for (ModConfigItem& item : m->config_items)
            if (item.key == key) return &item;
        m->config_items.push_back({});
        m->config_items.back().key = key;
        return &m->config_items.back();
    }
    static int l_cfg_bool(lua_State* L) {
        Impl* s = get_impl(L); LoadedMod* m = get_mod(L);
        const std::string key = luaL_checkstring(L, 1), label = luaL_checkstring(L, 2);
        const bool cur = m->config_json.bool_or(key, lua_toboolean(L, 3) != 0);
        m->config_json.obj[key] = json::Value::make_bool(cur);
        auto* it = find_or_add_cfg(m, key);
        it->kind = ModConfigItem::Kind::Bool; it->label = label; it->bool_val = cur;
        s->publish_snapshots();
        lua_pushboolean(L, cur ? 1 : 0);
        return 1;
    }
    static int l_cfg_int(lua_State* L) {
        Impl* s = get_impl(L); LoadedMod* m = get_mod(L);
        const std::string key = luaL_checkstring(L, 1), label = luaL_checkstring(L, 2);
        const int64_t def = luaL_checkinteger(L, 3), mn = luaL_checkinteger(L, 4), mx = luaL_checkinteger(L, 5);
        const int64_t step = std::max<int64_t>(1, luaL_optinteger(L, 6, 1));
        const int64_t cur = std::clamp(m->config_json.int_or(key, def), mn, mx);
        m->config_json.obj[key] = json::Value::make_number(double(cur));
        auto* it = find_or_add_cfg(m, key);
        it->kind = ModConfigItem::Kind::Int; it->label = label; it->int_val = cur;
        it->min_val = double(mn); it->max_val = double(mx); it->step = double(step);
        s->publish_snapshots();
        lua_pushinteger(L, lua_Integer(cur));
        return 1;
    }
    static int l_cfg_float(lua_State* L) {
        Impl* s = get_impl(L); LoadedMod* m = get_mod(L);
        const std::string key = luaL_checkstring(L, 1), label = luaL_checkstring(L, 2);
        const double def = luaL_checknumber(L, 3), mn = luaL_checknumber(L, 4), mx = luaL_checknumber(L, 5);
        const double step = std::max(0.01, luaL_optnumber(L, 6, 0.1));
        const double cur = std::clamp(m->config_json.number_or(key, def), mn, mx);
        m->config_json.obj[key] = json::Value::make_number(cur);
        auto* it = find_or_add_cfg(m, key);
        it->kind = ModConfigItem::Kind::Float; it->label = label; it->float_val = cur;
        it->min_val = mn; it->max_val = mx; it->step = step;
        s->publish_snapshots();
        lua_pushnumber(L, cur);
        return 1;
    }
    static int l_cfg_choice(lua_State* L) {
        Impl* s = get_impl(L); LoadedMod* m = get_mod(L);
        const std::string key = luaL_checkstring(L, 1), label = luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TTABLE);
        std::vector<std::string> choices;
        const lua_Integer n = luaL_len(L, 3);
        for (lua_Integer i = 1; i <= n; i++) {
            lua_rawgeti(L, 3, i);
            if (lua_isstring(L, -1)) choices.push_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        if (choices.empty()) choices.push_back("Default");
        int cur_0 = std::clamp(int(luaL_optinteger(L, 4, 1)), 1, int(choices.size())) - 1;
        if (const json::Value* ex = m->config_json.get(key)) {
            if (ex->is_string()) { for (size_t i = 0; i < choices.size(); i++) if (choices[i] == ex->str) cur_0 = int(i); }
            else if (ex->is_number()) cur_0 = std::clamp(int(ex->number) - 1, 0, int(choices.size()) - 1);
        }
        m->config_json.obj[key] = json::Value::make_string(choices[size_t(cur_0)]);
        auto* it = find_or_add_cfg(m, key);
        it->kind = ModConfigItem::Kind::Choice; it->label = label; it->int_val = cur_0; it->choices = choices;
        s->publish_snapshots();
        lua_pushinteger(L, cur_0 + 1);
        lua_pushstring(L, choices[size_t(cur_0)].c_str());
        return 2;
    }
    static int l_cfg_hotkey(lua_State* L) {
        Impl* s = get_impl(L); LoadedMod* m = get_mod(L);
        const std::string key = luaL_checkstring(L, 1), label = luaL_checkstring(L, 2);
        const std::string def = luaL_optstring(L, 3, "None");
        const std::string cur = m->config_json.string_or(key, def);
        m->config_json.obj[key] = json::Value::make_string(cur);
        auto* it = find_or_add_cfg(m, key);
        it->kind = ModConfigItem::Kind::Hotkey; it->label = label; it->string_val = cur;
        s->publish_snapshots();
        lua_pushlstring(L, cur.data(), cur.size());
        return 1;
    }
    static int l_cfg_get(lua_State* L) {
        LoadedMod* m = get_mod(L);
        if (const json::Value* v = m->config_json.get(luaL_checkstring(L, 1))) { push_json_to_lua(L, *v); return 1; }
        if (lua_gettop(L) >= 2) { lua_pushvalue(L, 2); return 1; }
        lua_pushnil(L);
        return 1;
    }
    static int l_cfg_set(lua_State* L) {
        Impl* s = get_impl(L); LoadedMod* m = get_mod(L);
        const std::string key = luaL_checkstring(L, 1);
        const json::Value val = lua_to_json(L, 2);
        m->config_json.obj[key] = val;
        for (size_t i = 0; i < m->config_items.size(); i++) {
            auto& item = m->config_items[i];
            if (item.key != key) continue;
            const auto prev = item_to_value(item);
            if (item.kind == ModConfigItem::Kind::Bool && val.is_bool()) item.bool_val = val.boolean;
            else if (item.kind == ModConfigItem::Kind::Int && val.is_number())
                item.int_val = std::clamp<int64_t>(int64_t(std::llround(val.number)), int64_t(item.min_val), int64_t(item.max_val));
            else if (item.kind == ModConfigItem::Kind::Float && val.is_number())
                item.float_val = std::clamp(val.number, item.min_val, item.max_val);
            else if ((item.kind == ModConfigItem::Kind::String || item.kind == ModConfigItem::Kind::Hotkey) && val.is_string())
                item.string_val = val.str;
            else if (item.kind == ModConfigItem::Kind::Choice) {
                if (val.is_string()) { for (size_t j = 0; j < item.choices.size(); j++) if (item.choices[j] == val.str) item.int_val = int64_t(j); }
                else if (val.is_number() && !item.choices.empty()) item.int_val = std::clamp<int64_t>(int64_t(val.number) - 1, 0, int64_t(item.choices.size()) - 1);
            }
            s->notify_config_change(*m, i, prev, item_to_value(item));
        }
        s->publish_snapshots();
        return 0;
    }
    static int l_cfg_subscribe(lua_State* L) {
        Impl* s = get_impl(L); LoadedMod* m = get_mod(L);
        const std::string key = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        for (size_t i = 0; i < m->config_items.size(); i++) {
            if (m->config_items[i].key != key) continue;
            lua_pushvalue(L, 2);
            const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            const uint64_t sid = s->next_sub_id++;
            m->config_subs.push_back({sid, uint64_t(i + 1), nullptr, nullptr, ref});
            lua_pushinteger(L, lua_Integer(sid));
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }
    static int l_cfg_save(lua_State* L) { get_impl(L)->save_mod_config(*get_mod(L)); return 0; }

    // ph.resource (Dusklight-style res/ reads) & ph.fs (mod data_dir / dir reads & writes)
    static int l_res_load(lua_State* L) {
        LoadedMod* m = get_mod(L);
        fs::path p;
        if (!safe_subpath(m->dir / "res", luaL_checkstring(L, 1), p) || !fs::is_regular_file(p))
            return lua_pushnil(L), 1;
        const std::string data = read_text_file(p);
        lua_pushlstring(L, data.data(), data.size());
        return 1;
    }
    static int l_res_exists(lua_State* L) {
        LoadedMod* m = get_mod(L);
        fs::path p; std::error_code ec;
        lua_pushboolean(L, (safe_subpath(m->dir / "res", luaL_checkstring(L, 1), p) && fs::is_regular_file(p, ec)) ? 1 : 0);
        return 1;
    }
    static int l_fs_read(lua_State* L) {
        LoadedMod* m = get_mod(L);
        const std::string rel = luaL_checkstring(L, 1);
        fs::path p;
        if (!safe_subpath(m->data_dir, rel, p) || !fs::is_regular_file(p)) {
            if (!safe_subpath(m->dir, rel, p) || !fs::is_regular_file(p)) return lua_pushnil(L), 1;
        }
        const std::string data = read_text_file(p);
        lua_pushlstring(L, data.data(), data.size());
        return 1;
    }
    static int l_fs_write(lua_State* L) {
        LoadedMod* m = get_mod(L);
        const fs::path root = m->is_archive ? m->data_dir : m->dir;
        fs::path p;
        if (!safe_subpath(root, luaL_checkstring(L, 1), p)) return lua_pushboolean(L, 0), 1;
        size_t len = 0; const char* data = luaL_checklstring(L, 2, &len);
        lua_pushboolean(L, write_text_file(p, std::string(data, len)) ? 1 : 0);
        return 1;
    }
    static int l_fs_exists(lua_State* L) {
        LoadedMod* m = get_mod(L);
        const std::string rel = luaL_checkstring(L, 1);
        fs::path p; std::error_code ec;
        const bool ok = (safe_subpath(m->data_dir, rel, p) && fs::exists(p, ec)) ||
                        (safe_subpath(m->dir, rel, p) && fs::exists(p, ec));
        lua_pushboolean(L, ok ? 1 : 0);
        return 1;
    }
    static int l_fs_list(lua_State* L) {
        LoadedMod* m = get_mod(L);
        const std::string rel = luaL_optstring(L, 1, "");
        fs::path dir = m->dir;
        if (!rel.empty() && !safe_subpath(m->dir, rel, dir)) return lua_newtable(L), 1;
        lua_newtable(L);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return 1;
        int idx = 1;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            const std::string name = entry.path().filename().generic_string();
            lua_pushlstring(L, name.data(), name.size());
            lua_rawseti(L, -2, idx++);
        }
        return 1;
    }

    // ph.save (per-mod persistent binary blobs)
    static int l_save_set(lua_State* L) {
        Impl* s = get_impl(L); LoadedMod* m = get_mod(L);
        const char* key = luaL_checkstring(L, 1);
        size_t len = 0; const char* data = luaL_checklstring(L, 2, &len);
        lua_pushboolean(L, c_save_set_blob(s->context_for(m), key, data, len) == MOD_OK ? 1 : 0);
        return 1;
    }
    static int l_save_get(lua_State* L) {
        Impl* s = get_impl(L); LoadedMod* m = get_mod(L);
        const char* key = luaL_checkstring(L, 1);
        size_t sz = 0;
        if (c_save_get_blob(s->context_for(m), key, nullptr, &sz) != MOD_OK) return lua_pushnil(L), 1;
        std::string buf(sz, '\0');
        if (c_save_get_blob(s->context_for(m), key, buf.data(), &sz) != MOD_OK) return lua_pushnil(L), 1;
        lua_pushlstring(L, buf.data(), sz);
        return 1;
    }
    static int l_save_delete(lua_State* L) {
        Impl* s = get_impl(L); LoadedMod* m = get_mod(L);
        lua_pushboolean(L, c_save_delete_blob(s->context_for(m), luaL_checkstring(L, 1)) == MOD_OK ? 1 : 0);
        return 1;
    }

    // ph.json / ph.bus / ph.rom
    static int l_json_decode(lua_State* L) {
        size_t len = 0; const char* s = luaL_checklstring(L, 1, &len);
        json::Value v; std::string err;
        if (!json::parse(std::string_view(s, len), v, err)) { lua_pushnil(L); lua_pushlstring(L, err.data(), err.size()); return 2; }
        push_json_to_lua(L, v);
        return 1;
    }
    static int l_json_encode(lua_State* L) {
        const std::string out = json::dump(lua_to_json(L, 1), lua_toboolean(L, 2) != 0);
        lua_pushlstring(L, out.data(), out.size());
        return 1;
    }
    static int l_bus_on(lua_State* L) {
        LoadedMod* m = get_mod(L);
        const char* topic = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_pushvalue(L, 2);
        m->bus_handlers.push_back({topic, luaL_ref(L, LUA_REGISTRYINDEX)});
        return 0;
    }
    static int l_bus_emit(lua_State* L) {
        Impl* s = get_impl(L); LoadedMod* sender = get_mod(L);
        const std::string topic = luaL_checkstring(L, 1);
        const json::Value payload = lua_gettop(L) >= 2 ? lua_to_json(L, 2) : json::Value::make_null();
        const std::string sender_id = sender ? sender->id : "";
        for (auto& target : s->mods) {
            if (!target->L || !target->loaded || target->faulted) continue;
            for (size_t i = 0; i < target->bus_handlers.size(); i++) {
                if (target->bus_handlers[i].topic != topic) continue;
                lua_rawgeti(target->L, LUA_REGISTRYINDEX, target->bus_handlers[i].lua_ref);
                push_json_to_lua(target->L, payload);
                lua_pushlstring(target->L, sender_id.data(), sender_id.size());
                s->call_lua(*target, 2, 0);
                if (target->faulted) break;
            }
        }
        return 0;
    }
    static int l_bus_mods(lua_State* L) {
        Impl* s = get_impl(L);
        lua_createtable(L, int(s->mods.size()), 0);
        int idx = 1;
        for (const auto& m : s->mods) {
            lua_createtable(L, 0, 6);
            lua_pushlstring(L, m->id.data(), m->id.size()); lua_setfield(L, -2, "id");
            lua_pushlstring(L, m->name.data(), m->name.size()); lua_setfield(L, -2, "name");
            lua_pushlstring(L, m->version.data(), m->version.size()); lua_setfield(L, -2, "version");
            lua_pushstring(L, m->kind == ModKind::Native ? "native" : m->kind == ModKind::Lua ? "lua" : "asset");
            lua_setfield(L, -2, "kind");
            lua_pushboolean(L, m->enabled ? 1 : 0); lua_setfield(L, -2, "enabled");
            lua_pushboolean(L, m->loaded ? 1 : 0); lua_setfield(L, -2, "loaded");
            lua_rawseti(L, -2, idx++);
        }
        return 1;
    }
    static int l_rom_list(lua_State* L) {
        Impl* s = get_impl(L);
        const std::string prefix = luaL_optstring(L, 1, "");
        lua_newtable(L);
        int idx = 1;
        for (const NitroFileEntry& e : s->nitro_files) {
            if (!prefix.empty() && e.path.compare(0, prefix.size(), prefix) != 0) continue;
            lua_pushlstring(L, e.path.data(), e.path.size());
            lua_rawseti(L, -2, idx++);
        }
        return 1;
    }
    static int l_rom_file_size(lua_State* L) {
        const auto* e = get_impl(L)->find_nitro(luaL_checkstring(L, 1));
        if (!e) return lua_pushnil(L), 1;
        lua_pushinteger(L, lua_Integer(e->size));
        return 1;
    }
    static int l_rom_read(lua_State* L) {
        Impl* s = get_impl(L);
        const auto* e = s->find_nitro(luaL_checkstring(L, 1));
        if (!e) return lua_pushnil(L), 1;
        const size_t off = size_t(std::max<lua_Integer>(0, luaL_optinteger(L, 2, 0)));
        if (off >= e->size) return lua_pushlstring(L, "", 0), 1;
        const size_t avail = size_t(e->size) - off;
        const size_t req = lua_isnoneornil(L, 3) ? avail : size_t(std::max<lua_Integer>(0, lua_tointeger(L, 3)));
        const size_t n = std::min({avail, req, size_t(4u * 1024u * 1024u)});
        std::string buf(n, '\0');
        const size_t got = c_ovl_read(s->context_for(get_mod(L)), e->path.c_str(), uint32_t(off),
                                      reinterpret_cast<uint8_t*>(buf.data()), n);
        lua_pushlstring(L, buf.data(), got);
        return 1;
    }

    void register_ph_api(LoadedMod& m) {
        lua_State* L = m.L;
        luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1); lua_pop(L, 1);

        luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 1);
        for (const char* banned : {"execute", "exit", "getenv", "remove", "rename", "setlocale", "tmpname"}) {
            lua_pushnil(L); lua_setfield(L, -2, banned);
        }
        lua_pop(L, 1);

        for (const char* banned : {"dofile", "loadfile"}) { lua_pushnil(L); lua_setglobal(L, banned); }
        lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, "_PH_LOADED");
        lua_pushcfunction(L, &l_log); lua_setglobal(L, "print");
        lua_pushcfunction(L, &l_require); lua_setglobal(L, "require");

        lua_pushlightuserdata(L, this); lua_setfield(L, LUA_REGISTRYINDEX, kRegManagerKey);
        lua_pushlightuserdata(L, &m); lua_setfield(L, LUA_REGISTRYINDEX, kRegModKey);

        lua_newtable(L);
        lua_pushinteger(L, kApiVersion); lua_setfield(L, -2, "api_version");
        lua_pushstring(L, kHostVersion); lua_setfield(L, -2, "host_version");
        lua_pushstring(L, platform_name()); lua_setfield(L, -2, "platform");
        lua_pushstring(L, game_code.c_str()); lua_setfield(L, -2, "game_code");
        lua_pushboolean(L, dev_mode ? 1 : 0); lua_setfield(L, -2, "dev_mode");
        lua_pushinteger(L, 0); lua_setfield(L, -2, "frame");
        lua_pushnumber(L, 60.0); lua_setfield(L, -2, "fps");
        lua_pushnumber(L, 1.0 / 60.0); lua_setfield(L, -2, "dt");
        lua_pushcfunction(L, &l_log); lua_setfield(L, -2, "log");
        lua_pushcfunction(L, &l_notify); lua_setfield(L, -2, "notify");
        lua_pushcfunction(L, &l_fail); lua_setfield(L, -2, "fail");
        lua_pushcfunction(L, &l_on); lua_setfield(L, -2, "on");
        lua_pushcfunction(L, &l_off); lua_setfield(L, -2, "off");

        // ph.logger (also accessible via require("ph.log"))
        static const luaL_Reg kLogLib[] = {
            {"info", l_log_info}, {"warn", l_log_warn}, {"error", l_log_error}, {"debug", l_log_debug},
            {nullptr, nullptr},
        };
        luaL_newlib(L, kLogLib);
        lua_setfield(L, -2, "logger");

        lua_createtable(L, 0, 5);
        lua_pushstring(L, m.id.c_str()); lua_setfield(L, -2, "id");
        lua_pushstring(L, m.name.c_str()); lua_setfield(L, -2, "name");
        lua_pushstring(L, m.version.c_str()); lua_setfield(L, -2, "version");
        lua_pushstring(L, m.author.c_str()); lua_setfield(L, -2, "author");
        lua_pushstring(L, m.description.c_str()); lua_setfield(L, -2, "description");
        lua_setfield(L, -2, "mod");

        static const luaL_Reg kMemLib[] = {
            {"u8", l_mem_u8}, {"s8", l_mem_s8}, {"u16", l_mem_u16}, {"s16", l_mem_s16},
            {"u32", l_mem_u32}, {"s32", l_mem_s32}, {"f32", l_mem_f32}, {"q12", l_mem_q12},
            {"write_u8", l_mem_write_u8}, {"write_u16", l_mem_write_u16},
            {"write_u32", l_mem_write_u32}, {"write_s32", l_mem_write_u32},
            {"read_bytes", l_mem_read_bytes}, {"write_bytes", l_mem_write_bytes},
            {"read_cstr", l_mem_read_cstr}, {"find", l_mem_find}, {nullptr, nullptr},
        };
        luaL_newlib(L, kMemLib); lua_setfield(L, -2, "mem");

        static const luaL_Reg kHookLib[] = {
            {"resolve", l_hook_resolve}, {"symbol_name", l_hook_symbol_name},
            {"pre", l_hook_pre}, {"post", l_hook_post}, {"replace", l_hook_replace},
            {"remove", l_hook_remove}, {"call_original", l_hook_call_original}, {nullptr, nullptr},
        };
        luaL_newlib(L, kHookLib); lua_setfield(L, -2, "hook");

        static const luaL_Reg kDsLib[] = {
            {"display", l_ds_display}, {"wide_extra", l_ds_wide_extra}, {"wide_layer", l_ds_wide_layer},
            {"hud_moves", l_ds_hud_moves}, {"set_mic_blow", l_ds_set_mic_blow}, {"set_mic_sample", l_ds_set_mic_sample},
            {"set_lid_closed", l_ds_set_lid_closed}, {"set_cpu_speed", l_ds_set_cpu_speed},
            {"set_fast_cart", l_ds_set_fast_cart}, {"save_state", l_ds_save_state}, {"load_state", l_ds_load_state},
            {"set_layout", l_ds_set_layout}, {"get_layout", l_ds_get_layout}, {"screenshot", l_ds_screenshot},
            {nullptr, nullptr},
        };
        luaL_newlib(L, kDsLib); lua_setfield(L, -2, "ds");

        lua_newtable(L);
        auto sym = [&](const char* k, uint32_t v) { lua_pushinteger(L, lua_Integer(v)); lua_setfield(L, -2, k); };
        sym("DISPCNT_A", 0x04000000u); sym("DISPSTAT", 0x04000004u); sym("VCOUNT", 0x04000006u);
        sym("DISP3DCNT", 0x04000060u); sym("MASTER_BRIGHT_A", 0x0400006Cu); sym("KEYINPUT", 0x04000130u);
        sym("POWCNT1", 0x04000304u); sym("DISPCNT_B", 0x04001000u); sym("MASTER_BRIGHT_B", 0x0400106Cu);
        sym("PALETTE_A", 0x05000000u); sym("PALETTE_B", 0x05000400u); sym("OAM_A", 0x07000000u); sym("OAM_B", 0x07000400u);
        if (game_code == "AZEE") {
            sym("gPlayerControl", 0x027E0FB8u); sym("PC_TOUCH_X", 0x10u); sym("PC_TOUCH_Y", 0x14u);
            sym("PC_TOUCH_DIST", 0x58u); sym("PC_TOUCH_ANGLE", 0x6Au); sym("PC_MOVE_ALLOWED", 0x7Cu);
        }
        lua_setfield(L, -2, "sym");

        static const luaL_Reg kInputLib[] = {
            {"held", l_input_held}, {"pressed", l_input_pressed}, {"is_down", l_input_is_down},
            {"touch", l_input_touch}, {"pointer", l_input_pointer}, {"stick", l_input_stick},
            {"key_down", l_input_key_down}, {"key_pressed", l_input_key_pressed}, {nullptr, nullptr},
        };
        luaL_newlib(L, kInputLib);
        lua_createtable(L, 0, 12);
        auto key = [&](const char* k, uint32_t v) { lua_pushinteger(L, lua_Integer(v)); lua_setfield(L, -2, k); };
        key("A", ntr::KeyA); key("B", ntr::KeyB); key("SELECT", ntr::KeySelect); key("START", ntr::KeyStart);
        key("RIGHT", ntr::KeyRight); key("LEFT", ntr::KeyLeft); key("UP", ntr::KeyUp); key("DOWN", ntr::KeyDown);
        key("R", ntr::KeyR); key("L", ntr::KeyL); key("X", ntr::KeyX); key("Y", ntr::KeyY);
        lua_setfield(L, -2, "KEY");
        lua_setfield(L, -2, "input");

        static const luaL_Reg kGfxLib[] = {
            {"win_size", l_gfx_win_size}, {"default_views", l_gfx_default_views},
            {"draw_screen", l_gfx_draw_screen}, {"fill", l_gfx_fill}, {"outline", l_gfx_outline},
            {"text", l_gfx_text}, {"set_clip", l_gfx_set_clip}, {"clear_clip", l_gfx_clear_clip},
            {"load_image", l_gfx_load_image}, {"draw_image", l_gfx_draw_image}, {"to_ds", l_gfx_to_ds},
            {"set_touch_rect", l_gfx_set_touch_rect}, {"clear_touch_rects", l_gfx_clear_touch_rects},
            {nullptr, nullptr},
        };
        luaL_newlib(L, kGfxLib); lua_setfield(L, -2, "gfx");

        static const luaL_Reg kCfgLib[] = {
            {"bool", l_cfg_bool}, {"int", l_cfg_int}, {"float", l_cfg_float}, {"choice", l_cfg_choice},
            {"hotkey", l_cfg_hotkey}, {"get", l_cfg_get}, {"set", l_cfg_set}, {"subscribe", l_cfg_subscribe},
            {"save", l_cfg_save}, {nullptr, nullptr},
        };
        luaL_newlib(L, kCfgLib); lua_setfield(L, -2, "config");

        static const luaL_Reg kResLib[] = {{"load", l_res_load}, {"exists", l_res_exists}, {nullptr, nullptr}};
        luaL_newlib(L, kResLib); lua_setfield(L, -2, "resource");

        static const luaL_Reg kFsLib[] = {
            {"read", l_fs_read}, {"write", l_fs_write}, {"exists", l_fs_exists}, {"list", l_fs_list}, {nullptr, nullptr},
        };
        luaL_newlib(L, kFsLib); lua_setfield(L, -2, "fs");

        static const luaL_Reg kSaveLib[] = {
            {"set", l_save_set}, {"get", l_save_get}, {"delete", l_save_delete}, {nullptr, nullptr},
        };
        luaL_newlib(L, kSaveLib); lua_setfield(L, -2, "save");

        static const luaL_Reg kJsonLib[] = {{"decode", l_json_decode}, {"encode", l_json_encode}, {nullptr, nullptr}};
        luaL_newlib(L, kJsonLib); lua_setfield(L, -2, "json");

        static const luaL_Reg kBusLib[] = {{"on", l_bus_on}, {"emit", l_bus_emit}, {"mods", l_bus_mods}, {nullptr, nullptr}};
        luaL_newlib(L, kBusLib); lua_setfield(L, -2, "bus");

        static const luaL_Reg kRomLib[] = {{"list", l_rom_list}, {"file_size", l_rom_file_size}, {"read", l_rom_read}, {nullptr, nullptr}};
        luaL_newlib(L, kRomLib); lua_setfield(L, -2, "rom");

        lua_setglobal(L, "ph");
    }

    void apply_ram_patches(LoadedMod& m, bool freeze_only) {
        if (!rt) return;
        for (const RamPatchSpec& p : m.ram_patches) {
            if (freeze_only && !p.freeze) continue;
            if (!p.region.empty() && p.region != game_code) continue;
            if (!p.bytes.empty()) rt->write_mem(p.addr, p.bytes.data(), p.bytes.size());
        }
    }

    void shutdown_single_mod(LoadedMod& m) {
        if (m.kind == ModKind::Native) {
            if (m.initialized && m.fn_shutdown && !m.faulted) {
                try { m.fn_shutdown(); } catch (...) {}
            }
            cleanup_mod_resources(m);
            if (m.initialized) fire_lifecycle(&m, MOD_LIFECYCLE_DETACHED);
            if (m.dynlib) { dynlib_close(m.dynlib); m.dynlib = nullptr; }
            m.desc = nullptr;
            m.fn_init = nullptr; m.fn_update = nullptr; m.fn_input = nullptr;
            m.fn_draw = nullptr; m.fn_post_draw = nullptr; m.fn_shutdown = nullptr;
        } else if (m.L) {
            if (!m.faulted) {
                for (const EventHandler& h : m.handlers) {
                    if (h.event != "shutdown") continue;
                    lua_rawgeti(m.L, LUA_REGISTRYINDEX, h.lua_ref);
                    call_lua(m, 0, 0);
                }
            }
            cleanup_mod_resources(m);
            lua_close(m.L);
            m.L = nullptr;
        } else {
            cleanup_mod_resources(m);
        }
        m.handlers.clear();
        m.bus_handlers.clear();
        m.loaded = false;
        m.initialized = false;
        m.startup_patches_applied = false;
    }

    // Locate a native shared library in `<mod_dir>/lib/<platform>/`.
    static fs::path find_native_library(const fs::path& mod_dir) {
        const fs::path plat_dir = mod_dir / "lib" / native_platform_dir();
        std::error_code ec;
        if (!fs::is_directory(plat_dir, ec)) return {};
        const std::string want_ext = native_lib_ext();
        fs::path fallback;
        for (const auto& entry : fs::directory_iterator(plat_dir, ec)) {
            if (ec) break;
            std::error_code sub_ec;
            if (!entry.is_regular_file(sub_ec)) continue;
            std::string ext = entry.path().extension().string();
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext != want_ext) continue;
            const std::string fname = entry.path().filename().string();
            if (fname == "mod.dll" || fname == "libmod.so" || fname == "libmod.dylib") return entry.path();
            if (fallback.empty()) fallback = entry.path();
        }
        return fallback;
    }

    static fs::path find_lua_entry(const fs::path& mod_dir, const std::string& explicit_entry) {
        std::error_code ec;
        fs::path p;
        if (!explicit_entry.empty()) {
            if (safe_subpath(mod_dir, explicit_entry, p) && fs::is_regular_file(p, ec)) return p;
            if (safe_subpath(mod_dir / "res", explicit_entry, p) && fs::is_regular_file(p, ec)) return p;
            return {};
        }
        if (fs::is_regular_file(mod_dir / "res" / "main.lua", ec)) return mod_dir / "res" / "main.lua";
        if (fs::is_regular_file(mod_dir / "main.lua", ec)) return mod_dir / "main.lua";
        return {};
    }
};

ModManager::ModManager() : impl_(std::make_unique<Impl>()) {}
ModManager::~ModManager() { shutdown_all(nullptr); }

void ModManager::discover(const fs::path& mods_dir, const std::string& game_code, bool dev_mode) {
    shutdown_all(nullptr);
    mods_dir_ = mods_dir;
    game_code_ = game_code;
    impl_->mods_dir = mods_dir;
    impl_->game_code = game_code;
    impl_->dev_mode = dev_mode;
    impl_->mods.clear();
    impl_->mod_ctxs.clear();
    impl_->init_order.clear();
    impl_->load_mods_ini();

    const fs::path cache_dir = mods_dir / ".cache";
    const auto bundles = discover_mod_bundles(mods_dir, cache_dir);

    for (const DiscoveredBundle& bundle : bundles) {
        auto m = std::make_unique<LoadedMod>();
        m->is_archive = bundle.is_archive;
        const std::string stem = sanitize_id(bundle.source_path.stem().string());
        m->id = stem.empty() ? "mod" : stem;
        m->name = bundle.source_path.stem().string();

        if (!bundle.extract_error.empty()) {
            m->error = bundle.extract_error;
            m->compatible = false;
            m->enabled = false;
            std::fprintf(stderr, "mod [%s] archive error: %s\n", m->id.c_str(), m->error.c_str());
            impl_->mods.push_back(std::move(m));
            continue;
        }

        m->dir = bundle.content_dir;
        m->dir_str = m->dir.string();

        const std::string text = read_text_file(m->dir / "mod.json");
        json::Value root;
        std::string err;
        if (!json::parse(text, root, err) || !root.is_object()) {
            m->error = "mod.json: " + (err.empty() ? "root must be an object" : err);
            m->compatible = false;
            m->enabled = false;
            std::fprintf(stderr, "mod [%s] manifest error: %s\n", m->id.c_str(), m->error.c_str());
            impl_->mods.push_back(std::move(m));
            continue;
        }

        if (const std::string mid = sanitize_id(root.string_or("id")); !mid.empty()) m->id = mid;
        m->name = root.string_or("name", m->name);
        m->version = root.string_or("version", "1.0.0");
        m->author = root.string_or("author", "");
        m->description = root.string_or("description", "");
        m->runtime_spec = root.string_or("runtime", "");
        m->entry = root.string_or("entry", "");
        m->priority = int(root.int_or("priority", 0));
        m->enabled_by_default = root.bool_or("enabled_by_default", true);

        m->data_dir = mods_dir / ".data" / m->id;
        m->data_dir_str = m->data_dir.string();

        std::error_code sub_ec;
        m->has_overlay = fs::is_directory(m->dir / "overlay", sub_ec) || fs::is_directory(m->dir / "romfs", sub_ec);
        m->has_textures = fs::is_directory(m->dir / "textures", sub_ec);
        m->native_lib = Impl::find_native_library(m->dir);

        // Classify mod kind (Dusklight 3-tier model: Native C++, Script Lua 5.4, Asset-only).
        const fs::path lua_entry = Impl::find_lua_entry(m->dir, m->entry);
        if (m->runtime_spec.rfind("ph.native", 0) == 0 || !m->native_lib.empty()) {
            m->kind = ModKind::Native;
            if (m->native_lib.empty()) {
                m->compatible = false;
                m->error = std::string("no native library for platform ") + native_platform_dir();
            }
        } else if (m->runtime_spec.rfind("ph.lua", 0) == 0 || !lua_entry.empty()) {
            m->kind = ModKind::Lua;
        } else {
            m->kind = ModKind::AssetOnly;
        }

        if (const json::Value* regs = root.get("regions"); regs && regs->is_array()) {
            for (const json::Value& r : regs->arr)
                if (r.is_string() && !r.str.empty()) m->regions.push_back(r.str);
        }
        if (const json::Value* deps = root.get("dependencies"); deps && deps->is_array()) {
            for (const json::Value& d : deps->arr)
                if (d.is_string() && !d.str.empty()) m->dependencies.push_back(sanitize_id(d.str));
        }

        auto parse_patches = [&](const char* field, bool is_rom) {
            const json::Value* arr = root.get(field);
            if (!arr || !arr->is_array()) return;
            for (const json::Value& p : arr->arr) {
                if (!p.is_object()) continue;
                uint32_t addr = 0;
                const char* addr_key = is_rom ? "offset" : "addr";
                if (const json::Value* av = p.get(addr_key)) {
                    if (av->is_number()) addr = uint32_t(av->number);
                    else if (av->is_string()) {
                        if (!parse_hex_u32(av->str, addr)) continue;
                    } else continue;
                } else continue;
                std::vector<uint8_t> bytes;
                if (!parse_hex_bytes(p.string_or("bytes"), bytes)) continue;
                if (is_rom) m->rom_patches.push_back({addr, std::move(bytes), p.string_or("region")});
                else m->ram_patches.push_back({addr, std::move(bytes), p.bool_or("freeze", false), p.string_or("region")});
            }
        };
        parse_patches("patches", false);
        parse_patches("rom_patches", true);

        if (m->compatible && !m->regions.empty()) {
            m->compatible = std::find(m->regions.begin(), m->regions.end(), game_code) != m->regions.end();
            if (!m->compatible) m->error = "incompatible region (requires " + m->regions.front() + ")";
        }

        if (auto it = impl_->saved_enabled.find(m->id); it != impl_->saved_enabled.end())
            m->enabled = it->second;
        else
            m->enabled = m->enabled_by_default;

        impl_->load_mod_config(*m);
        impl_->mods.push_back(std::move(m));
    }

    std::sort(impl_->mods.begin(), impl_->mods.end(), [](const auto& a, const auto& b) {
        if (a->priority != b->priority) return a->priority < b->priority;
        return a->id < b->id;
    });

    // Check manifest dependencies.
    std::set<std::string> active_ids;
    for (const auto& m : impl_->mods)
        if (m->enabled && m->compatible) active_ids.insert(m->id);
    for (auto& m : impl_->mods) {
        if (!m->enabled || !m->compatible) continue;
        for (const std::string& dep : m->dependencies) {
            if (!active_ids.count(dep)) {
                m->compatible = false;
                m->error = "missing dependency '" + dep + "'";
                std::fprintf(stderr, "mod [%s]: %s\n", m->id.c_str(), m->error.c_str());
                break;
            }
        }
    }

    impl_->publish_snapshots();
}

std::vector<RomModInput> ModManager::rom_mod_inputs() const {
    std::vector<RomModInput> out;
    for (const auto& m : impl_->mods) {
        if (!m->enabled || !m->compatible) continue;
        if (m->has_overlay || !m->rom_patches.empty()) out.push_back({m->id, m->dir, m->rom_patches});
    }
    return out;
}

void ModManager::set_rom_view(const std::vector<uint8_t>* rom_bytes, std::vector<NitroFileEntry> nitro_files) {
    impl_->rom_bytes = rom_bytes;
    impl_->nitro_files = std::move(nitro_files);
    impl_->nitro_by_path.clear();
    impl_->nitro_by_lower.clear();
    for (size_t i = 0; i < impl_->nitro_files.size(); i++) {
        const std::string& p = impl_->nitro_files[i].path;
        impl_->nitro_by_path[p] = i;
        std::string lo = p;
        for (char& c : lo) c = char(std::tolower(static_cast<unsigned char>(c)));
        impl_->nitro_by_lower[lo] = i;
    }
}

void ModManager::set_callbacks(ModHostCallbacks cb) { impl_->cb = std::move(cb); }
void ModManager::set_save_blob_dir(const fs::path& dir) { impl_->save_blob_dir = dir; }
void ModManager::attach_runtime(ntr::Runtime* rt) { impl_->rt = rt; }

int ModManager::load_all(GlPresenter* gl) {
    Impl::s_active_impl = impl_.get();
    if (gl) impl_->gl_presenter = gl;
    impl_->reload_generation++;
    impl_->init_order.clear();
    impl_->register_builtin_services();

    const fs::path native_staging = impl_->mods_dir / ".cache" / "_native";
    std::vector<LoadedMod*> native_candidates;

    // Phase 1: Stage & open all enabled Native C++ DLLs/.so and register their service exports.
    for (auto& m : impl_->mods) {
        if (!m->enabled || !m->compatible) continue;
        m->faulted = false;
        m->error.clear();
        if (m->kind != ModKind::Native) continue;

        fs::path staged;
        std::string err;
        if (!stage_native_library(m->native_lib, native_staging, m->id, impl_->reload_generation, staged, err)) {
            impl_->fault_mod(*m, err);
            continue;
        }
        m->native_dir = staged.parent_path();
        m->native_dir_str = m->native_dir.string();

        m->dynlib = dynlib_open(staged, err);
        if (!m->dynlib) {
            impl_->fault_mod(*m, err);
            continue;
        }
        auto get_desc = reinterpret_cast<PhModDescriptorFn>(dynlib_sym(m->dynlib, PH_MOD_DESCRIPTOR_SYMBOL));
        if (!get_desc) {
            impl_->fault_mod(*m, "missing ph_mod_descriptor export (did you forget DEFINE_MOD()?)");
            dynlib_close(m->dynlib); m->dynlib = nullptr;
            continue;
        }
        m->desc = get_desc();
        if (!m->desc || m->desc->abi_version != PH_MOD_SDK_ABI_VERSION) {
            impl_->fault_mod(*m, "incompatible native mod SDK ABI version");
            dynlib_close(m->dynlib); m->dynlib = nullptr; m->desc = nullptr;
            continue;
        }

        // Resolve lifecycle callbacks from descriptor OR standard C exports (`mod_initialize`, etc.).
        m->fn_init = m->desc->initialize ? m->desc->initialize
                                         : reinterpret_cast<ModResult (*)(ModError*)>(dynlib_sym(m->dynlib, "mod_initialize"));
        m->fn_update = m->desc->update ? m->desc->update
                                       : reinterpret_cast<ModResult (*)(ModError*)>(dynlib_sym(m->dynlib, "mod_update"));
        m->fn_input = m->desc->on_input ? m->desc->on_input
                                        : reinterpret_cast<ModResult (*)(PhInputState*, ModError*)>(dynlib_sym(m->dynlib, "mod_input"));
        m->fn_draw = m->desc->on_draw ? m->desc->on_draw
                                      : reinterpret_cast<ModResult (*)(const PhDrawContext*, bool*, ModError*)>(dynlib_sym(m->dynlib, "mod_draw"));
        m->fn_post_draw = m->desc->on_post_draw ? m->desc->on_post_draw
                                                : reinterpret_cast<ModResult (*)(const PhDrawContext*, ModError*)>(dynlib_sym(m->dynlib, "mod_post_draw"));
        m->fn_shutdown = m->desc->shutdown ? m->desc->shutdown
                                           : reinterpret_cast<ModResult (*)(void)>(dynlib_sym(m->dynlib, "mod_shutdown"));

        if (m->desc->ctx_slot) *m->desc->ctx_slot = impl_->context_for(m.get());

        // Register exported services (detect duplicate (id, major) conflicts!).
        bool conflict = false;
        for (size_t i = 0; i < m->desc->export_count; i++) {
            const ModServiceExport& exp = m->desc->exports[i];
            if (!exp.id) continue;
            ServiceKey key{exp.id, exp.major_version};
            if (impl_->services.count(key)) {
                impl_->fault_mod(*m, std::string("duplicate service export '") + exp.id + "' v" +
                                         std::to_string(exp.major_version));
                conflict = true;
                break;
            }
            impl_->services[key] = {exp.minor_version, exp.service, m.get(), exp.service == nullptr};
        }
        if (conflict) {
            impl_->shutdown_single_mod(*m);
            continue;
        }
        native_candidates.push_back(m.get());
    }

    // Phase 2: Build dependency graph between native mods from their service imports (Dusklight contract!).
    std::unordered_map<LoadedMod*, std::set<LoadedMod*>> req_deps, opt_deps;
    for (LoadedMod* m : native_candidates) {
        if (m->faulted) continue;
        for (size_t i = 0; i < m->desc->import_count; i++) {
            const ModServiceImport& imp = m->desc->imports[i];
            if (!imp.id) continue;
            const auto it = impl_->services.find({imp.id, imp.major_version});
            if (it == impl_->services.end() || it->second.minor < imp.min_minor_version) {
                if (!imp.optional) {
                    impl_->fault_mod(*m, std::string("missing required service '") + imp.id + "' v" +
                                             std::to_string(imp.major_version) + "." + std::to_string(imp.min_minor_version));
                    impl_->shutdown_single_mod(*m);
                    break;
                }
                continue;
            }
            if (it->second.provider && it->second.provider != m) {
                if (imp.optional) opt_deps[m].insert(it->second.provider);
                else req_deps[m].insert(it->second.provider);
            }
        }
    }

    // Topological sort respecting required + optional dependencies (breaking optional-only cycles cleanly).
    std::vector<LoadedMod*> topo_native;
    {
        std::set<LoadedMod*> alive;
        for (LoadedMod* m : native_candidates) if (!m->faulted) alive.insert(m);
        std::map<LoadedMod*, int> indeg;
        std::map<LoadedMod*, std::vector<LoadedMod*>> adj;
        for (LoadedMod* m : alive) indeg[m] = 0;
        for (LoadedMod* m : alive) {
            for (LoadedMod* dep : req_deps[m]) {
                if (alive.count(dep)) { adj[dep].push_back(m); indeg[m]++; }
            }
            for (LoadedMod* dep : opt_deps[m]) {
                if (alive.count(dep) && !req_deps[m].count(dep)) { adj[dep].push_back(m); indeg[m]++; }
            }
        }
        std::vector<LoadedMod*> ready;
        for (LoadedMod* m : native_candidates)
            if (alive.count(m) && indeg[m] == 0) ready.push_back(m);
        while (!ready.empty()) {
            LoadedMod* cur = ready.front();
            ready.erase(ready.begin());
            topo_native.push_back(cur);
            for (LoadedMod* nxt : adj[cur]) {
                if (--indeg[nxt] == 0) ready.push_back(nxt);
            }
        }
        // Any remaining mods are in a cycle: check if it's a required-only cycle (error!) or optional cycle (break).
        if (topo_native.size() < alive.size()) {
            std::set<LoadedMod*> placed(topo_native.begin(), topo_native.end());
            std::vector<LoadedMod*> remaining;
            for (LoadedMod* m : native_candidates) if (alive.count(m) && !placed.count(m)) remaining.push_back(m);
            // Retry with required deps only.
            std::map<LoadedMod*, int> rindeg;
            std::map<LoadedMod*, std::vector<LoadedMod*>> radj;
            for (LoadedMod* m : remaining) rindeg[m] = 0;
            for (LoadedMod* m : remaining)
                for (LoadedMod* dep : req_deps[m])
                    if (rindeg.count(dep)) { radj[dep].push_back(m); rindeg[m]++; }
            std::vector<LoadedMod*> rready;
            for (LoadedMod* m : remaining) if (rindeg[m] == 0) rready.push_back(m);
            while (!rready.empty()) {
                LoadedMod* cur = rready.front(); rready.erase(rready.begin());
                topo_native.push_back(cur);
                placed.insert(cur);
                for (LoadedMod* nxt : radj[cur]) if (--rindeg[nxt] == 0) rready.push_back(nxt);
            }
            for (LoadedMod* m : remaining) {
                if (!placed.count(m)) {
                    impl_->fault_mod(*m, "circular required service dependency");
                    impl_->shutdown_single_mod(*m);
                }
            }
        }
    }

    // Phase 3: Resolve imports & call mod_initialize in topological order (so deferred services are published in time!).
    int count = 0;
    for (LoadedMod* m : topo_native) {
        if (m->faulted) continue;
        bool imports_ok = true;
        for (size_t i = 0; i < m->desc->import_count; i++) {
            const ModServiceImport& imp = m->desc->imports[i];
            if (!imp.id || !imp.target_slot) continue;
            const auto it = impl_->services.find({imp.id, imp.major_version});
            if (it == impl_->services.end() || !it->second.ptr || it->second.minor < imp.min_minor_version) {
                if (imp.optional) {
                    *imp.target_slot = nullptr;
                } else {
                    impl_->fault_mod(*m, std::string("required service unavailable: '") + imp.id + "'");
                    impl_->shutdown_single_mod(*m);
                    imports_ok = false;
                    break;
                }
            } else {
                *imp.target_slot = it->second.ptr;
            }
        }
        if (!imports_ok) continue;

        m->loaded = true;
        if (m->fn_init) {
            ModError err{};
            ModResult r = MOD_OK;
            try {
                r = m->fn_init(&err);
            } catch (const std::exception& e) {
                r = MOD_ERROR;
                std::snprintf(err.message, sizeof err.message, "C++ exception: %s", e.what());
            } catch (...) {
                r = MOD_ERROR;
                std::snprintf(err.message, sizeof err.message, "unknown C++ exception in mod_initialize");
            }
            if (r != MOD_OK || m->faulted) {
                impl_->fault_mod(*m, err.message[0] ? err.message : "mod_initialize failed");
                impl_->shutdown_single_mod(*m);
                continue;
            }
        }
        m->initialized = true;
        impl_->init_order.push_back(m);
        impl_->fire_lifecycle(m, MOD_LIFECYCLE_ATTACHED);
        count++;
        std::fprintf(stderr, "mod: loaded native '%s' v%s (%s)\n", m->name.c_str(), m->version.c_str(), m->id.c_str());
    }

    // Phase 4: Load Asset-only and Lua 5.4 script mods.
    for (auto& m : impl_->mods) {
        if (!m->enabled || !m->compatible || m->kind == ModKind::Native) continue;
        m->faulted = false;
        m->error.clear();

        if (m->kind == ModKind::AssetOnly) {
            m->loaded = true;
            m->initialized = true;
            impl_->init_order.push_back(m.get());
            count++;
            std::fprintf(stderr, "mod: loaded asset mod '%s' v%s (%s)\n", m->name.c_str(), m->version.c_str(), m->id.c_str());
            continue;
        }

        const fs::path script_path = Impl::find_lua_entry(m->dir, m->entry);
        if (script_path.empty()) {
            impl_->fault_mod(*m, "Lua entry script not found (" + (m->entry.empty() ? "res/main.lua" : m->entry) + ")");
            continue;
        }
        m->L = luaL_newstate();
        if (!m->L) { impl_->fault_mod(*m, "failed to allocate Lua VM"); continue; }
        impl_->register_ph_api(*m);

        const std::string src = read_text_file(script_path);
        const std::string chunk = "@" + m->id + "/" + script_path.filename().string();
        if (luaL_loadbufferx(m->L, src.data(), src.size(), chunk.c_str(), "t") != LUA_OK) {
            const char* err = lua_tostring(m->L, -1);
            impl_->fault_mod(*m, err ? err : "syntax error");
            impl_->shutdown_single_mod(*m);
            continue;
        }
        if (!impl_->call_lua(*m, 0, 0)) {
            impl_->shutdown_single_mod(*m);
            continue;
        }
        m->loaded = true;
        m->initialized = true;
        impl_->init_order.push_back(m.get());
        count++;
        std::fprintf(stderr, "mod: loaded lua '%s' v%s (%s)\n", m->name.c_str(), m->version.c_str(), m->id.c_str());
    }

    // Phase 5: Fire Lua "init" hooks after all mods are loaded so inter-mod ph.bus works cleanly!
    for (LoadedMod* m : impl_->init_order) {
        if (m->kind != ModKind::Lua || !m->loaded || !m->L || m->faulted) continue;
        for (size_t i = 0; i < m->handlers.size(); i++) {
            if (m->handlers[i].event != "init") continue;
            lua_rawgeti(m->L, LUA_REGISTRYINDEX, m->handlers[i].lua_ref);
            impl_->call_lua(*m, 0, 0);
            if (m->faulted) break;
        }
    }

    impl_->publish_snapshots();
    return count;
}

void ModManager::shutdown_all(GlPresenter* gl) {
    if (gl) impl_->gl_presenter = gl;
    // Shut down in REVERSE initialization order (Dusklight contract: providers stay alive while dependents tear down!).
    for (auto it = impl_->init_order.rbegin(); it != impl_->init_order.rend(); ++it) {
        impl_->shutdown_single_mod(**it);
    }
    for (auto& m : impl_->mods) {
        if (m->dynlib || m->L || m->loaded) impl_->shutdown_single_mod(*m);
    }
    impl_->init_order.clear();
    if (impl_->rt) impl_->rt->clear_function_hooks();
    impl_->hook_records.clear();
    if (GlPresenter* g = gl ? gl : impl_->gl_presenter) g->clear_mod_images();
    if (gl) impl_->gl_presenter = nullptr;
    impl_->frame_touch_rects.clear();
    impl_->publish_touch_rects();
    impl_->publish_snapshots();
}

int ModManager::reload_all(GlPresenter* gl) {
    GlPresenter* keep_gl = gl ? gl : impl_->gl_presenter;
    shutdown_all(keep_gl);
    discover(mods_dir_, game_code_, impl_->dev_mode);
    return load_all(keep_gl);
}

void ModManager::set_input_snapshot(const ModInputState& input) {
    impl_->cached_input = input;
    impl_->input_ctx = &impl_->cached_input;
}

void ModManager::on_frame(uint64_t frame_num, float fps, double dt) {
    Impl::s_active_impl = impl_.get();
    impl_->frame_num = frame_num;
    impl_->fps = fps;
    impl_->dt = dt;
    for (LoadedMod* m : impl_->init_order) {
        if (!m->loaded || m->faulted) continue;
        if (!m->startup_patches_applied) { impl_->apply_ram_patches(*m, false); m->startup_patches_applied = true; }
        else impl_->apply_ram_patches(*m, true);

        if (m->kind == ModKind::Native && m->fn_update) {
            ModError err{};
            ModResult r = MOD_OK;
            try { r = m->fn_update(&err); }
            catch (const std::exception& e) { r = MOD_ERROR; std::snprintf(err.message, sizeof err.message, "%s", e.what()); }
            catch (...) { r = MOD_ERROR; std::snprintf(err.message, sizeof err.message, "C++ exception in mod_update"); }
            if (r != MOD_OK) impl_->fault_mod(*m, err.message[0] ? err.message : "mod_update failed");
        } else if (m->kind == ModKind::Lua && m->L) {
            impl_->update_ph_globals(*m);
            for (size_t i = 0; i < m->handlers.size(); i++) {
                if (m->handlers[i].event != "frame") continue;
                lua_rawgeti(m->L, LUA_REGISTRYINDEX, m->handlers[i].lua_ref);
                lua_pushinteger(m->L, lua_Integer(frame_num));
                lua_pushnumber(m->L, dt);
                impl_->call_lua(*m, 2, 0);
                if (m->faulted) break;
            }
        }
    }
}

void ModManager::on_input(ModInputState& input) {
    Impl::s_active_impl = impl_.get();
    impl_->input_ctx = &input;
    for (LoadedMod* m : impl_->init_order) {
        if (!m->loaded || m->faulted) continue;
        if (m->kind == ModKind::Native && m->fn_input) {
            PhInputState st{};
            Impl::c_input_get_state(impl_->context_for(m), &st);
            ModError err{};
            ModResult r = MOD_OK;
            try { r = m->fn_input(&st, &err); }
            catch (...) { r = MOD_ERROR; }
            if (r != MOD_OK) {
                impl_->fault_mod(*m, err.message[0] ? err.message : "mod_input failed");
                continue;
            }
            input.held = st.held; input.pressed = st.pressed;
            input.touching = st.touching; input.tx = st.tx; input.ty = st.ty;
            input.blow = st.blow; input.blow_sample = st.blow_sample; input.lid = st.lid;
        } else if (m->kind == ModKind::Lua && m->L) {
            for (size_t i = 0; i < m->handlers.size(); i++) {
                if (m->handlers[i].event != "input") continue;
                lua_State* L = m->L;
                lua_rawgeti(L, LUA_REGISTRYINDEX, m->handlers[i].lua_ref);
                lua_createtable(L, 0, 17);
                lua_pushinteger(L, input.held); lua_setfield(L, -2, "held");
                lua_pushinteger(L, input.pressed); lua_setfield(L, -2, "pressed");
                lua_pushboolean(L, input.touching ? 1 : 0); lua_setfield(L, -2, "touching");
                lua_pushinteger(L, input.tx); lua_setfield(L, -2, "tx");
                lua_pushinteger(L, input.ty); lua_setfield(L, -2, "ty");
                lua_pushboolean(L, input.blow ? 1 : 0); lua_setfield(L, -2, "blow");
                lua_pushboolean(L, input.blow_sample ? 1 : 0); lua_setfield(L, -2, "blow_sample");
                lua_pushboolean(L, input.lid ? 1 : 0); lua_setfield(L, -2, "lid");
                lua_pushboolean(L, input.stick_on ? 1 : 0); lua_setfield(L, -2, "stick_on");
                lua_pushnumber(L, input.stick_x); lua_setfield(L, -2, "stick_x");
                lua_pushnumber(L, input.stick_y); lua_setfield(L, -2, "stick_y");
                lua_pushnumber(L, input.pointer_x); lua_setfield(L, -2, "mouse_x");
                lua_pushnumber(L, input.pointer_y); lua_setfield(L, -2, "mouse_y");
                lua_pushboolean(L, input.pointer_down ? 1 : 0); lua_setfield(L, -2, "mouse_down");
                lua_pushboolean(L, input.pointer_just_pressed ? 1 : 0); lua_setfield(L, -2, "mouse_clicked");
                lua_pushinteger(L, input.win_w); lua_setfield(L, -2, "win_w");
                lua_pushinteger(L, input.win_h); lua_setfield(L, -2, "win_h");
                lua_pushvalue(L, -1);
                const int tbl_ref = luaL_ref(L, LUA_REGISTRYINDEX);
                if (impl_->call_lua(*m, 1, 1)) {
                    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, LUA_REGISTRYINDEX, tbl_ref); }
                    if (lua_istable(L, -1)) {
                        lua_getfield(L, -1, "held"); if (lua_isinteger(L, -1)) input.held = uint32_t(lua_tointeger(L, -1)); lua_pop(L, 1);
                        lua_getfield(L, -1, "pressed"); if (lua_isinteger(L, -1)) input.pressed = uint32_t(lua_tointeger(L, -1)); lua_pop(L, 1);
                        lua_getfield(L, -1, "touching"); if (lua_isboolean(L, -1)) input.touching = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
                        lua_getfield(L, -1, "tx"); if (lua_isnumber(L, -1)) input.tx = int(lua_tonumber(L, -1)); lua_pop(L, 1);
                        lua_getfield(L, -1, "ty"); if (lua_isnumber(L, -1)) input.ty = int(lua_tonumber(L, -1)); lua_pop(L, 1);
                        lua_getfield(L, -1, "blow"); if (lua_isboolean(L, -1)) input.blow = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
                        lua_getfield(L, -1, "blow_sample"); if (lua_isboolean(L, -1)) input.blow_sample = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
                        lua_getfield(L, -1, "lid"); if (lua_isboolean(L, -1)) input.lid = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
                    }
                    lua_pop(L, 1);
                }
                luaL_unref(L, LUA_REGISTRYINDEX, tbl_ref);
                if (m->faulted) break;
            }
        }
    }
}

namespace {
PhDrawContext to_c_draw_ctx(const ModDrawContext& ctx) {
    PhDrawContext d{};
    d.struct_size = sizeof(PhDrawContext);
    d.win_w = ctx.win_w;
    d.win_h = ctx.win_h;
    d.ui_scale = ctx.ui_scale;
    d.smooth = ctx.smooth;
    d.wide = ctx.wide;
    d.stretch = ctx.stretch;
    d.layout = int(ctx.layout);
    d.wide_extra = ctx.wide_extra;
    d.wide_layer = ctx.wide_layer;
    d.default_views.top_shown = ctx.default_views.top_shown;
    d.default_views.bottom_shown = ctx.default_views.bottom_shown;
    d.default_views.top = {ctx.default_views.top.x, ctx.default_views.top.y, ctx.default_views.top.w, ctx.default_views.top.h};
    d.default_views.bottom = {ctx.default_views.bottom.x, ctx.default_views.bottom.y, ctx.default_views.bottom.w, ctx.default_views.bottom.h};
    return d;
}
}  // namespace

bool ModManager::on_draw(const ModDrawContext& ctx) {
    Impl::s_active_impl = impl_.get();
    impl_->draw_ctx = &ctx;
    impl_->frame_touch_rects.clear();
    bool custom_drawn = false;
    const PhDrawContext c_ctx = to_c_draw_ctx(ctx);

    for (LoadedMod* m : impl_->init_order) {
        if (!m->loaded || m->faulted) continue;
        if (m->kind == ModKind::Native && m->fn_draw) {
            bool handled = false;
            ModError err{};
            ModResult r = MOD_OK;
            try { r = m->fn_draw(&c_ctx, &handled, &err); }
            catch (...) { r = MOD_ERROR; }
            if (r != MOD_OK) impl_->fault_mod(*m, err.message[0] ? err.message : "mod_draw failed");
            else if (handled) custom_drawn = true;
            if (ctx.gl) ctx.gl->clear_clip();
        } else if (m->kind == ModKind::Lua && m->L) {
            for (size_t i = 0; i < m->handlers.size(); i++) {
                if (m->handlers[i].event != "draw") continue;
                lua_State* L = m->L;
                lua_rawgeti(L, LUA_REGISTRYINDEX, m->handlers[i].lua_ref);
                lua_createtable(L, 0, 10);
                lua_pushinteger(L, ctx.win_w); lua_setfield(L, -2, "win_w");
                lua_pushinteger(L, ctx.win_h); lua_setfield(L, -2, "win_h");
                lua_pushnumber(L, ctx.ui_scale); lua_setfield(L, -2, "ui_scale");
                lua_pushboolean(L, ctx.smooth ? 1 : 0); lua_setfield(L, -2, "smooth");
                lua_pushboolean(L, ctx.wide ? 1 : 0); lua_setfield(L, -2, "wide");
                lua_pushboolean(L, ctx.stretch ? 1 : 0); lua_setfield(L, -2, "stretch");
                lua_pushinteger(L, ctx.wide_extra); lua_setfield(L, -2, "wide_extra");
                lua_pushinteger(L, ctx.wide_layer); lua_setfield(L, -2, "wide_layer");
                lua_pushstring(L, layout_name(ctx.layout)); lua_setfield(L, -2, "layout");
                push_views(L, ctx.default_views); lua_setfield(L, -2, "default_views");
                if (impl_->call_lua(*m, 1, 1)) {
                    if (lua_toboolean(L, -1)) custom_drawn = true;
                    lua_pop(L, 1);
                }
                if (ctx.gl) ctx.gl->clear_clip();
                if (m->faulted) break;
            }
        }
    }
    return custom_drawn;
}

void ModManager::on_post_draw(const ModDrawContext& ctx) {
    Impl::s_active_impl = impl_.get();
    impl_->draw_ctx = &ctx;
    const PhDrawContext c_ctx = to_c_draw_ctx(ctx);
    for (LoadedMod* m : impl_->init_order) {
        if (!m->loaded || m->faulted) continue;
        if (m->kind == ModKind::Native && m->fn_post_draw) {
            ModError err{};
            ModResult r = MOD_OK;
            try { r = m->fn_post_draw(&c_ctx, &err); }
            catch (...) { r = MOD_ERROR; }
            if (r != MOD_OK) impl_->fault_mod(*m, err.message[0] ? err.message : "mod_post_draw failed");
            if (ctx.gl) ctx.gl->clear_clip();
        } else if (m->kind == ModKind::Lua && m->L) {
            for (size_t i = 0; i < m->handlers.size(); i++) {
                if (m->handlers[i].event != "post_draw") continue;
                lua_State* L = m->L;
                lua_rawgeti(L, LUA_REGISTRYINDEX, m->handlers[i].lua_ref);
                lua_createtable(L, 0, 10);
                lua_pushinteger(L, ctx.win_w); lua_setfield(L, -2, "win_w");
                lua_pushinteger(L, ctx.win_h); lua_setfield(L, -2, "win_h");
                lua_pushnumber(L, ctx.ui_scale); lua_setfield(L, -2, "ui_scale");
                lua_pushboolean(L, ctx.smooth ? 1 : 0); lua_setfield(L, -2, "smooth");
                lua_pushboolean(L, ctx.wide ? 1 : 0); lua_setfield(L, -2, "wide");
                lua_pushboolean(L, ctx.stretch ? 1 : 0); lua_setfield(L, -2, "stretch");
                lua_pushinteger(L, ctx.wide_extra); lua_setfield(L, -2, "wide_extra");
                lua_pushinteger(L, ctx.wide_layer); lua_setfield(L, -2, "wide_layer");
                lua_pushstring(L, layout_name(ctx.layout)); lua_setfield(L, -2, "layout");
                push_views(L, ctx.default_views); lua_setfield(L, -2, "default_views");
                impl_->call_lua(*m, 1, 0);
                if (ctx.gl) ctx.gl->clear_clip();
                if (m->faulted) break;
            }
        }
    }
    impl_->publish_touch_rects();
    impl_->draw_ctx = nullptr;
}

void ModManager::on_cartridge_save() {
    const auto obs = impl_->save_observers;
    for (const SaveObserver& o : obs)
        if (o.cb && o.owner && o.owner->loaded && !o.owner->faulted)
            o.cb(impl_->context_for(o.owner), PH_SAVE_EVENT_CARTRIDGE_WRITTEN, 0, o.user_data);
}

void ModManager::on_save_state(int slot) {
    const auto obs = impl_->save_observers;
    for (const SaveObserver& o : obs)
        if (o.cb && o.owner && o.owner->loaded && !o.owner->faulted)
            o.cb(impl_->context_for(o.owner), PH_SAVE_EVENT_STATE_SAVED, slot, o.user_data);
    for (LoadedMod* m : impl_->init_order) {
        if (m->kind != ModKind::Lua || !m->loaded || !m->L || m->faulted) continue;
        for (size_t i = 0; i < m->handlers.size(); i++) {
            if (m->handlers[i].event != "save_state") continue;
            lua_rawgeti(m->L, LUA_REGISTRYINDEX, m->handlers[i].lua_ref);
            lua_pushinteger(m->L, slot);
            impl_->call_lua(*m, 1, 0);
            if (m->faulted) break;
        }
    }
}

void ModManager::on_load_state(int slot, bool ok) {
    if (ok) {
        const auto obs = impl_->save_observers;
        for (const SaveObserver& o : obs)
            if (o.cb && o.owner && o.owner->loaded && !o.owner->faulted)
                o.cb(impl_->context_for(o.owner), PH_SAVE_EVENT_STATE_LOADED, slot, o.user_data);
    }
    for (LoadedMod* m : impl_->init_order) {
        if (m->kind != ModKind::Lua || !m->loaded || !m->L || m->faulted) continue;
        for (size_t i = 0; i < m->handlers.size(); i++) {
            if (m->handlers[i].event != "load_state") continue;
            lua_rawgeti(m->L, LUA_REGISTRYINDEX, m->handlers[i].lua_ref);
            lua_pushinteger(m->L, slot);
            lua_pushboolean(m->L, ok ? 1 : 0);
            impl_->call_lua(*m, 2, 0);
            if (m->faulted) break;
        }
    }
}

std::vector<ModSummary> ModManager::summaries() const {
    std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
    return impl_->published_summaries;
}

std::vector<ModTouchRect> ModManager::touch_rects() const {
    std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
    return impl_->published_touch_rects;
}

bool ModManager::has_loaded_mods() const {
    std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
    for (const ModSummary& s : impl_->published_summaries)
        if (s.loaded) return true;
    return false;
}

bool ModManager::set_mod_enabled(const std::string& id, bool enabled) {
    for (auto& m : impl_->mods) {
        if (m->id != id) continue;
        m->enabled = enabled;
        impl_->saved_enabled[id] = enabled;
        impl_->save_mods_ini();
        impl_->publish_snapshots();
        return true;
    }
    return false;
}

bool ModManager::step_mod_config(const std::string& id, const std::string& key, int dir) {
    for (auto& m : impl_->mods) {
        if (m->id != id) continue;
        for (size_t i = 0; i < m->config_items.size(); i++) {
            ModConfigItem& item = m->config_items[i];
            if (item.key != key) continue;
            const auto prev = Impl::item_to_value(item);
            switch (item.kind) {
                case ModConfigItem::Kind::Bool:
                    item.bool_val = !item.bool_val;
                    m->config_json.obj[key] = json::Value::make_bool(item.bool_val);
                    break;
                case ModConfigItem::Kind::Int: {
                    const int64_t step = std::max<int64_t>(1, int64_t(std::llround(item.step)));
                    item.int_val = std::clamp<int64_t>(item.int_val + int64_t(dir) * step,
                                                       int64_t(item.min_val), int64_t(item.max_val));
                    m->config_json.obj[key] = json::Value::make_number(double(item.int_val));
                    break;
                }
                case ModConfigItem::Kind::Float:
                    item.float_val = std::clamp(item.float_val + double(dir) * item.step, item.min_val, item.max_val);
                    item.float_val = std::round(item.float_val * 1000.0) / 1000.0;
                    m->config_json.obj[key] = json::Value::make_number(item.float_val);
                    break;
                case ModConfigItem::Kind::Choice:
                    if (!item.choices.empty()) {
                        const int64_t n = int64_t(item.choices.size());
                        item.int_val = (item.int_val + dir + n * 4) % n;
                        m->config_json.obj[key] = json::Value::make_string(item.choices[size_t(item.int_val)]);
                    }
                    break;
                case ModConfigItem::Kind::String:
                case ModConfigItem::Kind::Hotkey:
                    return false;
            }
            impl_->save_mod_config(*m);
            impl_->notify_config_change(*m, i, prev, Impl::item_to_value(item));
            impl_->publish_snapshots();
            return true;
        }
    }
    return false;
}

bool ModManager::set_mod_config_string(const std::string& id, const std::string& key, const std::string& value) {
    for (auto& m : impl_->mods) {
        if (m->id != id) continue;
        for (size_t i = 0; i < m->config_items.size(); i++) {
            ModConfigItem& item = m->config_items[i];
            if (item.key != key) continue;
            if (item.kind != ModConfigItem::Kind::String && item.kind != ModConfigItem::Kind::Hotkey) return false;
            const std::string prev_str = item.string_val;
            ConfigVarValue prev{CONFIG_VAR_STRING, false, 0, 0.0, prev_str.c_str()};
            item.string_val = value;
            m->config_json.obj[key] = json::Value::make_string(value);
            impl_->save_mod_config(*m);
            impl_->notify_config_change(*m, i, prev, Impl::item_to_value(item));
            impl_->publish_snapshots();
            return true;
        }
    }
    return false;
}

}  // namespace ph
