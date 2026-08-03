# Coax

A Windows live-TV player built directly on libmpv, where video and UI are
composited into **one** top-level application surface.

That single surface is the point. Coax began as an Electron shell embedding mpv
out-of-process through `--wid`, which put video in a separate window hierarchy:
sharing the application window captured the UI and left a black rectangle where
the picture should be. No amount of reparenting fixed it, because the design was
multi-surface by construction. Hosting libmpv in-process and letting it render
into a composition swap chain under the application's own DirectComposition tree
removes the boundary rather than working around it.

The Electron implementation is frozen. This is the version under development.

## Features

- Xtream Codes login, from a pasted portal link or explicit URL/username/password
- Channel list with live search and provider category grouping
- D3D11 hardware decoding via libmpv (`hwdec=d3d11va`)
- NVIDIA RTX Video Super Resolution through the `d3d11vpp` filter, applied only
  when the source is genuinely smaller than the viewport
- A bounded, generation-scoped recovery supervisor around the libmpv owner:
  five attempts inside one 30-second episode, with buffer targets that grow from
  1 second while tuning to 10 seconds once playback has been healthy for five
- Credentials encrypted at rest with DPAPI, and redacted from logs
- Startup update check against GitHub releases, silent on every uninteresting
  answer — someone trying to watch television does not need to hear about a
  failed update check

## Installing

Download the installer or the portable archive from
[Releases](https://github.com/jonocairns/coaxn/releases). The binary is
unsigned, so SmartScreen warns anyone who did not build it themselves.

## Controls

| Key | Action |
|---|---|
| `Tab` | Show/hide the channel list |
| `F1` | Show/hide diagnostics |
| `Space` | Pause/resume |
| `Alt`+`Enter` | Toggle fullscreen |
| `Esc` | Leave fullscreen |

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

The executable and `libmpv-2.dll` land in `build/`, and from WSL you can launch
it directly with `./build/coax.exe`.

The portable model and its tests use a native compiler and require neither
Windows nor libmpv:

```bash
nix develop .#core --command bash -c \
  'cmake -S . -B build-core -G Ninja -DCOAX_BUILD_APP=OFF -DBUILD_TESTING=ON && \
   cmake --build build-core && ctest --test-dir build-core --output-on-failure'
```

Commit subjects follow [Conventional Commits](https://www.conventionalcommits.org):
`feat:` bumps the minor version, `fix:` the patch, and everything else lands in
the changelog without moving the version. The prefixes are not decoration —
release-please derives the next release from them.

Packaging and release mechanics live in [docs/packaging.md](docs/packaging.md).

## Architecture

```
src/
├── core/      portable playback supervisor, health fold, channel model
├── xtream/    Xtream Codes client (WinHTTP + JSON)
├── player/    libmpv wrapper: composition output, diagnostics, event pump
├── util/      HTTP, logging, credential redaction
├── win/       window, DirectComposition tree, D3D11 UI layer, DPAPI storage
└── app/       glue and the ImGui frame loop
```

`core/` is kept free of Windows and UI types on purpose: it is the part a second
platform would reuse unchanged, and it is the part covered by tests that run
without Windows or libmpv.

### How the single surface works

mpv is configured with `d3d11-output-mode=composition`, so it renders into a
composition swap chain instead of creating a window. Coax observes the
`display-swapchain` property and attaches that swap chain to a
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

## Documentation

| | |
|---|---|
| [PRD.md](PRD.md) | Product requirements and the rationale for the native rewrite |
| [docs/design/live-playback.md](docs/design/live-playback.md) | Live playback, recovery and buffering design |
| [docs/packaging.md](docs/packaging.md) | Packaging, releases, debug symbols, pinned libmpv |
