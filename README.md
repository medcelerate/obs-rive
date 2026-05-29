# obs-rive

An OBS Studio plugin that renders [Rive](https://rive.app) animations
(`.riv` files) as a video source — feeding the same Rive C++ runtime +
GPU renderer used by [TDRive](../TDRive) into OBS via an async video source.

- **macOS** (arm64) — Metal backend, `obs-rive.plugin`
- **Windows** (x64) — D3D11 backend, `obs-rive.dll`
- **Linux** (x64) — GL backend (EGL pbuffer + desktop GL), `obs-rive.so`

## Layout

```
obs-rive/
├── src/
│   ├── plugin-main.cpp          # OBS module entry points
│   ├── rive-source.{cpp}        # obs_source_info, properties, video_tick
│   ├── rive-core.{h,cpp}        # framework-agnostic Rive plumbing
│   ├── IBackend.h               # backend interface
│   ├── backend_metal.mm         # macOS Metal
│   ├── backend_d3d11.cpp        # Windows D3D11
│   └── backend_gl.cpp           # Linux EGL + desktop GL
├── data/locale/en-US.ini        # OBS UI strings
├── scripts/
│   ├── build_rive.sh            # macOS + Linux Rive build
│   └── build_rive.bat           # Windows Rive build
├── third_party/rive-runtime/    # cloned by build_rive.{sh,bat}
└── CMakeLists.txt
```

## Architecture

`obs-rive` runs Rive's renderer **outside** OBS's graphics subsystem — we
own a Metal / D3D11 / GL context, render the artboard offscreen, read pixels
back to CPU, and hand the BGRA buffer to OBS via `obs_source_output_video()`.

That keeps interop simple at the cost of one GPU→CPU readback per frame.
For higher throughput a future v2 could share textures with `gs_texture_t`,
but the async path is the same one OBS's own Image / Media sources use.

The `IBackend` abstraction is identical to the one in
[TDRive](../TDRive) — only the host glue layer differs.

## Prerequisites

| Platform | Tools |
|---|---|
| macOS    | Xcode CLT, `brew install premake cmake obs` |
| Windows  | Visual Studio 2022 (C++ workload), [`premake5.exe`](https://github.com/premake/premake-core/releases) on `PATH`. OBS doesn't publish a Windows SDK, so for local builds either install OBS via the installer with the "Plugin Development Kit" component checked, or follow the same clone-and-build-libobs flow CI uses (see `.github/workflows/build.yml`). |
| Linux    | `build-essential cmake ninja-build clang libobs-dev libegl1-mesa-dev libgl1-mesa-dev`, plus `premake5` ([Linux release zip](https://github.com/premake/premake-core/releases) — Ubuntu has no package). |

## Build

Step 1 builds the Rive runtime once per Rive commit (5–10 min, cached on CI).

**macOS / Linux**
```sh
./scripts/build_rive.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DLIBOBS_PATH="$(brew --prefix obs)"   # macOS
cmake -B build -DCMAKE_BUILD_TYPE=Release                                       # Linux (libobs-dev gives pkg-config)
cmake --build build --config Release
```

**Windows**
```bat
scripts\build_rive.bat
cmake -B build -G "Visual Studio 17 2022" -A x64 -DLIBOBS_PATH="C:\Program Files\obs-studio"
cmake --build build --config Release
```

## Install

Drop the produced binary into OBS's plugin path:

- **macOS**: `~/Library/Application Support/obs-studio/plugins/obs-rive.plugin`
- **Windows**: `%PROGRAMFILES%\obs-studio\obs-plugins\64bit\obs-rive.dll`
- **Linux**: `~/.config/obs-studio/plugins/obs-rive/bin/64bit/obs-rive.so`

Restart OBS, then add **+ → Rive Source** in any scene.

## Source properties

| Property | What it does |
|---|---|
| Riv File | File picker for the `.riv`. |
| Reload | Re-reads the file from disk. |
| Artboard | Dropdown populated from the loaded file. |
| State Machine | Dropdown populated from the chosen artboard. |
| Width / Height | Output texture size. |
| Fit / Alignment | Same Fit + 3×3 anchor as the Rive editor. |
| Speed | Playback multiplier. |
| Background | RGBA clear color. Alpha = 0 → transparent output. |
| State Machine Inputs | One control per SMI input found in the file (float / toggle / pulse-via-toggle). |
| View Model | One control per VM property (string / number / bool / trigger). |

The dropdowns and dynamic input/VM groups rebuild whenever the file or
artboard changes — no manual wiring.

### Triggers

OBS doesn't have a dedicated pulse control, so trigger-typed inputs and VM
properties show as a toggle. **Switching the toggle ON fires the trigger.**
Switch it OFF and ON again to fire it again.

## Status

This is a first cut. macOS + Windows mirror the TDRive code paths and link
cleanly. Linux's GL backend is implemented but unverified — I expect 1–2
rounds of "missing GL extension / EGL config" issues on the first real run.
PRs welcome.
