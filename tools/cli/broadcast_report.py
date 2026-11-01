#!/usr/bin/env python3
"""The broadcast testbench's baseline and comparison.

  broadcast_report.py aggregate <reports-dir> <out-dir>
      Groups `tgcalls_cli --mode broadcast` reports into cells (scenario, profile, events) and writes one baseline
      file per cell, engine-<scenario>-<profile>-<none|events>.json: per metric its median, p10 and p90 over the
      repetitions.

  broadcast_report.py compare <baseline-dir> <candidate-reports-dir> [--metrics prefix,prefix]
      Exit 0 only if every candidate cell passes against its baseline cell:
        lower is better (audio.startMs, audio.stallCount, audio.stallMs, audio.latencyMedianMs, each tile's startMs,
        stallCount, stallMs, latencyMedianMs, handoff.gapBlocks, handoff.overlapBlocks):
            candidate median <= max(baseline p90, baseline median + 50 for millisecond metrics);
        higher is better (audio.heardBlocks.* of the tones the scenario plays, each tile's frames): candidate
            median >= baseline p10. The unified tone (1000) in a listener cell, or the marker and speaker tones in a
            unified cell, is leakage from neighbouring frequencies and informational;
        levels.*.speakingFraction: within the baseline's p10..p90 widened by 0.05 on each side (the old engine's own
            runs spread wider than 0.05 around their median);
        levels.*.meanLevel: candidate median >= baseline p10 (the new player reports the 100 ms maximum);
        server.* and everything else: informational.
      A metric the baseline has but the candidate lacks fails, unless --metrics (prefixes to compare) leaves it out.
"""

import json
import os
import re
import sys
from collections import defaultdict

MS_SLACK = 50
SPEAKING_TOLERANCE = 0.05
UNIFIED_TONE = "1000"  # kUnifiedFrequency in broadcast_signals.h; listener cells play the marker and speaker tones


def flatten(value, prefix=""):
    """Numeric leaves of a report as {"a.b.c": number}; null and non-numeric leaves are left out."""
    result = {}
    if isinstance(value, dict):
        for key, item in value.items():
            result.update(flatten(item, f"{prefix}{key}."))
    elif isinstance(value, bool):
        pass
    elif isinstance(value, (int, float)):
        result[prefix[:-1]] = float(value)
    return result


def report_metrics(report):
    metrics = {}
    for section in ("audio", "video", "levels", "handoff", "server"):
        if report.get(section) is not None:
            metrics.update(flatten(report[section], section + "."))
    return metrics


def cell_of(report):
    return (report["scenario"], report["profile"], report.get("events", ""))


def cell_name(cell):
    scenario, profile, events = cell
    return f"{scenario}-{profile}-{'events' if events else 'none'}"


def percentile(values, q):
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = q * (len(ordered) - 1)
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def load_reports(directory):
    reports = []
    for name in sorted(os.listdir(directory)):
        if name.endswith(".json"):
            with open(os.path.join(directory, name)) as f:
                reports.append(json.load(f))
    return reports


def summarize(reports):
    """Per metric: median, p10, p90 over the reports that have it."""
    values = defaultdict(list)
    for report in reports:
        for metric, value in report_metrics(report).items():
            values[metric].append(value)
    return {metric: {"median": percentile(items, 0.5), "p10": percentile(items, 0.1), "p90": percentile(items, 0.9),
                     "n": len(items)}
            for metric, items in sorted(values.items())}


def group(reports):
    cells = defaultdict(list)
    for report in reports:
        cells[cell_of(report)].append(report)
    return cells


