# PRD — Coax Native

**A Windows live-TV player built directly on libmpv, where video and UI are
composited into one top-level application surface.**

|                  |                                             |
| ---------------- | ------------------------------------------- |
| Status           | v0.3 — composition, playback and recovery delivered |
| Owner            | Jono                                        |
| Date             | 3 August 2026                               |
| Platform         | Windows 11 x64                              |
| Primary hardware | NVIDIA GeForce RTX 5080, 4K display         |

---

## 1. What this is

Coax hosts libmpv in-process. mpv renders through its native D3D11 backend into
a windowless composition swap chain, and Coax attaches that swap chain to its
own DirectComposition tree alongside the UI. Video and controls belong to one
top-level application surface.

The predecessor was an Electron shell embedding mpv out-of-process through
`--wid`. That put video in a separate window hierarchy, so sharing the
application window captured the UI and left a black rectangle where the picture
should be. Reparenting, stacking and native-layer composition did not make it
reliable, because the design was multi-surface by construction. Hosting libmpv
in-process removes the boundary rather than working around it.

mpv remains the playback engine. The rewrite was never permission to replace
proven playback behaviour with custom media code, and this document does not
schedule work that re-qualifies mpv against itself.

The Electron implementation is frozen. It is a behavioural reference, not a
runtime host and not a comparison target.

## 2. Goals

Goals are ordered. A lower goal must not compromise a higher one.

1. **Unified composition:** video, overlays and controls in one native
   application surface that remains visible in application-window capture.
2. **Playback robustness:** bounded, generation-scoped recovery from the network
   and media faults live streams actually produce.
3. **Windows video quality:** D3D11VA decoding, mpv GPU rendering and the
   NVIDIA VSR-capable D3D11VPP path.
4. **Native efficiency:** no Chromium, no renderer processes, no JSON playback
   IPC, no child-window alignment, no transparent overlay windows.
5. **Fast operation:** quick channel navigation under keyboard and mouse, at a
   television viewing distance.
6. **Portable core:** product state, provider access, recovery policy and
   libmpv control independent of the Windows UI and presentation code.
7. **Honest observability:** distinguish requested, configured and actually
   confirmed hardware features. Never infer VSR activation from GPU model or
   filter attachment alone.

## 3. Non-goals

- Re-qualifying mpv. Decoder correctness, deinterlace quality, field order,
  cadence, clocking and format support are upstream's tests, not Coax's.
- Comparative measurement against the Electron build. It is frozen; paired
  baselines would be archaeology.
- An EPG, guide storage or guide UI in the current scope. See §9.
- M3U and XMLTV inputs. Xtream is the only provider path.
- Accessibility semantics via UI Automation. See §7.3.
- Maintaining Electron, a browser renderer or `--wid` embedding.
- Passing decoded frames through CPU memory.
- Replacing mpv's demux, decode, audio, clocking or filter pipeline.
- Maintaining a private mpv fork.
- Shipping macOS or Linux builds.
- Guaranteeing RTX VSR on unsupported hardware, drivers, formats or driver
  configurations.
- VOD libraries, recording, DRM playback or user profiles.

## 4. Target user and use cases

The target user is the project owner, watching live IPTV on Windows 11 with an
NVIDIA GPU and a 4K display.

1. Start Coax and select a live channel.
2. Browse and search categories and channels.
3. Change channels rapidly without an old request taking control.
4. Watch progressive and interlaced sports streams with hardware acceleration.
5. Recover from expected network and media faults without intervention.
6. Enter and leave fullscreen, resize, minimise, restore and resume after
   display disruption without losing video or control.
7. Share the Coax application window and have the recipient see moving video
   and its UI.

## 5. Delivered

The following is implemented and in use. It is recorded here because the rest of
this document is written against it.

**Composition.** libmpv loaded in-process from a pinned runtime, configured
`vo=gpu-next`, `gpu-api=d3d11`, `d3d11-output-mode=composition`,
`force-window=no`. No mpv-owned presentation HWND exists. The
`display-swapchain` property is acquired by observation and attached to an
application-owned DirectComposition visual; the ImGui layer draws into a second
transparent swap chain in the same visual tree, under one HWND. No CPU readback.

**Presentation lifetime.** The swap chain is reference-held while attached and
identified by address *and* epoch, so a replacement reusing a freed address is
still re-attached rather than mistaken for what it replaced. Device loss on
present, resize or resume rebuilds the UI device and composition tree and
resumes the channel through the existing generation-scoped recovery path,
bounded on both halves. Display and power lifecycle messages are handled. The
contract is §7.3.

