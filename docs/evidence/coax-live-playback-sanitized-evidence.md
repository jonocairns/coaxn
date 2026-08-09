# Coax live playback — sanitized evidence appendix

Evidence source: `build/coax.log`

Fixed analysis boundary: lines 1–17,896, ending at `10:05:02.271`
Handling: provider host redacted; no raw playback URL, credential, header, cookie, or token reproduced.

## Evidence index

### Privacy and session identity

**E01 — L10**

```text
[07:23:49.306] INF Restored saved portal for https://[REDACTED_HOST]
```

**E02 — L12**

```text
[07:23:53.048] INF Provider session 1 connected to https://[REDACTED_HOST]
```

**E03 — L13**

```text
[07:23:58.659] INF Channel selected generation 1 provider-session=1 channel-session=1
```

**E04 — L15**

```text
[07:23:58.661] INF Load request generation 1 provider-session=1 channel-session=1 intent=fresh-selection command=loadfile mode=replace transport=mpeg-ts scheme=https target=xtream-live query=absent userinfo=absent forced-format=no; HTTP method/range/headers unobserved below libmpv
```

**E05 — L32**

```text
[07:24:02.416] INF Supervisor zap -> zap generation 1 attempt 0 reason first-frame budget 0ms
```

### Ordinary buffering comparator

**E06 — L963**

```text
[07:31:47.112] DBG Timeline sample generation 1 kind=paused-no-progress elapsed=+0.504s playback-move=+0.000s playback-deviation=-0.504s cache-end-move=+0.960s control-playback-move=+0.000s control-playback-deviation=-0.504s control-baseline=adjacent cache-paused=yes previous-cache-paused=yes engine-messages-since-sample=0 unattributed-engine-messages-since-sample=0 warning-severity=none warning-component=none warning-category=none
```

### Large timebase discontinuities and decode stall

**E07 — L4361–L4362**

```text
[08:00:14.633] DBG Timeline sample generation 1 kind=forward-jump elapsed=+0.504s playback-move=+91853.148s playback-deviation=+91852.644s cache-end-move=+91856.642s control-playback-move=+91853.148s control-playback-deviation=+91852.644s control-baseline=adjacent cache-paused=no previous-cache-paused=no engine-messages-since-sample=2 unattributed-engine-messages-since-sample=0 warning-severity=warning warning-component=player warning-category=other
[08:00:14.633] WRN Timeline discontinuity #2 generation 1 kind=forward-jump playback-move=+91853.148s playback-deviation=+91852.644s cache-end-move=+91856.642s control-playback-move=+91853.148s control-playback-deviation=+91852.644s control-baseline=adjacent engine-messages-since-sample=2 unattributed-engine-messages-since-sample=0 warning-severity=warning warning-component=player warning-category=other
```

**E08 — L7064–L7067**

```text
[08:22:51.443] DBG Timeline sample generation 1 kind=backward elapsed=+0.504s playback-move=-95383.170s playback-deviation=-95383.674s cache-end-move=unavailable control-playback-move=-95383.170s control-playback-deviation=-95383.674s control-baseline=adjacent cache-paused=no previous-cache-paused=no engine-messages-since-sample=2 unattributed-engine-messages-since-sample=0 warning-severity=warning warning-component=player warning-category=other
[08:22:51.444] WRN Timeline discontinuity #3 generation 1 kind=backward playback-move=-95383.170s playback-deviation=-95383.674s cache-end-move=unavailable control-playback-move=-95383.170s control-playback-deviation=-95383.674s control-baseline=adjacent engine-messages-since-sample=2 unattributed-engine-messages-since-sample=0 warning-severity=warning warning-component=player warning-category=other
[08:22:51.947] DBG Timeline sample generation 1 kind=no-progress elapsed=+0.504s playback-move=+0.000s playback-deviation=-0.504s cache-end-move=-0.054s control-playback-move=+0.000s control-playback-deviation=-0.504s control-baseline=adjacent cache-paused=no previous-cache-paused=no engine-messages-since-sample=0 unattributed-engine-messages-since-sample=0 warning-severity=none warning-component=none warning-category=none
```

**E09 — L7092, L7095, L7098**

```text
[08:23:04.547] DBG Timeline sample generation 1 kind=no-progress elapsed=+0.504s playback-move=+0.000s playback-deviation=-0.504s cache-end-move=+3.840s control-playback-move=+0.000s control-playback-deviation=-0.504s control-baseline=adjacent cache-paused=no previous-cache-paused=no engine-messages-since-sample=0 unattributed-engine-messages-since-sample=0 warning-severity=none warning-component=none warning-category=none
[08:23:05.555] INF Supervisor steady -> recovering generation 1 attempt 1 reason decode-stall budget 0ms
[08:23:06.060] INF Load request generation 1 provider-session=1 channel-session=1 intent=recovery-reopen command=loadfile mode=replace transport=mpeg-ts scheme=https target=xtream-live query=absent userinfo=absent forced-format=no; HTTP method/range/headers unobserved below libmpv
```

