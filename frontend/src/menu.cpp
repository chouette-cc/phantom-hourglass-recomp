#include "menu.hpp"

#include <algorithm>
#include <cstdio>

#include "gl_present.hpp"

namespace ph {
namespace {

// The Android menu's colours.
constexpr uint32_t kShade = 0x55000000, kPanel = 0xF0181820, kRow = 0xFF2A2A34, kRowOff = 0xFF202028,
                   kAccent = 0xFF8AB4FF, kWhite = 0xFFFFFFFF, kHint = 0xFF9090A0, kOff = 0xFF606068,
                   kEmpty = 0xFF121218, kGold = 0xFFF5C542, kDanger = 0xFF9E3A3A, kGreen = 0xFF6EE08A;

enum RowId { kScreen, kRenderer, kResolution, kTextures, kFixes, kPicture, kSize, kWide, kFps, kFullscreen, kStick, kContinue, kQuit,
             kRowCount };
constexpr int kScales[] = {2, 4, 6, 8};
constexpr int kColumns = 4;  // of the save state slots

}  // namespace

Rect Menu::panel(float win_w, float win_h, float ui) {
    const float w = std::min(win_w, std::max(420.0f * ui, win_w * 0.42f));
    return {win_w - w, 0, w, win_h};
}

Menu::Geometry Menu::geometry(const MenuState& s, float win_w, float win_h, float ui) {
    Geometry g;
    g.panel = panel(win_w, win_h, ui);
    const Rect& p = g.panel;
    const float pad = 16 * ui, text = 16 * ui, chip_h = 36 * ui, gap = 6 * ui;
    float y = pad + text * 2.2f;
    const float half = (p.w - 2 * pad - gap) / 2;
    const float third = (p.w - 2 * pad - 2 * gap) / 3;
    g.tabs = {Rect{p.x + pad, y, third, chip_h}, Rect{p.x + pad + third + gap, y, third, chip_h},
              Rect{p.x + pad + 2 * (third + gap), y, third, chip_h}};
    y += chip_h + 18 * ui;

    // Settings: each row's label over its line of chips.
    std::vector<Row>& r = g.rows;
    r.resize(kRowCount);
    r[kScreen] = {"Screen", {"Touch", "Top", "Both"}, int(s.layout), {true, true, true}, {}, {}};
    r[kRenderer] = {"Renderer", {"Software", "GPU", "Compute"}, !s.gpu ? 0 : s.compute ? 2 : 1,
                    {true, !s.gpu_failed, !s.gpu_failed}, {}, {}};
    {
        int chosen = 0;
        for (int i = 0; i < 4; i++)
            if (s.gpu && kScales[i] == s.scale) chosen = i + 1;
        r[kResolution] = {"Resolution", {"1x", "2x", "4x", "6x", "8x"}, chosen, {!s.gpu, s.gpu, s.gpu, s.gpu, s.gpu}, {}, {}};
    }
    const bool gl = s.gpu && !s.compute;  // texture filtering is only implemented by the classic OpenGL renderer
    r[kTextures] = {"3D textures (GPU)", {"Pixels", "Smooth", "Sharp"}, gl ? s.textures : 0, {true, gl, gl}, {}, {}};
    // (Seams: the GPU renderer's better polygons. Warp: textures the DS bends with the view, cut into a grid.)
    r[kFixes] = {"3D fixes", {"Off", "Seams", "Warp", "Both"}, s.gpu ? s.fixes : s.fixes & 2, {true, s.gpu, true, s.gpu}, {}, {}};
    r[kPicture] = {"Picture", {"Sharp", "Smooth"}, s.smooth ? 1 : 0, {true, true}, {}, {}};
    r[kSize] = {"Picture size", {"Fit", "Stretch"}, s.stretch ? 1 : 0, {true, true}, {}, {}};
    r[kWide] = {"Widescreen (test)", {"4:3", "16:9"}, s.wide && s.gpu ? 1 : 0, {true, s.gpu}, {}, {}};
    r[kFps] = {"FPS counter", {"Off", "Top L", "Top R", "Bot L", "Bot R"}, s.fps ? s.fps_corner + 1 : 0,
               {true, true, true, true, true}, {}, {}};
    r[kFullscreen] = {"Fullscreen", {"Off", "On"}, s.fullscreen ? 1 : 0, {true, true}, {}, {}};
    r[kStick] = {"Gamepad's left stick", {"D-pad", "Analog"}, s.stick ? 1 : 0, {true, true}, {}, {}};
    r[kContinue] = {"", {s.resume_time.empty() ? std::string("Continue: nothing yet") : "Continue from " + s.resume_time},
                    -1, {!s.resume_time.empty()}, {}, {}};
    r[kQuit] = {"", {"Quit the game"}, -1, {true}, {}, {}};
    // Each row's label over its chips, or (when that does not fit the window) beside them.
    float stacked = y;
    for (const Row& row : r) stacked += (row.label.empty() ? 0 : text + 8 * ui) + chip_h + 14 * ui;
    const bool beside = stacked > win_h - pad - 10 * ui * 1.5f;
    const float label_w = beside ? (p.w - 2 * pad) * 0.36f : 0;
    float ry = y;
    for (Row& row : r) {
        float cx = p.x + pad, cwidth = p.w - 2 * pad;
        if (!row.label.empty()) {
            if (beside) {
                row.label_at = {cx, ry + (chip_h - text) / 2, label_w - gap, text};
                cx += label_w, cwidth -= label_w;
            } else {
                row.label_at = {cx, ry, cwidth, text};
                ry += text + 8 * ui;
            }
        }
        const int n = int(row.choices.size());
        const float cw = (cwidth - gap * float(n - 1)) / float(n);
        for (int i = 0; i < n; i++) row.chips.push_back({cx + float(i) * (cw + gap), ry, cw, chip_h});
        ry += chip_h + (beside ? 8 : 14) * ui;
    }

    // Save states: a grid of cards (the picture, then the slot's number and time), then Save and Load.
    const float cw = (p.w - 2 * pad - gap * float(kColumns - 1)) / float(kColumns);
    const float th = cw * 0.75f, label = 10 * ui, ch = th + label + 10 * ui;
    for (int i = 0; i < kStateSlots; i++) {
        const float cx = p.x + pad + float(i % kColumns) * (cw + gap), cy = y + float(i / kColumns) * (ch + gap);
        g.cards[size_t(i)] = {cx, cy, cw, ch};
        g.thumbs[size_t(i)] = {cx + 2 * ui, cy + 2 * ui, cw - 4 * ui, th - 4 * ui};
    }
    const float by = y + float((kStateSlots + kColumns - 1) / kColumns) * (ch + gap) + 12 * ui;
    g.buttons = {Rect{p.x + pad, by, half, chip_h + 6 * ui}, Rect{p.x + pad + half + gap, by, half, chip_h + 6 * ui}};

    // Mods page: Installed vs Browse Online sub-tabs, list/config items, and bottom action bar.
    const float sub_h = 30 * ui;
    g.mod_subtabs = {Rect{p.x + pad, y, half, sub_h}, Rect{p.x + pad + half + gap, y, half, sub_h}};
    const float mod_body_y = y + sub_h + 10 * ui;
    const float reload_h = chip_h + 2 * ui;
    const float small_h = 12 * ui;
    const float btn_y = win_h - pad - small_h * 2.2f - reload_h;
    const float rem_w = (p.w - 2 * pad - gap) * 0.34f;
    g.mod_reload_btn = {p.x + pad, btn_y, (p.w - 2 * pad - gap) - rem_w, reload_h};
    g.mod_remove_btn = {p.x + p.w - pad - rem_w, btn_y, rem_w, reload_h};

    float my = mod_body_y;
    const float row_h = 32 * ui;
    const float toggle_w = 56 * ui;
    const float list_limit = g.mod_reload_btn.y - 10 * ui;
    for (size_t i = 0; i < s.mods.size() && my + row_h <= list_limit; i++) {
        g.mod_rows.push_back({{p.x + pad, my, p.w - 2 * pad - toggle_w - gap, row_h},
                              {p.x + p.w - pad - toggle_w, my, toggle_w, row_h}});
        my += row_h + gap;
    }
    if (!s.mods.empty()) {
        const int idx = std::clamp(s.selected_mod, 0, int(s.mods.size()) - 1);
        const ModSummary& mod = s.mods[size_t(idx)];
        my += 6 * ui;
        g.mod_info_rect = {p.x + pad, my, p.w - 2 * pad, 28 * ui};
        my += 32 * ui;
        const float btn_w = 30 * ui;
        const float ctrl_w = (p.w - 2 * pad) * 0.48f;
        const float lbl_w = (p.w - 2 * pad) - ctrl_w - gap;
        const float step_h = row_h + gap;
        const int total_cfgs = int(mod.config.size());
        const int fit_full = std::max(0, int((list_limit - my + gap) / step_h));
        const bool need_scroll = total_cfgs > fit_full && fit_full >= 2;
        const float scroll_bar_h = need_scroll ? (26 * ui + gap) : 0.0f;
        const float cfg_limit = list_limit - scroll_bar_h;
        const int visible = std::max(1, int((cfg_limit - my + gap) / step_h));
        const int max_start = std::max(0, total_cfgs - visible);
        const int start = std::clamp(s.mod_cfg_scroll, 0, max_start);
        g.mod_cfg_start = start;
        g.mod_cfg_visible = visible;
        g.mod_cfg_total = total_cfgs;
        for (int i = start; i < total_cfgs && my + row_h <= cfg_limit + 0.5f; i++) {
            const ModConfigItem& cfg = mod.config[size_t(i)];
            const float cx = p.x + pad + lbl_w + gap;
            ModCfgGeom cg;
            cg.key = cfg.key;
            cg.item_index = size_t(i);
            cg.kind = cfg.kind;
            cg.label_rect = {p.x + pad, my, lbl_w, row_h};
            if (cfg.kind == ModConfigItem::Kind::Bool || cfg.kind == ModConfigItem::Kind::Hotkey) {
                cg.value_rect = {cx, my, ctrl_w, row_h};
            } else {
                cg.minus_rect = {cx, my, btn_w, row_h};
                cg.value_rect = {cx + btn_w + 4 * ui, my, std::max(20 * ui, ctrl_w - 2 * btn_w - 8 * ui), row_h};
                cg.plus_rect = {cx + ctrl_w - btn_w, my, btn_w, row_h};
            }
            g.mod_cfgs.push_back(std::move(cg));
            my += step_h;
        }
        if (need_scroll) {
            const float sy = cfg_limit + 2 * ui;
            const float nav_w = 82 * ui;
            g.mod_cfg_prev = {p.x + pad, sy, nav_w, 24 * ui};
            g.mod_cfg_next = {p.x + p.w - pad - nav_w, sy, nav_w, 24 * ui};
        }
    }

    // Mods Browse Online sub-page geometry
    const float cat_top = mod_body_y + 24 * ui;
    const float cat_h = std::max(80 * ui, btn_y - 10 * ui - cat_top);
    g.cat_list = {p.x + pad, cat_top, p.w - 2 * pad, cat_h};
    g.cat_buttons = {
        Rect{p.x + pad, btn_y, third, reload_h},
        Rect{p.x + pad + third + gap, btn_y, third, reload_h},
        Rect{p.x + pad + 2 * (third + gap), btn_y, third, reload_h},
    };
    return g;
}

MenuResult Menu::choose(int row, int choice, MenuState& s) {
    switch (row) {
        case kScreen: s.layout = Layout(choice); break;
        case kRenderer:
            if (choice >= 1 && s.gpu_failed) return MenuResult::None;
            s.gpu = choice >= 1;
            s.compute = choice == 2;
            break;
        case kResolution:
            if (!s.gpu || choice == 0) return MenuResult::None;  // (1x is the software renderer's)
            s.scale = kScales[choice - 1];
            break;
        case kTextures:
            if (choice != 0 && (!s.gpu || s.compute)) return MenuResult::None;
            s.textures = choice;
            break;
        case kFixes:
            if ((choice & 1) && !s.gpu) return MenuResult::None;
            s.fixes = choice;
            break;
        case kPicture: s.smooth = choice == 1; break;
        case kSize: s.stretch = choice == 1; break;
        case kWide:
            if (choice == 1 && !s.gpu) return MenuResult::None;
            s.wide = choice == 1;
            break;
        case kFps:
            s.fps = choice != 0;
            if (choice) s.fps_corner = choice - 1;
            break;
        case kFullscreen: s.fullscreen = choice == 1; break;
        case kStick: s.stick = choice == 1; break;
        case kContinue: return s.resume_time.empty() ? MenuResult::None : MenuResult::Continue;
        case kQuit: return MenuResult::Quit;
        default: return MenuResult::None;
    }
    return MenuResult::Changed;
}

MenuResult Menu::capture_binding(const std::string& binding, MenuState& s) {
    if (!capturing_hotkey) return MenuResult::None;
    s.action_mod_id = capture_mod_id;
    s.action_cfg_key = capture_cfg_key;
    s.action_cfg_str = binding.empty() ? "None" : binding;
    capturing_hotkey = false;
    capture_mod_id.clear();
    capture_cfg_key.clear();
    return MenuResult::SetModConfigString;
}

MenuResult Menu::key(SDL_Scancode key, MenuState& s) {
    if (capturing_hotkey) {
        if (key == SDL_SCANCODE_ESCAPE) {
            capturing_hotkey = false;
            capture_mod_id.clear();
            capture_cfg_key.clear();
            return MenuResult::None;
        }
        if (key == SDL_SCANCODE_BACKSPACE || key == SDL_SCANCODE_DELETE) {
            return capture_binding("None", s);
        }
        const char* name = SDL_GetScancodeName(key);
        if (name && name[0]) return capture_binding(name, s);
        return MenuResult::None;
    }
    if (key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_F1) {
        open = false;
        return MenuResult::None;
    }
    if (key == SDL_SCANCODE_TAB) {
        page = (page + 1) % 3;
        return MenuResult::None;
    }
    if (page == 2) {  // Mods: Installed vs Browse Online
        if (key == SDL_SCANCODE_LEFT || key == SDL_SCANCODE_RIGHT) {
            mod_subpage = 1 - mod_subpage;
            confirm_native_id.clear();
            confirm_remove_id.clear();
            if (mod_subpage == 1 && s.catalog.status == ModCatalogSnapshot::Status::Idle)
                return MenuResult::RefreshCatalog;
            return MenuResult::None;
        }
        if (mod_subpage == 1) {
            if (key == SDL_SCANCODE_R) return MenuResult::RefreshCatalog;
            const int max_s = std::max(0, int(s.catalog.entries.size()) - 1);
            if (key == SDL_SCANCODE_UP) catalog_scroll = std::max(0, catalog_scroll - 1);
            else if (key == SDL_SCANCODE_DOWN) catalog_scroll = std::min(max_s, catalog_scroll + 1);
            return MenuResult::None;
        }
        if (key == SDL_SCANCODE_R || key == SDL_SCANCODE_F6) return MenuResult::ReloadMods;
        if (!s.mods.empty()) {
            const int n = int(s.mods.size());
            selected_mod = std::clamp(selected_mod, 0, n - 1);
            if (key == SDL_SCANCODE_UP) { selected_mod = (selected_mod + n - 1) % n; mod_cfg_scroll = 0; confirm_remove_id.clear(); }
            if (key == SDL_SCANCODE_DOWN) { selected_mod = (selected_mod + 1) % n; mod_cfg_scroll = 0; confirm_remove_id.clear(); }
            const int max_cfg = std::max(0, int(s.mods[size_t(selected_mod)].config.size()) - 1);
            if (key == SDL_SCANCODE_PAGEUP) mod_cfg_scroll = std::max(0, mod_cfg_scroll - 4);
            if (key == SDL_SCANCODE_PAGEDOWN) mod_cfg_scroll = std::min(max_cfg, mod_cfg_scroll + 4);
            s.selected_mod = selected_mod;
            s.mod_cfg_scroll = mod_cfg_scroll;
            if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_SPACE) {
                const ModSummary& m = s.mods[size_t(selected_mod)];
                s.action_mod_id = m.id;
                s.action_mod_enable = !m.enabled;
                return MenuResult::ToggleMod;
            }
        }
        return MenuResult::None;
    }
    if (page == 1) {  // arrows choose the slot; S saves to it, L (or Enter) loads it
        const int i = s.slot - 1;
        int to = i;
        if (key == SDL_SCANCODE_LEFT) to = (i + kStateSlots - 1) % kStateSlots;
        if (key == SDL_SCANCODE_RIGHT) to = (i + 1) % kStateSlots;
        if (key == SDL_SCANCODE_UP) to = (i + kStateSlots - kColumns) % kStateSlots;
        if (key == SDL_SCANCODE_DOWN) to = (i + kColumns) % kStateSlots;
        if (to != i) {
            s.slot = to + 1;
            return MenuResult::Changed;
        }
        if (key == SDL_SCANCODE_S) return MenuResult::SaveState;
        if ((key == SDL_SCANCODE_L || key == SDL_SCANCODE_RETURN) && !s.slot_time[size_t(i)].empty())
            return MenuResult::LoadState;
        return MenuResult::None;
    }
    const std::vector<Row> r = geometry(s, 1000, 1000, 1).rows;  // (choices only: the geometry does not matter here)
    if (key == SDL_SCANCODE_UP) selected = (selected + kRowCount - 1) % kRowCount;
    if (key == SDL_SCANCODE_DOWN) selected = (selected + 1) % kRowCount;
    const Row& row = r[size_t(selected)];
    const int n = int(row.choices.size());
    auto step = [&](int dir) {
        for (int i = 1; i < n; i++) {  // the next enabled choice that way
            const int c = (std::max(row.chosen, 0) + dir * i + n * 4) % n;
            if (row.enabled[size_t(c)]) return choose(selected, c, s);
        }
        return MenuResult::None;
    };
    if (key == SDL_SCANCODE_LEFT) return step(-1);
    if (key == SDL_SCANCODE_RIGHT) return step(1);
    if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_SPACE) {
        if (selected == kQuit || selected == kContinue) return choose(selected, 0, s);  // (buttons)
        return step(1);
    }
    return MenuResult::None;
}

