# Modding Guide — Phantom Hourglass Recomp

Phantom Hourglass Recomp features a **Dusklight-inspired, three-tier Mod System** supporting both single-file **`.phmod` bundles** (ZIP archives) and unpacked folders, with hot-reload (`F6`) and cross-platform PC & Android support.

## Three Types of Mods

1. **Asset-only mods** (zero code): Replace game files in NitroFS (`overlay/` or `romfs/`), apply static RAM/ROM patches, or bundle resources. Works on all platforms.
2. **Native mods (C / C++ SDK)**: Dynamically linked libraries (`.dll` on Windows, `.so` on Android/Linux) communicating with the host and with each other through **individually versioned C services** (`LogService`, `HostService`, `HookService`, `GfxService`, `MemoryService`, `InputService`, `DsService`, `ConfigService`, `ResourceService`, `OverlayService`, `SaveService`). Can hook any of the game's **24,608 recompiled ARM9 functions** (`pre` / `post` / `replace`) and export custom services for other mods to import.
3. **Script mods (Lua 5.4)**: Sandboxed, cross-platform scripts requiring no compilation. Full access to rendering, screen composition, custom touch regions, DS memory/registers, input interception, ARM9 function hooks, persistent config, save blobs, and inter-mod events.

---

## Installing Mods

### PC (Windows / Linux)
- Drop `.phmod` files or unpacked mod folders into `<data-dir>/mods/` (in portable builds: `PhantomHourglass/data/mods/`).
- Press `F1` / `Esc` → **Mods** tab to enable/disable mods, adjust mod settings live, or reload (`F6`).
- CLI flags: `--mods-dir <dir>`, `--no-mods`, `--mod-dev`.

### Android
- Open the launcher → **Mods** → **Import mod (.phmod / .zip)…**.
- Toggle mods on/off from the launcher, or hot-reload scripts live from the in-game `≡` menu → **Mods** → **Reload mods**.

---

## Bundle Layout & `mod.json`

A `.phmod` bundle is a standard ZIP archive containing `mod.json` at its root (or inside a single top-level folder):

```text
my_mod.phmod   (or unpacked folder data/mods/my_mod/)
├── mod.json              # Required manifest
├── res/                  # Bundled resources (scripts, images, data, icon.png, banner.png)
├── overlay/              # Optional NitroFS file overrides (romfs/ also supported)
├── textures/             # Optional texture replacements
└── lib/                  # Optional native libraries (for Native C++ mods)
    ├── windows-x64/mod.dll
    ├── android-arm64/libmod.so
    └── linux-x64/libmod.so
```

### `mod.json`

```json
{
  "id": "com.example.my_mod",
  "name": "My Mod",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "A short description shown in the Mods menu.",
  "icon": "res/icon.png",
  "banner": "res/banner.png",
  "runtime": "ph.lua@1.0",
  "priority": 0,
  "regions": ["AZEE"],
  "dependencies": [],
  "enabled_by_default": true
}
```

- `id`: unique reverse-DNS identifier (e.g. `com.example.single_screen`).
- `runtime`:
  - `"ph.native@1.0"` — Native C/C++ mod (auto-detected if `lib/<platform>/` is present).
  - `"ph.lua@1.0"` — Lua 5.4 script mod (auto-detected if `res/main.lua` or `main.lua` is present).
  - Omitted — Asset-only mod.
- `priority`: load order (higher runs later, draws on top, and wins `overlay/` conflicts).
- `patches` / `rom_patches`: optional declarative ARM9 RAM patches (`"addr"`, `"bytes"`, `"freeze"`, `"region"`) and raw ROM patches (`"offset"`, `"bytes"`, `"region"`).

---

## 1. Asset-Only Mods

Place replacement NitroFS files under `overlay/` (or `romfs/`) matching their path inside the DS ROM:

```text
my_assets.phmod
├── mod.json
└── overlay/
    └── English/Message/common.bmg
```

- Files equal or smaller than the original overwrite in memory in place.
- Larger replacement files are automatically appended to the in-memory ROM with updated FAT entries and header CRC-16. Your original `.nds` ROM on disk is never modified.

---

## 2. Native Mods (C / C++ SDK)

