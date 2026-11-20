// Comprehensive unit tests for the 3-tier Dusklight-style Mod System:
//   1. JSON parser & serializer
//   2. .phmod ZIP bundle extraction + Native C++ DLL mods + custom inter-mod Service Export/Import
//      + topological dependency ordering + HookService (Pre/Post/Replace/call_original)
//      + ConfigService subscriptions + SaveService binary blobs + ResourceService + Hot-Reload
//   3. Lua 5.4 script mods (global `ph.*` + `require("ph.*")`, ARM9 hooks, sandbox, watchdog, error isolation, ph.bus)
//   4. Asset-only mods (`overlay/` and `romfs/` NitroFS overrides, in-place + expanded FAT append)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "mod_json.hpp"
#include "mod_manager.hpp"
#include "mod_romfs.hpp"
#include "ntr/rom.hpp"
#include "ntr/runtime.hpp"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

// Stub GlPresenter methods referenced by mod_manager.cpp (headless unit test without GL/SDL).
#include "gl_present.hpp"
namespace ph {
void GlPresenter::screen_ex(const ntr::Frame&, int, const Rect&, float, float, float, float, bool, bool, float, uint32_t) {}
void GlPresenter::draw_hd_text(int, const Rect&, int, const HudMove*, int) {}
void GlPresenter::fill(const Rect&, uint32_t) {}
void GlPresenter::outline(const Rect&, float, uint32_t) {}
void GlPresenter::set_clip(const Rect&) {}
void GlPresenter::clear_clip() {}
float GlPresenter::text(float, float, float, const std::string&, uint32_t) { return 0.0f; }
int GlPresenter::create_image(const uint32_t*, int, int, bool) { return 0; }
void GlPresenter::destroy_image(int) {}
void GlPresenter::clear_mod_images() {}
bool GlPresenter::image_size(int, int&, int&) const { return false; }
void GlPresenter::draw_image(int, const Rect&, float, float, float, float, float, uint32_t) {}
}  // namespace ph

namespace fs = std::filesystem;

namespace {

// Mock Runtime backed by 4 MB Main RAM, ROM buffer, and full HookService dispatch!
class MockRuntime final : public ntr::Runtime {
public:
    std::vector<uint8_t> ram = std::vector<uint8_t>(4 * 1024 * 1024, 0);
    std::vector<uint8_t> rom;
    DisplayInfo disp{};
    NtrCpu cpu{};

    struct HookItem {
        uint64_t handle;
        uint32_t addr;
        HookKind kind;
        FunctionHookCallback cb;
        void* ud;
        int prio;
    };
    uint64_t next_h = 1;
    std::vector<HookItem> hooks;
    NtrFunc active_orig = nullptr;
    uint64_t active_handle = 0;

    std::map<std::string, uint32_t> syms = {
        {"TestTargetFn", 0x02010000u},
        {"TestReplaceFn", 0x02010040u},
        {"TestLuaHookFn", 0x02010080u},
    };

    int run() override { return 0; }
    void set_buttons(uint32_t, uint32_t) override {}
    void touch(int, int) override {}
    void release_touch() override {}
    void set_video(const ntr::VideoSettings&) override {}
    ntr::VideoSettings video() const override { return {}; }
    void set_fast_cart(bool) override {}
    void set_cpu_speed(int) override {}
    unsigned render_between(double) override { return 0; }
    void request_save_state() override {}
    void request_load_state(std::vector<uint8_t>) override {}

    static bool in_ram(uint32_t a) { return a >= 0x02000000u && a < 0x02400000u; }

