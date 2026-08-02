---
description: Task specification for porting the recovery supervisor and playback-health model from the Electron implementation into the native C++ build.
tags: [handoff, plan, supervisor, recovery, health, playback, port, mpv]
---

# Task: port the playback supervisor and health model to Coax Native

Port the recovery supervisor and playback-health model from the Electron
implementation (`/home/jonoc/coax`, TypeScript) into the native rewrite
(`/home/jonoc/coaxn`, C++20). The native player, frame loop and live-offset
controller already exist; this task adds the remaining policy layer and the
lossless adapter events needed to drive it.

**Port the behaviour, not the syntax.** The TypeScript encodes years of contact
with real provider streams, and nearly every constant carries a comment
explaining the evidence that set it. Preserve the semantics and the reasoning.
Do not transliterate TypeScript idiom into C++, and do not "improve" a decision
silently. Raise a proposed behaviour change and record the evidence for it.

## Read first

Start with `/home/jonoc/coax/src/main/supervisor/policy.ts`. It is short, and
every constant is justified in a comment. It is the best available description
of how this system is meant to behave.

Then read the pure model before the controller integration:

1. `src/main/mpv/playback-health.ts`
2. `src/main/supervisor/state-machine.ts`
3. `src/main/supervisor/effects.ts`
4. `src/main/supervisor/host.ts`

## Scope

### In

| File | Contains |
|---|---|
| `src/main/supervisor/policy.ts` | Recovery, buffer and health constants, each with rationale |
| `src/main/supervisor/state-machine.ts` | Recovery reducer — the core of the port |
| `src/main/supervisor/effects.ts` | Effects the reducer emits |
| `src/main/supervisor/host.ts` | Clock and deadline ownership |
| `src/main/supervisor/diagnostics.ts` | Supervisor diagnostic projection |
| `src/main/mpv/playback-health.ts` | Health fold over normalized observations |
| `src/main/mpv/stream-stats.ts`, `src/shared/stream-stats.ts` | Observation, verdict and buffer-phase types |
| `src/shared/generation.ts` | Generation scoping |
| Selected behaviour from `src/main/mpv/controller.ts` | Event normalization, sampling cadence and buffer-phase application |

### Out

Electron main/preload/renderer boundaries, browser IPC contracts, named-pipe
ownership, child PIDs, heartbeat plumbing and Electron process supervision are
out of scope. `src/main/mpv/controller.ts` is not a transliteration target.

The source action named `restart-process` expresses a recovery intent, not a
required native mechanism. Translate it to a platform-neutral native effect such
as `recreate-player`: destroy and reinitialize the in-process libmpv owner while
leaving the UI alive. Port the retry bound, generation ownership and controlled
failure cases; do not port named-pipe, PID or child-process mechanics.

## Time and units

**Time must be injected, never read by the model.** The source signature is
`foldPlaybackHealth(previous, observation, now, options)`, and
`reduceSupervisorState(state, event, now, policy)` follows the same rule. Core
logic must not call `std::chrono::steady_clock::now()` internally. The host reads
the clock and supplies `now`, so every deadline remains deterministic in tests.

Use strong C++ time types for control-plane time:

```cpp
using Duration = std::chrono::duration<double>;
using TimePoint = std::chrono::time_point<std::chrono::steady_clock, Duration>;
```

Policy delays and windows are durations; absolute deadlines and observations
are time points. Tests construct both directly. Do not store retry milliseconds,
deadlines and absolute timestamps as interchangeable `double` values.

mpv media properties such as playback position, cache end and buffer depth
arrive as seconds. Normalize them at the player boundary and retain `Seconds` in
every field name (or use a small strong wrapper). Convert the source `*Ms`
policy values exactly once:

- `attemptDelaysMs`, `wallClockBudgetMs`, `steadyHealthyWindowMs`
- `sampleIntervalMs`, `stallConfirmationMs`, `openStallConfirmationMs`
- `decodeStallConfirmationMs`

The source values already named `*Seconds` remain seconds:
`cacheSeconds`, `readaheadSeconds`, `discontinuityJumpSeconds`,
`progressEpsilonSeconds` and `depletedBufferSeconds`.

## Architecture and dependency direction

New policy code belongs in `src/core/`. It must not include `windows.h`,
`mpv/client.h`, ImGui or any application/UI header. Core consumes normalized
data and returns decisions; it neither queries mpv nor performs effects.

