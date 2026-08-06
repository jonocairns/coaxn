# AGENTS.md

Things that are easy to get wrong here, and the rules that are mechanical
rather than stylistic. Reasoning lives in [README.md](README.md),
[docs/packaging.md](docs/packaging.md) and [PRD.md](PRD.md) — this file is the
short version.

## Tools live in nix, not on PATH

- `cmake`, `ninja`, `nsis` and `gh` do not exist outside the nix shells. A bare
  `cmake` is "command not found" — prefix the command, do not install anything.
- Two shells, two build directories:
  - `nix develop --command …` — mingw cross-build to Windows, builds into `build/`.
  - `nix develop .#core --command …` — native compiler for the portable core, builds into `build-core/`.
- Cross-build:
  `cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo`
  then `cmake --build build`.
- Core tests:
  `cmake -S . -B build-core -G Ninja -DCOAX_BUILD_APP=OFF -DBUILD_TESTING=ON`,
  build, then `ctest --test-dir build-core --output-on-failure`.
- `build/coax.exe` runs directly from WSL; there is no Windows runner anywhere
  in this project.

## Commit prefixes decide the next version

- Conventional Commits are wired to the release machinery. release-please reads
  them to compute every version. The prefix is not a label.
- `fix:` → patch. `feat:` → minor. `feat!:`, `fix!:` or a `BREAKING CHANGE:`
  footer → major.
- `docs:`, `ci:`, `chore:`, `refactor:`, `build:`, `test:` → no version change
  **and no changelog entry**. release-please marks those sections hidden by
  default and `release-please-config.json` sets no `changelog-types` override,
  so they are dropped silently. Any other prefix is treated the same way. If a
  change should be visible to users, it has to be `feat:` or `fix:`.
- Choose the prefix for the effect on users, not the size of the diff. A
  one-line behaviour change is `fix:`; a thousand-line rename is `refactor:`.

## Do not hand-edit

- The version in `CMakeLists.txt` — release-please owns that line.
- The `# x-release-please-version` marker: it must stay **trailing on the
  `project()` line**. The updater matches per line, so a marker on its own line
  above `project()` matches nothing and silently stops bumping the version.
- `CHANGELOG.md`, `version.txt`, `.release-please-manifest.json` — bot-owned.
- Everything in `assets/` — `coax.ico` and the two `coax-mark-*.svg` files are
  all generated from one geometry; rerun `python3 scripts/make-icon.py` instead.
  The mark is also drawn a third time, in `theme::draw_logo`, and the constants
  there and in the script are meant to agree.
- Changing `assets/coax.ico` does not rebuild the resource on its own: ninja
  does not track it as an input to the RC step, so `rm -f
  build/CMakeFiles/coax.dir/coax.rc.res` before rebuilding or the executable
  keeps the old icon and the build still looks green.
- Never create a tag or a GitHub release by hand. Releasing is merging the
  release PR, then publishing the draft it stages.

## Keep the portable core portable

- `coax_core` is `src/core/*.cpp` plus `src/util/redact.cpp`. It must not
  reference Windows, libmpv, ImGui or any UI type.
- `coax_player_core` sits above it: the half of `src/player/` that links
  nothing. It may name player concepts `coax_core` must not, but it is built by
  the same native GCC and the same rule applies — a Windows or libmpv include
  breaks that build even when the cross-build is still green.
- Only `src/player/mpv_player.cpp` needs libmpv, and it is the whole of
  `coax_mpv`. A new file under `src/player/` belongs in `coax_player_core`
  unless it genuinely cannot.
- CI executes everything both native targets build: `coax_core_tests` and
  `coax_player_core_tests`. Anything added to either library needs a test under
  `test/`.
