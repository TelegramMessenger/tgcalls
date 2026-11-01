#!/usr/bin/env python3
"""The broadcast testbench's baseline and comparison.

  broadcast_report.py aggregate <reports-dir> <out-dir>
      Groups `tgcalls_cli --mode broadcast` reports into cells (scenario, profile, events) and writes one baseline
      file per cell, engine-<scenario>-<profile>-<none|events>.json: per metric its median, p10 and p90 over the
      repetitions.

  broadcast_report.py compare <baseline-dir> <candidate-reports-dir> [--metrics prefix,prefix] [--confirm <dir>]
      Exit 0 only if every candidate cell passes against its baseline cell. The margin of every rule is the baseline's
      spread, p90 - p10, and at least the metric's slack (50 ms; 5 blocks of 10 ms; 2 frames; 1 stall; overlap
      blocks and mean levels none):
        lower is better (audio.startMs, audio.stallCount, audio.stallMs, audio.latencyMedianMs, each tile's startMs,
        stallCount, stallMs, latencyMedianMs, repeatedFrames, outOfOrderFrames, droppedFrames, handoff.gapBlocks,
        handoff.overlapBlocks):
            candidate median <= baseline p90 + margin;
        higher is better (audio.heardBlocks.* of the tones the scenario plays, each tile's frames, levels.*.meanLevel,
            which the new player reports as the 100 ms maximum): candidate median >= baseline p10 - margin.
            The unified tone (1000) in a listener cell, or the marker and speaker tones in a unified cell, is
            leakage from neighbouring frequencies and informational;
        levels.*.speakingFraction, server.* and everything else: informational. (webrtc's VAD judges the sine tones
            speech in one of two modes picked per run, so the speaking fraction carries no signal.)
      Calibration (`calibrate` on the recorded baseline's 10 runs a cell, 10000 matrices): a candidate as good as the
      old engine fails 0.18 of the matrix's 620 checks on average, and with --confirm 19 checks in 10000 matrices
      (99.8% of matrices pass); one worse by max(3 sd, 2 slack) fails 53% of its checks, 42% with --confirm.
      A metric the baseline has in every run fails if any candidate run lacks it (a run that never started has no
      audio.startMs), unless --metrics (prefixes to compare) leaves it out.
      --confirm <dir>: a second candidate run of the same cells; a check fails only if it fails against both, and
      checks failing against one are listed as unconfirmed.

  broadcast_report.py calibrate <reports-dir> [--matrices N]
      The rules' error rates, simulated from recorded runs as compare is used: the runs' summary is the baseline,
      fixed, and each of N matrices (default 1000, seed fixed) draws every cell's 5 candidate runs with replacement
      from that cell's runs, 5 more for --confirm. Prints the false failures per matrix, the matrices that pass with
      --confirm, the share of checks a candidate shifted toward worse by max(3 sd, 2 slack) fails, and the checks that
      fail most often. Draws from 10 runs understate a cell's spread, so these are lower bounds on false failures.
"""

import json
import os
import random
import re
import statistics
import sys
from collections import defaultdict

MS_SLACK = 50
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


def summarize_values(items):
    return {"median": percentile(items, 0.5), "p10": percentile(items, 0.1), "p90": percentile(items, 0.9),
            "n": len(items)}


def metric_values(reports):
    """Per metric: its values over the reports that have it."""
    values = defaultdict(list)
    for report in reports:
        for metric, value in report_metrics(report).items():
            values[metric].append(value)
    return values


def summarize(reports):
    """Per metric: median, p10, p90 over the reports that have it."""
    return {metric: summarize_values(items) for metric, items in sorted(metric_values(reports).items())}


def slack_for(metric):
    """The least margin a rule allows, in the metric's unit: what an equal candidate's median can miss by when the
    baseline's runs happen to tie (say, 4 stalls in each)."""
    if metric.endswith("Ms"):
        return MS_SLACK
    if metric.endswith("stallCount"):
        return 1
    if ".heardBlocks." in metric or metric.endswith("gapBlocks"):
        return 5
    if metric.endswith(".frames") or metric.endswith("Frames"):
        return 2
    return 0


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
    r"video\.[^.]+\.repeatedFrames", r"video\.[^.]+\.outOfOrderFrames", r"video\.[^.]+\.droppedFrames",
]
HIGHER_IS_BETTER = [r"audio\.heardBlocks\.[^.]+", r"video\.[^.]+\.frames", r"levels\.[^.]+\.meanLevel"]


def rule_for(metric, scenario):
    tone = re.fullmatch(r"audio\.heardBlocks\.([^.]+)", metric)
    if tone and (tone.group(1) == UNIFIED_TONE) != (scenario == "unified"):
        return None  # a tone this scenario does not play: what is heard is leakage from neighbouring frequencies
    if any(re.fullmatch(pattern, metric) for pattern in LOWER_IS_BETTER):
        return "lower"
    if any(re.fullmatch(pattern, metric) for pattern in HIGHER_IS_BETTER):
        return "higher"
    return None


def check(metric, rule, candidate, baseline):
    """None if the candidate median passes, else the reason."""
    median, p10, p90 = baseline["median"], baseline["p10"], baseline["p90"]
    margin = max(slack_for(metric), p90 - p10)
    if rule == "lower":
        limit = p90 + margin
        if candidate > limit:
            return f"{candidate:g} > limit {limit:g} (baseline median {median:g}, p10 {p10:g}, p90 {p90:g})"
    elif rule == "higher":
        limit = p10 - margin
        if candidate < limit:
            return f"{candidate:g} < limit {limit:g} (baseline median {median:g}, p10 {p10:g}, p90 {p90:g})"
    return None