**Playback.** RAII libmpv owner, asynchronous command and event boundary,
generation-scoped load/stop/reconfiguration, volume, mute, pause, fullscreen and
clean shutdown.

**Provider.** Xtream credentials entered as portal URL, username and password,
DPAPI-encrypted at rest, catalog fetch off the UI thread, category grouping and
live search, playback by internal channel ID.

**Recovery.** The supervisor, health fold and buffer-phase policy in a
Windows-free `core/`, covered by tests that run without Windows or libmpv:
bounded attempts within an episode, transport reopen, HLS reload, probed reopen
and player recreation, all generation-scoped.

**Quality.** D3D11VA decoding with `hwdec-current` reported; NVIDIA D3D11VPP
scaling applied only when the source is genuinely smaller than the viewport;
super resolution reported as requested and filter-attached, never as confirmed.

**Packaging.** NSIS installer, portable archive, split debug archive, startup
update check against GitHub releases, and CI covering both the portable core
tests and the Windows cross-build.

## 6. What must remain true

These are the properties Coax itself can break. Each is Coax-owned; none of them
restates an mpv guarantee.

### 6.1 Composition and capture

- The selected Coax application window includes moving video, controls and
  overlays in a real application-window share. This is confirmed by human
  observation once, and again after any change to the composition tree or the
  swap-chain lifetime — not by screenshots, frame counters or window probes.
  Last confirmed 3 August 2026, after the swap-chain lifetime and device-loss
  work in §7.3.
- Overlay show/hide does not cause black frames, blinking, stale video or focus
  transfer to a hidden surface.
- Video remains visible across resize, fullscreen, DPI change and
  minimise/restore.

### 6.2 Playback and quality

- The video filter chain is composed, not overwritten. Scaling and
  deinterlacing coexist without duplicate D3D11VPP filters, and a source or
  viewport change cannot leave a stale scale factor behind.
- Scaling is applied only when the source is smaller than the viewport.
- Diagnostics expose hardware-decode, deinterlace and VSR state as separate
  requested / attached / confirmed readings. `vsrConfirmed` stays false unless a
  reliable independent signal proves activation; today none exists, and the UI
  says so rather than guessing.

### 6.3 Responsiveness

- No UI-thread operation waits synchronously on libmpv, network access or
  provider work.
- UI input remains responsive while playback opens, stalls or recovers.
- Channel-change intent reaches the newest channel; no stale command, event or
  recovery action can resurrect an older one.

### 6.4 Reliability

- Network reset, stall, truncation, retryable HTTP error, authentication
  rejection and media reconfiguration stay within the bounded-recovery contract
  in `core/`, which is where they are tested.
- The libmpv instance can be destroyed and recreated after a recoverable
  internal failure without restarting the UI.
- A fatal in-process native crash is an accepted reduced isolation boundary
  relative to the old child-process design.

## 7. Architecture

```text
Coax Native process
├── Platform-independent core          src/core/
│   ├── channel model and index
│   ├── playback health fold
│   ├── recovery supervisor and policy
│   └── generations, version comparison
├── Provider client                    src/xtream/
├── libmpv adapter                     src/player/
│   ├── in-process mpv instance
│   ├── asynchronous commands and property observation
│   ├── buffer-phase gate and live sync
│   └── playback event normalisation
└── Windows platform adapter           src/win/
    ├── Win32 window and event loop
    ├── DirectComposition visual tree
    │   ├── mpv composition swap chain
    │   └── D3D11 UI layer
    ├── input, DPI and display lifecycle
    └── DPAPI credential storage
```

### 7.1 Language, build and runtime

- C++20, CMake, cross-compiled from Linux/WSL to Windows x64 with mingw-w64
  supplied by nix. Nothing is installed on the Windows side.
- `core/` builds and tests with a native compiler, requiring neither Windows nor
  libmpv. That is what CI runs first, and it is what a second platform would
  reuse unchanged.
- A system mpv is never silently selected.

**The libmpv pin.** mpv `v0.41.0` registers `display-swapchain` and documents
`--d3d11-output-mode` and `--d3d11-composition-size`, so the composition path
exists in a tagged release. What the tag does not provide is a *usable artifact*:
none of upstream's Windows archives ships a libmpv DLL, import library or
headers. Coax therefore pins a prebuilt libmpv development package — shinchiro
winbuild `20260610` of mpv commit `304426c390901436fb1d4a63efbd582ae80c88f4` —
recorded in `third_party/mpv/PINNED.txt`. The pin is chosen for artifact
availability, not for any feature missing from the tag.

