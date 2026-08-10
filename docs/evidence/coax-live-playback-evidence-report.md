# Coax live playback rollback and recovery evidence report

**Source:** `build/coax.log`

**Evidence boundary:** lines 1–17,896, ending at `10:05:02.271`

**Scope:** one provider session, one channel-session alias, one playback generation

**Method:** read-only log analysis; no source-code changes
**Companion evidence:** [sanitized evidence appendix](coax-live-playback-sanitized-evidence.md)

> **Verdict:** The log contains strong evidence of a repeatable playback-timeline loop. Three consecutive recovery loads move backward by roughly 9–9.5 seconds while unpaused, advance normally again, then end at EOF. Actual earlier-content replay is plausible but not proven because the log observes timestamps, not media identity. Operationally, Coax should optimize for restoring healthy playback at the current live edge; it should not assume that provider-originated missing content can be recovered.

## 1. Privacy review

No obvious username, password, bearer/JWT-like token, API key, authorization header, cookie, signed query parameter, or URL userinfo was detected.

Two lines contain the exact provider portal host. It is private infrastructure information and is reproduced only as `https://[REDACTED_HOST]` in the evidence appendix:

- E01 / `coax.log:L10`: restored saved portal.
- E02 / `coax.log:L12`: provider session connected.

The URLs are host-only: no path, query, or userinfo was present. Nevertheless, the original log should not be shared publicly without redacting these two occurrences.

Playback request records are already sanitized. They report:

- HTTPS;
- Xtream-live target shape;
- `query=absent`;
- `userinfo=absent`; and
- HTTP method, Range, and headers unobserved below libmpv.

See E04.

This scan materially reduces the likelihood of an obvious credential leak, but cannot guarantee that every possible arbitrary secret format has been recognized.

## 2. Session division

| Identity | Observed value |
|---|---:|
| Provider sessions | 1 (`provider-session=1`) |
| Channel-session aliases | 1 (`channel-session=1`) |
| Playback generations | 1 (`generation=1`) |
| Channel selections | 1 |
| Fresh-selection loads | 1 |
| Recovery-reopen loads | 8 |
| Total loads | 9 |
| First-frame/playback starts | 8 |
| Loads failing before first frame | 1 |
| Player recreations | 0 logged |

Composition swap-chain detach/attach events occur during stream endings and reopens, but the log does not identify them as player recreations.

### Load attempts

| Load | Intent / supervisor attempt | Trigger | Result |
|---:|---|---|---|
| 1 | Fresh selection / 0 | Channel selection | First frame at `07:24:02`; long-running initial load. E03–E05. |
| 2 | Recovery reopen / 1 | Decode stall | First frame at `08:23:09`; later EOF after about 49m50s. E09. |
| 3 | Recovery reopen / 1 | Post-suspend EOF | Failed after 62 ms; no first frame. E10–E11. |
| 4 | Recovery reopen / 2 | Immediate retry | First frame at `09:13:04`; later progress stall. E11–E13. |
| 5 | Recovery reopen / 1 | Progress stall | First frame at `10:00:16`; EOF at `10:00:29`. E13–E14. |
| 6 | Recovery reopen / 1 | EOF | First rollback/advance/EOF repetition. E14–E15. |
| 7 | Recovery reopen / 1 | EOF | Second rollback/advance/EOF repetition. E16. |
| 8 | Recovery reopen / 1 | EOF | Third rollback/advance/EOF repetition. E17. |
| 9 | Recovery reopen / 1 | EOF | First frame at `10:01:47`; still active at the evidence cutoff. E18. |

The supervisor attempt number resets after a load becomes steady, so load ordinal and supervisor attempt are deliberately shown separately.

## 3. Concise event timeline