void Menu::wheel(float delta_y, const MenuState& s) {
    if (!open || page != 2) return;
    const int step = delta_y > 0 ? -2 : (delta_y < 0 ? 2 : 0);
    if (mod_subpage == 1) {
        const int max_scroll = std::max(0, int(s.catalog.entries.size()) - 1);
        catalog_scroll = std::clamp(catalog_scroll + step, 0, max_scroll);
    } else if (!s.mods.empty()) {
        const int idx = std::clamp(selected_mod, 0, int(s.mods.size()) - 1);
        const int max_scroll = std::max(0, int(s.mods[size_t(idx)].config.size()) - 1);
        mod_cfg_scroll = std::clamp(mod_cfg_scroll + step, 0, max_scroll);
    }
}

MenuResult Menu::click(float x, float y, float win_w, float win_h, float ui, MenuState& s) {
    s.selected_mod = selected_mod;
    s.mod_cfg_scroll = mod_cfg_scroll;
    const Geometry g = geometry(s, win_w, win_h, ui);
    if (!g.panel.contains(x, y)) {
        open = false;
        capturing_hotkey = false;
        confirm_native_id.clear();
        confirm_remove_id.clear();
        return MenuResult::None;
    }
    for (int t = 0; t < 3; t++)
        if (g.tabs[size_t(t)].contains(x, y)) {
            page = t;
            capturing_hotkey = false;
            confirm_native_id.clear();
            confirm_remove_id.clear();
            return MenuResult::None;
        }
    if (page == 2) {
        if (g.mod_subtabs[0].contains(x, y)) {
            mod_subpage = 0;
            capturing_hotkey = false;
            confirm_native_id.clear();
            return MenuResult::None;
        }
        if (g.mod_subtabs[1].contains(x, y)) {
            mod_subpage = 1;
            capturing_hotkey = false;
            confirm_remove_id.clear();
            if (s.catalog.status == ModCatalogSnapshot::Status::Idle) return MenuResult::RefreshCatalog;
            return MenuResult::None;
        }
        if (mod_subpage == 1) {
            const float card_h = 62 * ui, card_gap = 6 * ui;
            const int visible = std::max(1, int((g.cat_list.h + card_gap) / (card_h + card_gap)));
            const int max_scroll = std::max(0, int(s.catalog.entries.size()) - visible);
            const int start = std::clamp(catalog_scroll, 0, max_scroll);
            if (g.cat_buttons[0].contains(x, y)) {
                catalog_scroll = std::max(0, start - 3);
                confirm_native_id.clear();
                return MenuResult::None;
            }
            if (g.cat_buttons[1].contains(x, y)) {
                catalog_scroll = std::min(max_scroll, start + 3);
                confirm_native_id.clear();
                return MenuResult::None;
            }
            if (g.cat_buttons[2].contains(x, y)) {
                confirm_native_id.clear();
                return MenuResult::RefreshCatalog;
            }
            if (!s.catalog.installing && s.catalog.status == ModCatalogSnapshot::Status::Ready) {
                const float btn_w = 88 * ui, btn_h = 30 * ui;
                for (int i = 0; i < visible && start + i < int(s.catalog.entries.size()); i++) {
                    const Rect card{g.cat_list.x, g.cat_list.y + float(i) * (card_h + card_gap), g.cat_list.w, card_h};
                    const Rect btn{card.x + card.w - btn_w - 8 * ui, card.y + (card.h - btn_h) / 2, btn_w, btn_h};
                    if (btn.contains(x, y)) {
                        const CatalogModEntry& e = s.catalog.entries[size_t(start + i)];
                        if (e.kind == "native" && confirm_native_id != e.id) {
                            confirm_native_id = e.id;
                            return MenuResult::None;
                        }
                        confirm_native_id.clear();
                        s.action_catalog_mod = e;
                        return MenuResult::InstallCatalogMod;
                    }
                }
            }
            confirm_native_id.clear();
            return MenuResult::None;
        }
        if (g.mod_reload_btn.contains(x, y)) {
            capturing_hotkey = false;
            confirm_remove_id.clear();
            return MenuResult::ReloadMods;
        }
        if (g.mod_remove_btn.contains(x, y) && !s.mods.empty()) {
            capturing_hotkey = false;
            const int idx = std::clamp(selected_mod, 0, int(s.mods.size()) - 1);
            const std::string& mid = s.mods[size_t(idx)].id;
            if (confirm_remove_id != mid) {
                confirm_remove_id = mid;
                return MenuResult::None;
            }
            confirm_remove_id.clear();
            s.action_mod_id = mid;
            return MenuResult::UninstallMod;
        }
        if (g.mod_cfg_prev.w > 0 && g.mod_cfg_prev.contains(x, y)) {
            capturing_hotkey = false;
            mod_cfg_scroll = std::max(0, g.mod_cfg_start - std::max(1, g.mod_cfg_visible - 1));
            s.mod_cfg_scroll = mod_cfg_scroll;
            return MenuResult::None;
        }
        if (g.mod_cfg_next.w > 0 && g.mod_cfg_next.contains(x, y)) {
            capturing_hotkey = false;
            const int max_s = std::max(0, g.mod_cfg_total - g.mod_cfg_visible);
            mod_cfg_scroll = std::min(max_s, g.mod_cfg_start + std::max(1, g.mod_cfg_visible - 1));
            s.mod_cfg_scroll = mod_cfg_scroll;
            return MenuResult::None;
        }
        confirm_remove_id.clear();
        for (size_t i = 0; i < g.mod_rows.size() && i < s.mods.size(); i++) {
            if (g.mod_rows[i].toggle_rect.contains(x, y)) {
                capturing_hotkey = false;
                if (selected_mod != int(i)) mod_cfg_scroll = 0;
                selected_mod = int(i);
                s.selected_mod = selected_mod;
                s.mod_cfg_scroll = mod_cfg_scroll;
                s.action_mod_id = s.mods[i].id;
                s.action_mod_enable = !s.mods[i].enabled;
                return MenuResult::ToggleMod;
            }
            if (g.mod_rows[i].select_rect.contains(x, y)) {
                capturing_hotkey = false;
                if (selected_mod != int(i)) mod_cfg_scroll = 0;
                selected_mod = int(i);
                s.selected_mod = selected_mod;
                s.mod_cfg_scroll = mod_cfg_scroll;
                return MenuResult::None;
            }
        }
        if (!s.mods.empty()) {
            const int idx = std::clamp(selected_mod, 0, int(s.mods.size()) - 1);
            const ModSummary& m = s.mods[size_t(idx)];
            for (const ModCfgGeom& cg : g.mod_cfgs) {
                if (cg.kind == ModConfigItem::Kind::Hotkey && cg.value_rect.contains(x, y)) {
                    if (capturing_hotkey && capture_mod_id == m.id && capture_cfg_key == cg.key) {
                        capturing_hotkey = false;
                        capture_mod_id.clear();
                        capture_cfg_key.clear();
                    } else {
                        capturing_hotkey = true;
                        capture_mod_id = m.id;
                        capture_cfg_key = cg.key;
                    }
                    return MenuResult::None;
                }
                if (cg.minus_rect.w > 0 && cg.minus_rect.contains(x, y)) {
                    capturing_hotkey = false;
                    s.action_mod_id = m.id;
                    s.action_cfg_key = cg.key;
                    s.action_cfg_dir = -1;
                    return MenuResult::StepModConfig;
                }
                if ((cg.plus_rect.w > 0 && cg.plus_rect.contains(x, y)) ||
                    (cg.minus_rect.w <= 0 && cg.value_rect.contains(x, y))) {
                    capturing_hotkey = false;
                    s.action_mod_id = m.id;
                    s.action_cfg_key = cg.key;
                    s.action_cfg_dir = 1;
                    return MenuResult::StepModConfig;
                }
            }
        }
        capturing_hotkey = false;
        return MenuResult::None;
    }
    if (page == 1) {
        for (int i = 0; i < kStateSlots; i++)
            if (g.cards[size_t(i)].contains(x, y)) {
                s.slot = i + 1;
                return MenuResult::Changed;
            }
        if (g.buttons[0].contains(x, y)) return MenuResult::SaveState;
        if (g.buttons[1].contains(x, y) && !s.slot_time[size_t(s.slot - 1)].empty()) return MenuResult::LoadState;
        return MenuResult::None;
    }
    for (size_t i = 0; i < g.rows.size(); i++)
        for (size_t c = 0; c < g.rows[i].chips.size(); c++)
            if (g.rows[i].chips[c].contains(x, y) && g.rows[i].enabled[c]) {
                selected = int(i);
                return choose(int(i), int(c), s);
            }
    return MenuResult::None;
}