Both halves of that were checked against upstream on 3 August 2026, rather than
asserted:

- `DOCS/man/input.rst` at tag `v0.41.0` documents `display-swapchain` as a
  read-only int64 swap-chain address, and notes it "may not always be
  available" when `d3d11-output-mode` is not `composition` — which is the
  same unavailability the acquisition paths in §7.3 are built around.
  `DOCS/man/options.rst` at the same tag documents both `--d3d11-output-mode`
  and `--d3d11-composition-size`.
- The release's `x86_64-pc-windows-msvc` archive holds six entries —
  `mpv.exe`, `mpv.com`, `mpv.pdb`, `vulkan-1.dll` and two registration
  scripts. The `x86_64-w64-mingw32` archive wraps a nested player build of
  27 entries: FFmpeg, libass, libplacebo and friends alongside `mpv.exe`.
  Neither contains a libmpv DLL, an import library or a single C header.

That choice carries obligations still outstanding, listed in §8:

- the fetched archive must be verified against a recorded hash; and
- redistributing a libmpv/FFmpeg binary requires shipping its licence terms and
  corresponding source offer alongside Coax's own.

### 7.2 libmpv integration

- Options required before `mpv_initialize` are applied through the client API.
- Playback URLs and scoped HTTP values are supplied through the API after
  initialization, never in process arguments or logs.
- Commands that can overlap use asynchronous client API calls.
- The event pump publishes normalised, immutable application events; the UI
  thread never blocks on mpv.
- Every load and recovery intent carries a monotonically increasing generation.
- mpv properties are the source of truth for playback time, tracks, format,
  decoder, cache, pause, reconfiguration and terminal playback state.
- `--wid` is never used. In-process window embedding would recreate the separate
  surface that caused this rewrite.

### 7.3 Windows presentation

`display-swapchain` is a read-only `int64` holding a D3D11 swap-chain address.
Its addition is absent from mpv's `DOCS/interface-changes.rst`, and its
documentation defines no COM ownership, reference-count or lifetime behaviour.
It is a pinned, version-specific integration boundary, not an announced stable
interface, and every clause below is re-checked on any runtime upgrade.

**The swap-chain contract, as implemented.**

- **Acquisition needs both paths, because they race and neither reliably wins.**
  `mpv_observe_property(..., MPV_FORMAT_INT64)` notifies, and
  `MPV_EVENT_VIDEO_RECONFIG` triggers an explicit read. Measured on the pinned
  runtime against a synthetic source, the observation first reports `0` while no
  video output exists, and then *either* delivers the address before any
  reconfiguration *or* is beaten to it by the reconfiguration read. Both
  outcomes occurred across runs of the same build on the same input, and both
  occurred again on the attachment following a mid-session rebuild — so it is a
  genuine race, not a startup artefact. Neither path is sufficient alone; each
  acquisition records which one produced it, and both the log and F1 report it.
  Polling after `mpv_initialize` remains wrong regardless: the property is
  unavailable until the video output is configured.
- **A reference is held for exactly as long as the swap chain is attached.**
  It is taken before the address is passed anywhere, because between reading the
  property and `IDCompositionVisual::SetContent` taking DirectComposition's own
  reference the object is otherwise unowned, and mpv's video output does not
  tear down on the UI thread. It is released only after the visual's content has
  been cleared.
- **Identity is address plus epoch, never address alone.** A monotonic epoch
  advances wherever the video output can be torn down or reconfigured. Equal
  addresses within one epoch are the same object and are ignored as duplicates;
  equal addresses across epochs are two objects that landed in the same
  allocation, and force a detach and re-attach. Reconfiguration is frequent —
  starting one synthetic source advances the epoch five times, and a resize
  drag advances it continuously — so most re-attachments are precautionary,
  report the same address, and are silent in the log. Diagnostics count
  replacements and re-attachments separately, because one combined number
  would be dominated by that churn and would hide the case that matters.
- **Content is cleared before it is replaced.** Both `SetContent` calls land in
  one `Commit`, so the swap is atomic to the compositor and the previous object
  leaves the visual before anything releases it.
- **A pointer from a superseded epoch is never read, presented or released.**