    uint8_t peek8(uint32_t a) const override { return in_ram(a) ? ram[a - 0x02000000u] : 0; }
    uint16_t peek16(uint32_t a) const override {
        a &= ~1u;
        if (!in_ram(a)) return 0;
        uint16_t v; std::memcpy(&v, ram.data() + (a - 0x02000000u), 2); return v;
    }
    uint32_t peek32(uint32_t a) const override {
        a &= ~3u;
        if (!in_ram(a)) return 0;
        uint32_t v; std::memcpy(&v, ram.data() + (a - 0x02000000u), 4); return v;
    }
    bool poke8(uint32_t a, uint8_t v) override { if (!in_ram(a)) return false; ram[a - 0x02000000u] = v; return true; }
    bool poke16(uint32_t a, uint16_t v) override {
        a &= ~1u; if (!in_ram(a)) return false;
        std::memcpy(ram.data() + (a - 0x02000000u), &v, 2); return true;
    }
    bool poke32(uint32_t a, uint32_t v) override {
        a &= ~3u; if (!in_ram(a)) return false;
        std::memcpy(ram.data() + (a - 0x02000000u), &v, 4); return true;
    }
    size_t read_mem(uint32_t a, uint8_t* out, size_t size) const override {
        if (!in_ram(a)) return 0;
        const size_t off = a - 0x02000000u;
        const size_t n = std::min(size, ram.size() - off);
        std::memcpy(out, ram.data() + off, n);
        return n;
    }
    size_t write_mem(uint32_t a, const uint8_t* data, size_t size) override {
        if (!in_ram(a)) return 0;
        const size_t off = a - 0x02000000u;
        const size_t n = std::min(size, ram.size() - off);
        std::memcpy(ram.data() + off, data, n);
        return n;
    }
    size_t read_rom(uint32_t offset, uint8_t* out, size_t size) const override {
        if (offset >= rom.size()) return 0;
        const size_t n = std::min(size, rom.size() - offset);
        std::memcpy(out, rom.data() + offset, n);
        return n;
    }
    DisplayInfo display_info() const override { return disp; }

    uint32_t resolve_symbol(const char* name) const override {
        if (!name) return 0;
        const auto it = syms.find(name);
        return it == syms.end() ? 0 : it->second;
    }
    const char* symbol_name(uint32_t addr) const override {
        for (const auto& [k, v] : syms) if ((v & ~1u) == (addr & ~1u)) return k.c_str();
        return nullptr;
    }
    uint64_t add_function_hook(uint32_t addr, HookKind kind, FunctionHookCallback cb, void* ud, int prio) override {
        const uint64_t h = next_h++;
        hooks.push_back({h, addr & ~1u, kind, cb, ud, prio});
        return h;
    }
    bool remove_function_hook(uint64_t handle) override {
        const auto it = std::find_if(hooks.begin(), hooks.end(), [&](const HookItem& x) { return x.handle == handle; });
        if (it == hooks.end()) return false;
        hooks.erase(it);
        return true;
    }
    void clear_function_hooks() override { hooks.clear(); }
    bool call_hook_original(uint64_t handle, NtrCpu* c) override {
        if (handle == active_handle && active_orig) { active_orig(c ? c : &cpu); return true; }
        return false;
    }
    bool call_guest_function(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*) override { return false; }

    // Simulates calling a recompiled ARM9 function through the hook dispatcher.
    uint32_t dispatch_simulated(uint32_t addr, uint32_t arg0, NtrFunc orig) {
        cpu.r[0] = arg0;
        bool skip = false;
        const HookItem* rep = nullptr;
        for (const HookItem& h : hooks) {
            if (h.addr != (addr & ~1u)) continue;
            if (h.kind == HookKind::Pre && h.cb(&cpu, addr, h.ud) == HookAction::SkipOriginal) skip = true;
            else if (h.kind == HookKind::Replace) rep = &h;
        }
        if (!skip) {
            if (rep) {
                active_handle = rep->handle;
                active_orig = orig;
                rep->cb(&cpu, addr, rep->ud);
                active_handle = 0;
                active_orig = nullptr;
            } else if (orig) {
                orig(&cpu);
            }
        }
        for (const HookItem& h : hooks) {
            if (h.addr == (addr & ~1u) && h.kind == HookKind::Post) h.cb(&cpu, addr, h.ud);
        }
        return cpu.r[0];
    }
};

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(content.data(), std::streamsize(content.size()));
    CHECK(f.good());
}