| Time | Event | Evidence and interpretation |
|---|---|---|
| `07:23:58–07:24:02` | Fresh selection | Load 1 reaches first frame. A small `+1.862s` startup jump follows. E03–E05. |
| `07:31:07–08:03:22` | Rebuffers 1–7 | Short cache pauses. `paused-no-progress` is explicitly `cache-paused=yes`. E06. |
| `08:00:14` | Large forward discontinuity | Playback `+91853.148s`; cache end `+91856.642s`; normal advancement immediately resumes. E07. |
| `08:22:51–08:23:06` | Backward reset and decode stall | Playback `-95383.170s`, followed by 29 no-progress samples over 14.1 seconds while unpaused. Cache intermittently advances; recovery follows. E08–E09. |
| `09:00:14` | Rebuffer 8 | Ordinary paused buffering. |
| `09:12:55–09:13:04` | Suspend/resume handover | `760.701s` telemetry gap, EOF, immediate failed reopen, successful retry, then a small startup jump. E10–E12. |
| `10:00:10–10:00:16` | Rebuffer 9 and progress stall | Paused no-progress leads to recovery load 5. E13. |
| `10:00:29–10:01:41` | Repeating short-load loop | Three consecutive active loads each roll back about 9–9.5s, advance, reach EOF, and reopen. E14–E17. |
| `10:01:47–10:02:04` | Final observed load | First frame, then rebuffers 10 and 11; playback subsequently continues through the cutoff. E18. |

### Timeline-classification totals

- 3 actual forward jumps.
- 4 actual backward movements.
- 29 no-progress samples, all in one 14.1-second unpaused episode.
- 11 paused-no-progress samples.
- 71 unavailable samples, concentrated around load boundaries and suspend.
- No `forward-lag` or `resume-lag` classifications.

The raw `kind=` occurrence count is twice the actual discontinuity count for backward/forward events because each event appears once as a timeline sample and once as a discontinuity warning.

## 4. Evidence consistent with replaying earlier content

The strongest sequence begins after the progress-stall recovery at `10:00:12`.

In plain terms, loads 6–8 behave like a short loop:

1. A recovery load reaches first frame.
2. Playback advances normally for approximately ten seconds.
3. The reported playback clock moves backward approximately ten seconds.
4. Playback is not cache-paused.
5. Normal forward movement resumes across the repeated timestamp interval.
6. EOF occurs approximately ten seconds later.
7. Coax reopens the same channel target and the pattern recurs.

| Load | First frame | Rollback | Playback movement | Deviation from elapsed time | Cache-end movement | Paused | EOF |
|---:|---|---|---:|---:|---:|---|---|
| 6 | `10:00:32.735` | `10:00:42.801` | `-9.321s` | `-9.825s` | `+0.051s` | No | `10:00:53.265` |
| 7 | `10:00:56.392` | `10:01:06.857` | `-9.500s` | `-10.005s` | `+0.017s` | No | `10:01:16.915` |
| 8 | `10:01:20.073` | `10:01:31.141` | `-9.033s` | `-10.026s` | `+0.000s` | No | `10:01:40.628` |

All three sequences have the same:

- provider session (`1`);
- channel-session alias (`1`);
- playback generation (`1`);
- recovery-reopen request intent;
- unpaused state;
- active-entry engine-message attribution;
- two engine messages in the rollback sample;
- player-warning category in that sample;
- preceding demuxer warnings, including `corrupt-packet`; and
- EOF/reopen behavior.

See E14–E17.

This pattern is consistent with either:

- the same short media section being returned or replayed after each reopen; or
- a finite/corrupt stream section reusing or resetting its timestamps at the same structural point.

It is not sufficient by itself to distinguish those two cases.

## 5. Distinguishing competing explanations

### Ordinary buffering

Ordinary buffering is directly observable elsewhere in the session as `cache-paused=yes` plus `paused-no-progress`. The three repeated rollback events are `cache-paused=no` and immediately resume forward movement.

Ordinary buffering does not explain the repeated negative timestamp movement well.

### UI-thread or sample delay

Two rollback samples have normal intervals of approximately `0.504–0.505s`. The third sample interval is `0.992s`, but its signed deviation remains `-10.026s` after elapsed time is accounted for.