Include `sdk/` in your mod's `CMakeLists.txt` and call `ph_add_mod(...)` to compile and package a `.phmod` automatically:

```cmake
cmake_minimum_required(VERSION 3.26)
project(my_native_mod CXX)
add_subdirectory(path/to/phantom-hourglass-recomp/sdk ph-mods-sdk EXCLUDE_FROM_ALL)

ph_add_mod(my_native_mod
  SOURCES src/mod.cpp
  MOD_JSON "${CMAKE_CURRENT_SOURCE_DIR}/mod.json"
  RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/res"
)
```

### Example Native Mod (`src/mod.cpp`)

```cpp
#include "ph_mods/service.hpp"
#include "ph_mods/svc/log.h"
#include "ph_mods/svc/hook.h"
#include "ph_mods/svc/gfx.h"

DEFINE_MOD();                         // once in one translation unit
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(GfxService, svc_gfx);

static PhHookAction on_perspective_pre(ModContext*, PhArm9Cpu* cpu, uint32_t, void*) {
    // Inspect or modify guest ARM9 registers cpu->r[0..15]!
    return PH_HOOK_CONTINUE;
}

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    svc_log->info(mod_ctx, "Native mod initialized!");
    const uint32_t addr = svc_hook->resolve_symbol(mod_ctx, "G3i_PerspectiveW_");
    if (addr) svc_hook->add_pre(mod_ctx, addr, on_perspective_pre, nullptr, 0, nullptr);
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError* error) { return MOD_OK; }

MOD_EXPORT ModResult mod_draw(const PhDrawContext* ctx, bool* handled, ModError* error) {
    // Return *handled = true to take full control of drawing both DS screens!
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(void) { return MOD_OK; }

}
```

### Built-in Versioned C Services (`sdk/include/ph_mods/svc/`)

| Service | Header | Capabilities |
|---|---|---|
| `LogService` | `svc/log.h` | `write`, `trace`, `debug`, `info`, `warn`, `error` |
| `HostService` | `svc/host.h` | `host_version`, `game_code`, `platform`, `mod_dir`, `data_dir`, `native_dir`, `notify`, `fail`, `get_service`, `publish_service`, `watch_mod_lifecycle` |
| `ResourceService` | `svc/resource.h` | Binary-safe `load` / `free` / `exists` from `res/` |
| `ConfigService` | `svc/config.h` | Persistent typed vars (`BOOL`, `INT`, `FLOAT`, `STRING`, `CHOICE`) with `subscribe` callbacks and auto-generated menu UI |
| `MemoryService` | `svc/memory.h` | `peek8/16/32`, `poke8/16/32`, `read`, `write`, `find_pattern` (with `??` wildcards) |
| `HookService` | `svc/hook.h` | `resolve_symbol`, `symbol_name`, `add_pre`, `add_post`, `replace`, `remove`, `call_original`, `call_guest` across all 24,608 recompiled ARM9 functions |
| `GfxService` | `svc/gfx.h` | Full screen composition (`draw_screen` with crop/alpha/tint), `fill`, `outline`, `text`, `set_clip`, custom PNG/RGBA textures, custom touch hitboxes (`set_touch_rect`) |
| `InputService` | `svc/input.h` | Live DS buttons/stylus/mic/lid/stick + host pointer and keyboard |
| `DsService` | `svc/ds.h` | `display_info` (`POWCNT1`, `DISPCNT_A/B`, `screen_swap`, `new_3d_frame`), layout, save states, screenshots, CPU speed, fast cart |
| `OverlayService` | `svc/overlay.h` | Inspect active NitroFS (`rom_file_count`, `rom_file_path`, `rom_file_size`, `rom_read`) |
| `SaveService` | `svc/save.h` | Per-mod persistent binary blobs stored beside the `.sav` (`set_blob`, `get_blob`, `delete_blob`, `observe_saves`) |

### Exporting Custom Services & Dependency Ordering

Native mods can export their own versioned C services using `EXPORT_SERVICE(g_my_service)` (or `EXPORT_DEFERRED_SERVICE` + `svc_host->publish_service`). Other mods import them with `IMPORT_SERVICE(MyService, svc_my)` or `IMPORT_OPTIONAL_SERVICE(...)`.

