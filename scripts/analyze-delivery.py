#!/usr/bin/env python3
"""Summarise coax.log into the evidence behind a playable-headroom budget.

Reads the delivery, recovery and timeline lines the player logs and prints:
how long the cache end goes still between movements, how long loads take to
show first data (and which never did), silences cut short by a load ending,
where recovered loads land against the pre-recovery anchor, and how often
playback stalled.

The closing "conditional headroom" is the buffer that would have covered the
silences seen here, given loads that eventually showed data. It is not a
measured distance behind live, and it says nothing about whether a reconnect
restores that headroom.

Use a log saved from the diagnostics panel, or read the live one through
Windows (not /mnt/c, which can serve stale content):
    powershell.exe -NoProfile -Command "Get-Content $env:LOCALAPPDATA\\Coax\\coax.log" > coax.log
    python3 scripts/analyze-delivery.py coax.log
"""

import math
import re
import sys

TIME = re.compile(r"^\[(\d+):(\d+):(\d+\.\d+)\]")
FIELD = re.compile(r"([\w-]+)=(\S+)")
DAY = 86400.0


def seconds(value):
    """Parse '1.23s', '+1.23s' or '450ms' into seconds; None if unavailable."""
    if value is None or value == "unavailable":
        return None
    if value.endswith("ms"):
        return float(value[:-2]) / 1000.0
    if value.endswith("s"):
        return float(value[:-1])
    return None


class Clock:
    """Log times are UTC time of day; add a day each time they wrap."""

    def __init__(self):
        self.days = 0
        self.last = None

    def at(self, line):
        match = TIME.match(line)
        if not match:
            return None
        hours, minutes, secs = match.groups()
        now = int(hours) * 3600 + int(minutes) * 60 + float(secs)
        if self.last is not None and now < self.last - DAY / 2:
            self.days += 1
        self.last = now
        return now + self.days * DAY


def percentile(values, fraction):
    ordered = sorted(values)
    rank = min(len(ordered), max(1, math.ceil(fraction * len(ordered))))
    return ordered[rank - 1]


def describe(values, unit="s"):
    if not values:
        return "no data"
    return (f"n={len(values)} p50={percentile(values, 0.5):.2f}{unit} "
            f"p90={percentile(values, 0.9):.2f}{unit} p99={percentile(values, 0.99):.2f}{unit} "
            f"max={max(values):.2f}{unit}")


def main(path):
    loads = {}  # (generation, load-attempt) -> latest delivery fields; a summary outranks snapshots
    decisions, landings, stalls = [], [], []
    rebuffers = 0
    sampled = 0.0
    previous_sample = None
    pause_started = None
    clock = Clock()

    with open(path, encoding="utf-8", errors="replace") as log:
        for line in log:
            now = clock.at(line)
            fields = dict(FIELD.findall(line))
            if line[19:].startswith("Delivery "):
                key = (fields.get("generation"), fields.get("load-attempt"))
                final = "Delivery summary" in line
                if final or not loads.get(key, {}).get("final"):
                    loads[key] = dict(fields, final=final)
            elif "Recovery telemetry" in line and fields.get("outcome") == "fault-decided":
                decisions.append(fields)
            elif ("Recovery edge telemetry" in line and fields.get("point") == "first-frame"
                    and fields.get("schema", "").endswith("v2")):
                landings.append(seconds(fields.get("cache-end-wall-residual")))
            elif "Rebuffer #" in line:
                rebuffers += 1
            elif "Timeline sample" in line and now is not None:
                if previous_sample is not None and now - previous_sample < 5.0:
                    sampled += now - previous_sample
                previous_sample = now
                paused = fields.get("cache-paused") == "yes"
                if paused and pause_started is None:
                    pause_started = now
                elif not paused and pause_started is not None:
                    stalls.append(now - pause_started)
                    pause_started = None

    records = list(loads.values())
    delivered = [r for r in records if seconds(r.get("load-to-first-data")) is not None]
    never = [r for r in records if seconds(r.get("load-to-first-data")) is None]
    first_data = [seconds(r["load-to-first-data"]) for r in delivered]
    gap_max = [v for v in (seconds(r.get("gap-max")) for r in records) if v is not None]
    gap_p99 = [v for v in (seconds(r.get("gap-p99")) for r in records) if v is not None]
    censored = [v for v in (seconds(r.get("silent-at-end")) for r in records) if v is not None]
    throttled = sum(int(r.get("throttled-gaps", 0)) for r in records)
    resets = sum(int(r.get("timestamp-resets", 0)) for r in records)
    missing = sum(int(r.get("missing-samples", 0)) for r in records)
    landings = [v for v in landings if v is not None]

    print(f"Loads with delivery records: {len(records)} "
          f"({sum(1 for r in records if r['final'])} final, "
          f"{sum(1 for r in records if not r['final'])} from snapshots only)")
    print(f"  source gap p99 per load: {describe(gap_p99)}")
    print(f"  source gap max per load: {describe(gap_max)}")
    print(f"  silence cut short at report (at least this long): {describe(censored)}")
    print(f"  throttled gaps (buffer at target): {throttled}; timestamp resets: {resets}; "
          f"missing samples: {missing}")
    print(f"Load issue to first data, loads that got there: {describe(first_data)}")
    print(f"Loads that never showed data: {len(never)}")

    if sampled > 0:
        hours = sampled / 3600.0
        print(f"Stalls: {len(stalls)} over {hours:.2f}h sampled "
              f"({len(stalls) / hours:.1f}/h); durations {describe(stalls)}")
    print(f"Rebuffers logged: {rebuffers}")

    print(f"Recovery decisions: {len(decisions)}")
    for decision in decisions:
        print(f"  mechanism={decision.get('mechanism')} "
              f"input-silence={decision.get('input-silence', 'unavailable')} "
              f"last-progress-to-decision={decision.get('last-progress-to-decision')}")
    print(f"Recovered first frames against anchor (v2, + means ahead): {describe(landings)}")

    if gap_max and first_data:
        threshold = max(gap_max) + 1.0
        reconnect = percentile(first_data, 0.9)
        print(f"\nConditional headroom: silence threshold {threshold:.1f}s (worst completed gap + 1s)"
              f" + first data {reconnect:.1f}s (p90 of loads that delivered) + 2s margin"
              f" = {threshold + reconnect + 2.0:.1f}s of buffered media.")
        print("Excludes teardown, backoff, decode to first usable frame and any overlap or loss at"
              " the join; not a distance behind live.")
        if censored and max(censored) > threshold:
            print(f"Note: a silence cut short at {max(censored):.1f}s exceeds the threshold.")
    else:
        print("\nNot enough delivery data yet for a headroom estimate.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