def evaluate(baseline_dir, candidate_dir, prefixes):
    """(number of checks, {(cell name, metric or None): reason}) for one candidate directory."""
    failures = {}
    checked = 0
    for cell, reports in sorted(group(load_reports(candidate_dir)).items()):
        name = cell_name(cell)
        path = os.path.join(baseline_dir, f"engine-{name}.json")
        if not os.path.exists(path):
            failures[(name, None)] = f"no baseline cell ({path})"
            continue
        with open(path) as f:
            baseline_cell = json.load(f)
        baseline = baseline_cell["metrics"]
        repetitions = baseline_cell.get("repetitions")
        candidate = metric_values(reports)
        for metric, stats in baseline.items():
            rule = rule_for(metric, cell[0])
            if rule is None:
                continue
            if prefixes and not any(metric.startswith(prefix) for prefix in prefixes):
                continue
            checked += 1
            values = candidate.get(metric, [])
            if not values:
                failures[(name, metric)] = "missing from the candidate"
                continue
            if stats.get("n") == repetitions and len(values) < len(reports):
                failures[(name, metric)] = f"missing in {len(reports) - len(values)} of {len(reports)} runs"
                continue
            reason = check(metric, rule, percentile(values, 0.5), stats)
            if reason:
                failures[(name, metric)] = reason
    return checked, failures


def compare(baseline_dir, candidate_dir, prefixes, confirm_dir=None):
    checked, failures = evaluate(baseline_dir, candidate_dir, prefixes)
    unconfirmed = {}
    if confirm_dir:
        _, second = evaluate(baseline_dir, confirm_dir, prefixes)
        unconfirmed = {key: reason for key, reason in failures.items() if key not in second}
        failures = {key: reason for key, reason in failures.items() if key in second}
    for (name, metric), reason in sorted(unconfirmed.items(), key=lambda item: (item[0][0], item[0][1] or "")):
        print(f"unconfirmed {name} {metric or ''}: {reason}")
    for (name, metric), reason in sorted(failures.items(), key=lambda item: (item[0][0], item[0][1] or "")):
        print(f"FAIL {name}{' ' + metric if metric else ''}: {reason}")
    print(f"{checked} checks, {len(failures)} failures" + (f", {len(unconfirmed)} unconfirmed" if confirm_dir else ""))
    return 1 if failures else 0


CANDIDATE_RUNS = 5


def calibrate(reports_dir, matrices=1000, seed=11):
    """compare as it is used, simulated on recorded runs: the runs' summary is the baseline, fixed; every matrix draws
    each cell's 5 candidate runs with replacement from that cell's runs, and 5 more for --confirm when a check fails.
    Power: the same draws shifted toward worse by max(3 sd, 2 slack), over a twentieth as many matrices."""
    rng = random.Random(seed)
    checks = []
    for cell, reports in sorted(group(load_reports(reports_dir)).items()):
        for metric, values in sorted(metric_values(reports).items()):
            rule = rule_for(metric, cell[0])
            if rule:
                checks.append((cell_name(cell), metric, rule, values, summarize_values(values)))

    def fails(metric, rule, values, stats, shift=0.0):
        draws = [max(rng.choice(values) + shift, 0.0) for _ in range(CANDIDATE_RUNS)]
        return check(metric, rule, percentile(draws, 0.5), stats) is not None

    plain, confirmed, passing = 0, 0, 0
    worst = defaultdict(int)
    for _ in range(matrices):
        failed = 0
        for name, metric, rule, values, stats in checks:
            if fails(metric, rule, values, stats):
                plain += 1
                worst[(name, metric)] += 1
                failed += fails(metric, rule, values, stats)
        confirmed += failed
        passing += failed == 0
    detected, detected_confirmed, tried = 0, 0, 0
    for _ in range(max(1, matrices // 20)):
        for name, metric, rule, values, stats in checks:
            sd = statistics.stdev(values) if len(values) > 1 else 0.0
            step = max(3 * sd, 2 * slack_for(metric) + 1e-9)
            shift = step if rule == "lower" else -step
            if fails(metric, rule, values, stats, shift):
                detected += 1
                detected_confirmed += fails(metric, rule, values, stats, shift)
            tried += 1
    print(f"{len(checks)} checks over {matrices} matrices: an equal candidate fails {plain / matrices:.2f} checks per "
          f"matrix, {confirmed} in all with --confirm ({passing} of {matrices} matrices pass with --confirm); a "
          f"candidate worse by max(3 sd, 2 slack) fails {detected / max(tried, 1):.0%} of checks, "
          f"{detected_confirmed / max(tried, 1):.0%} with --confirm")
    for (name, metric), count in sorted(worst.items(), key=lambda item: -item[1])[:6]:
        print(f"  {name} {metric}: {count / matrices:.3f} false failures per matrix")
    return 0


def main(argv):
    if len(argv) >= 3 and argv[0] == "aggregate":
        return aggregate(argv[1], argv[2])
    if len(argv) >= 2 and argv[0] == "calibrate":
        matrices = int(argv[argv.index("--matrices") + 1]) if "--matrices" in argv else 1000
        return calibrate(argv[1], matrices)
    if len(argv) >= 3 and argv[0] == "compare":
        prefixes = []
        if "--metrics" in argv:
            index = argv.index("--metrics")
            prefixes = [item for item in argv[index + 1].split(",") if item]
        confirm_dir = None
        if "--confirm" in argv:
            confirm_dir = argv[argv.index("--confirm") + 1]
        return compare(argv[1], argv[2], prefixes, confirm_dir)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