### Suspend/resume and unattributed replacement-window messages

**E10 — L11539, L11543–L11547**

```text
[09:12:55.407] INF System resumed from suspend
[09:12:55.675] DBG Timeline sample generation 1 kind=unavailable elapsed=+760.701s playback-move=unavailable playback-deviation=unavailable cache-end-move=unavailable control-playback-move=unavailable control-playback-deviation=unavailable control-baseline=unavailable cache-paused=no previous-cache-paused=yes engine-messages-since-sample=0 unattributed-engine-messages-since-sample=0 warning-severity=none warning-component=none warning-category=none
[09:12:55.727] INF Supervisor steady -> recovering generation 1 attempt 1 reason stream-ended-eof budget 0ms
[09:12:56.231] INF Load request generation 1 provider-session=1 channel-session=1 intent=recovery-reopen command=loadfile mode=replace transport=mpeg-ts scheme=https target=xtream-live query=absent userinfo=absent forced-format=no; HTTP method/range/headers unobserved below libmpv
```

**E11 — L11556–L11558, L11562**

```text
[09:12:56.240] ERR mpv diagnostic generation=unattributed attribution=unattributed severity=error component=other category=other
[09:12:56.293] INF Supervisor zap -> recovering generation 1 attempt 2 reason stream-ended-error budget 567ms
[09:12:56.736] DBG Timeline sample generation 1 kind=unavailable elapsed=unavailable playback-move=unavailable playback-deviation=unavailable cache-end-move=unavailable control-playback-move=unavailable control-playback-deviation=unavailable control-baseline=unavailable cache-paused=no previous-cache-paused=unavailable engine-messages-since-sample=3 unattributed-engine-messages-since-sample=4 warning-severity=error warning-component=other warning-category=other
[09:12:57.298] INF Load request generation 1 provider-session=1 channel-session=1 intent=recovery-reopen command=loadfile mode=replace transport=mpeg-ts scheme=https target=xtream-live query=absent userinfo=absent forced-format=no; HTTP method/range/headers unobserved below libmpv
```

**E12 — L11588–L11590**

```text
[09:13:04.562] DBG Timeline sample generation 1 kind=forward-jump elapsed=+0.500s playback-move=+1.762s playback-deviation=+1.262s cache-end-move=+0.486s control-playback-move=+1.762s control-playback-deviation=+1.262s control-baseline=adjacent cache-paused=no previous-cache-paused=no engine-messages-since-sample=0 unattributed-engine-messages-since-sample=0 warning-severity=none warning-component=none warning-category=none
[09:13:04.562] WRN Timeline discontinuity #1 generation 1 kind=forward-jump playback-move=+1.762s playback-deviation=+1.262s cache-end-move=+0.486s control-playback-move=+1.762s control-playback-deviation=+1.262s control-baseline=adjacent engine-messages-since-sample=0 unattributed-engine-messages-since-sample=0 warning-severity=none warning-component=none warning-category=none
```

### Progress-stall recovery leading into the repeating short-load sequence

**E13 — L17205, L17207, L17211, L17214**

```text
[10:00:10.691] INF Rebuffer #9; live target now 8.5s
[10:00:11.204] DBG Timeline sample generation 1 kind=paused-no-progress elapsed=+0.504s playback-move=+0.000s playback-deviation=-0.504s cache-end-move=+0.000s control-playback-move=+0.000s control-playback-deviation=-0.504s control-baseline=adjacent cache-paused=yes previous-cache-paused=yes engine-messages-since-sample=0 unattributed-engine-messages-since-sample=0 warning-severity=none warning-component=none warning-category=none
[10:00:12.212] INF Supervisor steady -> recovering generation 1 attempt 1 reason progress-stall budget 0ms
[10:00:12.716] INF Load request generation 1 provider-session=1 channel-session=1 intent=recovery-reopen command=loadfile mode=replace transport=mpeg-ts scheme=https target=xtream-live query=absent userinfo=absent forced-format=no; HTTP method/range/headers unobserved below libmpv
```

**E14 — L17264, L17267, L17281**

```text
[10:00:29.554] INF Supervisor steady -> recovering generation 1 attempt 1 reason stream-ended-eof budget 0ms
[10:00:30.056] INF Load request generation 1 provider-session=1 channel-session=1 intent=recovery-reopen command=loadfile mode=replace transport=mpeg-ts scheme=https target=xtream-live query=absent userinfo=absent forced-format=no; HTTP method/range/headers unobserved below libmpv
[10:00:32.735] INF Supervisor zap -> zap generation 1 attempt 1 reason first-frame budget 3182ms
```

### Repeating rollback / advance / EOF sequence

**E15 — first repetition: L17305, L17310, L17312, L17334, L17338**

