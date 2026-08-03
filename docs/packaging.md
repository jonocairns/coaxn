# Packaging and releases

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

## Shipping a release

Push a version tag. [`.github/workflows/release.yml`](../.github/workflows/release.yml)
runs the core tests, cross-compiles, packages, and attaches all three artifacts
to a GitHub release:

```bash
git tag v0.2.0 && git push origin v0.2.0
```

The version lives in `project(coax_native VERSION ...)` in
[CMakeLists.txt](../CMakeLists.txt), and **the tag must match it**. The workflow
names the expected artifacts from the tag and fails if they are missing, which
is deliberate: the in-app update check compares the release tag against the
version compiled into the binary, so a tag ahead of `CMakeLists.txt` would tell
everyone running the new build to upgrade to the version they already have, and
the prompt would never clear.

## Update notifications

On startup the app asks GitHub for the latest release on a background thread.
If it is newer than the running build, a dismissable notice appears offering
the download page. Every uninteresting answer — offline, rate-limited, no
releases, an unreadable tag — is silent: someone trying to watch television
does not need to hear about a failed update check.

Tag comparison lives in `core/version.{hpp,cpp}` with the rest of the portable
logic, so it is covered by the tests that run without Windows or libmpv.
Pre-release tags deliberately fail to parse, and `releases/latest` excludes
them anyway, so a tagged beta never prompts anyone.

## Debug symbols

A `RelWithDebInfo` executable carries about 25 MiB of DWARF, so the packaged
binary is stripped and its debug info split into its own archive rather than
being dropped. The runtime archive and the debug archive share a folder name:
unpack the debug one over the runtime one and `coax.debug` lands beside the
executable, which is where the `.gnu_debuglink` section points a debugger. It
is DWARF, so gdb reads it and WinDbg does not. `build/coax.exe` itself is left
unstripped and stays directly runnable and debuggable.

## Icon and metadata

The executable carries an icon and version metadata from `src/win/coax.rc.in`.
`assets/coax.ico` is generated rather than committed opaque — rerun
`python3 scripts/make-icon.py` after editing the shape or colours in it.

## The pinned libmpv

`scripts/fetch-libmpv.sh` pins a specific upstream mpv commit
(`304426c390901436fb1d4a63efbd582ae80c88f4`) rather than a tagged release. The
reason is artifact availability, not a missing feature: the tag carries the
composition path, but none of upstream's Windows archives ships a libmpv DLL,
import library or headers, so the pin follows a build that publishes a
development package.

PRD §7.1 is the canonical statement of this and records the checks behind both
halves. It is deliberately the only place the argument is made in full — the
same reasoning written out here and in the fetch script drifted apart once
already.

The commit and its source URL are recorded in `third_party/mpv/PINNED.txt`
after fetching.

## Known gaps

The binary is unsigned, so SmartScreen warns anyone who did not build it
themselves; fixing that needs a code-signing certificate, not a build change.
`scripts/fetch-libmpv.sh` pins libmpv by tag and URL but does not verify a
checksum, so the 117 MB blob that makes up most of a release is not
content-verified. And a release stays two files: one self-contained `.exe`
would mean embedding `libmpv-2.dll` as a resource and extracting it on first
run, since static libmpv would mean building ffmpeg and its dependency tree
here.
