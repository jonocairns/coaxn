# PRD — Coax Native

**A Windows-first, cross-platform-capable live-TV player built directly on
libmpv.**

|                      |                                                 |
| -------------------- | ----------------------------------------------- |
| Status               | Draft v0.2                                      |
| Owner                | Jono                                            |
| Date                 | 2 August 2026                                   |
| Initial platform     | Windows 11 x64                                  |
| Primary hardware     | NVIDIA GeForce RTX 5080, 4K display             |
| Product relationship | Native successor to the Electron implementation |

---

## 1. Executive summary

Coax Native replaces the Electron shell and out-of-process `--wid` embedding
model with a native application that hosts libmpv in-process. On Windows, mpv
renders through its native D3D11 backend into a windowless composition swap
chain attached to the application's own DirectComposition tree. Video and UI
therefore belong to one top-level application surface.

The rewrite retains mpv as the playback engine rather than replacing its
networking, demuxing, decoding, synchronization, filters, hardware acceleration
or image-quality capabilities. It removes the window and compositor boundary
that prevented reliable application-window capture in the Electron version.

Windows is the only v1 delivery target. The application core and libmpv control
boundary must nevertheless remain portable so that macOS and Linux presentation
adapters can be added later without rewriting provider, guide, navigation or
playback-supervision logic.

This is a new application, not an incremental native addon for Electron. The
existing implementation is frozen as a behavioral reference and source of
reusable fixtures, schemas and acceptance cases.

## 2. Problem

The current Electron architecture displays mpv video in a separate native
window hierarchy. That arrangement gives mpv direct ownership of its D3D11
output, but Electron's UI and mpv's video are not one composited application
surface. Application-window sharing can consequently capture the Coax shell
while showing a black video region.

Attempts to correct parentage, stacking and native-layer composition did not
make the shipping application reliable. They also increased the amount of
platform-specific lifecycle and recovery code without removing the fundamental
multi-surface design.

Replacing mpv with GStreamer could produce frames for Electron to composite,
but it would introduce a second playback engine, a native GPU texture bridge,
a large runtime and new quality/recovery work. In particular, it would no
longer preserve mpv's already-qualified D3D11VPP and NVIDIA scaling path by
construction.

The product needs one architecture that provides all of the following:

- mpv's playback behavior and configuration surface;
- D3D11 hardware decoding and video processing on Windows;
- an available NVIDIA RTX Video Super Resolution path;
- video and controls in one captureable application surface;
- lower shell overhead and fewer graphics/process boundaries; and
- a portable core with Windows-specific presentation isolated behind an
  adapter.

## 3. Product thesis

> Build a focused native live-TV appliance around libmpv, allowing mpv to own
> playback while Coax owns the final application composition.

The rewrite succeeds only if it preserves the qualities that motivated mpv in
the first place. A native shell is not permission to replace proven playback
behavior with custom media code.

## 4. Goals

Goals are ordered. A lower goal must not compromise a higher one.

1. **Playback robustness:** match or exceed the current pinned-mpv behavior on
   Coax's deterministic clean and hostile stream corpus.
2. **Unified composition:** present video, overlays and controls in one native
   application surface that remains visible in application-window capture.
3. **Windows video quality:** retain D3D11VA decoding, hardware
   deinterlacing, mpv GPU rendering and the NVIDIA VSR-capable D3D11VPP path.
4. **Native efficiency:** eliminate Chromium, renderer processes, JSON
   playback IPC, native child-window alignment and transparent overlay windows.
5. **TV-appliance interaction:** deliver fast channel navigation, controller
   and keyboard operation, now/next information and an EPG designed for a
   television viewing distance.
6. **Portable application core:** keep product state, provider access, guide
   data, recovery policy and libmpv control independent of the Windows UI and
   presentation implementation.
7. **Honest observability:** distinguish requested, configured and actually
   confirmed hardware features. Do not infer VSR activation from GPU model or
   filter attachment alone.

## 5. Non-goals

