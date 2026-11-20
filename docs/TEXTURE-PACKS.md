# Custom Texture Packs

Phantom Hourglass Recomp supports user-supplied HD texture packs for all three Nintendo DS graphics layers when using the **GPU (OpenGL)** or **Compute** renderer:

- **2D OBJ sprites (`obj/`)**: buttons (`Start`, `Copy`, `Erase`, `Menu`, `Save`, `Items`, `Map`), HUD hearts, rupees, icons, cursors, and dialogue frames.
- **2D Backgrounds (`bg/`)**: sea charts, island maps, menu backgrounds, and 2D overlays.
- **3D Textures (`3d/`)**: terrain, grass, architecture, characters, and ship textures (works in both **OpenGL** and **Compute** renderers).

> This repository contains **no game assets**. Players dump textures locally from their own ROM and load their own replacement packs at runtime.

## Folder layout

By default, textures live inside the game's data folder (`data/textures/` next to `PhantomHourglass.exe` in portable mode, or `<app files>/textures/` on Android). You can also point to a custom directory with `--textures-dir <path>`.

```text
data/textures/
  dump/
    obj/   <hash16>_<w>x<h>.png
    bg/    <hash16>_<w>x<h>.png
    3d/    <hash16>_<w>x<h>.png
  load/
    obj/   <hash16>.png           (or <hash16>_my_label.png)
    bg/    <hash16>.png
    3d/    <hash16>.png
```

## 1. Dumping textures (PC)

Run the game with `--dump-textures`:

```powershell
PhantomHourglass.exe rom\azee.nds --dump-textures
```

Every unique sprite, background, and 3D texture rendered while you play is hashed (64-bit `XXH64` of its decoded pixels) and exported once as a PNG into `data/textures/dump/{obj,bg,3d}/` on a background thread so gameplay does not stutter.

## 2. Creating replacement textures

1. Copy the PNGs you want to replace from `dump/{obj,bg,3d}/` into `load/{obj,bg,3d}/`.
2. Redraw or upscale them to any higher resolution (for example 4× or 8×).
3. Keep the leading 16-character hexadecimal hash in the filename — optional descriptive suffixes after `_` or `-` are allowed (e.g. `a1b2c3d4e5f60708_start_button.png`).
4. Supported formats: `.png` (recommended, with alpha), `.jpg`, `.bmp`, `.tga`. Subfolders inside `load/obj/`, `load/bg/`, and `load/3d/` are scanned recursively.

## 3. Enabling custom textures

### On PC
- Open the side menu (**F1** or **Esc**) and set **Custom textures: On**, or launch with `--custom-textures`.
- **Live hot-reload**: while editing a pack, toggle **Custom textures** `Off` → `On` in the menu to rescan `load/` and immediately refresh all 2D and 3D textures without restarting the game.

### On Android
1. Zip your `load/` folder (containing `obj/`, `bg/`, and/or `3d/`).
2. In the Android launcher, tap **Settings → Import texture pack (.zip)…** and choose the `.zip` file.
3. Custom textures are turned on automatically and can be toggled at any time in the launcher **Settings** or the in-game menu (`≡` → **Display → Custom textures**).