The session does contain genuine longer UI/sample gaps:

- `2.880s` followed by normal `+2.960s` playback movement;
- `5.633s` followed by normal `+5.760s` playback movement; and
- `760.701s` during system suspend, with timeline data unavailable.

Those events demonstrate that elapsed-time correction behaves differently from the repeated rollbacks.

### Missing telemetry

Unavailable samples cluster around load boundaries and the system suspend. The three approximately ten-second rollback movements are directly recorded inside active loads; they are not inferred across missing samples.

Missing absolute playback and cache-end values still limit what can be proved about repeated timestamp ranges.

### Legitimate timestamp discontinuity

This is the strongest competing explanation.

Corrupt MPEG-TS packets or a muxer resetting/reusing PTS/DTS values could make new content revisit earlier timestamps without replaying earlier audiovisual content. The enormous `+91853.148s` and `-95383.170s` movements establish that this stream's timebase is unstable.

The huge forward jump's cache end moves by almost the same amount, and playback immediately continues normally. That strongly favors a timeline epoch/timebase change over a real 25-hour content skip.

### Channel change

No later channel selection, provider session, channel-session alias, or generation appears. The repeated problem therefore affects one observed alias only.

The log cannot show whether other channels from the same provider would behave similarly.

### Recovery handover

Every repeated rollback occurs after a recovery reopen. The endpoint, CDN, or intermediary could therefore be returning the same finite chunk after each new request.

However, the negative movement does not occur at the load boundary. It occurs about 10–11 seconds after first frame inside an active entry, which makes a simple stale handover event less likely.

## 6. Cache-end movement

Normal playback frequently has zero cache-end movement for several samples, followed by multi-second positive bursts. The largest normal cache-end burst in the fixed evidence window is `+10.464s`; this is compatible with chunked arrival and does not imply a playback jump.

Important correlations:

- Huge forward discontinuity: cache end also jumps `+91856.642s`, supporting a shared timebase reset. E07.
- Huge backward discontinuity: cache end is unavailable, followed by an unpaused decode stall. E08–E09.
- Repeated approximately ten-second rollbacks: cache-end movement is only `+0.051s`, `+0.017s`, and `+0.000s`. E15–E17.
- The decode-stall no-progress period sometimes shows cache-end bursts up to `+3.840s` while playback remains fixed, indicating that input/cache activity can continue while decoding or presentation does not. E09.

## 7. Engine messages and attribution

Within the evidence boundary:

- Timeline samples report 860 categorized engine messages across 29 samples.
- 30 diagnostic summary lines are attributed to generation 1's active entry.
- 1 diagnostic summary line is unattributed.
- The sole unattributed period contains 4 raw unattributed messages in one sample.

Diagnostic summaries are category-gated and therefore are not a raw-message count.

The unattributed event occurs in the replacement window of the 62 ms failed post-suspend load. It is followed by a structured stream error and a second recovery attempt. It is unrelated to the three repeated rollback events. See E11.

Each repeated rollback sample reports:

- `engine-messages-since-sample=2`;
- `unattributed-engine-messages-since-sample=0`;
- `warning-component=player`; and
- active-entry diagnostic warnings around the event.

## 8. What the log establishes

The log establishes that:

- Coax/libmpv reported repeated unpaused backward playback movement inside active entries.
- The events are correlated to the same provider session, channel alias, generation, request intent, warning pattern, near-zero cache-end movement, and EOF/reopen sequence.
- The approximately ten-second rollback/advance/EOF pattern occurs on three consecutive recovery loads.
- Ordinary buffering, UI delays, suspend gaps, unavailable telemetry, and load replacement windows are separately visible and do not match the repeated rollback signature.
- The issue is demonstrated on one channel-session alias only.

## 9. What the log cannot establish

The log cannot establish:

- that equal or repeated timestamps represent identical video frames or audio;
- byte identity between pre-jump and post-jump data;
- the contents of HTTP request or response bodies;
- raw HTTP headers, authorization values, or cookies;
- whether a Range request was sent;
- the requested or returned byte range;
- whether `Content-Range` was present;
- response ETag, safe content identity, or content length;
- connection reuse or pooling below libmpv;
- whether the provider, CDN, proxy, or client originated the repeated timestamp range; or
- whether the same problem affects other channels.

Equal timestamps can label different content after a legitimate MPEG-TS discontinuity. For that reason, the log proves playback-clock rollback, not content replay.

## 10. Alignment with Media3/ExoPlayer

The proposed recovery objective is broadly consistent with Android Media3/ExoPlayer, with an important distinction between adaptive and progressive live sources.

### Adaptive live streams

HLS and DASH expose a moving live window containing addressable media segments. If playback falls behind that window, Media3 documents re-initializing playback at the default live position with `seekToDefaultPosition()` and `prepare()`. That abandons the unavailable interval and resumes near the current live edge rather than reconstructing every missed frame.

Media3 also supports MPEG-TS as an HLS segment container. In that case, the playlist and segment sequence provide recovery structure that a single continuous URL does not.

### Progressive live streams

Media3 explicitly states that progressive live streams have no live window and can only be played at one position. Coax's logged source is a direct Xtream-live MPEG-TS request rather than a logged HLS playlist. For this shape of source, reopening normally means opening a new connection to whatever the provider is transmitting now.

Consequently:

- provider-originated MPEG-TS data that never arrived is normally unrecoverable;
- reopening can restore current playback but cannot be assumed to retrieve the missing interval;
- a timestamp rollback alone should not be treated as proof that old content is being replayed; and
- the recovery target should be the first healthy current-live frame, accepting that the viewer may see a gap.

### Retry hierarchy

Media3's default load-error policy gives progressive live loads more retry opportunities than ordinary loads: six minimum retries by default. Retry delay increases with the error count up to five seconds. Media3 also supports a custom error policy for applications that need to fail fast or use different backoff.

For terminal playback failures, Media3 supports application-level retry using `prepare()`. The cited Media3 guidance does not prescribe releasing and recreating the whole player as the next recovery step. In Coax, full mpv recreation would be a libmpv-specific last-resort experiment when source re-preparation produces repeated short-lived loads or engine state appears wedged; its benefit must be measured rather than claimed as ExoPlayer parity.

The closest Coax equivalents are:

| Media3/ExoPlayer concept | Coax equivalent |
|---|---|
| Retry or re-prepare the media source | `loadfile replace` recovery reopen |
| Resume adaptive playback at the live edge | Reopen the direct live URL and accept the first current frame supplied |
| No direct counterpart required by the cited guidance | Optional mpv recreation as a Coax-specific escalation |
| Custom fail-fast/backoff policy | Coax supervisor thresholds, pacing, attempt budget, and escalation |

This does not mean Coax should copy Media3's default retry timing. Television viewing values time-to-picture highly, and several seconds of progressive backoff may be worse than promptly opening a fresh live connection.

Official references:

