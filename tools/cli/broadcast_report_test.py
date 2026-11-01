"""Tests for broadcast_report.py: aggregating the broadcast testbench's reports into a baseline and comparing a
candidate against it. Run: python3 -m unittest discover -s submodules/TgVoipWebrtc/tgcalls/tools/cli -p 'broadcast_report_test.py'
"""

import io
import json
import os
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
        candidate_dir = os.path.join(self.root, "candidate")
        write_reports(candidate_dir, [report(levels=levels(0.95, 0.40), repetition=i) for i in range(5)])
        code, out = self.run_main(["compare", baseline_dir, candidate_dir])
        self.assertNotEqual(code, 0)
        self.assertIn("levels.speaker0.speakingFraction", out)
        self.assertNotIn("levels.speaker0.meanLevel", out)

    def test_speaking_fraction_tolerance_follows_the_baselines_spread(self):
        # The old engine's VAD judges the sine tones speech in a fraction that varies run to run by more than 0.05
        # around its median: a candidate inside the baseline's p10..p90 widened by 0.05 passes, one beyond it fails.
        levels = lambda speaking: {"speaker0": {"meanLevel": 0.8, "speakingFraction": speaking, "updates": 240}}
        spread = [0.30, 0.34, 0.39, 0.41, 0.44]  # p10 0.316, median 0.39, p90 0.428
        _, baseline_dir = self.make_baseline([report(levels=levels(s), repetition=i) for i, s in enumerate(spread)])
        inside_dir = os.path.join(self.root, "inside")
        write_reports(inside_dir, [report(levels=levels(0.32), repetition=i) for i in range(5)])
        code, out = self.run_main(["compare", baseline_dir, inside_dir])
        self.assertEqual(code, 0, out)
        outside_dir = os.path.join(self.root, "outside")
        write_reports(outside_dir, [report(levels=levels(0.25), repetition=i) for i in range(5)])
        code, out = self.run_main(["compare", baseline_dir, outside_dir])
        self.assertNotEqual(code, 0)
        self.assertIn("levels.speaker0.speakingFraction", out)

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
