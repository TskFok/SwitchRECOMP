# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ("switchrecomp.py", "a64.py", "codegen.py", "nxloader.py", "tools_sym.py")


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.work = Path(work.name)

    @unittest.skipIf(sys.platform == "win32", "uses an executable compiler stub with a shebang")
    def test_runtime_build_requires_explicit_release_flag(self):
        for name in SCRIPTS:
            shutil.copyfile(ROOT / name, self.work / name)
        cc = self.work / "gcc"
        cc.write_text(
            '#!/usr/bin/env python3\nimport json, pathlib, sys\n'
            'pathlib.Path(__file__).with_suffix(".json").write_text(json.dumps(sys.argv[1:]))\n',
            encoding="utf-8",
        )
        cc.chmod(0o755)
        cli = [sys.executable, str(self.work / "switchrecomp.py")]
        build = cli + ["--build-runtime", "--target", "linux", "--backend", "headless", "--cc", str(cc)]
        subprocess.run(build, check=True, capture_output=True)
        self.assertNotIn('-DSWR_RUNTIME_VERSION="v190"', json.loads(cc.with_suffix(".json").read_bytes()))
        subprocess.run(build + ["--release"], check=True, capture_output=True)
        self.assertIn('-DSWR_RUNTIME_VERSION="v190"', json.loads(cc.with_suffix(".json").read_bytes()))
        bad = subprocess.run(cli + ["--release"], capture_output=True, text=True)
        self.assertEqual(bad.returncode, 2)
        self.assertIn("--release requires --build-runtime", bad.stderr)

    def package(self, target, version=b"v190\0", missing=None, optional_lib=None):
        bins = self.work / target
        bins.mkdir()
        windows = target == "win64"
        suffix = ".exe" if windows else ""
        magic = b"MZ" if windows else b"\x7fELF"
        for backend in ("headless", "win32" if windows else "x11"):
            runtime = f"runtime_{target}_{backend}{suffix}"
            (bins / runtime).write_bytes(magic + version)
            link_map = "Linker script and memory map\n"
            if optional_lib:
                link_map += f"LOAD /tmp/runtime/{optional_lib}\n"
            (bins / f"{runtime}.map").write_text(link_map, encoding="utf-8")
        (bins / ("SwitchRecomp.exe" if windows else "SwitchRecomp.bin")).write_bytes(magic)
        (bins / "Lossless.dll").write_bytes(b"not a package input")
        notices = bins / "notices"
        notices.mkdir()
        for name in ("GCC", "glibc", "MinGW-w64", "SDL2", "SDL2_image", "SDL2_ttf",
                     "FreeType", "HarfBuzz"):
            if name != missing:
                (notices / f"{name}.txt").write_text(name, encoding="utf-8")
        (notices / "unlisted.txt").write_text("not a package input", encoding="utf-8")
        source = bins / "SOURCE.txt"
        source.write_text("test source index\n", encoding="utf-8")
        archive = bins / ("test.zip" if windows else "test.tar.gz")
        run = subprocess.run(
            [sys.executable, str(ROOT / "tool/package_release.py"), "--target", target,
             "--binaries", str(bins), "--link-maps", str(bins), "--notices", str(notices),
             "--source-index", str(source),
             "--output", str(archive)],
            capture_output=True, text=True,
        )
        return archive, run

    def test_linux_archive_contains_fonts_and_licenses(self):
        archive, run = self.package("linux")
        self.assertEqual(run.returncode, 0, run.stderr)
        with tarfile.open(archive) as package:
            self.assertEqual(package.getmember("SwitchRecomp/SwitchRecomp").mode & 0o111, 0o111)
            self.assertEqual(package.getmember("SwitchRecomp/SwitchRecomp.bin").mode & 0o111, 0o111)
            self.assertIn("SwitchRecomp/third-party/glibc.txt", package.getnames())
            self.assertNotIn("SwitchRecomp/third-party/SDL2.txt", package.getnames())
            self.assertNotIn("SwitchRecomp/third-party/unlisted.txt", package.getnames())
            for name in ("Hind-Regular.ttf", "Hind-Medium.ttf", "Hind-SemiBold.ttf",
                         "BarlowSemiCondensed-Medium.ttf", "OFL-Barlow.txt", "OFL-Hind.txt"):
                path = f"tool/fonts/{name}"
                self.assertEqual(package.extractfile(f"SwitchRecomp/{path}").read(), (ROOT / path).read_bytes())

    def test_windows_archive_contains_static_dependency_notices(self):
        archive, run = self.package("win64")
        self.assertEqual(run.returncode, 0, run.stderr)
        with zipfile.ZipFile(archive) as package:
            self.assertEqual(package.read("SwitchRecomp/bin/runtime_win64_win32.exe"), b"MZv190\0")
            self.assertIn("SwitchRecomp/third-party/HarfBuzz.txt", package.namelist())
            self.assertIn("SwitchRecomp/SOURCE.txt", package.namelist())
            self.assertNotIn("SwitchRecomp/Lossless.dll", package.namelist())
            self.assertNotIn("SwitchRecomp/third-party/unlisted.txt", package.namelist())

    def test_test_version_and_missing_notice_block_archive(self):
        archive, run = self.package("linux", version=b"v190 test\0")
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("runtime_linux_headless", run.stderr)
        self.assertFalse(archive.exists())
        archive, run = self.package("win64", missing="FreeType")
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("FreeType.txt", run.stderr)
        self.assertFalse(archive.exists())

    def test_vulkan_runtime_cannot_use_opengl_package_profile(self):
        archive, run = self.package("win64", optional_lib="libsnxvk.a")
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("libsnxvk.a", run.stderr)
        self.assertFalse(archive.exists())


if __name__ == "__main__":
    unittest.main()