**Device loss.** `Present` and `ResizeBuffers` are checked for
`DXGI_ERROR_DEVICE_REMOVED` and `DXGI_ERROR_DEVICE_RESET`, and resume from
suspend verifies the device rather than waiting for the next frame to discover
it. A loss raises exactly one event, however many frames present into a dead
device. Recovery is a rebuild of the UI device and composition tree, bounded
and paced by `core::PresentationRebuildBudget`; resuming the channel afterwards
is dispatched as `core::PresentationLost` and handled by the existing
supervisor as `RecoveryAction::RecreatePlayer`, so it inherits that path's
bounded attempts and generation fence rather than owning a second one.
`WM_DISPLAYCHANGE` re-evaluates size and DPI and re-commits the tree.

Because that path is re-checked on every runtime upgrade and its honest
triggers — disabling the display adapter, suspending the machine — are too
disruptive to run routinely, the diagnostics panel carries a **Force rebuild**
button that drives the same sequence without a real removal. It does not
fabricate a device loss: the loss counters and last-loss reason continue to
report only what DXGI actually said.

**UI toolkit.** Dear ImGui, drawn into an application-owned D3D11 swap chain in
the composition tree. It satisfies the composition, input, DPI and packaging
requirements. It does not expose UI Automation semantics; for a single-user TV
appliance that is an accepted deviation rather than an open gate.

### 7.4 Hardware acceleration and processing

The Windows profile preserves mpv's capabilities rather than reimplementing
them: D3D11VA decoding where supported, mpv's own software fallback for playable
unsupported formats, D3D11-compatible rendering on the high-performance adapter,
D3D11VPP deinterlacing for interlaced material, and NVIDIA D3D11VPP scaling when
an eligible source is being upscaled, falling back to a vendor-neutral mpv
scaler otherwise.

Coax's responsibility is the *graph*, not the filters: filter changes are atomic
and generation-aware, deinterlacing and scaling compose into one chain rather
than overwriting each other, and source or viewport changes recalculate rather
than leave a stale scale factor.

The runtime records the selected GPU, decoder, hardware frame format, renderer,
output size, requested processing graph and observed filter attachment. It does
not claim a vendor enhancement processed a frame without confirmation.

### 7.5 Portable core

These must not depend on Win32 types: playback intents and normalised events;
provider authentication and channel normalisation; navigation and view models;
recovery policy and timers; settings and migration; diagnostic schemas; and
secure-storage requests.

Platform adapters own: window creation and final presentation; GPU context and
swap-chain integration; monitor, DPI, fullscreen and power events; input; secure
credential implementation; filesystem locations; crash integration and updates;
and packaging.

The core owns a vendor-neutral enhancement contract — supported, requested,
configured, confirmed, fallback — which the Windows adapter maps to NVIDIA
D3D11VPP VSR. Another platform may map it elsewhere or report it unavailable.
This keeps diagnostic honesty portable without pretending NVIDIA VSR is.

### 7.6 UI

The UI is a live-TV player and nothing more: full-window and fullscreen video,
transient playback controls, channel and category navigation, channel-change and
recovery feedback, source setup, and playback and display settings.

- Keyboard operation is first-class; directional navigation, accept and back
  behave predictably.
- Pointer interaction is available but does not define focus order.
- Focus never becomes trapped in the video surface.
- Video state changes do not redraw the full UI tree.
- Animation is limited and must not contend with video presentation.
- Long channel lists are virtualised; the full catalog is never submitted.

### 7.7 Data, credentials and security

- Credentials are encrypted with DPAPI and never leave the process in clear.
- The UI receives internal channel identifiers, not authenticated playback URLs
  or provider credentials.
- libmpv receives scoped plaintext only for the active request.
- Logs are structured, bounded and redacted before persistence; full
  authenticated URLs, credentials, headers and cookies are prohibited from
  diagnostics and crash attachments.
- Provider data is held in memory and normalised on load. No database is
  required at the current scope.

## 8. Remaining work

Ordered by value. Each item is Coax-owned; nothing here re-tests mpv.

### 8.1 Settings and source management

A settings surface for playback and display options, and the ability to
change, re-authenticate or remove the saved source without editing storage by
hand.

Done when a provider can be replaced entirely from within the application.

### 8.2 Runtime provenance and licensing

Verification has landed: `scripts/fetch-libmpv.sh` checks the archive against a
recorded SHA-256 and refuses to unpack a mismatch, and the digest is carried in
`PINNED.txt` beside the runtime. Licensing has not. Ship mpv's and FFmpeg's
licence terms and the corresponding source offer in the installer and archive,
alongside Coax's own LICENSE.