- Maintaining Electron or a browser renderer in the new application.
- Embedding an mpv-owned child window with `--wid`.
- Passing decoded frames through CPU memory to make composition easier.
- Replacing mpv's demux, decode, audio, clocking or filter pipeline.
- Maintaining a private mpv fork or patch as a normal product dependency.
- Adopting GStreamer as a second playback engine.
- Shipping macOS or Linux builds in v1.
- Producing one universal GPU abstraction before the Windows path works.
- Guaranteeing RTX VSR on unsupported hardware, drivers, formats or driver
  configurations.
- Adding VOD libraries, recording, DRM playback, profiles or unrelated product
  features during the rewrite.
- Achieving feature parity by preserving obsolete implementation details from
  the Electron application.

## 6. Target user and primary use cases

The v1 target user remains the project owner, watching live IPTV on Windows 11
with an NVIDIA GPU and a 4K display.

Primary use cases:

1. Start Coax and resume or select a live channel.
2. Browse categories, channels and now/next guide data using a controller,
   keyboard or mouse.
3. Change channels rapidly without an old request taking control.
4. Watch progressive and interlaced sports streams with hardware acceleration
   and appropriate processing.
5. Recover from expected network and media faults without manual intervention.
6. Enter and leave fullscreen, resize, minimise, restore and resume after
   display disruption without losing video or control.
7. Share the Coax application window and have the recipient see moving video
   and its UI.

## 7. Success criteria

### 7.1 Architecture proof

Before product migration begins, a minimal native executable must pass all of
these criteria on the target Windows machine:

- libmpv is loaded in-process from a pinned, verified runtime;
- mpv creates no separate video-presentation HWND or separately composited or
  separately captured video surface; an auxiliary non-presenting window, if the
  pinned implementation creates one, does not fail this gate;
- mpv presents through D3D11 composition into the application's composition
  tree without CPU pixel readback;
- a native control or diagnostic overlay appears above moving video in the same
  top-level window;
- video remains visible during resize, fullscreen, minimise/restore and overlay
  show/hide;
- D3D11VA hardware decoding is observed on a supported fixture;
- the D3D11VPP NVIDIA scaling configuration is applied during a real upscale;
- video and overlay are visibly present and progressing in at least one actual
  application-window share using the conferencing/capture software that exposed
  the original failure; and
- shutdown releases mpv, audio, graphics, swap-chain and window resources
  without a hung process.

Failure of unified capture, GPU-resident presentation or the required mpv
quality path stops the rewrite before feature-porting work begins.

### 7.2 Playback and quality

- The same pinned mpv build and relevant playback options produce no material
  regression against the existing deterministic stream corpus.
- Hardware decode failure falls back to playable software decode when the input
  is otherwise supported.
- Progressive 720p50/59.94, 576i50 and 1080i50 fixtures pass the existing motion,
  field-order and cadence checks.
- A 720p source rendered into the target 4K viewport exercises the configured
  NVIDIA scaling path without duplicate D3D11VPP filters or stale scaling after
  reconfiguration.
- Diagnostics expose `hardwareDecodeActive`, `deinterlaceRequested`,
  `deinterlaceActive`, `vsrRequested`, `vsrFilterAttached` and
  `vsrConfirmed` as separate states.
- `vsrConfirmed` remains false unless a reliable independent confirmation
  signal or a documented provider API proves activation.
- Audible synchronized audio and acceptable local image quality receive human
  sign-off before release.

### 7.3 Capture and composition

- The selected Coax application window includes moving video, controls and
  overlays in the actual capture product.
- Capture continues progressing while Coax is fully occluded, where supported
  by that capture product.
- Overlay interaction does not cause black frames, blinking, stale video or
  focus transfer to a hidden surface.
- No acceptance result substitutes screenshots, frame counters or synthetic
  window probes for the final human capture observation.

### 7.4 Responsiveness and resources

Targets are measured on the same target machine and fixtures as the current
application:

- Channel-change intent reaches the newest channel at least as quickly as the
  current pinned-mpv baseline.
- UI input remains responsive while playback opens, stalls or recovers.
- No UI-thread operation waits synchronously on libmpv, network access or guide
  database work.
- A 30-minute playback run and ten alternating load/stop cycles show no
  unexplained monotonic growth in process working set, GPU resources, handles or
  threads.
- The native application uses materially less idle and steady-state memory than
  the Electron baseline. Exact pass thresholds are frozen from paired baseline
  measurements before the result is interpreted.

