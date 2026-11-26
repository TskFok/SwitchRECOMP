# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ANALYZER = Path(__file__).resolve().parents[2] / "tool" / "frame_timing.py"
FIELDS = (
    "frame", "elapsed_ns", "frame_ns", "submit_interval_ns", "queue_wait_ns",
    "present_work_ns", "gpu_thread_work_ns", "swap_ns", "gl_sync_ns", "shader_ns",
    "texture_ns", "buffer_ns", "compute_wait_ns", "draw_ns", "shader_wait_draws",
)


class FrameTimingTest(unittest.TestCase):
    def analyze(self, rows, *args):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frame_timing.csv"
            with path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.writer(stream)
                writer.writerow(FIELDS)
                writer.writerows(rows)
            return subprocess.run([sys.executable, str(ANALYZER), str(path), *args],
                                  text=True, capture_output=True)

    def test_percentiles_and_slowest_frame_after_warmup(self):
        rows = [[number + 1, 1_000_000_000 + number * 100_000_000,
                 (999 if number == 0 else number) * 1_000_000, *([0] * 12)]
                for number in range(101)]
        result = self.analyze(rows, "--skip-seconds", "1.01", "--top", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("100 frames", result.stdout)
        self.assertRegex(result.stdout, r"frame\s+50\.000\s+95\.000\s+99\.000\s+100\.000")
        self.assertRegex(result.stdout, r"(?m)^\s*101\s+11\.000\s+100\.000")

    def test_empty_filtered_capture_and_short_rows_fail(self):
        row = [2, 20_000_000, 20_000_000, *([0] * 12)]
        empty = self.analyze([row], "--skip-seconds", "1")
        short = self.analyze([row[:-1]])
        self.assertNotEqual(empty.returncode, 0)
        self.assertIn("no frame samples", empty.stderr)
        self.assertNotEqual(short.returncode, 0)
        self.assertIn("invalid row 2", short.stderr)


if __name__ == "__main__":
    unittest.main()
