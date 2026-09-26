#!/usr/bin/env python3
"""Summarise coax.log into the numbers behind a deliberate lag behind live.

Reads the delivery, recovery and timeline lines the player logs and prints:
how long input goes quiet between arrivals, how long a load takes to deliver
its first data, where recovered loads land against the anchor, how often
playback stalls, and the lag those numbers imply.

Read the log through Windows, not /mnt/c, which can serve stale content:
    powershell.exe -NoProfile -Command "Get-Content $env:LOCALAPPDATA\\Coax\\coax.log" > coax.log
    python3 scripts/analyze-delivery.py coax.log
"""

import math
import re
import sys

TIME = re.compile(r"^\[(\d+):(\d+):(\d+\.\d+)\]")
FIELD = re.compile(r"([\w-]+)=(\S+)")


def seconds(value):
    """Parse '1.23s', '+1.23s' or '450ms' into seconds; None if unavailable."""
    if value is None or value == "unavailable":
        return None
    if value.endswith("ms"):
        return float(value[:-2]) / 1000.0
    if value.endswith("s"):
        return float(value[:-1])
    return None


def stamp(line):
    match = TIME.match(line)
    if not match:
        return None
    hours, minutes, secs = match.groups()
    return int(hours) * 3600 + int(minutes) * 60 + float(secs)


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
    summaries, first_data, decisions, landings, stalls = [], [], [], [], []
    rebuffers = 0
    first_sample = last_sample = None
    pause_started = None

    with open(path, encoding="utf-8", errors="replace") as log:
        for line in log:
            fields = dict(FIELD.findall(line))
            if "Delivery summary" in line:
                summaries.append(fields)
            elif "First data generation" in line:
                match = re.search(r"after (\d+)ms", line)
                if match:
                    first_data.append(int(match.group(1)) / 1000.0)
            elif "Recovery telemetry" in line and fields.get("outcome") == "fault-decided":
                decisions.append(fields)
            elif ("Recovery edge telemetry" in line and fields.get("point") == "first-frame"
                    and fields.get("schema", "").endswith("v2")):
                landings.append(seconds(fields.get("cache-end-wall-residual")))
            elif "Rebuffer #" in line:
                rebuffers += 1
            elif "Timeline sample" in line:
                now = stamp(line)
                first_sample = first_sample if first_sample is not None else now
                last_sample = now
                paused = fields.get("cache-paused") == "yes"
                if paused and pause_started is None:
                    pause_started = now
                elif not paused and pause_started is not None:
                    stalls.append(now - pause_started)
                    pause_started = None

    gap_p99 = [seconds(s.get("gap-p99")) for s in summaries]
    gap_max = [seconds(s.get("gap-max")) for s in summaries]
    buffer_max = [seconds(s.get("buffer-max")) for s in summaries]
    gap_p99 = [v for v in gap_p99 if v is not None]
    gap_max = [v for v in gap_max if v is not None]
    buffer_max = [v for v in buffer_max if v is not None]
    landings = [v for v in landings if v is not None]

    print(f"Loads summarised: {len(summaries)}")
    print(f"  arrival gap p99 per load: {describe(gap_p99)}")
    print(f"  arrival gap max per load: {describe(gap_max)}")
    print(f"  deepest buffer per load:  {describe(buffer_max)}")
    print(f"Load issue to first data:   {describe(first_data)}")

    if first_sample is not None and last_sample > first_sample:
        hours = (last_sample - first_sample) / 3600.0
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
        print(f"\nImplied lag: silence threshold {threshold:.1f}s (worst gap + 1s) + "
              f"reconnect {reconnect:.1f}s (p90 first data) + 2s margin = "
              f"{threshold + reconnect + 2.0:.1f}s behind live")
    else:
        print("\nNot enough delivery data yet for an implied lag.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
