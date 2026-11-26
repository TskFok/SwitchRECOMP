# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Compare captured GPU regression frames, allowing 2/255 per channel."""

import argparse
from pathlib import Path
import re


def ppm(path):
    data = path.read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\n", data)
    if not header:
        raise ValueError(f"invalid P6 header in {path.name}")
    width, height = map(int, header.groups())
    pixels = data[header.end():]
    if not width or not height or len(pixels) != width * height * 3:
        raise ValueError(f"invalid P6 dimensions or payload in {path.name}")
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    parser.add_argument("base")
    parser.add_argument("root", nargs="?", type=Path, default=Path(__file__).with_name("programs"))
    args = parser.parse_args()
    same = different = 0
    for sample in sorted(args.root.glob("*/")):
        if not (sample / f"{sample.name}_code.so").is_file():
            continue
        failed = []
        for tag in (args.tag, args.base):
            status = sample / f"{tag}.exit"
            if status.is_file() and (code := status.read_text(encoding="ascii").strip()) != "0":
                failed.append(f"{tag} exit={code}")
        if failed:
            print(f"{sample.name:24} INVALID RUN ({', '.join(failed)})")
            different += 1
            continue
        try:
            wa, ha, pa = ppm(sample / f"{args.tag}.ppm")
            wb, hb, pb = ppm(sample / f"{args.base}.ppm")
        except (OSError, ValueError) as error:
            print(f"{sample.name:24} NO IMAGE ({error})")
            different += 1
            continue
        if (wa, ha) != (wb, hb):
            print(f"{sample.name:24} SIZE {wa}x{ha} vs {wb}x{hb}")
            different += 1
            continue
        pixels = sum(any(abs(pa[i + c] - pb[i + c]) > 2 for c in range(3))
                     for i in range(0, len(pa), 3))
        if pixels:
            print(f"{sample.name:24} DIFFERENT {pixels} pixels ({100.0 * pixels / (wa * ha):.2f}%)")
            different += 1
        else:
            print(f"{sample.name:24} same")
            same += 1
    print(f"{same} same, {different} different")
    return int(not same or different > 0)


if __name__ == "__main__":
    raise SystemExit(main())
