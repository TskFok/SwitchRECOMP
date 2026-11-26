# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
"""Summarize a SwitchRecomp OpenGL frame_timing.csv capture."""

import argparse
import csv
import math
from pathlib import Path


TIMES = (
    "frame_ns", "submit_interval_ns", "queue_wait_ns", "present_work_ns",
    "gpu_thread_work_ns", "swap_ns", "gl_sync_ns", "shader_ns", "texture_ns",
    "buffer_ns", "compute_wait_ns", "draw_ns",
)
FIELDS = ("frame", "elapsed_ns", *TIMES, "shader_wait_draws")
SLOW_FRAME_TIMES = (
    "frame_ns", "submit_interval_ns", "queue_wait_ns", "present_work_ns",
    "gpu_thread_work_ns", "swap_ns", "gl_sync_ns", "shader_ns", "compute_wait_ns",
)


def load_samples(path, skip_ns):
    samples = []
    last_frame = last_elapsed = 0
    with path.open(encoding="utf-8", newline="") as stream:
        reader = csv.reader(stream)
        if tuple(next(reader, ())) != FIELDS:
            raise ValueError("unexpected frame timing CSV header")
        for line, row in enumerate(reader, 2):
            try:
                if len(row) != len(FIELDS):
                    raise ValueError("wrong number of columns")
                sample = dict(zip(FIELDS, map(int, row)))
                if any(value < 0 for value in sample.values()) or sample["frame_ns"] == 0:
                    raise ValueError("negative counter or zero frame interval")
                if sample["frame"] <= last_frame or sample["elapsed_ns"] <= last_elapsed:
                    raise ValueError("non-increasing frame number or elapsed time")
            except ValueError as error:
                raise ValueError(f"invalid row {line}: {error}") from error
            last_frame, last_elapsed = sample["frame"], sample["elapsed_ns"]
            if last_elapsed >= skip_ns:
                samples.append(sample)
    if not samples:
        raise ValueError("no frame samples in the selected interval")
    return samples


def report(samples, budget_ns, top):
    over = sum(sample["frame_ns"] > budget_ns for sample in samples)
    print(f"{len(samples)} frames, {over} over the {budget_ns / 1e6:.3f} ms budget")
    print(f"{'time (ms)':<22}{'P50':>10}{'P95':>10}{'P99':>10}{'max':>10}")
    for field in TIMES:
        values = sorted(sample[field] for sample in samples)
        ranks = [math.ceil(len(values) * percent / 100) - 1 for percent in (50, 95, 99, 100)]
        times = "".join(f"{values[rank] / 1e6:>10.3f}" for rank in ranks)
        print(f"{field[:-3]:<22}{times}")

    if top:
        slow = sorted(samples, key=lambda sample: sample["frame_ns"], reverse=True)[:top]
        print("\nSlowest frames (times in ms except elapsed_s)")
        headings = "".join(f"{field[:-3]:>16}" for field in SLOW_FRAME_TIMES)
        print(f"{'id':>8}{'elapsed_s':>12}{headings}{'shader_wait_draws':>19}")
        for sample in slow:
            times = "".join(f"{sample[field] / 1e6:>16.3f}" for field in SLOW_FRAME_TIMES)
            print(f"{sample['frame']:>8}{sample['elapsed_ns'] / 1e9:>12.3f}"
                  f"{times}{sample['shader_wait_draws']:>19}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--fps", type=float, default=60, help="target FPS for the frame budget")
    parser.add_argument("--skip-seconds", type=float, default=0, help="exclude startup samples")
    parser.add_argument("--top", type=int, default=10, help="number of slow frame rows to list")
    args = parser.parse_args()
    if not math.isfinite(args.fps) or args.fps <= 0:
        parser.error("--fps must be positive and finite")
    if not math.isfinite(args.skip_seconds) or args.skip_seconds < 0 or args.top < 0:
        parser.error("--skip-seconds must be finite and both --skip-seconds and --top must be nonnegative")
    try:
        samples = load_samples(args.capture, args.skip_seconds * 1e9)
        report(samples, round(1e9 / args.fps), args.top)
    except (OSError, ValueError, csv.Error) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