Enforce the boundary with CMake targets rather than relying only on review:

```text
coax_core
  policy, health fold, supervisor reducer/host, generations, diagnostics
  no Windows, mpv or UI dependency

coax_player
  libmpv adapter, normalized observations/events, effect execution
  depends on coax_core and libmpv

coax
  App, Windows and UI
  depends on coax_player

coax_core_tests
  Catch2 tests
  depends only on coax_core
```

Move the top-level non-Windows fatal check and all Windows-only dependency/target
setup in `CMakeLists.txt` behind the Windows application option. A host-native
build must not fetch or configure libmpv, ImGui or D3D targets merely to compile
`coax_core` and run `coax_core_tests`; this is the mechanical proof that the
portable layer has not acquired a platform dependency.

### Levels and edges are different contracts

`Diagnostics` is a snapshot of the latest levels: cache depth, paused state,
health verdict and projected supervisor state. Do not use one mutable snapshot
field to represent transient player events.

Add a lossless, generation-scoped adapter contract for edges, for example:

```cpp
struct PlayerEvent {
    Generation generation;
    PlayerEventPayload payload; // closed std::variant of event structs
};
```

The exact storage may be a queue, callback or sequence-numbered journal, but it
must preserve event order and cannot collapse two events consumed by one
`MpvPlayer::pump()` call. At minimum normalize:

- load command accepted or rejected;
- first playback start for the load;
- end-file with mpv's structured reason and error;
- intentional stop versus unexpected end;
- backend shutdown/failure that can justify `recreate-player`;
- property-command results for buffer-phase changes.

Stamp the generation when the load or asynchronous request is issued, not when
its reply happens. Late replies from an older load must therefore remain stale
even if the active generation has changed.

Do not promote mpv log text into core events. mpv documents log messages as
human diagnostics whose wording can change. If a required transport
classification has no structured signal, keep an exact pinned-runtime parser in
`coax_player`, sanitize its output, and cover every accepted pattern with an
adapter test. Core sees only the closed classification.

## Clean-code invariants

- Model events and effects with `enum class`, small structs and `std::variant`;
  make switch handling exhaustive.
- Use a strong `Generation` value type rather than an unlabelled integer.
- Use `std::optional` for unavailable observations; zero is a real media value,
  not a missing-value sentinel.
- Keep the reducer and health fold free of logging, timers, callbacks, player
  commands, global state and hidden clock reads.
- The host owns exactly one deadline timer, always re-derived from state.
- The player adapter owns libmpv and is the only layer that includes its API.
- Health state resets per load. Recovery budget resets only after the tested
  steady window. Per-channel and lifetime diagnostic counters must be named and
  reset deliberately.
- Comments record provider evidence and non-obvious rationale; they do not
  narrate syntax.
- Prefer small value types and free functions over a new inheritance hierarchy.
- Enable `-Wall -Wextra -Wpedantic` on `coax_core`; do not add blanket warning
  suppressions to make the new core compile.

## Suggested order

1. Create the CMake target boundaries and host-native core-test path.
2. Add strong observation, event, decision, generation and policy types. No
   logic yet.
3. Port the pure health fold and its tests.
4. Port the pure reducer, diagnostic projection and their tests.
5. Port the host's single-timer behaviour with a fake clock.
6. Add the generation-scoped `PlayerEvent` adapter and effect executor.
7. Wire sampling and events into `App` after `player_.pump()`.
8. Apply and confirm buffer-phase transitions.
9. Reconcile `docs/design/live-playback.md` with the implemented policy.

Steps 3–5 must pass without libmpv, media files, Windows or a UI.

## What already exists — do not rebuild

- `src/player/live_sync.{hpp,cpp}` — live-offset control via playback speed, a
  port of ExoPlayer's `DefaultLivePlaybackSpeedControl`. Reset it on channel
  change; do not duplicate it in the supervisor.
- `src/player/mpv_player.{hpp,cpp}` — in-process libmpv wrapper. It already
  observes `demuxer-cache-duration`, `paused-for-cache`, `core-idle`,
  `width`/`height` and `hwdec-current`, and measures tune-in time. Extend the
  adapter with the remaining normalized observations and lossless events.
