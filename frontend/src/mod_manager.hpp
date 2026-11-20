// Mod System for Phantom Hourglass Recomp: discovers mods in <data-dir>/mods/, applies NitroFS/ROM
// and RAM patches, and runs sandboxed Lua 5.4 scripts with full access to rendering, screen composition,
// custom touch regions, DS memory/registers, input interception, persistent config, and inter-mod events.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "layout.hpp"
#include "mod_romfs.hpp"
#include "ntr/runtime.hpp"

struct lua_State;
struct ModContext;

namespace ph {

class GlPresenter;

// A custom touch hitbox registered by a mod during on_draw / on_post_draw.
struct ModTouchRect {
    Rect rect;            // window pixels
    Rect src_ds;          // DS sub-rect (default 0,0,256,192)
    bool has_src = false;
    bool wide = false;    // apply widescreen HUD coordinate mapping
    bool block = false;   // true: absorb touches here without sending them to DS touch screen
    int priority = 0;     // higher priority checked first
};

// A mod-defined setting registered via `ph.config.*` or `ConfigService`, exposed in menus and saved to config.json.
struct ModConfigItem {
    enum class Kind { Bool, Int, Float, String, Choice };
    Kind kind = Kind::Bool;
    std::string key;
    std::string label;
    bool bool_val = false;
    int64_t int_val = 0;
    double float_val = 0.0;
    std::string string_val;
    double min_val = 0.0;
    double max_val = 1.0;
    double step = 1.0;
    std::vector<std::string> choices;
};

enum class ModKind { AssetOnly, Native, Lua };

// Summary of one discovered mod (for the PC menu, Android UI, and `ph.bus.mods()`).
struct ModSummary {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    ModKind kind = ModKind::AssetOnly;
    bool is_archive = false;
    bool enabled = true;
    bool loaded = false;
    bool compatible = true;
    bool has_overlay = false;
    bool has_textures = false;
    std::string error;
    std::vector<ModConfigItem> config;
};

// Per-frame input state passed through `ph.on("input", fn)`.
struct ModInputState {
    uint32_t held = 0;
    uint32_t pressed = 0;
    bool touching = false;
    int tx = 0, ty = 0;
    bool blow = false;
    bool blow_sample = false;
    bool lid = false;
    bool stick_on = false;
    float stick_x = 0.0f, stick_y = 0.0f;
    // Host pointer (mouse on PC, primary finger on Android), in window pixels:
    float pointer_x = 0.0f, pointer_y = 0.0f;
    bool pointer_down = false;
    bool pointer_just_pressed = false;
    bool pointer_just_released = false;
    bool right_down = false;
    bool right_just_pressed = false;
    int win_w = 1, win_h = 1;
};

// Per-frame drawing context passed to `ph.on("draw", fn)` and `ph.on("post_draw", fn)`.
struct ModDrawContext {
    const ntr::Frame* frame = nullptr;
    GlPresenter* gl = nullptr;
    int win_w = 1, win_h = 1;
    float ui_scale = 1.0f;
    bool smooth = false;
    bool wide = false;
    bool stretch = false;
    Layout layout = Layout::Bottom;
    Views default_views{};
    int wide_extra = 0;
    int wide_layer = 1;
};

// Host callbacks so ModManager stays cleanly decoupled from SDL3 and main.cpp internals.
struct ModHostCallbacks {
    std::function<void(const std::string& msg, uint64_t ms)> notify;
    std::function<void(int slot)> save_state;
    std::function<void(int slot)> load_state;
    std::function<void(Layout layout)> set_layout;
    std::function<Layout()> get_layout;
    std::function<void(const std::string& name)> screenshot;
    std::function<void(bool on)> set_mic_blow;
    std::function<void(bool on)> set_mic_sample;
    std::function<void(bool closed)> set_lid_closed;
    std::function<bool(const std::string& key_name)> is_key_down;
    std::function<bool(const std::string& key_name)> was_key_pressed;
    // Decodes an image file (PNG/BMP) into RGBA32 pixels (top-down). Optional (null in headless tests).
    std::function<bool(const std::filesystem::path& path, std::vector<uint32_t>& rgba, int& w, int& h)> decode_image;
};

class ModManager {
public:
    ModManager();
    ~ModManager();

    ModManager(const ModManager&) = delete;
    ModManager& operator=(const ModManager&) = delete;

    // Discover manifests in `mods_dir` and read enable states from `<mods_dir>/mods.ini`.
    void discover(const std::filesystem::path& mods_dir, const std::string& game_code, bool dev_mode = false);

    // Collect enabled mods that have `romfs/` or `rom_patches`, in priority order, for apply_rom_mods().
    std::vector<RomModInput> rom_mod_inputs() const;

    // Provide a read-only view of the active (possibly patched) ROM bytes + NitroFS index for `ph.rom`.
    void set_rom_view(const std::vector<uint8_t>* rom_bytes, std::vector<NitroFileEntry> nitro_files);

    // Set host callbacks, save-blob storage directory, and attach the runtime.
    void set_callbacks(ModHostCallbacks cb);
    void set_save_blob_dir(const std::filesystem::path& dir);
    void attach_runtime(ntr::Runtime* rt);

    // Boot all enabled mods (Asset-only, Native C++ in topological service dependency order, and Lua 5.4).
    // Returns the number of successfully loaded mods.
    int load_all(GlPresenter* gl = nullptr);

    // Cleanly shut down all mods in reverse initialization order (fires shutdown, removes hooks, unloads DLLs/.so).
    void shutdown_all(GlPresenter* gl = nullptr);

    // Hot-reload: shuts down active mods, re-discovers bundles/manifests, reloads enabled mods.
    int reload_all(GlPresenter* gl = nullptr);

    // Per-frame hooks (called on the game thread).
    void set_input_snapshot(const ModInputState& input);
    void on_frame(uint64_t frame_num, float fps, double dt);
    void on_input(ModInputState& input);
    // Returns true if any mod handled drawing the DS screens (default screen draw should be skipped).
    bool on_draw(const ModDrawContext& ctx);
    void on_post_draw(const ModDrawContext& ctx);
    void on_cartridge_save();
    void on_save_state(int slot);
    void on_load_state(int slot, bool ok);

    // Thread-safe snapshots for UI & main-thread touch mapping.
    std::vector<ModSummary> summaries() const;
    std::vector<ModTouchRect> touch_rects() const;
    bool has_loaded_mods() const;

    // Toggle a mod enabled/disabled (persists mods.ini; call reload_all() on game thread to apply).
    bool set_mod_enabled(const std::string& id, bool enabled);
    // Update a mod's registered config item (saves config.json and notifies the running Lua VM).
    bool step_mod_config(const std::string& id, const std::string& key, int dir);

    const std::filesystem::path& mods_dir() const { return mods_dir_; }
    const std::string& game_code() const { return game_code_; }

private:
    struct Impl;
    friend struct ::ModContext;
    std::unique_ptr<Impl> impl_;
    std::filesystem::path mods_dir_;
    std::string game_code_;
};

}  // namespace ph
