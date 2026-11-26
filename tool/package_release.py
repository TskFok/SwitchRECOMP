# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Assemble the OpenGL/headless release archives from reviewed inputs."""
import argparse
import gzip
import io
import os
import tarfile
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMMON_FILES = (
    "LICENSE.txt", "NOTICE.txt", "README.md", "README.txt", "CHANGELOG_v190.txt",
    "switchrecomp.py", "a64.py", "codegen.py", "nxloader.py", "tools_sym.py", "brt.py",
    "tool/get_deps.py",
    "tool/fonts/Hind-Regular.ttf", "tool/fonts/Hind-Medium.ttf", "tool/fonts/Hind-SemiBold.ttf",
    "tool/fonts/BarlowSemiCondensed-Medium.ttf", "tool/fonts/OFL-Barlow.txt", "tool/fonts/OFL-Hind.txt",
    "runtime/bcn/LICENSE_texture2ddecoder.txt", "runtime/bcn/fp16.h", "runtime/bcn/endianness.h",
    "runtime/spvc/LICENSE-SPIRV-Cross.txt",
    "tool/audio_decode/dr_wav.h", "tool/audio_decode/dr_mp3.h", "tool/audio_decode/stb_vorbis.c",
)
WINDOWS_NOTICES = ("MinGW-w64", "SDL2", "SDL2_image", "SDL2_ttf", "FreeType", "HarfBuzz")
OPTIONAL_RUNTIME_LIBS = {
    "libsnxvk.a", "libspvc.a", "libdecoder.a", "libcommon.a", "libopenh264.a", "libopus.a", "libvpx.a",
}
LINUX_LAUNCHER = b'#!/bin/sh\ncd -- "$(dirname -- "$0")" || exit\nexec ./SwitchRecomp.bin "$@"\n'


def regular_file(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"Expected a regular file: {path}")
    data = path.read_bytes()
    if not data:
        raise ValueError(f"Empty package input: {path}")
    return data


def package_contents(binaries, link_maps, notices, source_index, target):
    windows = target == "win64"
    magic = b"MZ" if windows else b"\x7fELF"
    files = {name: regular_file(ROOT / name) for name in COMMON_FILES}
    files["SOURCE.txt"] = regular_file(source_index)
    if windows:
        files["Install.bat"] = regular_file(ROOT / "Install.bat")
    else:
        files["SwitchRecomp"] = LINUX_LAUNCHER

    tool = "SwitchRecomp.exe" if windows else "SwitchRecomp.bin"
    files[tool] = regular_file(binaries / tool)
    if not files[tool].startswith(magic):
        raise ValueError(f"{tool} has the wrong target format")

    suffix = ".exe" if windows else ""
    for backend in ("headless", "win32" if windows else "x11"):
        name = f"runtime_{target}_{backend}{suffix}"
        data = regular_file(binaries / name)
        if not data.startswith(magic):
            raise ValueError(f"{name} has the wrong target format")
        if b"v190\0" not in data or b"v190 test\0" in data:
            raise ValueError(f"{name} must be built with --build-runtime --release")
        link_map = regular_file(link_maps / f"{name}.map").decode("utf-8", errors="replace")
        if "Linker script and memory map" not in link_map:
            raise ValueError(f"{name}: expected a GNU ld link map")
        for line in link_map.splitlines():
            if line.startswith("LOAD "):
                archive = Path(line[5:].strip()).name
                if archive in OPTIONAL_RUNTIME_LIBS:
                    raise ValueError(f"{name} links unsupported optional archive {archive}")
        files[f"bin/{name}"] = data

    for name in ("GCC", *(WINDOWS_NOTICES if windows else ("glibc",))):
        files[f"third-party/{name}.txt"] = regular_file(notices / f"{name}.txt")
    return files


def write_package(output, files, windows):
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, path = tempfile.mkstemp(dir=output.parent)
    os.close(fd)
    temp = Path(path)
    try:
        if windows:
            with zipfile.ZipFile(temp, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                for name, data in sorted(files.items()):
                    member = zipfile.ZipInfo("SwitchRecomp/" + name)
                    member.create_system = 3
                    member.external_attr = 0o100644 << 16
                    archive.writestr(member, data, compress_type=zipfile.ZIP_DEFLATED)
        else:
            with temp.open("wb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0) as zipped, \
                    tarfile.open(fileobj=zipped, mode="w") as archive:
                for name, data in sorted(files.items()):
                    member = tarfile.TarInfo("SwitchRecomp/" + name)
                    member.size = len(data)
                    if name in ("SwitchRecomp", "SwitchRecomp.bin") or name.startswith("bin/"):
                        member.mode = 0o755
                    archive.addfile(member, io.BytesIO(data))
        size = temp.stat().st_size
        if size >= 20_000_000:
            raise ValueError(f"Package is {size} bytes, expected under 20 MB")
        temp.chmod(0o644)
        temp.replace(output)
        return size
    finally:
        temp.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("linux", "win64"), required=True)
    parser.add_argument("--binaries", type=Path, required=True, help="directory with both runtimes and GUI")
    parser.add_argument("--link-maps", type=Path, required=True, help="GNU ld map files for the built runtimes")
    parser.add_argument("--notices", type=Path, required=True, help="reviewed third-party license texts")
    parser.add_argument("--source-index", type=Path, required=True, help="source archive names, hashes and build inputs")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    windows = args.target == "win64"
    if not args.output.name.endswith(".zip" if windows else ".tar.gz"):
        parser.error("output must be .zip for Win64 or .tar.gz for Linux")
    try:
        files = package_contents(args.binaries, args.link_maps, args.notices, args.source_index, args.target)
        size = write_package(args.output, files, windows)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Done: {args.output} ({len(files)} files, {size} bytes)")


if __name__ == "__main__":
    main()
