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
| `coax-1.0.0-win64-setup.exe` | Most people. Installs to Program Files with a Start Menu entry and an uninstaller. | ~34 MB |
| `coax-1.0.0-win64.zip` | Portable use. Unpack anywhere and run `coax.exe`. | ~46 MB |
| `coax-1.0.0-win64-debug.zip` | Debugging a released build. | ~7 MB |

The installer is smaller than the archive despite carrying the same payload:
NSIS compresses with LZMA where the zip uses deflate.

## Shipping a release

Releasing is merging a pull request and then publishing a draft. No version is
typed, no tag is written by hand, and nothing happens on a development machine.

release-please watches `main` and keeps a release pull request open, rewriting
it on every push. The PR is a running proposal: the next version, derived from
the conventional-commit prefixes since the last release, and the `CHANGELOG.md`
entry that goes with it. Ignore it and it keeps growing. Merge it and that is
the decision to ship.

Merging it makes [`.github/workflows/release.yml`](../.github/workflows/release.yml)
stage a **draft** release and attach all three artifacts to it, after running
the core tests and cross-compiling. Then someone downloads the installer, runs
it on Windows, and presses Publish.

That last step is deliberate and it is the only manual one. The in-app update
check reads `releases/latest`, which skips drafts, so nobody is told to upgrade
until a human has confirmed the build installs. A draft that turns out to be
broken is deleted and no user ever saw it.

What a deleted draft does not undo is the version number. Merging the release PR
already moved `CMakeLists.txt`, `CHANGELOG.md` and the manifest on `main`, and
`force-tag-creation` means the tag exists from that moment too. So abandoning a
draft spends the version: the fix ships as the next one rather than reusing it,
and the dangling tag is cleaned up by hand if it bothers you. That setting is
not optional, incidentally — GitHub does not create a tag for a draft release
until it is published, and without a tag release-please cannot find where the
previous release ended, so the next changelog would repeat commits that had
already shipped.

Three things follow from the prefixes, so they are worth getting right:
`feat:` bumps the minor, `fix:` the patch, and anything else — `docs:`, `ci:`,
`refactor:` — lands in the changelog without moving the version. A breaking
change, written `feat!:` or with a `BREAKING CHANGE:` footer, bumps the major.

### Where the version lives

Still `project(coax_native VERSION ...)` in [CMakeLists.txt](../CMakeLists.txt),
which is what CPack names the artifacts from and what the update check compiles
into the binary. The difference is that release-please owns the line and edits
it in the release PR, so it is never bumped by hand. It finds the line by the
`x-release-please-version` comment **trailing that same line**: the generic
updater works line by line and rewrites only the version it finds on the
annotated line, so a marker sitting on its own line above `project()` matches
nothing and silently updates nothing.

Get that wrong and releases are tagged ahead of the build they contain, which
is why the release job re-reads `CMakeLists.txt` and fails if it disagrees with
the version just released. Note the one case that check cannot see: while the
file and the release happen to hold the same version, they agree whether or not
the annotation works. It is the second release that breaks.

`version.txt` and `.release-please-manifest.json` are release-please's own
bookkeeping. Nothing in the build reads either one; leave both to the bot.

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
