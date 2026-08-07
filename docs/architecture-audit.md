---
description: Architecture audit of the Coax source tree — what the structure gets right, where it does not hold, and the evidence behind each finding.
tags: [architecture, audit, testing, ci, portability, threading, review]
---

# Architecture audit

Audited at `f8a77d8`. Every finding below was read against the tree at
`5c1e05f` and re-checked at `f8a77d8`; nothing under `src/` changed between the
two, so the line references hold.

Findings 1 and the two live-sync defects it enabled testing for have since been
fixed, at `41843f9` and `5388982`; findings 2 and 8 at `dcba234` and `8deb198`;
findings 9 and 7 at `840f55a` and `5a691cc`; and the verification half of the P0
at `e8dc558`. Those entries are kept rather than deleted, marked with the commit
that closed them, so the reasoning stays readable and the same ground is not
re-audited. Every other finding is still open.

Line references for the still-open findings are as of `f8a77d8` unless a fix
since moved them, in which case that fix refreshed them: finding 10's
`log.cpp:22` is line 28 after finding 2's extraction, the function unchanged.
Finding 8's `mpv_player.cpp:189` was likewise correct when written and is line
180 today, moved by `5388982` rather than by anything in that finding.
`5a691cc` refreshed four more that its own edits to `app.cpp` and
`app_window.hpp` moved — finding 4's file length and `process_player_events`,
finding 6's `filtered()` call site, finding 13's `on_close`, and the two connect
functions under [Checked and clean](#checked-and-clean). Three of those were
already stale before it touched them; none of the underlying claims changed.

The detailed findings retain their discovery order. The priority table below is
the execution order: it balances impact, likelihood and cost rather than treating
source-file size or architectural neatness as severity.

`P0` blocks a release, `P1` should be fixed next, `P2` is important but can be
scheduled, and `P3` is cleanup or a narrowly exposed correctness issue.

Every claim here was verified mechanically. [How the findings were
checked](#how-the-findings-were-checked) gives the commands, and [Checked and
clean](#checked-and-clean) records what came back good — including five claims
raised during the audit that did not survive checking, so they are not raised
again.

## What holds up

**The portable core is real, not aspirational.** `coax_core` has no link
dependencies and is configured by a native GCC in the `.#core` shell. That is a
mechanical platform-boundary check rather than include discipline anyone has to
remember. Most projects claim a portable core; few enforce one with a second
build.

**The supervisor is a pure reducer.** `reduce_supervisor_state(state, event,
now, policy)` returns `{state, effects, transition}`
([supervisor.hpp:133](../src/core/supervisor.hpp)), and the host owns only
queueing, re-arming and callbacks. Effects are data, executed elsewhere.
`PlaybackSupervisor::dispatch` handles reentrancy correctly: an effect that
dispatches back into the supervisor is enqueued rather than recursing
([supervisor_host.cpp:14](../src/core/supervisor_host.cpp)). This is why five
attempts inside a 30-second episode can be tested against a fake clock with no
media anywhere near it.

**Generation fencing is a type, not an `int`,** and `decide_generation`
distinguishes `Stale` from `Current` from `Future`. For a player that reopens
streams under load this is the single highest-value decision in the tree.

**Policy is separated from mechanism and versioned.** `kRecoveryPolicyVersion`
reaches both the transition log and the diagnostics panel, so a log line from a
user's machine says which policy produced the behaviour it describes.

**The swap-chain epoch** ([presentation.hpp:25](../src/core/presentation.hpp))
solves ABA on an address mpv publishes with no ownership contract, and puts the
decision in a `constexpr` function in the core where it can be tested.

**Most secret-bearing paths are handled deliberately.** `http::get` never puts a
URL in an error string — only host and status. mpv's own log text is dropped
wholesale rather than filtered, because it can embed authenticated URLs. The
credential store uses `CryptProtectData`, `CRYPTPROTECT_UI_FORBIDDEN`, and
`SecureZeroMemory` on the decrypted DPAPI buffer. The one hole this audit found
was the separate URL-at-load log path (finding 8), where safety depended on
recognizing an Xtream-shaped URL; that dependence is gone, and every playback
target is now sanitized before it is logged whether its shape is recognized or
not.

## Priority

| Priority | Work | Why |
|---|---|---|
| P0 — release blocker, already tracked | Satisfy the libmpv/FFmpeg redistribution obligations ([PRD.md §8.2](../PRD.md#82-runtime-provenance-and-licensing)) | The shipped runtime is still not legally complete: mpv's and FFmpeg's licence terms and the corresponding source offer are not distributed with it. This is not newly discovered by this audit, so it is cross-referenced rather than counted as another finding. The other half of the row — content verification of the fetched archive — landed at `e8dc558`: `scripts/fetch-libmpv.sh` now checks a pinned SHA-256 between the download and the unpack, refuses to unpack a mismatch, and records the verified digest in `PINNED.txt`. An already-unpacked tree's bytes are deliberately not re-verified, which the script states rather than implies; its identity is, so a tree that does not match the current pin, or is missing a file the build or package consumes, falls through to a fresh verified fetch instead of being trusted. |
| ~~P1~~ Fixed at `dcba234` and `8deb198` | ~~Fix the log snapshot race (finding 2) and stop logging arbitrary authenticated URLs (finding 8)~~ | The ring is a portable class in `coax_core` handing out snapshots, and every playback target is sanitized before logging regardless of shape. Native CTest went from 98 cases to 111. |
| ~~P1~~ Fixed at `840f55a` and `5a691cc` | ~~Make presentation failure terminal without becoming an infinite spin, and propagate render-target recreation failure (findings 7 and 9)~~ | A resize that loses the render target now classifies the HRESULT and enters the same bounded rebuild, and the frame loop waits on its next deadline instead of spinning: measured at 96.8% of a core before and 0.1% after, in both the rebuilding and the terminal state. Native CTest went from 111 cases to 118. |
| ~~P1~~ Fixed at `5388982` | ~~Run the already-portable player suite in native CI and add `LiveSync` coverage (finding 1)~~ | The target split landed and the suite runs on every push. Native CTest went from 66 cases to 98. |
| P2 | Put `ChannelIndex` in the portable target and cache its derived view (findings 5 and 6) | This restores the documented boundary and removes full-catalogue work at frame rate. |
| P2 | Move the complete service tick out of `run()` and give deadlines a wakeup that survives a modal loop (finding 3) | Resize/move modal loops currently starve recovery and health work. `5a691cc` gave the frame loop a deadline-driven wait, but that wait does not run while Windows owns the thread, so this needs the hoisted tick and a `WM_TIMER` regardless. |
| P2 | Extract provider parsing/normalisation, then split playback orchestration from `App` behind a test seam (findings 11 and 4) | These are the largest remaining bodies of Coax-owned protocol logic with no runnable tests. |
| P2/P3 | Correct HTTP read failure and the installed log location; remove dead symbols (findings 10, 12 and 13) | These are real but narrower operational or correctness failures. |

## Findings

### 1. [Fixed at `41843f9` and `5388982`] Five of six `coax_player` units are portable, and their tests never ran

`coax_player` compiled the mpv adapter and the portable logic around it into
one target that linked libmpv. Only `mpv_player.cpp` actually needs Windows or
mpv. The other five — `live_sync.cpp`, `player_event_adapter.cpp`,
`recovery_effect_executor.cpp`, `transport_log_classifier.cpp` and
`load_diagnostics.cpp` — compiled clean under a native GCC with `-Wall -Wextra
-Wpedantic`. `load_diagnostics.cpp` qualified because `win/com_ptr.hpp`
includes only `<utility>`: it is a pure template over a forward-declared
`IUnknown`, and nothing in it touches a Windows header.

Because the target linked libmpv, `coax_player_adapter_tests` was built as a
Windows binary on a Linux runner and never executed. AGENTS.md and the CI
comment both said so, so this was a known limitation rather than an oversight.
What the audit added is that the limitation was unnecessary: the suite built and
passed natively, unmodified, in about a second.

```text
All tests passed (120 assertions in 15 test cases)
```

The file was named for the event adapter but covers five units — the adapter,
the buffer-phase gate, `reset_load_observations`, `execute_recovery_effect` and
`classify_transport_log`. That is the correlation model that decides whether one
physical failure spends one recovery attempt or two, and nothing verified it on
any push.

**What landed.** `coax_player_core` holds the five portable units and links only
`coax_core`, with no platform or engine library, so the `.#core` shell configures
and tests it exactly as it does `coax_core`; `coax_mpv` is `mpv_player.cpp`
alone. There were no logic changes and no porting — the code was already
portable, as the compile matrix showed.
The test file is now `test/player/player_core_tests.cpp`, named for what it
covers. `Diagnostics` and `reset_load_observations` moved to
`player/load_diagnostics.hpp`: the portable half reads and resets them, and
resting that on `mpv_player.hpp` meant resting it on `IUnknown` being
forward-declared and `win::ComPtr` not instantiating a Windows call in that
translation unit.

`LiveSync` was the one unit here with no test at all. It is a reduced version of
ExoPlayer's `DefaultLivePlaybackSpeedControl` — a control loop with six tuning
constants — and exactly the shape that wants a table test. It has 16 cases now,
added with the live-sync fixes in `5388982`. Native CTest went to 98 cases from
66, and stands at 111 after findings 2 and 8.

### 2. [Fixed at `dcba234`] `log::recent()` hands the UI thread a vector the workers are mutating

`log::write` takes `g_mutex` to mutate `g_recent`, including
`erase(g_recent.begin())` once the 400-entry ring fills.
[`log::recent()`](../src/util/log.cpp) returns `const std::vector<std::string>&`
with **no lock**, and `draw_diagnostics()` iterates it on the UI thread.

Both workers log while running: the connect thread at
[xtream_client.cpp:165](../src/xtream/xtream_client.cpp), the update thread at
[update_check.cpp:38](../src/app/update_check.cpp). `draw_diagnostics()` is
called outside the stage switch in `draw_frame`, so it is live during the login
screen — which is precisely when the update thread is running. Opening F1 and
expanding Log during a catalogue fetch iterates a vector another thread is
reallocating.

Return a snapshot taken under the lock, or fill a caller-provided buffer. While
there, note that `write` takes the lock twice per call; the `fflush` per line is
defensible for a crash log, the double lock is not.

**What landed.** The ring is now `coax::log::Ring` in
[log_ring.cpp](../src/util/log_ring.cpp), built into `coax_core` beside
`redact.cpp`; the Windows file and `OutputDebugString` sink stays in `log.cpp`.
That split is finding 1's, for finding 1's reason: `log.cpp` includes
`<windows.h>` and builds only into the `coax` executable, so a fix landing there
alone would have been untestable by CI, and the sharing rules are exactly the
part worth testing. `recent()` returns a copy taken under the lock and
`recent_into()` fills a caller-owned buffer, which is what the panel uses, so
redrawing while the Log header is expanded copies without allocating.

Two things came along with the extraction. `write` no longer locks the same
mutex twice — the ring owns its lock and the session-log stream has a separate
one, so a blocking disk write no longer holds up the panel — and a full ring
overwrites its oldest slot rather than `erase`ing the front, so the steady state
no longer shifts the whole buffer per line.

The fix was checked with ThreadSanitizer rather than by argument: the new ring
under two writers and a concurrent reader is clean, and a reduction of the old
unlocked-reference pattern under the same tool reports the data race. The
negative control matters — without it, "TSan is clean" would only mean TSan was
not looking.

### 3. [P2] The frame tick lives in `run()`, and a resize drag does not run it

`run()` does pump → `process_player_events` → `service_presentation` →
`supervisor_.poll` → `sample_playback_health` → live sync → `draw_frame`. But
during a sizing or moving drag the system enters a modal loop inside
`DefWindowProc`, and per Microsoft's documentation the operation *"is complete
when DefWindowProc returns"* — so `pump_messages()` does not return for the
whole gesture. `draw_frame()` still runs, because WM_SIZE and WM_PAINT call the
paint handler directly ([app_window.cpp:60](../src/win/app_window.cpp)).

Drawing therefore continues while nothing else does. The consequence that
matters is the supervisor. `supervisor_.poll()` cannot run, but the recovery
budget is measured against a real `steady_clock`:
[supervisor.cpp:100](../src/core/supervisor.cpp) fails the episode when a
subsequent failure would schedule `retry_at` beyond
`started + policy.wall_clock_budget`. Dragging the window for ten seconds
mid-recovery therefore spends a third of the 30-second budget on nothing and
can make the next failure terminal.

It does **not**, however, deterministically land in `Failed{BudgetExpired}` on
the first poll after the mouse comes up. The deadline reducer at
[supervisor.cpp:128](../src/core/supervisor.cpp) starts an already-scheduled
recovery effect without re-checking the wall-clock budget. Whether the first
post-drag turn fails, starts that overdue attempt, or processes a queued player
failure first depends on which state and events existed when the modal loop
began. The starvation and lost budget are real; the direct-failure sequence is
not universal.

The comment on the hoisted paint handler is right about why drawing belongs in
the message loop. The conclusion should have been to hoist the whole tick.
Extract the body of `run()`'s loop into a `service()` method and call it from
both places — the same comment already argues the reentrancy is safe, because
the paint handler is only reached from the message pump, which never runs
mid-frame. Paint and size messages alone are not a deadline mechanism, though:
if the pointer stops while Windows still owns the modal loop, neither is
guaranteed to arrive at the supervisor deadline. A timer or message-wait timeout
should provide the wakeup.

Half of that wakeup now exists, and it is worth being precise about which half.
`5a691cc` gave the frame loop a deadline-driven wait for finding 7's sake, and
`core::decide_frame_wait` already takes the supervisor's next deadline as one of
its inputs. What it does not do is run inside the modal loop, which is the whole
of this finding: nothing in that commit hoists the tick, and between
`WM_ENTERSIZEMOVE` and `WM_EXITSIZEMOVE` the application's pump — wait and all —
is not running. A `WM_TIMER` armed for the same deadline is the remaining piece,
and it wants the extracted `service()` to have something to call.

Mpv's event queue is **not** a serious risk here, for the record: property
changes are coalesced (see [Checked and clean](#checked-and-clean)), so the
starved `player_.pump()` is not the problem. The starved `supervisor_.poll()`
is.

### 4. [P2] `App` is four objects

[app.cpp](../src/app/app.cpp) is 1685 lines and `App` carries around fifty
members. It is at once the ImGui view (roughly 800 lines across `draw_login`,
`draw_browser`, `draw_status_bar` and `draw_diagnostics`), the playback
orchestrator, the presentation-lifetime manager, and the session and
credentials model. The class holds `search_was_active_`, `pre_mute_volume_` and
`overlay_menu_open_` — pure view state — next to `last_cache_state_dispatched_`,
`pending_stream_ends_` and `exact_failure_reported_`, which are playback
protocol state.

Size is not the complaint. The complaint is that everything *below* `App` is
testable and mostly tested, while `App` holds several hundred lines of
orchestration that is not UI and cannot be reached by a test.
`process_player_events` ([app.cpp:485](../src/app/app.cpp)) contains real
protocol logic: the exact-failure suppression rule and the 50 ms pending
stream-end window together decide whether one provider failure costs one
recovery attempt or two. That is supervisor-grade reasoning living in the view
layer.

Extract a `PlaybackSession` owning the supervisor, health fold, generation and
player-event translation. `App` becomes view plus wiring. Merely moving the
concrete `MpvPlayer` into that class does not make it testable: the session must
either depend on an injected player interface/fake, or itself be a pure
coordinator that emits player commands as data. This is the largest piece of
work on the list and should be done deliberately, after the cheaper items above
have made the surrounding code testable.

### 5. [P2] `channel_index` is core code that escaped the core target

`src/core/channel_index.cpp` is in namespace `coax::core`, and its header says
it is *"the part of the application a second platform would reuse unchanged"*.
It compiles clean natively. But
[CMakeLists.txt:144](../CMakeLists.txt) builds it into the `coax` executable
rather than into `coax_core`, so it is not built by the `.#core` shell, not
touched by CI, and has no tests — while AGENTS.md states that anything in
`coax_core` needs a test under `test/`.

The rule is right; the file slipped past it by living in `src/core/` without
being in the target. Two lines of CMake and a test file.

### 6. [P2] `filtered()` rebuilds the catalogue view every frame

`draw_browser` calls `channels_.filtered(search_)` at
[app.cpp:917](../src/app/app.cpp), guarded only by `show_browser_` and the
browsing stage. There is exactly one call site and it is unconditional per
frame: lowercase the query, build an `unordered_map` over every category, push a
pointer per surviving channel, then `erase_if` the empty groups.

For the tens of thousands of channels the README advertises, that is several
allocations and tens of thousands of pointer writes at frame rate, producing a
result that only changes when the query or the catalogue changes. The
`ImGuiListClipper` below it bounds ImGui submission, not this. Cache the result
against the query string and invalidate in `reset()`. The cost is established
from the call graph and algorithm; its precise frame-time impact has not been
benchmarked.

### 7. [Fixed at `5a691cc`] A presentation rebuild can busy-wait forever

While a rebuild is outstanding, `presentation_ready_` is false, so `draw_frame`
returns before reaching `Present`. `PresentationRebuildBudget::poll` returns
`Hold` until the retry delay elapses
([presentation.cpp:34](../src/core/presentation.cpp)), and `run()` has no sleep
and a non-blocking `PeekMessage`.

Vsync inside `Present` is the only throttle the frame loop has, and this path
bypasses it. The process therefore spins a core across the retry delays — on a
machine that has just lost its display adapter.

The terminal case is worse. When the fifth rebuild fails,
`PresentationRebuildBudget::poll` reports `Exhausted` once, clears
`outstanding_`, and subsequently returns `Hold`. `presentation_ready_` remains
false forever, so `draw_frame()` never reaches `Present` again and the loop has
no throttle for the rest of the process lifetime. This is not a five-second
spike; it is an indefinite busy-wait until the user closes the application.

Use `MsgWaitForMultipleObjects` (or an equivalent event-loop wait) with the next
presentation or supervisor deadline as its timeout. Exhaustion also needs an
explicit terminal presentation state that waits for messages without pretending
another rebuild is pending.

**What landed.** Both, split the way finding 2 was and for the same reason: the
message loop is Windows-only and CI cannot run it, so as much of the decision as
possible is portable and tested, and the platform sink is kept small enough to
read.

`coax_core` gains three things.
[`PresentationPhase`](../src/core/presentation.hpp) names what the budget alone
could not — `Rebuilding` will get another attempt, `Failed` never will — and
`decide_presentation_phase` derives it from the surface's usability and the
budget's exhaustion rather than tracking it, so a usable surface is `Ready`
whatever the budget has been through.
`PresentationRebuildBudget::next_decision_at` says when `poll` will next decide
something: the retry delay, or immediately when the ceiling is already reached
and the next poll will report exhaustion whatever the clock says, or never once
it is spent. `decide_frame_wait` turns the phase and the deadlines into how long
the loop may block — nothing in `Ready`, where the vsync wait inside `Present`
is the throttle, and otherwise the nearest of the presentation and supervisor
deadlines. Seven test cases, and native CTest went from 111 to 118.

The wait is ceilinged at 50 ms. That is not a fudge factor: it is the shortest
deadline in the frame loop that this decision does not model — the pending
stream-end window at [app.cpp:519](../src/app/app.cpp) — so nothing the loop
schedules can be made more than one tick late by sleeping. Health sampling is
500 ms and everything else is longer.

On the Windows side `AppWindow::pump_messages` takes a timeout and waits with
`MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT,
MWMO_INPUTAVAILABLE)` before draining
([app_window.cpp:308](../src/win/app_window.cpp)). `MWMO_INPUTAVAILABLE` is
required rather than defensive: `PeekMessage` marks everything it sees as old,
and without the flag Microsoft documents that *"the existing unread input
(received prior to the last time the thread checked the queue) is ignored"* — so
a wait entered right after a drain would sleep through whatever the drain left
behind. Zero handles is the documented *"waits only for an input event"* form.
The window procedure validates every `WM_PAINT` it is given, so a pending paint
cannot hold `QS_PAINT` set and turn the wait straight back into the spin.

One thing the finding did not say, found while fixing it. `presentation_ready_`
is not sufficient to decide that a turn will present: `end_frame` also returns
before `Present` on a lost device or a missing render target, either of which
with the phase reading `Ready` is the same busy-wait by another route.
`App::presentation_phase()` folds both in.

The terminal state remains visually blank, and that is not fixed here. `Failed`
sets a status the user cannot see, because the surface that would draw it is the
one that is gone; surfacing it would need a device-independent paint path, which
is a feature rather than a defect in this one. What the finding was about — the
core — is now idle.

### 8. [Fixed at `8deb198`] The load log can persist an arbitrary authenticated URL

`MpvPlayer::issue_load` logs every target through `redact_stream_url` at
[mpv_player.cpp:180](../src/player/mpv_player.cpp). That helper masked credentials
only when the URL contains an Xtream-shaped `/live/`, `/movie/` or `/series/`
path. If none is present it deliberately returns the input unchanged
([redact.cpp:33](../src/util/redact.cpp)); a test pins that behaviour.

The application also accepts an arbitrary direct-media URL from the command line
([main.cpp:40](../src/main.cpp)). A target such as
`https://example.invalid/stream?token=secret` is consequently written verbatim
to `coax.log`. Userinfo or query credentials embedded in a provider base URL can
survive too: masking the two path segments after `/live/` does not sanitize the
authority or an earlier query.

This contradicts PRD §7.7's prohibition on full authenticated URLs in logs. Do
not make persistence depend on recognizing every secret-bearing URL shape.
Construct the log value from known-safe fields — for example scheme, host and
internal channel identifier — or apply a general authority/query sanitizer
before the path-specific mask.

**What landed.** The second option, by composition rather than new parsing:
`redact_stream_url` is `redact_portal_url` followed by the Xtream path mask. The
general sanitizer masks userinfo and drops the query for every target, and the
path mask only refines what survives it — so recognising the shape decides how
much more is hidden, never whether anything is. `redact_portal_url` already had
the authority and query machinery, and it was already tested.

Review caught that the composition alone left a residual hole of the same kind,
and it is fixed in the same PR. The path mask required a *third* segment after
the credential pair, so `http://host/live/user/pass` — a target with nothing
following the password — matched the marker, failed the segment count and was
logged whole. Two credential segments being present is now the whole condition;
what follows them is preserved but decides nothing. Coax's own
`Client::stream_url` always appends `/{id}.ts` and cannot produce the truncated
form, so the reachable route was the direct-media command-line argument — the
same untrusted-input path this finding is about.

The test that pinned the old behaviour deserves an exact account, because it is
not the one this document implied. Its two assertions still pass: neither
example carries a query, userinfo or fragment, so both URLs survive intact for a
narrower reason than the case claimed. What encoded the defect was the case's
name — "a stream URL without the expected shape is left alone" — which stated
the bug as a guarantee. It is renamed to the fact it actually establishes, and
the guarantee it used to imply is now covered by cases that were run against the
pre-fix implementation and fail there, on exactly the token-in-query and userinfo
URLs.

### 9. [Fixed at `840f55a`] Resize can lose the UI render target without entering recovery

After `ResizeBuffers` succeeds, `UiLayer::resize` calls
`create_render_target()` and discards its Boolean result
(`ui_layer.cpp:176` as it then was; the discard is gone, so that line no longer
resolves). `create_render_target` in turn
collapses both `GetBuffer` and `CreateRenderTargetView` HRESULTs to `false`, so
the caller cannot classify a device removal or report any other cause.

If either operation fails, `render_target_` remains null. `UiLayer::end_frame`
then returns before `Present` (`ui_layer.cpp:187` then,
[ui_layer.cpp:203](../src/win/ui_layer.cpp) now — that guard is unchanged), no
device loss is latched, and `service_presentation` has nothing to rebuild. The
application can remain blank and unthrottled indefinitely. A same-size retry is
also short-circuited because `width_` and `height_` were updated before render
target creation.

Preserve the HRESULT, feed device removal/reset through `note_result`, and make
any other render-target failure visible to the presentation owner. A resize is
complete only after both buffers and the new target exist.

**What landed.** All three, in the order the finding gives them.
`create_render_target` returns the failing HRESULT
([ui_layer.cpp:148](../src/win/ui_layer.cpp)) and `resize` feeds it to
`note_result` ([ui_layer.cpp:183](../src/win/ui_layer.cpp)), which is the
existing classifier: it latches a loss for `DXGI_ERROR_DEVICE_REMOVED` and
`DXGI_ERROR_DEVICE_RESET` and logs anything else with its code. `width_` and
`height_` are recorded only once `ResizeBuffers` *and* the new target have both
succeeded, so a same-size retry is no longer short-circuited by a size the
surface never actually reached.

That leaves the third part, and it needed something the finding did not name.
The non-loss causes latch nothing by design, and no later call reports them
either, because `end_frame` stops before submitting anything — so there is no
edge to drive recovery from. `UiLayer::has_render_target()` exposes the fact
directly and `App::service_presentation` polls it
([app.cpp:267](../src/app/app.cpp)), requesting the same bounded rebuild the
device-loss path uses, guarded to fire once per episode rather than once per
frame.

Nothing portable was added. The classification this routes into —
`DeviceLossLatch` and `PresentationRebuildBudget` — is already in `coax_core`
and already covered; what is new here is HRESULT handling and DXGI call ordering
in a translation unit that builds only into `coax.exe`, which is why the two
findings in this row split the way they did.

### 10. [P2] The installed application normally has no persistent session log

`session_log()` opens `coax.log` beside the running executable with `_wfopen`
and silently disables file logging when that fails
([log.cpp:28](../src/util/log.cpp)). The portable archive may live in a writable
directory, but the primary installer deliberately installs under Program Files
([packaging.md:16](packaging.md)). An ordinarily launched, unelevated process
does not have write access to that directory under normal Windows ACLs. UAC file
virtualisation does not rescue it: Microsoft documents that it supports only
32-bit applications, while Coax is native 64-bit
([UAC architecture](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/architecture#virtualization)).

That makes the session log least likely to exist for the users the installer is
intended for, even though the comment calls it the only post-fact record of a GUI
process failure. Put it under `%LOCALAPPDATA%\Coax\Logs` and retain bounded
rotation. Portable beside-executable logging can remain an explicit mode when
the directory is writable.

### 11. [P2] Provider parsing and normalisation are platform-coupled and untested

`xtream::Client::fetch_catalog` combines WinHTTP transport, response-shape
validation, authentication interpretation, JSON parsing and channel/category
normalisation in one Windows-only translation unit
([xtream_client.cpp:102](../src/xtream/xtream_client.cpp)). There are no provider
tests. This falls short of PRD §7.5, which names provider authentication and
channel normalisation among the logic that must not depend on Win32 types.

Keep WinHTTP as the adapter, but extract pure functions that consume a response
body and return a normalized catalogue or closed error. Malformed JSON, numeric
versus string fields, rejected credentials, unknown categories and whitespace
normalisation can then run in the native suite without inventing a network
abstraction.

### 12. [P2] An HTTP read failure is treated as successful end-of-body

The response loop at [http.cpp:138](../src/util/http.cpp) breaks when
`WinHttpQueryDataAvailable` fails *or* when it successfully reports zero bytes.
Those are different outcomes, but both return `true`. A broken or truncated
transfer can therefore surface later as a misleading JSON/provider error built
from a partial body.

Check the API result separately from `available == 0`, populate the transport
error on failure, and likewise check the status-query call before trusting the
status value.

### 13. [P3] Symbols with no callers

Verified at zero call sites across `src/` and `test/`:

| Symbol | Where |
|---|---|
| `ChannelIndex::find` | [channel_index.hpp:32](../src/core/channel_index.hpp) |
| `ChannelIndex::category_count` | [channel_index.hpp:35](../src/core/channel_index.hpp) |
| `ChannelIndex::empty` | [channel_index.hpp:36](../src/core/channel_index.hpp) |
| `AppWindow::on_close` and `close_handler_` | [app_window.hpp:40](../src/win/app_window.hpp) — never registered, so the guarded call site is dead |
| `xtream::Client::credentials` | [xtream_client.hpp:35](../src/xtream/xtream_client.hpp) |

## How the findings were checked

**Native portability.** Every file claimed portable was compiled with the
system GCC, outside both nix shells, with the project's own warning flags:

```bash
g++ -std=c++20 -Wall -Wextra -Wpedantic -I src -fsyntax-only src/player/live_sync.cpp src/player/player_event_adapter.cpp src/player/recovery_effect_executor.cpp src/player/transport_log_classifier.cpp src/player/load_diagnostics.cpp src/core/channel_index.cpp
```

All six compile clean. As a negative control, `mpv_player.cpp` fails on
`unknwn.h`, and it is the only file under `src/player/` that does.

**The orphaned test suite.** Built against the Catch2 already fetched into
`build-core/`, linking the five portable player units and the core sources:

```bash
g++ -std=c++20 -I src -I build-core/_deps/catch2-src/src -I build-core/_deps/catch2-build/generated-includes test/player/player_event_adapter_tests.cpp src/player/player_event_adapter.cpp src/player/load_diagnostics.cpp src/player/recovery_effect_executor.cpp src/player/transport_log_classifier.cpp src/player/live_sync.cpp src/core/supervisor.cpp src/core/playback_health.cpp src/core/presentation.cpp src/core/supervisor_host.cpp src/core/version.cpp src/util/redact.cpp build-core/_deps/catch2-build/src/libCatch2Main.a build-core/_deps/catch2-build/src/libCatch2.a -o /tmp/adapter_tests && /tmp/adapter_tests
```

15 test cases, 120 assertions, all passing. Both this and the portability matrix
above were re-run before the split and reproduced exactly. The command is kept
as the evidence that led to finding 1; the ordinary build now does this, so
there is no longer any reason to run it by hand.

**The ordinary build paths.** The native `.#core` configuration passed all 66
discovered tests, and the mingw configuration completed a clean Windows
cross-build at `f8a77d8`. These did not close finding 1 — the 15 player cases
were absent from native CTest — but they established that the findings were not
artifacts of an already-broken tree. The same command now runs 118
cases, and the cross-build still produces `coax.exe`.

**The busy-wait measurement** deserves its own account, because what it
simulated matters. The probe runs `App::run`'s loop shape — the same pump, the
same `service_presentation` decisions, the same early return where `draw_frame`
gives up — around the real `coax_core` budget, phase and wait, compiled from
`src/core/presentation.cpp`. The retry delay and the attempt ceiling are the
shipped ones. Two things stand in. The device loss is simulated: the rebuild is
told to fail rather than an adapter actually being removed. And on the run that
produced the numbers above, the wait is `poll()` on a pipe with a timeout rather
than `MsgWaitForMultipleObjectsEx` — both a kernel wait for either an event or a
deadline, neither consuming CPU while it waits.

The pre-fix spin was reproduced once against the real Win32 pump, at 80.9% of a
core and 52,074,569 turns in five seconds, before Windows Defender quarantined
the unsigned probe binary. The post-fix wait was **not** measured on Windows —
only cross-compiled and linked. What that leaves unverified is the behaviour of
`MsgWaitForMultipleObjectsEx` specifically, not whether a deadline-driven wait
removes the spin.

**How the fixes above were checked**, since a fix asserted is worth no more than
a finding asserted. Each has a negative control, because a check that cannot
fail proves nothing:

| Fix | Check | Control |
|---|---|---|
| Archive verification (P0, first half) | One byte of the real 29 MiB archive flipped, the script run against it: exit 1, `third_party/mpv` never created | The same local-URL script over the unmodified bytes still unpacks, so the rejection is the digest and not the transport |
| The log ring (finding 2) | ThreadSanitizer over two writers and a concurrent reader: clean | A reduction of the old unlocked-reference pattern, same tool, reports the data race |
| Stream redaction (finding 8) | The new cases pass against the fix | They were run against the pre-fix `redact.cpp` and fail there, on exactly the token-in-query and userinfo URLs |
| The frame-loop wait (finding 7) | A probe driving the real budget through `run()`'s loop shape: 0.1% of a core over five seconds, both mid-rebuild and terminal | The same post-fix code with the phase forced to `Ready` still spins at 96.0%, so the idle is the wait decision and not the harness having nothing to do |
| Recovery is not traded for the idle (finding 7) | The same probe still spends six rebuild attempts in five seconds against the shipped 1 s retry delay | The pre-fix spin spends five in the same window; sleeping through the retry delay would have shown as fewer, not more |

The pinned digest was obtained by downloading the archive twice and confirming
both copies hash identically, then unpacking it and diffing against the
`third_party/mpv` already in the tree — byte-identical, so the digest describes
the runtime that is actually shipped rather than merely a download that
succeeded.

**External behaviour** was checked against primary sources rather than memory:

| Claim | Source |
|---|---|
| mpv coalesces property-change events | [`client.h:1185`](../third_party/mpv/include/mpv/client.h) in the pinned runtime — the same range PRD.md already cites |
| mpv's event ring is 1000 entries; async replies are reserved | [mpv `player/client.c` at the pinned commit](https://github.com/mpv-player/mpv/blob/304426c390901436fb1d4a63efbd582ae80c88f4/player/client.c) |
| A sizing drag runs a modal loop the application's pump does not | [WM_ENTERSIZEMOVE](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-entersizemove) |
| A message wait ignores queued input already seen by `PeekMessage` unless `MWMO_INPUTAVAILABLE` is set, and `nCount` of zero waits only for input | [MsgWaitForMultipleObjectsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex) |
| Program Files writes are not virtualised for a native 64-bit application | [UAC architecture](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/architecture#virtualization) |
| DPAPI optional entropy has to be supplied again to decrypt | [`CryptProtectData`](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata) and [`CryptUnprotectData`](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptunprotectdata) |
| The six `LiveSyncConfig` defaults | [AndroidX `DefaultLivePlaybackSpeedControl` at the revision checked on 2026-08-05](https://github.com/androidx/media/blob/5fb306449733dd71595700c1227ad6087578c559/libraries/exoplayer/src/main/java/androidx/media3/exoplayer/DefaultLivePlaybackSpeedControl.java) |
| nlohmann parse errors echo only a short `last read` token | [Parsing and exceptions](https://json.nlohmann.me/features/parsing/parse_exceptions/) |

## Checked and clean

**The ExoPlayer provenance is exact.** All six constants cited in
`live_sync.hpp` — `0.97`, `1.03`, `0.1`, `20 ms`, `1000 ms`, `500 ms` — match
the media3 source verbatim. The comments naming them are accurate.

**The credential store meets the encrypted-at-rest requirement.** It uses
`CryptProtectData` with optional entropy and `CRYPTPROTECT_UI_FORBIDDEN`, and
`SecureZeroMemory` over the decrypted DPAPI buffer before it is released. The
comment at [credential_store.cpp:15](../src/win/credential_store.cpp) overstates
what the entropy buys: it is a constant embedded in the binary, so it does not
prevent another same-user process that knows the constant from calling DPAPI.
That is a threat-model/documentation correction, not a failure of the stated
at-rest requirement. Microsoft's API contract says that the same optional
entropy must be supplied to decrypt; the point here is that this entropy is
publicly recoverable rather than independently secret.

**No credentials reach the log through HTTP error construction.** `util::http::get`
composes its error strings from the cracked host and the status code, never the
URL, which is what keeps the authenticated `player_api.php` query out of
`coax.log`. Finding 8 is a separate load-log path and is not contradicted by
this result.

Five claims raised during the audit did **not** survive checking, recorded here
so they are not raised again:

- **A resize drag can overflow mpv's event queue and force a player
  recreation.** It cannot, by the mechanism proposed. Property changes are
  coalesced — at most one pending event per observed property, delivered only
  once the queue drains — and async command replies hold reserved slots, so
  neither can fill the 1000-entry ring. Overflow would need a burst of
  uncoalesced `MPV_EVENT_LOG_MESSAGE` at warn level. Finding 3 stands on
  supervisor starvation and elapsed wall-clock budget instead; its exact
  post-drag transition is state-dependent as that finding now records.
- **`transport_log_classifier` and `recovery_effect_executor` are untested.**
  Both were tested, in the suite that never ran — test cases at lines 233, 270,
  291, 309 and 363 of what was then
  `test/player/player_event_adapter_tests.cpp`, now
  `test/player/player_core_tests.cpp`. The `RecoveryExecutor`
  struct-of-functions seam is used by two of them.
- **`App` holds `client_` and `credentials_` redundantly.** It does not.
  `credentials_` carries the portal across the asynchronous gap between
  `begin_connect` (line 371) and `finish_connect` (line 416), where `client_`
  does not yet exist. Only the `Client::credentials()` *accessor* is dead, which
  is finding 13.
- **The direct-media argument conversion writes one byte beyond its string.**
  The first `WideCharToMultiByte` result includes the null terminator and the
  string's logical size does not, which makes the second call's `size` capacity
  look one byte too large. `std::basic_string` nevertheless guarantees the
  null element at `data()[size()]`, and this conversion writes that element back
  as null. It is a brittle-looking contract worth simplifying when the path is
  touched, but it is not an out-of-bounds write.
- **A provider error body could carry credentials into the log via
  nlohmann's `what()`.** The `last read` field is a few characters of context,
  not a window over the input, and the URL is never part of the parsed body in
  any case.

One earlier finding — `.direnv/` untracked and unignored — was fixed by
`f8a77d8` before this document was written.