Done when a release is content-verified at fetch time — it now is — and legally
complete as distributed, which it is not.

### 8.3 Composable filter chain and deinterlacing

Compose `vf` rather than overwriting it, then attach D3D11VPP deinterlacing for
interlaced sources. mpv performs the deinterlacing; Coax decides when it is
attached and ensures it does not collide with scaling.

Done when an interlaced source deinterlaces while an upscaled source still
scales, with no duplicate filters and no stale scale factor.

## 9. Deferred

Not rejected, not scheduled. Each would be reopened by a concrete need.

- **EPG and now/next.** Real work, no current demand. Worth noting for whenever
  it returns: Xtream portals expose `xmltv.php` directly, so a guide needs no
  separate provider path, and for a single user a cached parse with an in-memory
  interval index would likely serve better than a database.
- **Controller input.** Reopening this means wiring XInput and feeding ImGui
  gamepad keys. The `NavEnableGamepad` flag was removed rather than left set
  with no backend behind it: a nominal capability is worse than its absence.
- **Crash-restart supervision.** Revisit if in-process crashes prove common.
- **Code signing.** Needs a certificate, not a build change.
- **Sanitized diagnostic export.** The in-memory log ring covers current needs.
- **Additional platforms.** Post-Windows: audit the core boundary, prove one
  presentation adapter, qualify that platform's decode and composition path,
  implement platform UI and secure storage.

## 10. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| `display-swapchain` is version-specific, absent from mpv's announced interface changes, and its COM lifetime contract is undefined | Upgrades or teardown can produce stale pointers, black frames or crashes | Pin the runtime; own the lifetime explicitly (§7.3); re-verify the property and its behaviour on every upgrade |
| The pinned libmpv is a third-party prebuilt binary | Supply-chain and reproducibility exposure | Verify against a recorded hash; keep provenance in `PINNED.txt`; be able to rebuild from the recorded commit if the source disappears |
| In-process native failure terminates the UI | Less crash containment than the old design | Strict RAII ownership, bounded instance recreation, pinned runtimes |
| Redistribution obligations for libmpv/FFmpeg | Release is not legally complete | §8.2, before the next tagged release |
| Scope expands into a guide or a second provider path | The working player stops improving | §9 stays deferred until something concrete demands it |

## 11. Decisions made

- The rewrite is a new native application; the Electron build is frozen.
- C++20, CMake, cross-compiled from Linux/WSL with mingw-w64 under nix.
- mpv is the sole playback engine, in-process, never via `--wid`.
- Windows 11 x64 is the only platform.
- Dear ImGui is the UI toolkit; UI Automation accessibility is an accepted gap.
- libmpv is pinned to a prebuilt dev package of commit `304426c`, chosen for
  artifact availability rather than any feature missing from `v0.41.0`.
- Linkage is dynamic: `libmpv-2.dll` ships beside the executable.
- VSR confirmation is unavailable; diagnostics report requested and attached
  only, and say so explicitly.
- Product logic is portable; final presentation is platform-specific.
- No EPG, no M3U/XMLTV, no guide database at the current scope.

## 12. References

- [mpv stable manual](https://mpv.io/manual/stable/) — libmpv embedding, D3D11
  GPU context, composition output, hardware decode and D3D11VPP options.
- [mpv v0.41.0 `input.rst`](https://github.com/mpv-player/mpv/blob/v0.41.0/DOCS/man/input.rst)
  — `display-swapchain` in a tagged release.
- [Pinned mpv property documentation](https://github.com/mpv-player/mpv/blob/304426c390901436fb1d4a63efbd582ae80c88f4/DOCS/man/input.rst)
  — the exact source of the shipped runtime.
- [Pinned interface-change record](https://github.com/mpv-player/mpv/blob/304426c390901436fb1d4a63efbd582ae80c88f4/DOCS/interface-changes.rst)
  — announces the composition options but not the swap-chain property.
- [mpv v0.41.0 client observation API](https://github.com/mpv-player/mpv/blob/v0.41.0/include/mpv/client.h#L1172-L1228)
  — initial notification behaviour, and the warning that some properties may not
  notify every later change.
- [Official libmpv examples](https://github.com/mpv-player/mpv-examples/tree/master/libmpv)
  — client API and render integration patterns.
- [docs/design/live-playback.md](docs/design/live-playback.md) — recovery,
  buffering and live-offset design.
- [docs/packaging.md](docs/packaging.md) — packaging, releases and the pinned
  runtime.