def aggregate(reports_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for cell, reports in sorted(group(load_reports(reports_dir)).items()):
        scenario, profile, events = cell
        first = reports[0]
        baseline = {
            "cell": {"scenario": scenario, "profile": profile, "events": events},
            "implementation": first.get("implementation"),
            "availabilityDelayMs": first.get("availabilityDelayMs"),
            "durationS": first.get("durationS"),
            "repetitions": len(reports),
            "metrics": summarize(reports),
        }
        path = os.path.join(out_dir, f"engine-{cell_name(cell)}.json")
        with open(path, "w") as f:
            json.dump(baseline, f, indent=1, sort_keys=True)
            f.write("\n")
        print(f"{cell_name(cell)}: {len(reports)} reports -> {path}")
    return 0


LOWER_IS_BETTER = [
    r"audio\.startMs", r"audio\.stallCount", r"audio\.stallMs", r"audio\.latencyMedianMs",
    r"video\.[^.]+\.startMs", r"video\.[^.]+\.stallCount", r"video\.[^.]+\.stallMs", r"video\.[^.]+\.latencyMedianMs",
    r"handoff\.gapBlocks", r"handoff\.overlapBlocks",
]
HIGHER_IS_BETTER = [r"audio\.heardBlocks\.[^.]+", r"video\.[^.]+\.frames"]
SPEAKING = r"levels\.[^.]+\.speakingFraction"
MEAN_LEVEL = r"levels\.[^.]+\.meanLevel"


def rule_for(metric, scenario):
    tone = re.fullmatch(r"audio\.heardBlocks\.([^.]+)", metric)
    if tone and (tone.group(1) == UNIFIED_TONE) != (scenario == "unified"):
        return None  # a tone this scenario does not play: what is heard is leakage from neighbouring frequencies
    if any(re.fullmatch(pattern, metric) for pattern in LOWER_IS_BETTER):
        return "lower"
    if any(re.fullmatch(pattern, metric) for pattern in HIGHER_IS_BETTER):
        return "higher"
    if re.fullmatch(SPEAKING, metric):
        return "speaking"
    if re.fullmatch(MEAN_LEVEL, metric):
        return "meanLevel"
    return None


def check(metric, rule, candidate, baseline):
    """None if the candidate passes, else the reason."""
    median, p10, p90 = baseline["median"], baseline["p10"], baseline["p90"]
    if rule == "lower":
        limit = max(p90, median + MS_SLACK) if metric.endswith("Ms") else p90
        if candidate > limit:
            return f"{candidate:g} > limit {limit:g} (baseline median {median:g}, p90 {p90:g})"
    elif rule == "higher":
        if candidate < p10:
            return f"{candidate:g} < baseline p10 {p10:g} (median {median:g})"
    elif rule == "speaking":
        low, high = p10 - SPEAKING_TOLERANCE, p90 + SPEAKING_TOLERANCE
        if not low <= candidate <= high:
            return (f"{candidate:g} outside baseline p10..p90 widened by {SPEAKING_TOLERANCE}: [{low:g}, {high:g}] "
                    f"(median {median:g})")
    elif rule == "meanLevel":
        if candidate < p10:
            return f"{candidate:g} < baseline p10 {p10:g} (median {median:g})"
    return None


def compare(baseline_dir, candidate_dir, prefixes):
    failures = []
    checked = 0
    for cell, reports in sorted(group(load_reports(candidate_dir)).items()):
        name = cell_name(cell)
        path = os.path.join(baseline_dir, f"engine-{name}.json")
        if not os.path.exists(path):
            failures.append(f"FAIL {name}: no baseline cell ({path})")
            continue
        with open(path) as f:
            baseline = json.load(f)["metrics"]
        candidate = summarize(reports)
        for metric, stats in baseline.items():
            rule = rule_for(metric, cell[0])
            if rule is None:
                continue
            if prefixes and not any(metric.startswith(prefix) for prefix in prefixes):
                continue
            checked += 1
            if metric not in candidate:
                failures.append(f"FAIL {name} {metric}: missing from the candidate")
                continue
            reason = check(metric, rule, candidate[metric]["median"], stats)
            if reason:
                failures.append(f"FAIL {name} {metric}: {reason}")
    for line in failures:
        print(line)
    print(f"{checked} checks, {len(failures)} failures")
    return 1 if failures else 0


def main(argv):
    if len(argv) >= 3 and argv[0] == "aggregate":
        return aggregate(argv[1], argv[2])
    if len(argv) >= 3 and argv[0] == "compare":
        prefixes = []
        if "--metrics" in argv:
            index = argv.index("--metrics")
            prefixes = [item for item in argv[index + 1].split(",") if item]
        return compare(argv[1], argv[2], prefixes)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