### 7.5 Reliability

- Rapid channel changes are generation-scoped; stale commands, events and
  recovery actions cannot resurrect an older channel.
- Network reset, stall, truncation, retryable HTTP error and media
  reconfiguration cases meet the current bounded-recovery contract.
- The application can destroy and recreate the libmpv instance after a
  recoverable internal failure without restarting the UI.
- A fatal in-process native crash is acknowledged as a reduced isolation
  boundary. A later crash-restart mechanism may restore the application, but it
  must not be described as equivalent to child-process containment.

## 8. Product architecture

```text
Coax Native process
├── Platform-independent application core
│   ├── navigation and view models
│   ├── provider and channel model
│   ├── guide queries
│   ├── playback intent generations
│   ├── recovery supervisor
│   └── structured diagnostics
├── libmpv adapter
│   ├── in-process mpv instance
│   ├── asynchronous commands and property observation
│   ├── stream and HTTP configuration
│   └── playback event normalization
└── Windows platform adapter
    ├── Win32 window and event loop
    ├── D3D11 device/adapter coordination
    ├── DirectComposition visual tree
    │   ├── mpv composition swap chain
    │   └── native UI/overlay content
    ├── input, controller, DPI and display lifecycle
    ├── secure credential storage
    └── packaging and update integration
```

### 8.1 Language and build system

- C++20 is the implementation language for the application core, libmpv
  integration and initial Windows shell.
- CMake is the cross-platform build description.
- Dependencies are pinned and fetched or supplied reproducibly; a system mpv is
  never silently selected for a release build.
- The existing playback baseline uses an untagged upstream development snapshot,
  but the native application does not inherit that choice automatically. mpv
  `v0.41.0` source and its upstream Windows x64 player artifact both register
  `display-swapchain`, so the composition path exists in a tagged release. M0
  first qualifies a reproducible libmpv build from that tag and uses a newer
  snapshot only if recorded evidence shows the tagged code cannot meet a gate.
- The upstream `v0.41.0` Windows player artifact proves feature presence but does
  not include a libmpv DLL, import library or headers. Acquiring libmpv therefore
  remains part of the build contract: it is built reproducibly from the selected
  source or obtained as a recorded, verifiable artifact. Source, toolchain,
  dependency versions, build configuration, provenance and licensing obligations
  are captured alongside its hash.
- Automated tests run in the existing Nix development environment where they do
  not require native Windows APIs. Native composition and playback acceptance
  run from the managed Windows mirror.

C++ is selected because libmpv, D3D11, DXGI, DirectComposition and the Windows
windowing APIs are native C/C++ boundaries. The project should still use RAII,
strong ownership types, sanitizers where available and narrow platform
interfaces to reduce manual lifetime risk.

### 8.2 libmpv integration

- Coax links to or dynamically loads a pinned libmpv runtime in-process.
- Options required before `mpv_initialize` are applied through the client API.
- Playback URLs and scoped HTTP values are supplied through the API after
  initialization, never exposed in process arguments or logs.
- Commands that can overlap or complete later use asynchronous client API calls.
- A dedicated control/event boundary drains mpv events and publishes immutable
  application events; the UI thread never blocks on mpv.
- Every load and recovery intent carries a monotonically increasing generation.
- mpv properties remain the source of truth for playback time, tracks, format,
  decoder, cache, pause, reconfiguration and terminal playback state.
- Lua scripts and standard mpv configuration may be supported when they do not
  weaken reproducibility or the trusted-input boundary.

The application must not use `--wid` for video. In-process window embedding
would recreate the separate native surface that caused this rewrite.

### 8.3 Windows presentation

The initial Windows implementation uses mpv's native D3D11 GPU context and
windowless composition output:

- `gpu-next` is the preferred mpv video renderer;
- the GPU API and context are explicitly D3D11;
- `d3d11-output-mode=composition` creates a composition swap chain without a
  presentation window;
- `d3d11-composition-size` follows the physical-pixel video viewport;
- Coax reads mpv's `display-swapchain` property and attaches that swap chain to
  an application-owned DirectComposition visual; and
- UI content is placed in the same application composition tree and top-level
  HWND.