- [Media3 live-streaming guidance](https://developer.android.com/media/media3/exoplayer/live-streaming)
- [Media3 default load-error policy](https://developer.android.com/reference/androidx/media3/exoplayer/upstream/DefaultLoadErrorHandlingPolicy)
- [Media3 player error and retry events](https://developer.android.com/media/media3/exoplayer/listening-to-player-events)
- [Media3 custom error handling](https://developer.android.com/media/media3/exoplayer/customization)
- [Media3 HLS support](https://developer.android.com/media/media3/exoplayer/hls)

## 11. Recommended next step

**Recommendation: optimize for rapid restoration of healthy forward playback, not recovery of the missing content.**

The missing interval may be permanently unavailable. Recovery is still useful because a fresh source connection can rejoin the provider's current output. The supervisor should therefore distinguish “content loss” from “playback health recovery”:

- **Content-loss recovery:** probably impossible for this direct live source unless the provider offers timeshift, DVR, HLS segments, or another addressable archive.
- **Playback-health recovery:** possible by abandoning the damaged connection and promptly reopening at the current live edge.

### Proposed recovery policy

1. **Tolerate isolated timestamp discontinuities while playback keeps advancing.**
   - Do not reopen solely because PTS moved backward.
   - Rebase internal timeline/control state when appropriate.
   - Continue observing forward progress and presentation health.

2. **Give explicit cache buffering a short grace period.**
   - `cache-paused=yes` is ordinary buffering evidence.
   - Avoid turning every short rebuffer into a reconnect.

3. **Reopen promptly on sustained unpaused no-progress.**
   - The 14.1-second unpaused stall at `08:22:51` is the clearest opportunity for faster recovery.
   - Decoder/demuxer warnings and cache-end movement can strengthen the signal, but forward progress should be decisive.

4. **Treat unexpected EOF as a live-source failure.**
   - Reopen immediately or with only the minimum pacing required to prevent a hot loop.
   - Do not wait for content that the closed connection can no longer deliver.

5. **Escalate repeated short-lived reopens.**
   - If several fresh reopens reach EOF again within roughly the same short interval, try a full mpv recreation to discard demuxer, decoder, and possible connection state.
   - Player recreation is an escalation, not the first response.

6. **Keep bounded attempts and backoff.**
   - A provider that remains broken must not produce an infinite reconnect storm.
   - Surface a degraded/failed state honestly after the recovery budget is exhausted.

### Minimum instrumentation for tuning quick recovery

The smallest useful instrumentation is operational rather than content-identification focused:

1. **Explicit load identity**
   - Monotonic load epoch or request ID.
   - Recorded on load request, `START_FILE`, first frame, timeline samples, and `END_FILE`.

2. **Recovery latency**
   - Time from last forward progress to fault declaration.
   - Time from fault declaration to reopen.
   - Time from reopen to first frame.
   - Time from first frame to steady playback or another EOF.

3. **Recovery outcome**
   - Reopen versus player recreation.
   - Whether normal forward playback resumed.
   - How long the recovered load survived.
   - Final reason if the recovery episode exhausted its budget.

4. **Absolute numeric clocks**
   - Absolute playback time and cache end around discontinuities.
   - Available audio/video PTS.
   - Existing signed movement/deviation, pause state, and monotonic elapsed time.

Content fingerprints remain useful only if users specifically report seeing the same pictures or audio again. In that case, short rolling decoded-video perceptual hashes or audio fingerprints can distinguish true content replay from timestamp reuse. They are not required to begin optimizing stall and EOF recovery.

Any added instrumentation must remain credential-safe: no raw playback URL, header, cookie, authorization value, or media payload should be logged.

## 12. Bottom line

| Item | Assessment |
|---|---|
| Verdict | Strong evidence of a repeatable playback-timeline/EOF loop; actual earlier-content replay is plausible but unproven. |
| Strongest evidence | Three consecutive recovery loads with unpaused `-9.0` to `-9.5s` movement, near-zero cache-end movement, resumed forward playback, then EOF. |
| Competing explanations | Repeated mux timestamp resets or corrupt finite chunks remain viable; ordinary buffering and UI delay fit poorly. |
| Recovery objective | Restore healthy playback at the provider's current live edge; do not assume the missing interval can be retrieved. |
| ExoPlayer alignment | Broadly aligned on bounded retry/source re-preparation and resuming live; full mpv recreation is a Coax-specific escalation, not an ExoPlayer requirement. |
| Confidence | High for the rollback/EOF pattern; moderate for content replay; high that quick forward-playback recovery is the appropriate optimization target. |
| Smallest sensible action | Instrument recovery latency/outcome by load epoch, then tune faster unpaused-stall and live-EOF recovery; reserve content fingerprints for confirmed user-visible repetition reports. |

## Evidence references

All evidence IDs refer to [coax-live-playback-sanitized-evidence.md](coax-live-playback-sanitized-evidence.md), which preserves selected sanitized log lines with their original timestamps and `coax.log` line numbers.