```text
[10:00:42.152] WRN mpv diagnostic generation=1 attribution=active-entry severity=warning component=demuxer category=corrupt-packet
[10:00:42.801] DBG Timeline sample generation 1 kind=backward elapsed=+0.504s playback-move=-9.321s playback-deviation=-9.825s cache-end-move=+0.051s control-playback-move=-9.321s control-playback-deviation=-9.825s control-baseline=adjacent cache-paused=no previous-cache-paused=no engine-messages-since-sample=2 unattributed-engine-messages-since-sample=0 warning-severity=warning warning-component=player warning-category=other
[10:00:42.802] WRN Timeline discontinuity #1 generation 1 kind=backward playback-move=-9.321s playback-deviation=-9.825s cache-end-move=+0.051s control-playback-move=-9.321s control-playback-deviation=-9.825s control-baseline=adjacent engine-messages-since-sample=2 unattributed-engine-messages-since-sample=0 warning-severity=warning warning-component=player warning-category=other
[10:00:53.265] INF Supervisor steady -> recovering generation 1 attempt 1 reason stream-ended-eof budget 0ms
[10:00:53.769] INF Load request generation 1 provider-session=1 channel-session=1 intent=recovery-reopen command=loadfile mode=replace transport=mpeg-ts scheme=https target=xtream-live query=absent userinfo=absent forced-format=no; HTTP method/range/headers unobserved below libmpv
```

**E16 — second repetition: L17353, L17377, L17382, L17384, L17405, L17409**

```text
[10:00:56.392] INF Supervisor zap -> zap generation 1 attempt 1 reason first-frame budget 3128ms
[10:01:05.865] WRN mpv diagnostic generation=1 attribution=active-entry severity=warning component=demuxer category=corrupt-packet
[10:01:06.857] DBG Timeline sample generation 1 kind=backward elapsed=+0.505s playback-move=-9.500s playback-deviation=-10.005s cache-end-move=+0.017s control-playback-move=-9.500s control-playback-deviation=-10.005s control-baseline=adjacent cache-paused=no previous-cache-paused=no engine-messages-since-sample=2 unattributed-engine-messages-since-sample=0 warning-severity=warning warning-component=player warning-category=other
[10:01:06.857] WRN Timeline discontinuity #1 generation 1 kind=backward playback-move=-9.500s playback-deviation=-10.005s cache-end-move=+0.017s control-playback-move=-9.500s control-playback-deviation=-10.005s control-baseline=adjacent engine-messages-since-sample=2 unattributed-engine-messages-since-sample=0 warning-severity=warning warning-component=player warning-category=other
[10:01:16.915] INF Supervisor steady -> recovering generation 1 attempt 1 reason stream-ended-eof budget 0ms
[10:01:17.419] INF Load request generation 1 provider-session=1 channel-session=1 intent=recovery-reopen command=loadfile mode=replace transport=mpeg-ts scheme=https target=xtream-live query=absent userinfo=absent forced-format=no; HTTP method/range/headers unobserved below libmpv
```

**E17 — third repetition: L17424, L17448, L17453, L17455, L17475, L17479**

```text
[10:01:20.073] INF Supervisor zap -> zap generation 1 attempt 1 reason first-frame budget 3158ms
[10:01:29.527] WRN mpv diagnostic generation=1 attribution=active-entry severity=warning component=demuxer category=corrupt-packet
[10:01:31.141] DBG Timeline sample generation 1 kind=backward elapsed=+0.992s playback-move=-9.033s playback-deviation=-10.026s cache-end-move=+0.000s control-playback-move=-9.033s control-playback-deviation=-10.026s control-baseline=adjacent cache-paused=no previous-cache-paused=no engine-messages-since-sample=2 unattributed-engine-messages-since-sample=0 warning-severity=warning warning-component=player warning-category=other
[10:01:31.141] WRN Timeline discontinuity #1 generation 1 kind=backward playback-move=-9.033s playback-deviation=-10.026s cache-end-move=+0.000s control-playback-move=-9.033s control-playback-deviation=-10.026s control-baseline=adjacent engine-messages-since-sample=2 unattributed-engine-messages-since-sample=0 warning-severity=warning warning-component=player warning-category=other
[10:01:40.628] INF Supervisor steady -> recovering generation 1 attempt 1 reason stream-ended-eof budget 0ms
[10:01:41.132] INF Load request generation 1 provider-session=1 channel-session=1 intent=recovery-reopen command=loadfile mode=replace transport=mpeg-ts scheme=https target=xtream-live query=absent userinfo=absent forced-format=no; HTTP method/range/headers unobserved below libmpv
```

**E18 — final observed load: L17503, L17526, L17547**

```text
[10:01:47.965] INF Supervisor zap -> zap generation 1 attempt 1 reason first-frame budget 7337ms
[10:01:55.344] INF Rebuffer #10; live target now 9.0s
[10:02:03.171] INF Rebuffer #11; live target now 9.5s
```

## Interpretation guardrail

These excerpts prove repeated app-observed playback-clock rollback, not repeated media identity. The log does not observe raw HTTP headers, byte identity, Range requests, returned `Content-Range`, or connection reuse below libmpv. Equal or repeated timestamps can label different content after a legitimate MPEG-TS timestamp discontinuity.