The property name is settled against the pinned Windows runtime before M0. The
verified binary reports `mpv v0.41.0-744-g304426c39`, matching pinned mpv commit
`304426c390901436fb1d4a63efbd582ae80c88f4`, and its native property inventory
contains:

```text
display-swapchain
```

The same pinned source registers the exact `display-swapchain` string and
documents it as a read-only `int64` containing the D3D11 swap-chain address.
However, its addition is absent from `DOCS/interface-changes.rst`, and the
property documentation does not define COM ownership, reference-count or
lifetime behavior. Coax therefore treats this as a pinned, version-specific
integration boundary rather than assuming the normal announced-interface
stability policy applies.

The tagged-release question is also settled before M0. The exact `v0.41.0`
source registers and documents `display-swapchain`. The upstream
`mpv-v0.41.0-x86_64-pc-windows-msvc.zip` artifact was verified against its
published SHA-256
`4e197f729f5071c6772f35fffd96e0f36e3e8a044bd9479b136bb09b7c6a80ff`, and
its native `--list-properties` output also contains `display-swapchain`. The
property is therefore not snapshot-only, although its interface-policy and
COM-lifetime limitations still apply.

The value is a borrowed raw pointer until proven otherwise. Before product code
depends on it, Coax must define and test:

- how the pointer is first acquired, given that the property reports
  unavailable until the video output is configured;
- whether and when the host calls `AddRef` and `Release`;
- the ordering for detaching `IDCompositionVisual` content before mpv releases
  or replaces the swap chain;
- whether swap-chain identity remains stable across video reconfiguration and
  `d3d11-composition-size` changes;
- how a replacement pointer is detected and attached after VO recreation,
  libmpv instance recreation or device loss; and
- how stale pointers are prevented from being read, presented or released.

Acquisition and replacement should share one mechanism. The preferred candidate
is `mpv_observe_property(handle, id, "display-swapchain", MPV_FORMAT_INT64)`.
The client API guarantees an initial property-change notification, using
`MPV_FORMAT_NONE` while the property is unavailable, but explicitly warns that
some properties may not notify every later change. M0 must therefore prove that
this property emits usable notifications when the swap chain first appears and
whenever its identity changes. If it does not, acquisition and reattachment are
driven from `MPV_EVENT_VIDEO_RECONFIG` followed by an explicit property read,
and that fallback becomes the recorded contract.

The exact Windows UI toolkit is selected during the architecture proof. It must:

- interoperate with the mpv composition swap chain without CPU copies;
- allow controls over video in the same captured top-level surface;
- provide correct per-monitor DPI, text, keyboard, pointer and controller input;
- support accessible semantics for product controls;
- package without a browser runtime; and
- not take ownership of playback timing.

Candidates may include a minimal Win32 shell with native drawing or a Windows UI
framework that exposes the required composition surface. Toolkit convenience is
subordinate to the presentation and capture gates.

### 8.4 Hardware acceleration and processing

The Windows default profile must preserve the existing mpv capabilities:

- D3D11VA hardware decoding where supported;
- automatic software fallback for playable unsupported formats;
- D3D11-compatible rendering on the selected high-performance adapter;
- D3D11VPP deinterlacing for interlaced material where qualification passes;
- NVIDIA D3D11VPP scaling mode when an eligible source is being upscaled; and
- a vendor-neutral mpv scaler when NVIDIA processing is unavailable or rejected.

Filter changes are atomic and generation-aware. Deinterlacing and scaling must
not create competing duplicate D3D11VPP filters. Source or viewport changes
recalculate the graph and cannot leave a stale scale factor.

The runtime records the selected GPU, decoder, hardware frame format, renderer,
output size, requested processing graph and observed filter attachment. It does
not claim that a vendor enhancement processed a frame without confirmation.

### 8.5 Portable core and future platforms

The following interfaces must not depend on Win32 types:

- playback intents and normalized events;
- provider authentication and channel normalization;
- guide storage and queries;
- navigation, focus and view models;
- recovery policy and timers;
- settings and migration;
- diagnostic event schemas; and
- secure-storage requests.

Platform adapters own:

- window creation and final presentation;
- GPU context and swap-chain integration;
- monitor, DPI, fullscreen and power events;
- keyboard, pointer, remote and controller input;
- secure credential implementation;
- filesystem and application data locations;
- notifications, crash integration and updates; and
- packaging.

Potential later presentation paths are Metal/Cocoa on macOS and Vulkan or
OpenGL under Wayland/X11 on Linux. They are feasibility directions, not v1
commitments. Windows-specific VSR behavior is not part of the portable contract.

The portable core nevertheless owns a vendor-neutral enhancement capability and
telemetry contract: supported, requested, configured, confirmed and fallback.
The Windows adapter maps that contract to NVIDIA D3D11VPP VSR; another platform
may map it to a different supported upscaler or report it unavailable. This
keeps the product behavior and diagnostic honesty portable without pretending
that NVIDIA VSR itself is cross-platform.

### 8.6 UI requirements

The v1 UI includes only the product surfaces necessary for a usable live-TV
player:

- full-window and fullscreen video;
- transient playback controls;
- channel/category navigation;
- now/next information;
- channel-change and recovery feedback;
- an EPG grid;
- source/account setup; and
- playback, display and input settings.

The interaction model is TV-first:

- directional navigation, accept and back are first-class;
- keyboard operation works independently of controller availability;
- pointer interaction remains available but does not define focus order;
- focus never becomes trapped in the video surface;
- video state changes do not cause the full UI tree to redraw; and
- animation is limited and must not contend with video presentation.

The EPG uses time- and channel-range queries plus two-axis virtualization or an
equivalent bounded rendering model. The entire guide is never materialized into
the visible UI tree.

### 8.7 Data, credentials and security

- SQLite remains the preferred local store for guide and normalized provider
  data, accessed outside the UI thread.
- Xtream, M3U and XMLTV inputs preserve the existing normalization and bounded
  parsing requirements.
- Credentials are encrypted with the platform secure-storage adapter. Windows
  uses a user-bound native mechanism such as DPAPI.
- The renderer/UI receives internal channel identifiers, not authenticated
  playback URLs or provider credentials.
- libmpv receives scoped plaintext only for the active request.
- Logs are structured, bounded and sanitized before persistence.
- Full authenticated URLs, credentials, headers, cookies and private provider
  payloads are prohibited from normal diagnostics and crash attachments.

## 9. Migration strategy

### 9.1 Source of truth

The existing Electron application is frozen as:

- a playback and UX behavioral reference;
- a source of deterministic media and network fixtures;
- a source of recovery policies and state-machine tests;
- a source of provider normalization and security requirements; and
- a comparison target for startup, memory and channel-change measurements.

It is not used as a runtime host for the native application.

### 9.2 What is ported

- Product behavior and acceptance contracts.
- Provider normalization rules and fixtures.
- Recovery state-machine semantics and retry budgets.
- Structured diagnostic schemas where still applicable.
- Guide data model and import behavior.
- Navigation and playback state concepts.

### 9.3 What is not ported

- Electron main/preload/renderer boundaries.
- Browser IPC contracts.
- mpv named-pipe ownership and child-process supervision.
- HWND parenting, stacking watchdogs and geometry synchronization.
- Transparent native overlay windows.
- GStreamer or experimental texture-transfer harnesses.
- Investigation tooling that does not enforce a retained acceptance contract.

### 9.4 Repository approach

The native application begins in an isolated top-level source/build area or a
new repository after the architecture proof. It must be buildable without
installing or launching the Electron application. Shared fixtures should have a
neutral location rather than creating a compile-time dependency on TypeScript
application code.

## 10. Delivery plan and gates

### M0 — Native composition proof

Deliver the smallest disposable native executable that initializes libmpv,
loads a controlled stream, attaches the D3D11 composition swap chain, draws one
interactive overlay and records sanitized diagnostics.

M0 also establishes the pinned libmpv runtime itself: a reproducible build from
the pinned commit or a recorded, verifiable snapshot artifact, with provenance,
build configuration and licensing obligations captured. Every later milestone
depends on it, so it is not deferred to M5 packaging.

Gate: all criteria in section 7.1 pass, and the pinned runtime can be rebuilt or
re-obtained from its recorded provenance. There is one consolidated human
acceptance after automated checks are green; the user is not asked to iterate
through speculative configuration toggles.

