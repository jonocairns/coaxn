---
description: Task specification for owning the mpv composition swap chain's lifetime and recovering from display, power and D3D11 device loss.
tags: [handoff, plan, presentation, swapchain, directcomposition, d3d11, device-loss, lifecycle]
---

# Task: own the composition swap chain's lifetime and survive device loss

Implement PRD §8.1 and §8.2. They are one task: both end in
the same recovery path, and doing either alone leaves the other half of the
failure unhandled.

The presentation path itself works and is not being redesigned. What is missing
is ownership of the pointer it depends on, and any reaction at all when the
graphics device underneath it goes away. Today a driver reset, a GPU hang or a
sleep/resume cycle leaves a running application that will never draw again.

**Fix the lifetime, not the architecture.** Do not restructure the composition
tree, do not change how mpv is configured, and do not introduce a second
recovery state machine beside the one in `src/core/`.

## Read first

1. `PRD.md` §7.3 — the presentation contract and the four items still open.
2. `src/player/mpv_player.cpp` — `publish_swapchain`, the `kDisplaySwapchain`
   case in `handle_property`, `destroy_backend`, `recreate_player`.
3. `src/win/composition.{hpp,cpp}` — the visual tree and `set_video_content`.
4. `src/win/ui_layer.{hpp,cpp}` — the UI device, its swap chain, `end_frame`.
5. `src/win/app_window.cpp` — `handle_message`.
6. `src/app/app.cpp` — the `on_swapchain` callback in `initialize`, and
   `handle_resize`.

## What already exists — do not rebuild

- **Acquisition by observation.** `mpv_observe_property(..., MPV_FORMAT_INT64)`
  on `display-swapchain` is correct and works. The property is unavailable until
  the video output exists, so polling after `mpv_initialize` races startup.
- **Detach-before-teardown ordering.** `destroy_backend` calls
  `publish_swapchain(nullptr)` before `mpv_terminate_destroy`, which detaches
  the visual content before mpv can release the swap chain. That ordering is the
  pattern every new path must follow.
- **`ComPtr`** in `src/win/com_ptr.hpp`. Use it; do not add WRL or ATL.
- **Filter state across recreation.** `applied_filter_` is cleared on
  `MPV_EVENT_START_FILE` and reapplied on `MPV_EVENT_FILE_LOADED`, so VSR already
  survives backend recreation. Do not add a second mechanism for it.
- **Generation scoping and bounded recovery** in `src/core/`. Presentation loss
  must be funnelled into this vocabulary, not given its own.

## The holes

1. **No reference is ever taken.** `publish_swapchain` stores a raw `void*`
   (`mpv_player.cpp:362`). `IDCompositionVisual::SetContent` does take its own
   reference, so once attached the object cannot die underneath DirectComposition
   — but the window between reading the property value and calling `SetContent`
   is unowned, and mpv's VO teardown does not run on the UI thread.

2. **Identity is by address.** `publish_swapchain` returns early when the new
   pointer equals the stored one (`mpv_player.cpp:363`). A destroyed swap chain
   whose replacement is allocated at the same address is then silently treated as
   "no change" and never re-attached, leaving a dead visual.

3. **No fallback acquisition.** mpv's client API documents an initial
   notification but warns that some properties may not notify every later
   change. There is no `MPV_EVENT_VIDEO_RECONFIG` plus explicit-read path.

4. **Device loss is invisible.** `UiLayer::end_frame` discards `Present`'s
   HRESULT (`ui_layer.cpp:157`) and `resize` only logs `ResizeBuffers` failure
   (`ui_layer.cpp:121`). `DXGI_ERROR_DEVICE_REMOVED` and `DXGI_ERROR_DEVICE_RESET`
   are never noticed.

5. **No display or power messages.** `handle_message` covers `WM_SIZE`,
   `WM_PAINT`, `WM_DPICHANGED` and keys. There is no `WM_DISPLAYCHANGE` and no
   `WM_POWERBROADCAST`.

## Design constraints

**Two devices, one recovery.** `UiLayer` creates its own D3D11 device
(`ui_layer.cpp:31`) and mpv creates another internally. The DirectComposition
device is created from the UI device's `IDXGIDevice` (`composition.cpp:10`).
Loss can therefore be observed on either side, but the recovery is shared:
rebuild the UI device and composition tree, recreate the mpv backend, reattach,
resume the current channel.

**Recovery belongs to the core.** `core::RecoveryAction::RecreatePlayer` and
`Generation` already exist. Normalize presentation loss into a player-adapter
event and let the existing supervisor decide, so bounding and generation
scoping are inherited rather than reimplemented. Anything that is policy
(attempt bounds, epoch comparison) goes in `src/core/` and is Catch2-tested;
anything that touches DXGI stays in `src/win/` or `src/player/` and stays thin.

**`src/core/` must not acquire a Windows dependency.** The host-native core
build and its tests remain the mechanical proof.

**UI thread only.** No new threads, no blocking waits, no busy retry loop.

## Work

1. **Take and hold a reference.** `AddRef` on acquire, hold for as long as the
   swap chain is attached, `Release` only after the visual content is detached.
   Correct the comment at `composition.hpp:23-25`: DirectComposition does take a
   reference, but that fact alone does not make the borrowed pointer safe to
   keep or re-present.

