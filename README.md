# Coax Native — proof of concept

A Windows live-TV player built directly on libmpv, where video and UI are
composited into **one** top-level application surface so the window can be
screen-shared without a black video region.

This is a proof of concept. It deliberately trades test coverage and stream
recovery for a working end-to-end slice: log in to an Xtream Codes portal,
find a channel, play it with hardware decoding and NVIDIA super resolution,
and share the window.

## What works

- Xtream Codes login from a pasted portal link or explicit URL/username/password
- Channel list with live search and provider category grouping
- D3D11 hardware decoding via libmpv (`hwdec=d3d11va`)
- NVIDIA RTX Video Super Resolution via the `d3d11vpp` filter, applied only
  when the source is genuinely smaller than the viewport
- Video and UI in one DirectComposition tree under one HWND
- Credentials encrypted at rest with DPAPI, never written to the log

## What is deliberately missing

Stream recovery. There is no retry policy, no reconnect budget and no
generation-scoped channel switching yet — a dropped stream stays dropped until
you pick a channel again. Buffering and network timeouts are whatever mpv
defaults to. See the PRD for the shape this takes when it matters.

## Building

The build cross-compiles from Linux/WSL to Windows x64 using mingw-w64 supplied
by nix. Nothing needs to be installed on the Windows side.

```bash
nix develop
```

Then, inside the shell:

```bash
./scripts/fetch-libmpv.sh
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

The executable and `libmpv-2.dll` land in `build/`. From WSL you can launch it
directly:

```bash
./build/coax.exe
```

### The pinned libmpv

`scripts/fetch-libmpv.sh` pins a specific upstream mpv commit
(`304426c390901436fb1d4a63efbd582ae80c88f4`), not a tagged release. The
composition presentation path this project depends on — `d3d11-output-mode`,
`d3d11-composition-size` and the `display-swapchain` property — landed after
v0.41.0, so no stable release is known to carry it. The commit and its source
URL are recorded in `third_party/mpv/PINNED.txt` after fetching.

## Controls

| Key | Action |
|---|---|
| `Tab` | Show/hide the channel list |
| `F1` | Show/hide diagnostics |
| `Space` | Pause/resume |
| `Alt`+`Enter` | Toggle fullscreen |
| `Esc` | Leave fullscreen |

## Architecture

```
src/
├── core/      portable channel model and filtering — no Windows types
├── xtream/    Xtream Codes client (WinHTTP + JSON)
├── player/    libmpv wrapper: composition output, diagnostics, event pump
├── win/       window, DirectComposition tree, D3D11 UI layer, DPAPI storage
└── app/       glue and the ImGui frame loop
```

`core/` is kept free of Windows and UI types on purpose: it is the part a
second platform would reuse unchanged.

### How the single surface works

mpv is configured with `d3d11-output-mode=composition`, so it renders into a
composition swap chain instead of creating a window. Coax observes the
`display-swapchain` property, and attaches that swap chain to a
DirectComposition visual it owns. The ImGui layer draws into a second,
transparent swap chain in the same visual tree. Both sit under one HWND.

The swap chain is observed rather than polled because the property reports
unavailable until mpv's video output is configured — polling after
`mpv_initialize` races startup.

### Diagnostics honesty

The diagnostics overlay reports super resolution as **requested** and **filter
attached** separately, and never claims it was **confirmed**. Attaching the
`d3d11vpp` filter does not mean the driver ran RTX VSR on a frame; no signal
exposing that is available, so the UI says so rather than guessing.