### M1 — Playback foundation

- Production-quality RAII wrappers for libmpv and composition resources.
- Asynchronous event/control boundary.
- Generation-scoped load, stop and reconfiguration.
- Volume, mute, fullscreen and clean shutdown.
- Structured, redacted diagnostics.
- Local deterministic TS/HLS playback.

Gate: clean playback, lifecycle and resource tests pass with visible video and
audible audio.

### M2 — First usable provider slice

- Secure Xtream credential import and storage.
- Category/channel loading and normalization.
- Minimal native channel UI and now/next view.
- TS and HLS selection by internal channel ID.
- Keyboard and controller navigation.

Gate: a private provider can be used without exposing credentials or URLs, and
rapid selection ends on the newest requested channel.

### M3 — Recovery parity

- Port the recovery reducer/policy into the portable core.
- Feed it normalized libmpv events and health observations.
- Cover reset, stall, EOF, retryable HTTP status, authentication rejection,
  media reconfiguration and stale generations.
- Recreate the libmpv instance when bounded recovery requires it.

Gate: match or exceed the current frozen deterministic recovery corpus.

### M4 — Video-quality and lifecycle qualification

- Hardware decode and fallback.
- Deinterlacing and field-order handling.
- 720p-to-4K NVIDIA scaling path and vendor-neutral fallback.
- Resize, fullscreen, monitor/DPI, minimise/restore, sleep/resume and device-loss
  behavior.
- Swap-chain identity, ownership, visual detachment and replacement across
  reconfiguration, composition-size changes, libmpv recreation and device loss.
- App-window capture with controls and occlusion.
- Long playback and resource soak.

Gate: the complete Windows quality, capture and lifecycle matrix passes,
including final human video/audio/capture observations.

### M5 — Product UI and packaging

- Virtualized EPG.
- Settings and source management.
- First-run and failure UX.
- Pinned libmpv packaging, licenses and corresponding dependency obligations.
- Installer, update and rollback path.
- Sanitized diagnostic export.

Gate: install, upgrade, uninstall and normal viewing pass on a clean Windows user
profile without system mpv or development tools.

### Post-v1 — Additional platforms

Only after Windows v1:

1. audit core/platform boundaries;
2. choose and prove one macOS or Linux presentation adapter;
3. qualify that platform's hardware decode and composition path independently;
4. implement platform UI and secure-storage behavior; and
5. run the same product-level playback and privacy contracts.

## 11. Risks and mitigations

| Risk                                                                      | Impact                                               | Mitigation                                                                                                                              |
| ------------------------------------------------------------------------- | ---------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| D3D11 composition is not captured correctly by the target sharing product | Rewrite does not solve the motivating problem        | Prove actual application-window capture in M0 before porting features                                                                   |
| NVIDIA VSR does not activate in composition mode                          | Target image-quality benefit is reduced              | Exercise real upscale in M0; keep requested/attached/confirmed telemetry separate; retain mpv scaler fallback                           |
| Core presentation depends on the version-specific `display-swapchain` property, whose addition is absent from mpv's announced interface changes and whose COM lifetime contract is incomplete | Upgrade or teardown can produce build breaks, stale pointers, intermittent black frames or crashes | Pin the runtime and source; verify the exact property with `--list-properties`; audit registration and ownership; test pointer identity/lifetime; repeat every check for each upgrade |
| Tagged mpv `v0.41.0` contains the composition path, but its upstream Windows player artifact does not supply libmpv development/runtime files | Coax must produce or source a separate libmpv build, increasing provenance, reproducibility and licensing work | Prefer a reproducible libmpv build from the `v0.41.0` tag; pin source, toolchain, dependencies and configuration; use a newer snapshot only if M0 evidence requires it; inspect each upstream release and its artifacts |
| In-process native failure terminates the UI                               | Less crash containment than the child-process design | Use pinned runtimes, strict ownership, sanitizers and bounded instance recreation; consider app-level restart only after core stability |
| UI implementation expands into a custom framework project                 | Delivery stalls                                      | Keep v1 UI narrow; select a toolkit during M0 using explicit composition/input/accessibility gates                                      |
| Platform abstraction weakens Windows integration                          | Performance or composition compromises               | Abstract product contracts, not GPU primitives; allow platform-specific presentation implementations                                    |
| libmpv or runtime upgrade changes behavior                                | Playback regressions                                 | Pin runtime facts and run the frozen Windows corpus for every upgrade                                                                   |
| C++ lifetime or threading defects                                         | Crashes, leaks or corruption                         | RAII-only ownership, explicit UI/mpv threads, asynchronous APIs, static analysis, sanitizers and resource-soak gates                    |
| Packaging or codec licensing is deferred too late                         | Release blocked                                      | Review libmpv/FFmpeg build provenance and redistribution obligations before M5 implementation completes                                 |
| Rewrite becomes a feature expansion                                       | No usable replacement ships                          | Freeze scope to current live-TV behavior until M5 passes                                                                                |

