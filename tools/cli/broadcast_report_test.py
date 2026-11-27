"""Tests for broadcast_report.py: aggregating the broadcast testbench's reports into a baseline and comparing a
candidate against it. Run: python3 -m unittest discover -s submodules/TgVoipWebrtc/tgcalls/tools/cli -p 'broadcast_report_test.py'
"""

import io
import json
import os
import random
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import broadcast_report  # noqa: E402


def report(scenario="audio", profile="lte", events="", repetition=0, start=1000, latency=3900, stalls=0, heard=2300,
           frames=None, levels=None, handoff=None, include_audio=True):
    result = {
        "schema": 1, "implementation": "engine", "scenario": scenario, "profile": profile, "events": events,
        "availabilityDelayMs": 300, "durationS": 25, "repetition": repetition, "rejoins": 0,
        "video": {}, "levels": levels or {}, "handoff": handoff,
        "server": {"requests": {}, "bytes": {}},
    }
    if include_audio:
        result["audio"] = {
            "startMs": start, "stallCount": stalls, "stallMs": stalls * 300, "latencyMedianMs": latency,
            "latencyMaxMs": latency + 20, "heardBlocks": {"marker": heard, "2300": heard, "2900": heard, "1000": 0},
        }
    else:
        result["audio"] = {"startMs": None, "stallCount": 0, "stallMs": 0, "latencyMedianMs": None,
                           "latencyMaxMs": None, "heardBlocks": {"marker": 0, "2300": 0, "2900": 0, "1000": 0}}
    if frames is not None:
        result["video"]["unified"] = {"startMs": 150, "frames": frames, "stallCount": 0, "stallMs": 0,
                                      "latencyMedianMs": 3500, "latencyMaxMs": 3520, "widths": {"1280": frames}}
    return result


def synthetic_metrics():
    """(metric, rule, mean, sd) shaped like the baseline's checks: millisecond metrics of small and large spread,
    stall counts, heard blocks, frames, gap blocks, mean levels."""
    shapes = []
    for i in range(40):
        shapes.append(("audio.startMs", 900 + 40 * i, 20 + 15 * i))
        shapes.append(("audio.latencyMedianMs", 3900 + 30 * i, 15 + 2 * i))
        shapes.append(("audio.stallMs", 300 * (i % 5), 40 + 60 * (i % 5)))
        shapes.append(("audio.stallCount", i % 5, 0.3 + 0.1 * (i % 4)))
        shapes.append(("audio.heardBlocks.2300", 2400 - 10 * i, 10 + i))
        shapes.append(("video.ep-0.frames", 650 - 5 * i, 8 + i))
        shapes.append(("handoff.gapBlocks", 11 + (i % 4), 1.5))
        shapes.append(("levels.speaker0.meanLevel", 0.8, 0.01 + 0.002 * i))
    return [(metric, broadcast_report.rule_for(metric, "audio"), mean, sd) for metric, mean, sd in shapes]


def draw(rng, metric, mean, sd, count):
    integer = not metric.endswith("Ms") and "meanLevel" not in metric
    values = []
    for _ in range(count):
        value = max(rng.gauss(mean, sd), 0.0)
        values.append(float(round(value)) if integer else value)
    return values


def write_reports(directory, reports):
    os.makedirs(directory, exist_ok=True)
    for index, item in enumerate(reports):
        with open(os.path.join(directory, f"report-{index}.json"), "w") as f:
            json.dump(item, f)


class BroadcastReportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, args):
        out = io.StringIO()
        with redirect_stdout(out):
            code = broadcast_report.main(args)
        return code, out.getvalue()

    def test_aggregate_prefix_names_the_files(self):
        reports_dir = os.path.join(self.root, "reports")
        write_reports(reports_dir, [report(start=100, repetition=0)])
        out_dir = os.path.join(self.root, "player")
        code, _ = self.run_main(["aggregate", reports_dir, out_dir, "--prefix", "player"])
        self.assertEqual(code, 0)
        self.assertEqual(os.listdir(out_dir), ["player-audio-lte-none.json"])

    def test_aggregate_percentiles(self):
        reports_dir = os.path.join(self.root, "reports")
        write_reports(reports_dir, [report(start=100, repetition=0), report(start=200, repetition=1),
                                    report(start=300, repetition=2)])
        out_dir = os.path.join(self.root, "baseline")
        code, _ = self.run_main(["aggregate", reports_dir, out_dir])
        self.assertEqual(code, 0)
        files = os.listdir(out_dir)
        self.assertEqual(files, ["engine-audio-lte-none.json"])
        with open(os.path.join(out_dir, files[0])) as f:
            cell = json.load(f)
        start = cell["metrics"]["audio.startMs"]
        self.assertEqual(cell["repetitions"], 3)
        self.assertAlmostEqual(start["median"], 200)
        self.assertAlmostEqual(start["p10"], 120)
        self.assertAlmostEqual(start["p90"], 280)
        self.assertEqual(cell["cell"], {"scenario": "audio", "profile": "lte", "events": ""})

    def make_baseline(self, reports):
        reports_dir = os.path.join(self.root, "baseline-reports")
        write_reports(reports_dir, reports)
        out_dir = os.path.join(self.root, "baseline")
        code, _ = self.run_main(["aggregate", reports_dir, out_dir])
        self.assertEqual(code, 0)
        return reports_dir, out_dir

    def test_compare_against_own_reports_passes(self):
        reports = [report(start=900 + 50 * i, latency=3880 + 10 * i, repetition=i) for i in range(5)]
        reports += [report(scenario="unified", frames=740 + i, include_audio=False, repetition=i) for i in range(5)]
        reports_dir, baseline_dir = self.make_baseline(reports)
        code, out = self.run_main(["compare", baseline_dir, reports_dir])
        self.assertEqual(code, 0, out)

    def test_late_start_fails_naming_cell_and_metric(self):
        _, baseline_dir = self.make_baseline([report(start=900 + 50 * i, repetition=i) for i in range(5)])
        candidate_dir = os.path.join(self.root, "candidate")
        # Baseline p90 is 1080; 200 ms later than that fails.
        write_reports(candidate_dir, [report(start=1280, repetition=i) for i in range(5)])
        code, out = self.run_main(["compare", baseline_dir, candidate_dir])
        self.assertNotEqual(code, 0)
        self.assertIn("audio-lte-none", out)
        self.assertIn("audio.startMs", out)

    def test_metrics_option_excludes_audio_for_a_unified_candidate(self):
        baseline = [report(scenario="unified", frames=740 + i, repetition=i) for i in range(5)]
        _, baseline_dir = self.make_baseline(baseline)
        candidate_dir = os.path.join(self.root, "candidate")
        write_reports(candidate_dir, [report(scenario="unified", frames=745, include_audio=False, repetition=i)
                                      for i in range(5)])
        code, out = self.run_main(["compare", baseline_dir, candidate_dir])
        self.assertNotEqual(code, 0, "missing audio metrics must fail by default")
        code, out = self.run_main(["compare", baseline_dir, candidate_dir, "--metrics", "video"])
        self.assertEqual(code, 0, out)

    def test_levels_rules(self):
        levels = lambda mean, speaking: {"speaker0": {"meanLevel": mean, "speakingFraction": speaking, "updates": 230}}
        _, baseline_dir = self.make_baseline([report(levels=levels(0.80 + 0.01 * i, 0.30), repetition=i)
                                              for i in range(5)])
        # A higher mean level passes (the new player reports the 100 ms maximum); a much lower one fails.
        higher_dir = os.path.join(self.root, "higher")
        write_reports(higher_dir, [report(levels=levels(0.95, 0.30), repetition=i) for i in range(5)])
        code, out = self.run_main(["compare", baseline_dir, higher_dir])
        self.assertEqual(code, 0, out)
        lower_dir = os.path.join(self.root, "lower")
        write_reports(lower_dir, [report(levels=levels(0.60, 0.30), repetition=i) for i in range(5)])
        code, out = self.run_main(["compare", baseline_dir, lower_dir])
        self.assertNotEqual(code, 0)
        self.assertIn("levels.speaker0.meanLevel", out)

    def test_speaking_fraction_is_informational(self):
        # webrtc's VAD judges a steady sine tone speech in one of two modes, picked per run (a speaker reads 0.0 in
        # some runs and 0.1-0.4 in others): the fraction carries no signal a candidate could be held to.
        levels = lambda speaking: {"speaker1": {"meanLevel": 0.8, "speakingFraction": speaking, "updates": 240}}
        modes = [0.0, 0.0, 0.004, 0.41, 0.42]
        _, baseline_dir = self.make_baseline([report(levels=levels(s), repetition=i) for i, s in enumerate(modes)])
        candidate_dir = os.path.join(self.root, "candidate")
        write_reports(candidate_dir, [report(levels=levels(0.9), repetition=i) for i in range(5)])
        code, out = self.run_main(["compare", baseline_dir, candidate_dir])
        self.assertEqual(code, 0, out)
        self.assertNotIn("speakingFraction", out)

    def test_an_equal_candidate_passes(self):
        # A candidate drawn from the baseline's own distribution must pass nearly always: with 5 runs a side, "median
        # at most p90" failed such a candidate on ~13% of checks (~44 of the matrix's 500, on a baseline whose 5 runs
        # were ~2.5 independent samples). The baseline has 10 runs a cell and the margin is the baseline's p10..p90
        # spread (at least the metric's slack).
        rng = random.Random(7)
        metrics = synthetic_metrics()
        failures = 0
        trials = 40
        for _ in range(trials):
            for metric, rule, mean, sd in metrics:
                baseline = broadcast_report.summarize_values(draw(rng, metric, mean, sd, 10))
                candidate = broadcast_report.percentile(draw(rng, metric, mean, sd, 5), 0.5)
                failures += broadcast_report.check(metric, rule, candidate, baseline) is not None
        self.assertLessEqual(failures / trials, 0.5, f"{failures} false failures over {trials} matrices of "
                                                     f"{len(metrics)} checks")

    def test_a_candidate_well_beyond_the_spread_fails(self):
        rng = random.Random(8)
        metrics = synthetic_metrics()
        detected = 0
        for metric, rule, mean, sd in metrics:
            baseline = broadcast_report.summarize_values(draw(rng, metric, mean, sd, 10))
            step = max(5 * sd, 3 * broadcast_report.slack_for(metric))
            shifted = mean + step if rule == "lower" else mean - step
            candidate = broadcast_report.percentile(draw(rng, metric, shifted, sd, 5), 0.5)
            detected += broadcast_report.check(metric, rule, candidate, baseline) is not None
        self.assertGreaterEqual(detected / len(metrics), 0.9, f"{detected} of {len(metrics)} regressions caught")

    def test_calibrate_draws_candidates_from_the_recorded_runs(self):
        # The figures the docs quote come from `calibrate`: the recorded baseline fixed, each candidate's 5 runs drawn
        # with replacement from the cell's recorded runs. Runs that tie can never fail an equal candidate, and one
        # worse by max(3 sd, 2 slack) = 2 slack always exceeds a margin of one slack.
        reports_dir = os.path.join(self.root, "reports")
        write_reports(reports_dir, [report(start=1000, latency=3900, repetition=i) for i in range(10)])
        code, out = self.run_main(["calibrate", reports_dir, "--matrices", "20"])
        self.assertEqual(code, 0, out)
        self.assertIn("7 checks over 20 matrices: an equal candidate fails 0.00 checks per matrix, 0 in all with "
                      "--confirm (20 of 20 matrices pass with --confirm)", out)
        self.assertIn("a candidate worse by max(3 sd, 2 slack) fails 100% of checks, 100% with --confirm", out)

    def test_frame_order_rules(self):
        # Repeated, out-of-order and dropped frames are lower-is-better (the old engine shows some segments twice
        # after a quality update; a player that does not passes); frames that could not be placed are informational.
        self.assertEqual(broadcast_report.rule_for("video.ep-2.repeatedFrames", "tiles"), "lower")
        self.assertEqual(broadcast_report.rule_for("video.ep-2.outOfOrderFrames", "tiles"), "lower")
        self.assertEqual(broadcast_report.rule_for("video.unified.droppedFrames", "unified"), "lower")
        self.assertIsNone(broadcast_report.rule_for("video.ep-2.unplacedFrames", "tiles"))
        self.assertEqual(broadcast_report.slack_for("video.ep-0.droppedFrames"), 2)
        baseline = broadcast_report.summarize_values([30.0, 30.0, 30.0, 30.0, 31.0, 30.0, 30.0, 30.0, 30.0, 29.0])
        self.assertIsNone(broadcast_report.check("video.ep-0.droppedFrames", "lower", 0.0, baseline))
        self.assertIsNotNone(broadcast_report.check("video.ep-0.droppedFrames", "lower", 90.0, baseline))

    def test_a_run_missing_a_metric_fails(self):
        # A run that never started has no audio.startMs; leaving it out of the median would pass a candidate that
        # fails to start in 2 of 5 runs.
        _, baseline_dir = self.make_baseline([report(start=900 + 50 * i, repetition=i) for i in range(5)])
        candidate_dir = os.path.join(self.root, "candidate")
        candidates = [report(start=900, repetition=i) for i in range(5)]
        candidates[3]["audio"]["startMs"] = None
        write_reports(candidate_dir, candidates)
        code, out = self.run_main(["compare", baseline_dir, candidate_dir])
        self.assertNotEqual(code, 0)
        self.assertIn("FAIL audio-lte-none audio.startMs: missing in 1 of 5 runs", out)

    def test_confirm_fails_only_checks_failing_against_both_candidates(self):
        _, baseline_dir = self.make_baseline([report(start=900 + 50 * i, repetition=i) for i in range(5)])
        late_dir = os.path.join(self.root, "late")
        write_reports(late_dir, [report(start=2000, repetition=i) for i in range(5)])
        on_time_dir = os.path.join(self.root, "on-time")
        write_reports(on_time_dir, [report(start=950, repetition=i) for i in range(5)])
        late_again_dir = os.path.join(self.root, "late-again")
        write_reports(late_again_dir, [report(start=2100, repetition=i) for i in range(5)])
        code, out = self.run_main(["compare", baseline_dir, late_dir, "--confirm", on_time_dir])
        self.assertEqual(code, 0, out)
        self.assertIn("unconfirmed", out)
        code, out = self.run_main(["compare", baseline_dir, late_dir, "--confirm", late_again_dir])
        self.assertNotEqual(code, 0)
        self.assertIn("FAIL audio-lte-none audio.startMs", out)

    def test_heard_blocks_of_a_tone_the_scenario_does_not_play_are_informational(self):
        # Neighbouring frequencies leak into the detector: the old engine "hears" the unified tone a few blocks a run
        # in listener cells, and the marker in unified cells. A candidate that leaks less must not fail for it.
        def with_heard(item, heard):
            item["audio"]["heardBlocks"] = heard
            return item

        def cells(listener_heard, unified_heard):
            return ([with_heard(report(repetition=i), listener_heard(i)) for i in range(5)]
                    + [with_heard(report(scenario="unified", frames=740, repetition=i), unified_heard(i))
                       for i in range(5)])

        _, baseline_dir = self.make_baseline(cells(
            lambda i: {"marker": 2400, "2300": 2400, "2900": 2400, "1000": 20 + 10 * i},
            lambda i: {"marker": 10 + i, "2300": 0, "2900": 0, "1000": 2480}))
        clean_dir = os.path.join(self.root, "clean")
        write_reports(clean_dir, cells(lambda i: {"marker": 2400, "2300": 2400, "2900": 2400, "1000": 0},
                                       lambda i: {"marker": 0, "2300": 0, "2900": 0, "1000": 2480}))
        code, out = self.run_main(["compare", baseline_dir, clean_dir])
        self.assertEqual(code, 0, out)
        # The tones a scenario does play are still checked.
        quiet_dir = os.path.join(self.root, "quiet")
        write_reports(quiet_dir, cells(lambda i: {"marker": 2400, "2300": 1200, "2900": 2400, "1000": 0},
                                       lambda i: {"marker": 0, "2300": 0, "2900": 0, "1000": 1200}))
        code, out = self.run_main(["compare", baseline_dir, quiet_dir])
        self.assertNotEqual(code, 0)
        self.assertIn("FAIL audio-lte-none audio.heardBlocks.2300:", out)
        self.assertIn("FAIL unified-lte-none audio.heardBlocks.1000:", out)

    def test_missing_baseline_cell_fails(self):
        _, baseline_dir = self.make_baseline([report(repetition=i) for i in range(3)])
        candidate_dir = os.path.join(self.root, "candidate")
        write_reports(candidate_dir, [report(profile="3g", repetition=0)])
        code, out = self.run_main(["compare", baseline_dir, candidate_dir])
        self.assertNotEqual(code, 0)
        self.assertIn("audio-3g-none", out)


if __name__ == "__main__":
    unittest.main()