2. **Stop trusting the address.** Carry a monotonically increasing swap-chain
   epoch, bumped whenever the video output is torn down or reconfigured. Equal
   addresses within an epoch are the same object; equal addresses across epochs
   are not, and must force a detach and re-attach. Duplicate notifications inside
   one epoch should still be suppressed — this is about correctness on reuse, not
   about re-attaching on every event.

3. **Add the reconfiguration fallback.** On `MPV_EVENT_VIDEO_RECONFIG`, read
   `display-swapchain` explicitly and publish the result. Log which path actually
   produced each acquisition; the answer settles what PRD §7.3 should record as
   the real contract, so report it rather than assuming the observation path is
   sufficient.

4. **Detect device loss.** Check the HRESULT from `Present` and `ResizeBuffers`.
   On `DXGI_ERROR_DEVICE_REMOVED` or `DXGI_ERROR_DEVICE_RESET`, log
   `GetDeviceRemovedReason` and raise exactly one loss event — not one per frame.

5. **Rebuild on loss.** Detach video content, tear down the UI layer and
   composition tree, recreate both, recreate the mpv backend, reattach, resume
   the current channel through the existing generation-scoped path. Bounded: a
   repeated failure surfaces a failed state, it does not loop.

6. **Handle display and power messages.** `WM_DISPLAYCHANGE` re-evaluates size
   and DPI and re-commits the tree. `WM_POWERBROADCAST` with
   `PBT_APMRESUMEAUTOMATIC` or `PBT_APMRESUMESUSPEND` treats the device as
   suspect and verifies it before continuing.

7. **Report it.** `Diagnostics::swapchain_state` is a two-value string today.
   Expand to attachment state, epoch, re-attach count and the last device-loss
   reason, and show them under F1.

## Invariants

- Detach visual content before mpv releases or replaces the swap chain.
- Never dereference, present or release a pointer from a previous epoch.
- One loss event per loss, never one per frame.
- A rebuild cannot resurrect a superseded channel; it goes through `Generation`.
- Rebuild attempts are bounded and surface failure honestly.
- Core stays free of Windows types; the host-native core tests keep passing.
- Comments record the hardware or API reasoning, not the syntax.

## Tests

Policy added to `src/core/` gets Catch2 coverage in `coax_core_tests`: epoch
comparison, bounded rebuild attempts, and the rule that a stale generation
cannot complete a rebuild.

DXGI and DirectComposition code is not unit-testable here. Keep it thin enough
that the untested surface is plumbing, and cover the decisions in core.

## Acceptance

- WHEN a `display-swapchain` value is observed, THE application SHALL hold a
  reference for as long as it is attached, and SHALL release it only after
  detaching the visual content.
- WHEN mpv replaces its swap chain at an address equal to the previous one, THE
  application SHALL still detach and re-attach.
- WHEN `display-swapchain` does not notify a later change, THE application SHALL
  acquire the replacement from `MPV_EVENT_VIDEO_RECONFIG` and an explicit
  property read.
- WHEN `Present` or `ResizeBuffers` reports `DXGI_ERROR_DEVICE_REMOVED` or
  `DXGI_ERROR_DEVICE_RESET`, THE application SHALL log the removal reason,
  rebuild the UI device, composition tree and mpv backend, and resume the
  current channel without restarting the process.
- WHEN a rebuild fails repeatedly, THE application SHALL surface a failed state
  rather than retry indefinitely.
- WHEN the machine sleeps and resumes, or a monitor is added, removed or
  changed, THE application SHALL continue playing with video visible.
- WHEN `F1` is pressed, THE diagnostics SHALL show attachment state, swap-chain
  epoch, re-attach count and the last device-loss reason.
- WHEN the work is complete, THE application window SHALL again be confirmed by
  human observation in a real application-window share, per PRD §6.1 — this
  change touches the composition tree, which is exactly what that clause exists
  for.
- WHEN the work is complete, THE open list in PRD.md §7.3 SHALL be replaced by
  the contract as implemented, and §8.1 and §8.2 SHALL be removed from remaining
  work.

## Build and verify

```bash
cmake -S . -B build-core -G Ninja -DCOAX_BUILD_APP=OFF -DBUILD_TESTING=ON
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

```bash
nix develop --command bash -c \
  'cmake --build build && ctest --test-dir build --output-on-failure'
```

Run on Windows:

```bash
cp build/coax.exe build/libmpv-2.dll /mnt/c/Users/jonoc/coax-poc/ \
  && cd /mnt/c/Users/jonoc/coax-poc && ./coax.exe
```

### Provoking the failures

Device loss cannot be honestly verified by reasoning about it. Real triggers on
the target machine:

- **Device removal:** disable and re-enable the display adapter in Device
  Manager, or install a driver update. Both produce a genuine
  `DXGI_ERROR_DEVICE_REMOVED` in a running application.
- **Sleep/resume:** `rundll32.exe powrprof.dll,SetSuspendState 0,1,0`.
- **Display change:** change the primary monitor's resolution or refresh rate,
  or attach and detach a second display, while playing.
- **Capture:** share the Coax window in the conferencing software that exposed
  the original failure, and watch it through a rebuild.

Operational notes:

- The session log is written beside the executable as `coax.log` and includes
  mpv warnings. A GUI-subsystem process has no console, so that file is the
  primary runtime diagnostic.
- Kill stray instances with `taskkill.exe /F /IM coax.exe` before copying a new
  build over the old one, or the copy fails with a permission error.
- Never persist raw mpv transport log lines that may contain authenticated URLs;
  log only sanitized classifications.