- `src/app/app.cpp` — frame loop; calls `player_.pump()` before controller
  updates.

The existing `MPV_EVENT_PLAYBACK_RESTART` diagnostic counter is not the ported
health model's discontinuity counter. mpv restart events and classified
timeline splices are different signals. Either remove the legacy counter in
favour of the health model or expose two explicitly named counters; never add
them together.

## Behaviour that must survive the port

Each rule below was learned against real provider streams or deterministic
fault fixtures. Breaking one reintroduces a known failure.

1. **No FFmpeg reconnect.** `PlayerConfig::transport_reconnect` stays `false`.
   Reconnect reopens a non-seekable live stream at the wrong position and can
   replay already-watched content. Recovery is a supervisor-owned fresh load.

2. **Leave socket timing at the pinned runtime default.** Remove the native
   `network-timeout=20` override. The health/progress model owns prolonged
   silence; do not add a fixture-oriented per-file deadline beneath it.

3. **Do not shorten stream probing.** `analyzeduration` and `probesize` stay at
   runtime defaults. Provider MPEG-TS needs a full PMT before a track list
   exists; truncated analysis makes slow-signalling channels unplayable.

4. **No stall may be inferred from a single mpv property.** The fold requires
   several signals to agree across several samples. Preserve every threshold
   and minimum-observation count from `DEFAULT_HEALTH_POLICY`.

5. **Classify discontinuities by deviation from expected progress.** For two
   adjacent samples:

   ```text
   moved = currentPlayback - previousPlayback
   expected = max(0, currentObservedAt - previousObservedAt)
   discontinuity = abs(moved - expected) > 1 second
   ```

   A ten-second movement over ten seconds is normal. A forward or backward
   movement more than one second away from elapsed wall time is a classified
   timeline discontinuity and must not trigger recovery by itself. The health
   discontinuity count resets with each load, matching
   `initialPlaybackHealth()`.

6. **Generation scoping.** Every load intent, player event, asynchronous command
   and recovery effect carries a monotonically increasing generation. Only a
   new user intent can advance it. Outcomes that do not exactly match the active
   generation are discarded.

7. **Recovery is bounded.** Backoff is `[500, 1000, 2000, 4000, 5000]` ms with a
   30-second wall-clock budget across the whole recovery episode. Clear the
   attempt count only after five healthy seconds in the zap state, not merely on
   the first frame.

8. **Buffer phasing.** Use `zap` targets of 1 second while tuning and `steady`
   targets of 10 seconds after the healthy window. `demuxer-max-bytes` remains a
   static 64 MiB ceiling, never a phase target. Apply `cache-secs` and
   `demuxer-readahead-secs` once per load when entering steady, track the command
   results, and reject a transition from a stale generation.

9. **Preserve transport-specific decisions.** The core model retains both
   `mpeg-ts` and `hls` even though the current Xtream adapter emits `.ts` URLs.
   Continuous TS recovery issues a fresh reopen; HLS recovery reloads the
   advertised live edge; `format-probe-required` uses the distinct probed reopen.
   Preserve `HLS_LIVE_START_INDEX` and `HLS_RUNTIME_RETRY_OPTIONS` from
   `policy.ts`. Transport is resolved with the load and must not be guessed from
   URL scheme alone.

## Tests

The tests specify behaviour, but they are not all pure and not every Electron
mechanism belongs in native code.

Port these cases completely into `coax_core_tests`:

```text
test/playback-health.test.ts
test/supervisor-state-machine.test.ts
test/supervisor-host.test.ts
test/supervisor-diagnostics.test.ts
test/generation.test.ts
```

Port `test/supervisor-effects.test.ts` against the native effect vocabulary,
including `recreate-player` rather than `restart-process`.

Mine these controller tests selectively for adapter invariants:

```text
test/mpv-controller-buffer-health.test.ts
test/mpv-controller-transport-recovery.test.ts
test/mpv-controller-process-recovery.test.ts
test/supervisor-process-recovery.test.ts
```

Required native adapter cases are:

- buffer targets applied once per load and only for the active generation;
- property-command rejection is observable;
- structured end reasons reach the matching generation;
- one backend failure produces one bounded `recreate-player` effect;
- a stale recreation result cannot replace current playback;
- repeated backend failure exhausts the shared schedule and budget.