std::vector<uint8_t> make_synthetic_rom() {
    std::vector<uint8_t> rom(0x2000, 0);
    auto wr16 = [&](size_t off, uint16_t v) { std::memcpy(rom.data() + off, &v, 2); };
    auto wr32 = [&](size_t off, uint32_t v) { std::memcpy(rom.data() + off, &v, 4); };
    std::memcpy(rom.data(), "TESTROM", 7);
    std::memcpy(rom.data() + 0x0C, "AZEE", 4);
    rom[0x14] = 0;
    wr16(0x15C, 0xCF56);

    const uint32_t fnt = 0x0400;
    wr32(0x40, fnt); wr32(0x44, 0x0100);
    wr32(fnt + 0, 16); wr16(fnt + 4, 0); wr16(fnt + 6, 2);
    wr32(fnt + 8, 32); wr16(fnt + 12, 0); wr16(fnt + 14, 0xF000);
    rom[fnt + 16] = 0x80 | 4; std::memcpy(rom.data() + fnt + 17, "data", 4); wr16(fnt + 21, 0xF001); rom[fnt + 23] = 0;
    rom[fnt + 32] = 8; std::memcpy(rom.data() + fnt + 33, "test.txt", 8); rom[fnt + 41] = 0;

    const uint32_t fat = 0x0600;
    wr32(0x48, fat); wr32(0x4C, 8);
    wr32(fat + 0, 0x1000); wr32(fat + 4, 0x1005);
    std::memcpy(rom.data() + 0x1000, "HELLO", 5);
    wr32(0x80, uint32_t(rom.size()));
    return rom;
}

void orig_add_ten(NtrCpu* c) { c->r[0] += 10u; }
void orig_mul_three(NtrCpu* c) { c->r[0] *= 3u; }

void test_json() {
    ph::json::Value v;
    std::string err;
    const bool ok = ph::json::parse(R"({ // comment
        "id": "com.example.demo", "num": 42, "arr": [1, true, "hi",] })", v, err);
    CHECK(ok && err.empty());
    CHECK(v.string_or("id") == "com.example.demo");
    CHECK(v.int_or("num") == 42);
    CHECK(v.get("arr")->arr.size() == 3);
    const std::string dumped = ph::json::dump(v, false);
    ph::json::Value rt;
    CHECK(ph::json::parse(dumped, rt, err));
    CHECK(rt.string_or("id") == "com.example.demo");
    std::fprintf(stderr, "[PASS] test_json\n");
}

