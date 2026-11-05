# sc2_xb_shim
A x86 Shim for Soulcalibur II on Xbox

After putting much thought into it, I decided we needed a better solution for running SC2 on Xbox. At first, I tried to see if I could help get Cxbx-Reloaded fixed to work with SC2, but without much avail. So, with all the Ghidra work I’ve done on the game, I decided maybe I could make some sort of amalgam.

Cxbx-Reloaded is basically a shim for Xbox games, and some do work (Half-Life 2 being one I used a lot for turbo testing). The nice thing was, Namco left a lot of strings and whatnot—even the primitives were right out in the open. I worked with AI to make an HLE replacement for the entire renderer, and sound too (don’t have a special DSP chip to do magic). This is bare-bones, but damn does it do its job... with a few issues cropping up, but nonetheless, it will help with making content for SC2 Xbox and the latter (got to make assets in one place and get them refined for the other two).

I am open to anyone making forks and improving it. Hell, I made this to spite the whole RECOMP thing—like come on, Xbox code is basically PC code with specially formulated GPU code and sounds, but it's still x86 code that you can run on your desktop or laptop. Now, for those on mobile devices, maybe this could work with box86 or FEX... SC2 runs on just a Pentium III after all, and there is a lot of HLE that could be done to remove x87 operations.

Ah, enough blabbering. Though this is just the tip of the iceberg, I do plan on working toward a functionally compatible decompilation—something along the lines of replacing things piece by piece until, well, everything is its own C/C++. Kinda a fever dream, but I want to make this game run on anything I can get my hands on.

I do need feedback on the behavior of the project on devices other than my own, as I’ve only tested it on my desktop and laptop (and my laptop had issues GPU-driver-wise).  
  
Oh and why not use XEMU. Well for me, i need speedy OLK rebuild and not have to keep a cmd open to rebuild the xiso yet another time.  That and it makes my PC rev up big time.

---

## Compatibility Notice

> [!IMPORTANT]
> **NTSC-U Only:** At the moment, this shim only supports the **NTSC-U (USA)** release of *SoulCalibur II* (including *SoulCalibur II Plus* mods based on the NTSC-U `default.xbe`). PAL and NTSC-J releases have different function/data addresses and are not currently supported.

---

## Setup & Usage

1. Extract your NTSC-U *SoulCalibur II* Xbox game files (`default.xbe` and game data folders/archives) into a folder.
2. Place `NativeSC2.exe` and `NativeSC2_core.dll` in the **same folder** as `default.xbe`.
3. Run `NativeSC2.exe` (save data is stored automatically in `Save\UDATA\` inside the game directory).

(Word of warning, when testing this on my Ryzen APU, Direct X 9 code did not play well. After installing D3D9On12 dll everything worked good, Also DXVK could work in replacment too thoe i have not yet tested it)

### Command-Line Arguments

You can pass command-line arguments to `NativeSC2.exe` to configure internal rendering resolution, scaling, window mode, or a custom `.xbe` path:

```powershell
NativeSC2.exe [optional_path_to.xbe] [options]
```

#### Resolution Presets
| Flag | Internal Resolution |
| :--- | :--- |
| `--720p` / `-720p` | `1280x720` (Default) |
| `--1080p` / `-1080p` | `1920x1080` |
| `--1440p` / `-1440p` / `--2k` / `-2k` | `2560x1440` |
| `--4k` / `-4k` / `--2160p` / `-2160p` | `3840x2160` |

#### Resolution Scaling & Custom Dimensions
| Flag | Description | Example |
| :--- | :--- | :--- |
| `--scale <factor>` / `--scale=<factor>` / `-s <factor>` | Scales internal resolution (`1.5` = 1080p, `2` = 1440p, `3` or `6` = 4K) | `NativeSC2.exe --scale 3` |
| `--res <WxH>` / `--res=<WxH>` / `--resolution=<WxH>` | Sets an arbitrary internal resolution (`320x200` up to `7680x4320`) | `NativeSC2.exe --res 3840x2160` |
| `--width <W>` (`-w`) / `--height <H>` (`-h`) | Sets width or height individually | `NativeSC2.exe -w 2560 -h 1440` |

#### Display & Modding Options
| Flag | Description |
| :--- | :--- |
| `--fullscreen` / `-fullscreen` / `--borderless` / `-f` | Launches in borderless fullscreen mode (or toggle anytime with **F12** / **Alt+Enter**) |
| `--log-files` / `--log` / `-l` | Live-logs all OLK container and sub-file loads in real time |

### Built-In HLE OLK Mod Loader

`NativeSC2` includes a built-in HLE OLK file replacement engine (compatible with the `SoulTools-2` 1-based OLK folder layout) with automatic in-RAM TOC expansion and live hot-reloading—no `.olk` or XISO rebuilding required:

1. On first launch, a `mods\` folder is automatically created in your game directory with subfolders for each OLK container (`mods_xbox\` is also supported):
   - `1_root` (`root`)
   - `2_motinfo` (`motinfo`)
   - `3_cpudata` (`cpudata`)
   - `4_cdata` (`cdata`)
   - `5_stage` (`stage`)
   - `6_human` (`human`)
2. Drop any replacement file named with its **1-based file number** (e.g., `001.bin`, `file_042.dat`) into the corresponding container folder.
3. Files modified or added while the game is running are **hot-reloaded automatically** on the next stage/character load (or press **F5** at any time to force an immediate rescan).

---

## Building from Source

### Prerequisites
- **Windows (x32)**
- **LLVM / Clang (`clang` & `clang++`)** in your system `PATH` with 32-bit x86 (`-m32`) target support
- **Visual Studio / Windows SDK Build Tools** (for the 32-bit Windows & Direct3D 9 import libraries: `kernel32`, `user32`, `gdi32`, `winmm`, `opengl32`, `d3d9`)

### Build Steps

#### Option A: CMake (Visual Studio / ClangCL)
```cmd
cmake -B build -A Win32 -T ClangCL
cmake --build build --config Release
```

#### Option B: Batch Script (`build.bat`)
Run [`build.bat`](build.bat) from the repository root:

```cmd
build.bat
```

Both methods compile two binaries into the `bin\` directory:
1. **`bin\NativeSC2_core.dll`** — Core compatibility shim, Xbox kernel/XAPI HLE, Direct3D 9 NV2A renderer, and DirectSound audio bridge (linked at base `0x10000000`).
2. **`bin\NativeSC2.exe`** — Low-memory bootstrap loader (linked at fixed base `0x00010000` with a 128 MB reservation so the original Xbox `.xbe` sections map into their native virtual addresses).

### Automated Builds & Releases (GitHub Actions)
This repository includes a GitHub Actions workflow ([`.github/workflows/build.yml`](.github/workflows/build.yml)) that:
- Automatically compiles `NativeSC2.exe` and `NativeSC2_core.dll` via CMake on every push and pull request, uploading `sc2_xb_shim-win32` as a workflow artifact.
- Automatically attaches `sc2_xb_shim-win32.zip`, `NativeSC2.exe`, and `NativeSC2_core.dll` to GitHub Releases whenever a version tag (`v*`, e.g. `v1.0.0`) is pushed or a release is published.