## 12. Decisions already made

- The rewrite is a new native application, not an Electron addon.
- C++20 and libmpv are the core implementation boundaries.
- mpv remains the sole playback engine.
- libmpv runs in-process.
- Windows 11 x64 is the only v1 platform.
- Windows video uses the native D3D11 path.
- Video is composed without `--wid` or a separate mpv-owned video-presentation
  HWND.
- Product logic is portable; final presentation is platform-specific.
- GStreamer is not part of the target architecture.
- The existing application is retained as a reference, not evolved into the
  native product.

## 13. Scheduled technical decisions

These decisions are made from M0 evidence rather than expanded into open-ended
research:

1. **Windows UI toolkit:** select the smallest option that passes composition,
   capture, input, DPI, accessibility and packaging gates.
2. **libmpv source, linkage and distribution:** select the tagged `v0.41.0`
   source or an evidence-required newer snapshot, then choose dynamic or static
   linkage based on reproducibility, update, licensing, symbol and packaging
   requirements.
3. **D3D11 device and swap-chain lifetime:** confirm the supported
   adapter/device relationship between Coax's UI resources and mpv's composition
   swap chain; define the `AddRef`/`Release` and DirectComposition detachment
   contract for the raw pointer returned through `display-swapchain`; and prove
   pointer identity, replacement and stale-pointer handling across video
   reconfiguration, `d3d11-composition-size` changes, VO/libmpv recreation and
   device loss. This includes deciding the acquisition mechanism: property
   observation if `display-swapchain` is proven to deliver change
   notifications, otherwise video-reconfiguration events.
4. **VSR confirmation signal:** identify a reliable external signal if one is
   available; otherwise ship honest requested/attached diagnostics only.
5. **Fatal-crash restart:** decide after measuring native stability; it is not a
   prerequisite for the composition proof.

Each decision receives a deadline, a default and a pass/fail artifact. None is
a reason to delay the M0 executable.

## 14. References

- [mpv stable manual](https://mpv.io/manual/stable/) — libmpv embedding,
  D3D11 GPU context, composition output, hardware decode and D3D11VPP options.
- [Pinned mpv property documentation](https://github.com/mpv-player/mpv/blob/304426c390901436fb1d4a63efbd582ae80c88f4/DOCS/man/input.rst)
  — the exact source used to verify `display-swapchain`.
- [Pinned mpv interface-change record](https://github.com/mpv-player/mpv/blob/304426c390901436fb1d4a63efbd582ae80c88f4/DOCS/interface-changes.rst)
  — announces the D3D11 composition options but not the swap-chain property.
- [mpv v0.41.0 release](https://github.com/mpv-player/mpv/releases/tag/v0.41.0)
  — tagged source and the verified upstream Windows x64 player artifact.
- [mpv v0.41.0 client observation API](https://github.com/mpv-player/mpv/blob/v0.41.0/include/mpv/client.h#L1172-L1228)
  — initial notification behavior and the warning that some properties may not
  notify every later change.
- [Official libmpv examples](https://github.com/mpv-player/mpv-examples/tree/master/libmpv)
  — client API and render integration patterns.
- [Existing Coax product requirements](https://github.com/jonocairns/coax/blob/main/PRD.md)
  — product scope, playback corpus, security model and live-TV UX requirements
  retained unless superseded here.
