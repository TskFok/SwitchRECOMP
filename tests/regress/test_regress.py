# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent


class RegressionScripts(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def compare(self):
        return subprocess.run([sys.executable, HERE / "cmpreg.py", "new", "base", self.root],
                              text=True, capture_output=True)

    def sample(self):
        sample = self.root / "demo"
        sample.mkdir()
        (sample / "demo_code.so").touch()
        return sample

    def test_empty_suite_fails_comparison(self):
        result = self.compare()
        self.assertEqual(result.returncode, 1)
        self.assertIn("0 same, 0 different", result.stdout)

    @unittest.skipUnless(os.name == "posix", "X11 runner requires a POSIX host")
    def test_empty_suite_fails_runner(self):
        result = subprocess.run(["bash", HERE / "regrun.sh", sys.executable, "new", self.root],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("0 samples", result.stdout)

    def test_missing_baseline_fails_comparison(self):
        sample = self.sample()
        (sample / "new.ppm").write_bytes(b"P6\n1 1\n255\n\x20\x0a\x0d")
        result = self.compare()
        self.assertEqual(result.returncode, 1)
        self.assertIn("NO IMAGE", result.stdout)

    def test_binary_pixel_prefix_is_preserved(self):
        sample = self.sample()
        for tag in ("base", "new"):
            (sample / f"{tag}.ppm").write_bytes(b"P6\n1 1\n255\n\x20\x0a\x0d")
        result = self.compare()
        self.assertEqual(result.returncode, 0)
        self.assertIn("1 same, 0 different", result.stdout)
        (sample / "new.ppm").write_bytes(b"P6\n1 1\n255\n\x24\x0a\x0d")
        result = self.compare()
        self.assertEqual(result.returncode, 1)
        self.assertIn("DIFFERENT 1 pixels", result.stdout)

    def test_truncated_image_fails_comparison(self):
        sample = self.sample()
        (sample / "base.ppm").write_bytes(b"P6\n1 1\n255\n\x01\x02\x03")
        (sample / "new.ppm").write_bytes(b"P6\n1 1\n255\n\x01\x02")
        result = self.compare()
        self.assertEqual(result.returncode, 1)
        self.assertIn("invalid P6 dimensions or payload", result.stdout)

    def test_invalid_runtime_exit_rejects_matching_images(self):
        sample = self.sample()
        for tag in ("base", "new"):
            (sample / f"{tag}.ppm").write_bytes(b"P6\n1 1\n255\n\x01\x02\x03")
        (sample / "new.exit").write_text("1\n", encoding="ascii")
        result = self.compare()
        self.assertEqual(result.returncode, 1)
        self.assertIn("INVALID RUN (new exit=1)", result.stdout)

    @unittest.skipUnless(all(shutil.which(cmd) for cmd in ("xvfb-run", "Xvfb", "xauth", "timeout")),
                         "requires the X11 regression tools")
    def test_fatal_log_rejects_capture_with_zero_exit(self):
        self.sample()
        runtime = self.root / "runtime"
        runtime.write_text("#!/bin/sh\nprintf 'P6\\n1 1\\n255\\nabc' > new.ppm\n"
                           "printf 'FATAL: test failure\\nframes=1 hash=01\\n'\n", encoding="ascii")
        runtime.chmod(0o755)
        result = subprocess.run(["bash", HERE / "regrun.sh", runtime, "new", self.root],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("exit=0", result.stdout)
        self.assertIn("1 failed captures", result.stdout)


if __name__ == "__main__":
    unittest.main()