- The loader initializes mods in **topological dependency order** and shuts them down in **reverse order**.
- Required providers that fail disable their required dependents cleanly; optional imports resolve to `NULL`.
- Native DLLs/`.so` are copied to a per-session staging directory before loading, so you can rebuild your C++ mod while the game is running and press **`F6`** to hot-reload without file locks!

---

## 3. Script Mods (Lua 5.4)

Each Lua mod runs in an isolated, sandboxed Lua 5.4 VM with an instruction watchdog and per-mod error isolation (an error disables only that mod without crashing the game).

Place your entry script at `res/main.lua` (or `main.lua`) and access services either via the global `ph.*` table OR Dusklight-style `require("ph.<module>")`:

- `require("ph.host")` / `ph` — `on`, `off`, `notify`, `fail`, `frame`, `fps`, `dt`, `game_code`, `platform`, `mod`
- `require("ph.logger")` — `info`, `warn`, `error`, `debug`
- `require("ph.resource")` — `load(rel)`, `exists(rel)` (from `res/`)
- `require("ph.config")` — `bool`, `int`, `float`, `choice`, `get`, `set`, `subscribe`, `save`
- `require("ph.mem")` — `u8/s8/u16/s16/u32/s32/f32/q12`, `write_u8/16/32`, `read_bytes`, `write_bytes`, `read_cstr`, `find`
- `require("ph.hook")` — `resolve`, `symbol_name`, `pre`, `post`, `replace`, `remove`, `call_original`
- `require("ph.gfx")` — `win_size`, `default_views`, `draw_screen` (crop `src = {x,y,w,h}`, `alpha`, `tint`, `smooth`, `wide`), `fill`, `outline`, `text`, `set_clip`, `clear_clip`, `load_image`, `draw_image`, `to_ds`, `set_touch_rect`, `clear_touch_rects`
- `require("ph.input")` — `KEY`, `held`, `pressed`, `is_down`, `touch`, `pointer`, `stick`, `key_down`, `key_pressed`
- `require("ph.ds")` — `display`, `wide_extra`, `wide_layer`, `hud_moves`, `set_layout`, `get_layout`, `save_state`, `load_state`, `screenshot`, `set_mic_blow`, `set_mic_sample`, `set_lid_closed`, `set_cpu_speed`, `set_fast_cart`
- `require("ph.save")` — `set(key, bytes)`, `get(key)`, `delete(key)`
- `require("ph.rom")` — `list(prefix?)`, `file_size(path)`, `read(path, offset?, len?)`
- `require("ph.fs")` / `require("ph.json")` / `require("ph.bus")`

### Example: Single-Screen Mode with Interactive Corner PiP (Lua)

```lua
-- res/main.lua
local gfx = require("ph.gfx")
local cfg = require("ph.config")
local input = require("ph.input")

cfg.bool("show_pip", "Show Top PiP", true)
cfg.float("pip_alpha", "PiP Opacity", 0.85, 0.2, 1.0, 0.05)
local zoomed = false

ph.on("draw", function(ctx)
    local main = gfx.default_views("bottom").bottom
    gfx.draw_screen("bottom", main.x, main.y, main.w, main.h, { smooth = ctx.smooth, wide = ctx.wide })
    gfx.set_touch_rect(main.x, main.y, main.w, main.h, { wide = ctx.wide, priority = 0 })

    if cfg.get("show_pip", true) then
        local pw = main.w * (zoomed and 0.50 or 0.28)
        local ph_h = pw * (192 / 256)
        local px = main.x + main.w - pw - 10 * ctx.ui_scale
        local py = main.y + 10 * ctx.ui_scale

        local ptr = input.pointer()
        if ptr.just_pressed and ptr.x >= px and ptr.x < px + pw and ptr.y >= py and ptr.y < py + ph_h then
            zoomed = not zoomed
        end
        gfx.set_touch_rect(px, py, pw, ph_h, { block = true, priority = 10 })
        gfx.draw_screen("top", px, py, pw, ph_h, { smooth = true, alpha = cfg.get("pip_alpha", 0.85) })
        gfx.outline(px, py, pw, ph_h, 2 * ctx.ui_scale, 0xCC8AB4FF)
    end
    return true
end)
```
