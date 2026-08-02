# Coax Native — proof of concept

A Windows live-TV player built directly on libmpv, where video and UI are
composited into **one** top-level application surface so the window can be
screen-shared without a black video region.

This is a proof of concept with a portable playback-health fold and bounded,
generation-scoped recovery supervisor around the native libmpv owner.

## What works

- Xtream Codes login from a pasted portal link or explicit URL/username/password
- Channel list with live search and provider category grouping
- D3D11 hardware decoding via libmpv (`hwdec=d3d11va`)
- NVIDIA RTX Video Super Resolution via the `d3d11vpp` filter, applied only
  when the source is genuinely smaller than the viewport
- Video and UI in one DirectComposition tree under one HWND
- Credentials encrypted at rest with DPAPI, never written to the log

Recovery uses five attempts (`500, 1000, 2000, 4000, 5000` ms) inside one
30-second episode. Buffer targets move from 1 second while tuning to 10 seconds
after five healthy seconds; the static cache ceiling is 64 MiB. FFmpeg reconnect
remains disabled for continuous live TS and socket timing stays at the pinned
runtime default.

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

The portable model and tests use a native compiler and require neither Windows
nor libmpv:

```bash
nix develop .#core --command bash -c \
  'cmake -S . -B build-core -G Ninja -DCOAX_BUILD_APP=OFF -DBUILD_TESTING=ON && \
   cmake --build build-core && ctest --test-dir build-core --output-on-failure'
```

The executable and `libmpv-2.dll` land in `build/`. From WSL you can launch it
directly:

```bash
./build/coax.exe
```

## Packaging and releases

`coax.exe` links everything but libmpv statically, and its only other imports
are stock Windows DLLs, so a release is two files. One command builds every
artifact:

```bash
nix develop --command cmake --build build --target package
```

The target depends on `all`, so it cannot package a stale binary. It writes
three things into `build/`:

| Artifact | For | Size |
|---|---|---|
| `coax-0.1.0-win64-setup.exe` | Most people. Installs to Program Files with a Start Menu entry and an uninstaller. | ~34 MB |
| `coax-0.1.0-win64.zip` | Portable use. Unpack anywhere and run `coax.exe`. | ~46 MB |
| `coax-0.1.0-win64-debug.zip` | Debugging a released build. | ~7 MB |

The installer is smaller than the archive despite carrying the same payload:
NSIS compresses with LZMA where the zip uses deflate.

### Shipping a release

Push a version tag. [`.github/workflows/release.yml`](.github/workflows/release.yml)
runs the core tests, cross-compiles, packages, and attaches all three artifacts
to a GitHub release:

```bash
git tag v0.2.0 && git push origin v0.2.0
```

The version lives in `project(coax_native VERSION ...)` in
[CMakeLists.txt](CMakeLists.txt), and **the tag must match it**. The workflow
names the expected artifacts from the tag and fails if they are missing, which
is deliberate: the in-app update check compares the release tag against the
version compiled into the binary, so a tag ahead of `CMakeLists.txt` would tell
everyone running the new build to upgrade to the version they already have, and
the prompt would never clear.

### Update notifications

On startup the app asks GitHub for the latest release on a background thread.
If it is newer than the running build, a dismissable notice appears offering
the download page. Every uninteresting answer — offline, rate-limited, no
releases, an unreadable tag — is silent: someone trying to watch television
does not need to hear about a failed update check.

Tag comparison lives in `core/version.{hpp,cpp}` with the rest of the portable
logic, so it is covered by the tests that run without Windows or libmpv.
Pre-release tags deliberately fail to parse, and `releases/latest` excludes
them anyway, so a tagged beta never prompts anyone.

### Debug symbols

A `RelWithDebInfo` executable carries about 25 MiB of DWARF, so the packaged
binary is stripped and its debug info split into its own archive rather than
being dropped. The runtime archive and the debug archive share a folder name:
unpack the debug one over the runtime one and `coax.debug` lands beside the
executable, which is where the `.gnu_debuglink` section points a debugger. It
is DWARF, so gdb reads it and WinDbg does not. `build/coax.exe` itself is left
unstripped and stays directly runnable and debuggable.

### Icon and metadata

The executable carries an icon and version metadata from `src/win/coax.rc.in`.
`assets/coax.ico` is generated rather than committed opaque — rerun
`python3 scripts/make-icon.py` after editing the shape or colours in it.

### Known gaps

The binary is unsigned, so SmartScreen warns anyone who did not build it
themselves; fixing that needs a code-signing certificate, not a build change.
`scripts/fetch-libmpv.sh` pins libmpv by tag and URL but does not verify a
checksum, so the 117 MB blob that makes up most of a release is not
content-verified. And a release stays two files: one self-contained `.exe`
would mean embedding `libmpv-2.dll` as a resource and extracting it on first
run, since static libmpv would mean building ffmpeg and its dependency tree
here.

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