namespace {
void chip(GlPresenter& gl, const Rect& b, const std::string& t, bool chosen, bool on, float text, float ui) {
    gl.fill(b, chosen ? kAccent : on ? kRow : kRowOff);
    const float size = std::min(text, (b.w - 8 * ui) / float(std::max<size_t>(t.size(), 1)));
    gl.text(b.x + (b.w - size * float(t.size())) / 2, b.y + (b.h - size) / 2, size, t,
            chosen ? 0xFF000000 : on ? kWhite : kOff);
}
}  // namespace

void Menu::draw_panel(GlPresenter& gl, const MenuState& s, float win_w, float win_h, float ui, float fps) const {
    const Geometry g = geometry(s, win_w, win_h, ui);
    const Rect& p = g.panel;
    const float pad = 16 * ui, text = 16 * ui;
    gl.fill({0, 0, win_w, win_h}, kShade);
    gl.fill(p, kPanel);
    gl.text(p.x + pad, pad, text * 1.4f, "Menu", kWhite);
    char rate[32];
    std::snprintf(rate, sizeof rate, "%.1f fps", double(fps));
    const float small = 8 * ui * 1.5f;
    gl.text(p.x + p.w - pad - small * float(std::string(rate).size()), pad + text * 0.3f, small, rate, kHint);
    chip(gl, g.tabs[0], "Settings", page == 0, true, text, ui);
    chip(gl, g.tabs[1], "States", page == 1, true, text, ui);
    chip(gl, g.tabs[2], "Mods", page == 2, true, text, ui);

    if (page == 2) {
        chip(gl, g.mod_subtabs[0], "Installed (" + std::to_string(s.mods.size()) + ")", mod_subpage == 0, true, 13 * ui, ui);
        chip(gl, g.mod_subtabs[1], "Browse Online", mod_subpage == 1, true, 13 * ui, ui);

        if (mod_subpage == 1) {
            const auto& cat = s.catalog;
            const float hdr_y = g.mod_subtabs[0].y + g.mod_subtabs[0].h + 8 * ui;
            std::string status_line;
            uint32_t status_col = kHint;
            if (cat.installing) {
                status_line = cat.last_message.empty() ? "Installing..." : cat.last_message;
                status_col = kGold;
            } else if (cat.status == ModCatalogSnapshot::Status::Fetching) {
                status_line = "Fetching online catalog...";
                status_col = kAccent;
            } else if (cat.status == ModCatalogSnapshot::Status::Error) {
                status_line = "Catalog error: " + cat.error;
                status_col = 0xFFFF8888;
            } else if (cat.status == ModCatalogSnapshot::Status::Ready) {
                status_line = cat.catalog_name + " (" + std::to_string(cat.entries.size()) + " mods)";
                status_col = kWhite;
            } else {
                status_line = "Click Refresh to load online mods.";
            }
            const float hsz = std::min(small, (p.w - 2 * pad) / float(std::max<size_t>(1, status_line.size())));
            gl.text(p.x + pad, hdr_y, hsz, status_line, status_col);

            const float card_h = 62 * ui, card_gap = 6 * ui;
            const int visible = std::max(1, int((g.cat_list.h + card_gap) / (card_h + card_gap)));
            const int max_scroll = std::max(0, int(cat.entries.size()) - visible);
            const int start = std::clamp(catalog_scroll, 0, max_scroll);

            if (cat.status == ModCatalogSnapshot::Status::Ready && cat.entries.empty()) {
                gl.text(g.cat_list.x + 6 * ui, g.cat_list.y + 12 * ui, small, "No mods listed in this catalog yet.", kHint);
            } else if (cat.status == ModCatalogSnapshot::Status::Error) {
                gl.text(g.cat_list.x + 6 * ui, g.cat_list.y + 12 * ui, small * 0.9f, "Override URL via data/mods/catalog.url", kHint);
            } else if (cat.status == ModCatalogSnapshot::Status::Ready) {
                const float btn_w = 88 * ui, btn_h = 30 * ui;
                for (int i = 0; i < visible && start + i < int(cat.entries.size()); i++) {
                    const CatalogModEntry& e = cat.entries[size_t(start + i)];
                    const Rect card{g.cat_list.x, g.cat_list.y + float(i) * (card_h + card_gap), g.cat_list.w, card_h};
                    gl.fill(card, kRowOff);
                    const uint32_t kind_col = (e.kind == "native") ? kGold : (e.kind == "asset") ? kGreen : kAccent;
                    gl.fill({card.x, card.y, 4 * ui, card.h}, kind_col);

                    const float tx = card.x + 10 * ui;
                    const float text_w = std::max(80 * ui, card.w - btn_w - 26 * ui);
                    const std::string title = e.name + " v" + e.version;
                    const float tsz = std::min(12.5f * ui, text_w / float(std::max<size_t>(1, title.size())));
                    gl.text(tx, card.y + 6 * ui, tsz, title, kWhite);

                    const char* ktag = (e.kind == "native") ? "[Native C++!]" : (e.kind == "asset") ? "[Asset]" : "[Lua]";
                    std::string meta = std::string(ktag) + (e.author.empty() ? "" : (" by " + e.author));
                    const float msz = std::min(9.5f * ui, text_w / float(std::max<size_t>(1, meta.size())));
                    gl.text(tx, card.y + 23 * ui, msz, meta, kind_col);

                    std::string desc = e.description;
                    const float dsz = 9.0f * ui;
                    const size_t max_ch = size_t(std::max(16.0f, text_w / dsz));
                    if (desc.size() > max_ch && max_ch > 4) desc = desc.substr(0, max_ch - 3) + "...";
                    const float ddraw = std::min(dsz, text_w / float(std::max<size_t>(1, desc.size())));
                    gl.text(tx, card.y + 39 * ui, ddraw, desc, kHint);

                    // Installed version lookup
                    std::string inst_ver;
                    for (const ModSummary& m : s.mods)
                        if (m.id == e.id) { inst_ver = m.version; break; }
                    const bool is_dl = cat.installing && cat.installing_id == e.id;
                    const bool is_confirm = (e.kind == "native" && confirm_native_id == e.id);
                    const bool has_update = !inst_ver.empty() && compare_mod_versions(e.version, inst_ver) > 0;
                    const Rect btn{card.x + card.w - btn_w - 8 * ui, card.y + (card.h - btn_h) / 2, btn_w, btn_h};
                    if (is_dl) {
                        chip(gl, btn, "Wait...", false, false, 11 * ui, ui);
                    } else if (is_confirm) {
                        gl.fill(btn, kDanger);
                        const std::string lbl = "Confirm?";
                        const float bsz = std::min(11 * ui, (btn.w - 8 * ui) / float(lbl.size()));
                        gl.text(btn.x + (btn.w - bsz * float(lbl.size())) / 2, btn.y + (btn.h - bsz) / 2, bsz, lbl, kWhite);
                    } else if (has_update) {
                        gl.fill(btn, kGold);
                        const std::string lbl = "Update";
                        const float bsz = std::min(11 * ui, (btn.w - 8 * ui) / float(lbl.size()));
                        gl.text(btn.x + (btn.w - bsz * float(lbl.size())) / 2, btn.y + (btn.h - bsz) / 2, bsz, lbl, 0xFF000000);
                    } else if (!inst_ver.empty()) {
                        chip(gl, btn, "Installed", false, true, 11 * ui, ui);
                    } else {
                        chip(gl, btn, "Install", true, !cat.installing, 11 * ui, ui);
                    }
                }
            }

            chip(gl, g.cat_buttons[0], "Prev", false, start > 0, 13 * ui, ui);
            chip(gl, g.cat_buttons[1], "Next", false, start < max_scroll, 13 * ui, ui);
            chip(gl, g.cat_buttons[2], "Refresh", false, !cat.installing, 13 * ui, ui);
            const char* hint = !confirm_native_id.empty()
                                   ? "WARNING: Native C++ mods run unsandboxed. Click Confirm? to install."
                                   : "Wheel/Arrows: scroll. R: refresh. Left/Right: switch view.";
            gl.text(p.x + pad, win_h - pad - small, small, hint, !confirm_native_id.empty() ? kGold : kHint);
            return;
        }

        if (s.mods.empty()) {
            const float ey = g.mod_subtabs[0].y + g.mod_subtabs[0].h + 24 * ui;
            gl.text(p.x + pad, ey, text, "No mods installed.", kWhite);
            gl.text(p.x + pad, ey + 26 * ui, small, "Click 'Browse Online' above to install mods,", kHint);
            gl.text(p.x + pad, ey + 44 * ui, small, "or drop .phmod files into data/mods/.", kHint);
        } else {
            const int sel = std::clamp(selected_mod, 0, int(s.mods.size()) - 1);
            for (size_t i = 0; i < g.mod_rows.size() && i < s.mods.size(); i++) {
                const ModSummary& m = s.mods[i];
                const Rect& sr = g.mod_rows[i].select_rect;
                const Rect& tr = g.mod_rows[i].toggle_rect;
                const bool is_sel = int(i) == sel;
                gl.fill(sr, is_sel ? 0xFF343C52 : kRow);
                if (is_sel) gl.fill({sr.x - 6 * ui, sr.y, 3 * ui, sr.h}, kAccent);
                std::string label = m.name + " v" + m.version;
                if (!m.error.empty()) label += " [!]";
                const float lsz = std::min(text * 0.9f, (sr.w - 10 * ui) / float(std::max<size_t>(label.size(), 1)));
                gl.text(sr.x + 6 * ui, sr.y + (sr.h - lsz) / 2, lsz, label, !m.error.empty() ? 0xFFFF8888 : kWhite);
                chip(gl, tr, m.enabled ? "ON" : "OFF", m.enabled && m.loaded, true, text * 0.9f, ui);
            }
            const ModSummary& cur = s.mods[size_t(sel)];
            if (g.mod_info_rect.h > 0) {
                std::string info = !cur.error.empty() ? ("Error: " + cur.error)
                                                      : (cur.description.empty() ? cur.id : cur.description);
                const float isz = std::min(small, (g.mod_info_rect.w - 4 * ui) / float(std::max<size_t>(info.size(), 1)));
                gl.text(g.mod_info_rect.x, g.mod_info_rect.y, isz, info, !cur.error.empty() ? 0xFFFF8888 : kHint);
                const char* kind_tag = cur.kind == ModKind::Native ? "[C++ Native]"
                                       : cur.kind == ModKind::Lua ? "[Lua]" : "[Asset]";
                std::string meta = std::string(kind_tag) + (cur.author.empty() ? "" : (" by " + cur.author)) +
                                   (cur.has_overlay ? " [overlay]" : "");
                gl.text(g.mod_info_rect.x, g.mod_info_rect.y + small + 4 * ui, small * 0.9f, meta, kAccent);
            }
            for (size_t i = 0; i < g.mod_cfgs.size(); i++) {
                const ModCfgGeom& cg = g.mod_cfgs[i];
                if (cg.item_index >= cur.config.size()) continue;
                const ModConfigItem& ci = cur.config[cg.item_index];
                const float lsz = std::min(small * 1.1f, (cg.label_rect.w - 4 * ui) / float(std::max<size_t>(ci.label.size(), 1)));
                gl.text(cg.label_rect.x, cg.label_rect.y + (cg.label_rect.h - lsz) / 2, lsz, ci.label, kWhite);
                if (ci.kind == ModConfigItem::Kind::Bool) {
                    chip(gl, cg.value_rect, ci.bool_val ? "On" : "Off", ci.bool_val, true, text * 0.9f, ui);
                } else if (ci.kind == ModConfigItem::Kind::Hotkey) {
                    const bool is_cap = capturing_hotkey && capture_mod_id == cur.id && capture_cfg_key == ci.key;
                    if (is_cap) {
                        gl.fill(cg.value_rect, kGold);
                        const std::string lbl = "Press key...";
                        const float sz = std::min(text * 0.85f, (cg.value_rect.w - 8 * ui) / float(lbl.size()));
                        gl.text(cg.value_rect.x + (cg.value_rect.w - sz * float(lbl.size())) / 2,
                                cg.value_rect.y + (cg.value_rect.h - sz) / 2, sz, lbl, 0xFF000000);
                    } else {
                        const std::string disp = ci.string_val.empty() ? "None" : ci.string_val;
                        const bool bound = disp != "None";
                        chip(gl, cg.value_rect, disp, bound, true, text * 0.85f, ui);
                    }
                } else {
                    std::string val;
                    if (ci.kind == ModConfigItem::Kind::Int) val = std::to_string(ci.int_val);
                    else if (ci.kind == ModConfigItem::Kind::Float) {
                        char buf[32];
                        std::snprintf(buf, sizeof buf, "%.2f", ci.float_val);
                        val = buf;
                    } else if (ci.kind == ModConfigItem::Kind::Choice && !ci.choices.empty()) {
                        val = ci.choices[size_t(std::clamp<int64_t>(ci.int_val, 0, int64_t(ci.choices.size()) - 1))];
                    }
                    chip(gl, cg.minus_rect, "<", false, true, text * 0.9f, ui);
                    chip(gl, cg.value_rect, val, false, true, text * 0.85f, ui);
                    chip(gl, cg.plus_rect, ">", false, true, text * 0.9f, ui);
                }
            }
            if (g.mod_cfg_prev.w > 0) {
                const int end_i = std::min(g.mod_cfg_total, g.mod_cfg_start + int(g.mod_cfgs.size()));
                const bool can_prev = g.mod_cfg_start > 0;
                const bool can_next = end_i < g.mod_cfg_total;
                chip(gl, g.mod_cfg_prev, "< Prev", false, can_prev, 11 * ui, ui);
                chip(gl, g.mod_cfg_next, "Next >", false, can_next, 11 * ui, ui);
                const std::string pg = std::to_string(g.mod_cfg_start + 1) + "-" + std::to_string(end_i) +
                                       " / " + std::to_string(g.mod_cfg_total);
                const float mid_w = g.mod_cfg_next.x - (g.mod_cfg_prev.x + g.mod_cfg_prev.w);
                const float psz = std::min(10.5f * ui, (mid_w - 8 * ui) / float(std::max<size_t>(1, pg.size())));
                gl.text(g.mod_cfg_prev.x + g.mod_cfg_prev.w + (mid_w - psz * float(pg.size())) / 2,
                        g.mod_cfg_prev.y + (g.mod_cfg_prev.h - psz) / 2, psz, pg, kHint);
            }
        }
        chip(gl, g.mod_reload_btn, "Reload (F6)", false, true, text * 0.9f, ui);
        const bool has_sel = !s.mods.empty();
        const bool is_rem_confirm = has_sel &&
            confirm_remove_id == s.mods[size_t(std::clamp(selected_mod, 0, int(s.mods.size()) - 1))].id;
        if (is_rem_confirm) {
            gl.fill(g.mod_remove_btn, kDanger);
            const std::string lbl = "Confirm?";
            const float sz = std::min(text * 0.85f, (g.mod_remove_btn.w - 8 * ui) / float(lbl.size()));
            gl.text(g.mod_remove_btn.x + (g.mod_remove_btn.w - sz * float(lbl.size())) / 2,
                    g.mod_remove_btn.y + (g.mod_remove_btn.h - sz) / 2, sz, lbl, kWhite);
        } else {
            chip(gl, g.mod_remove_btn, "Remove", false, has_sel, text * 0.9f, ui);
        }
        const char* mod_hint = capturing_hotkey
                                   ? "Press key, pad or mouse button. Backspace: clear. Esc: cancel."
                                   : "Up/Down: select. Enter: toggle. R/F6: reload. Left/Right: Online.";
        gl.text(p.x + pad, win_h - pad - small, small, mod_hint, capturing_hotkey ? kGold : kHint);
        return;
    }

    if (page == 1) {
        const float label = 10 * ui;
        for (int i = 0; i < kStateSlots; i++) {
            const Rect& c = g.cards[size_t(i)];
            const std::string& when = s.slot_time[size_t(i)];
            if (i + 1 == s.slot) gl.fill(c, kAccent);  // the chosen slot: framed
            const Rect& t = g.thumbs[size_t(i)];
            gl.fill(t, when.empty() ? kEmpty : kRow);  // (the picture is drawn over this, see draw())
            if (i + 1 != s.slot) gl.fill({c.x, t.y + t.h + 2 * ui, c.w, c.h - t.h - 4 * ui}, kRowOff);
            std::string name = std::to_string(i + 1) + (when.empty() ? " empty" : " " + when);
            const float size = std::min(label, (c.w - 6 * ui) / float(name.size()));
            gl.text(c.x + 3 * ui, t.y + t.h + 4 * ui, size, name, i + 1 == s.slot ? 0xFF000000 : when.empty() ? kOff : kWhite);
        }
        const bool used = !s.slot_time[size_t(s.slot - 1)].empty();
        chip(gl, g.buttons[0], "Save to " + std::to_string(s.slot), false, true, text, ui);
        chip(gl, g.buttons[1], "Load " + std::to_string(s.slot), false, used, text, ui);
        gl.text(p.x + pad, win_h - pad - small * 2.6f, small, "Arrows: slot. S: save. L or Enter: load.", kHint);
        gl.text(p.x + pad, win_h - pad - small, small, "F5 saves, F8 loads (menu closed). Tab: page.", kHint);
        return;
    }

    for (size_t i = 0; i < g.rows.size(); i++) {
        const Row& row = g.rows[i];
        if (!row.label.empty()) {  // (beside its chips: as large as its room allows)
            const float size = std::min(text, row.label_at.w / float(row.label.size()));
            gl.text(row.label_at.x, row.label_at.y + (text - size) / 2, size, row.label, kAccent);
        }
        if (int(i) == selected) {  // the keyboard's place: a bar beside the row's chips
            const Rect& c0 = row.chips.front();
            gl.fill({c0.x - 8 * ui, c0.y, 3 * ui, c0.h}, kAccent);
        }
        for (size_t c = 0; c < row.chips.size(); c++)
            chip(gl, row.chips[c], row.choices[c], int(c) == row.chosen, row.enabled[c], text, ui);
    }
    const char* hint = s.gpu_failed ? "The GPU renderer could not start: software only."
                                    : "F1 or Esc: close. Arrows: choose. Tab: page.";
    gl.text(p.x + pad, win_h - pad - small, small, hint, kHint);
}

}  // namespace ph