void test_full_three_tier_mod_system(const fs::path& root) {
    const fs::path mods_dir = root / "mods_3tier";
    std::error_code ec;
    fs::remove_all(mods_dir, ec);
    fs::create_directories(mods_dir);

    // 1. Copy the real compiled .phmod native C++ bundles (consumer sorts alphabetically BEFORE provider,
    //    proving topological service dependency sorting initializes provider BEFORE consumer!).
    const fs::path bundles_dir(PH_TEST_BUNDLES_DIR);
    fs::copy_file(bundles_dir / "test_provider_mod.phmod", mods_dir / "z_provider.phmod");
    fs::copy_file(bundles_dir / "test_consumer_mod.phmod", mods_dir / "a_consumer.phmod");

    // 2. Asset-only mod using Dusklight-style `overlay/` directory (larger replacement file -> tests FAT append!).
    write_file(mods_dir / "asset_pack" / "mod.json", R"({
        "id": "com.phrecomp.test.assets",
        "name": "Asset Overlay Pack",
        "version": "1.0.0",
        "priority": 5
    })");
    write_file(mods_dir / "asset_pack" / "overlay" / "data" / "test.txt", "OVERLAY_REPLACED_OK");

    // 3. Lua 5.4 script mod using Dusklight-style `runtime: "ph.lua@1.0"`, `res/main.lua`, `require("ph.*")`,
    //    ARM9 function hooking from Lua, SaveService blobs, sandbox checks, config, and inter-mod bus!
    write_file(mods_dir / "lua_alpha" / "mod.json", R"({
        "id": "com.phrecomp.test.lua_alpha",
        "name": "Lua Alpha",
        "version": "1.2.0",
        "runtime": "ph.lua@1.0",
        "priority": 10,
        "regions": ["AZEE"],
        "patches": [
            { "addr": "0x02000100", "bytes": "DE AD BE EF", "freeze": true }
        ]
    })");
    write_file(mods_dir / "lua_alpha" / "res" / "helper.lua", "return { magic = 1337 }\n");
    write_file(mods_dir / "lua_alpha" / "res" / "note.txt", "RES_OK");
    write_file(mods_dir / "lua_alpha" / "res" / "main.lua", R"(
        local helper = require("helper")
        local log = require("ph.logger")
        local res = require("ph.resource")
        local mem = require("ph.mem")
        local hook = require("ph.hook")
        local save = require("ph.save")
        local cfg = require("ph.config")
        assert(helper.magic == 1337)
        assert(res.exists("note.txt") and res.load("note.txt") == "RES_OK")
        -- Sandbox checks
        assert(os.execute == nil and io == nil and dofile == nil and loadfile == nil)
        assert(ph.fs.write("../escape.txt", "nope") == false)
        assert(save.set("lua_key", "SAVED_BLOB_123") == true)
        assert(save.get("lua_key") == "SAVED_BLOB_123")
        log.info("lua_alpha loaded")

        local sub_hit = 0
        local num = cfg.int("num", "Test Num", 10, 0, 100, 5)
        assert(num == 10)
        cfg.subscribe("num", function(v, prev) sub_hit = v - prev end)
        cfg.set("num", 20)
        assert(sub_hit == 10)
        cfg.set("num", 10)
        cfg.save()

        -- Hook an ARM9 function directly from Lua!
        hook.pre("TestLuaHookFn", function(cpu)
            cpu.r0 = cpu.r0 + 50
            return false
        end)

        ph.on("init", function() ph.bus.emit("ping", { v = 77 }) end)
        ph.on("frame", function()
            mem.write_u32(0x02000200, 0x11223344)
            assert(mem.u32(0x02000100) == 0xEFBEADDE)
            assert(mem.find(0x020001F0, 64, "44 33 ?? 11") == 0x02000200)
            assert(ph.rom.read("data/test.txt") == "OVERLAY_REPLACED_OK")
        end)
        ph.on("input", function(inp)
            inp.held = inp.held | ph.input.KEY.A
            inp.touching = true; inp.tx = 123; inp.ty = 45
        end)
    )");

    // 4. Second Lua mod receiving bus message + two faulty Lua mods (error + infinite loop watchdog)
    write_file(mods_dir / "lua_beta" / "mod.json", R"({
        "id": "com.phrecomp.test.lua_beta", "name": "Lua Beta", "priority": 20,
        "dependencies": ["com.phrecomp.test.lua_alpha"]
    })");
    write_file(mods_dir / "lua_beta" / "main.lua", R"(
        ph.bus.on("ping", function(p, from)
            if from == "com.phrecomp.test.lua_alpha" and p.v == 77 then ph.mem.write_u32(0x02000300, 77) end
        end)
    )");
    write_file(mods_dir / "lua_crashy" / "mod.json", R"({ "id": "crashy", "name": "Crashy", "priority": 30 })");
    write_file(mods_dir / "lua_crashy" / "main.lua", R"(ph.on("frame", function() error("boom") end))");
    write_file(mods_dir / "lua_loopy" / "mod.json", R"({ "id": "loopy", "name": "Loopy", "priority": 40 })");
    write_file(mods_dir / "lua_loopy" / "main.lua", R"(ph.on("frame", function() while true do end end))");

    MockRuntime rt;
    rt.rom = make_synthetic_rom();

    ph::ModManager mgr;
    mgr.set_save_blob_dir(root / "save_blobs");
    mgr.discover(mods_dir, "AZEE", true);

    const auto stats = ph::apply_rom_mods(rt.rom, "AZEE", mgr.rom_mod_inputs());
    CHECK(stats.files_replaced == 1 && stats.files_appended == 1);
    mgr.set_rom_view(nullptr, ph::index_rom_nitrofs(rt.rom));
    mgr.attach_runtime(&rt);

    // Total enabled mods: 2 native (.phmod) + 1 asset-only + 4 Lua = 7!
    const int loaded = mgr.load_all();
    CHECK(loaded == 7);

    // Verify native provider ran BEFORE consumer and consumer invoked provider's TestMathService(23) == 123!
    CHECK(rt.peek32(0x02000410u) == 123u);
    // Verify Lua Beta received Alpha's bus event!
    CHECK(rt.peek32(0x02000300u) == 77u);

    // Verify Native C++ HookService:
    //   TestTargetFn: pre doubles r0 (5 -> 10), orig adds 10 (10 -> 20), post adds 7 (20 -> 27)!
    CHECK(rt.dispatch_simulated(0x02010000u, 5u, &orig_add_ten) == 27u);
    //   TestReplaceFn: replace calls original (4 * 3 = 12), then adds 1000 -> 1012!
    CHECK(rt.dispatch_simulated(0x02010040u, 4u, &orig_mul_three) == 1012u);
    // Verify Lua ARM9 hook:
    //   TestLuaHookFn: Lua pre adds 50 (3 -> 53), orig adds 10 -> 63!
    CHECK(rt.dispatch_simulated(0x02010080u, 3u, &orig_add_ten) == 63u);

    // Tick frame 1: runs native mod_update + Lua frame + isolates crashy & loopy!
    mgr.on_frame(1, 60.0f, 1.0 / 60.0);
    CHECK(rt.peek32(0x02000400u) == 100u);  // provider offset
    CHECK(rt.peek32(0x02000414u) == 105u);  // consumer calling provider(5)
    CHECK(rt.peek32(0x02000200u) == 0x11223344u);
    CHECK(rt.peek32(0x02000100u) == 0xEFBEADDEu);

    // Step native ConfigService variable -> fires C++ subscribe callback live!
    CHECK(mgr.step_mod_config("com.phrecomp.test.provider", "offset", 1));  // 100 + 25 = 125
    mgr.on_frame(2, 60.0f, 1.0 / 60.0);
    CHECK(rt.peek32(0x02000400u) == 125u);
    CHECK(rt.peek32(0x02000414u) == 130u);  // consumer sees updated offset immediately!

    // Input interception test
    ph::ModInputState inp;
    mgr.on_input(inp);
    CHECK((inp.held & ntr::KeyA) != 0 && inp.touching && inp.tx == 123 && inp.ty == 45);

    // Verify crashy & loopy were isolated while the other 5 mods stay healthy.
    int healthy = 0, faulted = 0;
    for (const auto& s : mgr.summaries()) {
        if (s.id == "crashy" || s.id == "loopy") { CHECK(!s.loaded && !s.error.empty()); faulted++; }
        else { CHECK(s.loaded && s.error.empty()); healthy++; }
    }
    CHECK(healthy == 5 && faulted == 2);

    // Hot-reload test: disable crashy & loopy, reload ALL mods (including unloading & reloading native DLLs!),
    // verify provider's mod_shutdown ran AND persisted config (125) survived reload!
    CHECK(mgr.set_mod_enabled("crashy", false));
    CHECK(mgr.set_mod_enabled("loopy", false));
    const int reloaded = mgr.reload_all();
    CHECK(reloaded == 5);
    CHECK(rt.peek32(0x02000404u) == 0x55555555u);  // provider mod_shutdown ran cleanly
    CHECK(rt.peek32(0x02000410u) == 148u);         // 125 (persisted!) + 23 == 148!

    mgr.shutdown_all();
    std::fprintf(stderr, "[PASS] test_full_three_tier_mod_system (Asset + Native C++ .phmod + Lua 5.4 + Hooks + Services)\n");
}

}  // namespace

int main() {
    test_json();
    const fs::path tmp = fs::temp_directory_path() / "ph_recomp_mod_tests";
    fs::create_directories(tmp);
    test_full_three_tier_mod_system(tmp);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    std::fprintf(stderr, "ALL MOD TESTS PASSED\n");
    return 0;
}
