<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/coax-mark-on-dark.svg">
  <img src="assets/coax-mark-on-light.svg" alt="" width="76" height="76">
</picture>

# Coax

**A Windows live-TV player for Xtream Codes portals, built directly on libmpv.**

Video and interface are composited into one top-level surface, so the window
screenshots, records and screen-shares like any other application — no black
rectangle where the picture should be.

[![ci](https://github.com/jonocairns/coaxn/actions/workflows/ci.yml/badge.svg)](https://github.com/jonocairns/coaxn/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/release/jonocairns/coaxn)](https://github.com/jonocairns/coaxn/releases)
[![licence](https://img.shields.io/github/license/jonocairns/coaxn)](LICENSE)

## Features

- **Xtream Codes login** from a portal URL, username and password, encrypted
  at rest and remembered between runs
- **Channel browser** with live search and provider category grouping, and a
  clipper that keeps tens of thousands of channels scrolling smoothly
- **D3D11 hardware decoding** through libmpv (`hwdec=d3d11va`)
- **NVIDIA RTX Video Super Resolution** via the `d3d11vpp` filter, applied only
  when the source is genuinely smaller than the viewport
- **Bounded recovery.** A generation-scoped supervisor wraps the libmpv owner:
  five attempts inside one 30-second episode, with buffer targets growing from
  1 second while tuning to 10 seconds once playback has been healthy for five
- **Credentials encrypted at rest** with DPAPI, and redacted from logs
- **A quiet update check** against GitHub releases at startup, silent on every
  uninteresting answer — someone trying to watch television does not need to
  hear about a failed update check

## Requirements

- Windows 10 or 11, x64 — the player needs D3D11 and DirectComposition
- An Xtream Codes portal (Coax is a client; it hosts nothing and ships no
  channels)
- An NVIDIA RTX GPU, only if you want super resolution

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

The build cross-compiles from Linux or WSL to Windows x64 using mingw-w64
supplied by nix. Nothing needs to be installed on the Windows side.

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

## Contributing

Commit subjects follow [Conventional Commits](https://www.conventionalcommits.org):
`feat:` bumps the minor version and `fix:` the patch, while `docs:`, `ci:`,
`chore:`, `refactor:`, `build:` and `test:` neither move the version nor appear
in the changelog — release-please hides those sections by default and this repo
does not override that. A breaking change — `feat!:`, `fix!:`, or a
`BREAKING CHANGE:` footer — bumps the major. An unrecognised prefix is treated
as one of the hidden kinds: no bump, no entry. The prefixes are not decoration:
release-please derives the next release from them, so anything a user should
read about in the changelog has to land as `feat:` or `fix:`.

Some files are generated and should not be hand-edited — the version in
`CMakeLists.txt`, the changelog, and everything under `assets/`. See
[AGENTS.md](AGENTS.md) for the full list and the build commands in short form,
and [docs/packaging.md](docs/packaging.md) for packaging and release mechanics.

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

## Background

Coax began as an Electron shell embedding mpv out-of-process through `--wid`,
which put video in a separate window hierarchy: sharing the application window
captured the UI and left a black rectangle where the picture should be. No
amount of reparenting fixed it, because the design was multi-surface by
construction. Hosting libmpv in-process and letting it render into a
composition swap chain under the application's own DirectComposition tree
removes the boundary rather than working around it.

The Electron implementation is frozen. This is the version under development.

## Documentation

| | |
|---|---|
| [PRD.md](PRD.md) | Product requirements and the rationale for the native rewrite |
| [docs/design/live-playback.md](docs/design/live-playback.md) | Live playback, recovery and buffering design |
| [docs/packaging.md](docs/packaging.md) | Packaging, releases, debug symbols, pinned libmpv |
| [AGENTS.md](AGENTS.md) | Build commands, commit-prefix rules, generated files — the short version |

## Licence

[MIT](LICENSE) © Jono Cairns