Do not port assertions about Electron GPU events, child PIDs, named pipes,
socket heartbeats or orphan-process cleanup. Do not describe the controller
tests as pure functions: they are fake-adapter contract tests.

No ported core test uses media, the network or libmpv. Keep the input sequence,
clock values and expected decisions; do not copy vitest structure.

### Catch2 and CTest

- Pin an immutable Catch2 v3 release tag or commit in `FetchContent`; do not
  follow a moving branch.
- Link tests to `Catch2::Catch2WithMain`.
- Add Catch2's `extras` directory to `CMAKE_MODULE_PATH`, then use
  `include(CTest)`, `include(Catch)` and `catch_discover_tests()`.
- Use `DISCOVERY_MODE PRE_TEST` for the cross-compiled test target so test
  discovery is not forced during the build step.
- Register a host-native `coax_core_tests` target as the primary pure-model test
  path. A Windows adapter test executable may additionally run through WSL
  interop or on Windows.

## Acceptance

- WHEN `coax_core` and `coax_core_tests` are configured with a host-native
  compiler, THE BUILD SHALL succeed and CTest SHALL pass without Windows,
  libmpv or UI dependencies.
- WHEN the reducer or health fold is supplied an observation/event sequence and
  injected time, THE result SHALL match the applicable TypeScript case.
- WHEN one `pump()` drains multiple player events, THE adapter SHALL deliver
  each event once, in order, with the generation captured at issue time.
- WHEN a load fails repeatedly, THE SYSTEM SHALL use the `[500, 1000, 2000,
  4000, 5000]` ms schedule, abandon recovery at 30 seconds wall clock, and
  surface the failure.
- WHEN an event or effect carries a superseded generation, THE SYSTEM SHALL
  discard it.
- WHEN channels are changed rapidly, THE SYSTEM SHALL settle on the newest
  requested channel.
- WHEN playback movement differs from elapsed monotonic time by more than one
  second, THE SYSTEM SHALL classify a discontinuity and SHALL NOT recover for
  that fact alone.
- WHEN playback completes five healthy seconds after its first frame, THE
  SYSTEM SHALL move the buffer target from zap to steady exactly once for that
  load.
- WHEN the same failure is supplied under MPEG-TS and HLS transports, THE core
  SHALL emit the transport-specific action asserted by the source reducer.
- WHEN the backend fails, THE SYSTEM SHALL perform bounded, generation-scoped
  in-process player recreation without restarting the UI.
- WHEN `F1` is pressed, THE diagnostics SHALL show supervisor state, health
  verdict, recovery reason/budget and clearly named discontinuity counters.
- WHEN the work is complete, THE Buffer, Supervisor, generations and trade-off
  sections of `docs/design/live-playback.md` SHALL describe the implemented
  1s/10s phases, 64 MiB ceiling and five-attempt policy; removing `STUB` alone
  is insufficient.

## Build and verify

The implementation must provide a host-native core-test configuration (a CMake
preset or equivalent documented command). The equivalent explicit sequence is:

```bash
cmake -S . -B build-core -G Ninja \
  -DCOAX_BUILD_APP=OFF -DBUILD_TESTING=ON
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

Cross-compile the Windows application and run any registered Windows adapter
tests rather than stopping after compilation:

```bash
cd /home/jonoc/coaxn
nix develop --command bash -c \
  'cmake --build build && ctest --test-dir build --output-on-failure'
```

If CTest cannot launch a PE test executable in the active WSL environment, run
that adapter test on Windows or configure an explicit cross-compiling emulator.
The host-native core suite remains mandatory either way.

Run the application on Windows:

```bash
cp build/coax.exe build/libmpv-2.dll /mnt/c/Users/jonoc/coax-poc/ \
  && cd /mnt/c/Users/jonoc/coax-poc && ./coax.exe
```

Operational notes:

- The session log is written beside the executable as `coax.log` and includes
  mpv warnings. A GUI-subsystem process has no console, so that file is the
  primary runtime diagnostic.
- Kill stray instances with `taskkill.exe /F /IM coax.exe` before copying a new
  build over the old one, or the copy fails with a permission error.
- `MpvPlayer::set_option` logs any option the pinned libmpv rejects. Check the
  log after an option change.
- Never persist raw mpv transport log lines that may contain authenticated URLs;
  log only sanitized classifications.
