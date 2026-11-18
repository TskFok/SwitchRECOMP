# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Build and run the NHA host contracts and compare the original ABI 1 fixture."""
import argparse
from collections import Counter
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

from make_guest import make_guest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT))
from switchrecomp import RUNTIME_SRC

BASE = "713c8618689afb26b904673a0f2f9901eb6baf7b"
NATIVE_GUEST_APIS = {
    "svc.0x1e", "svc.0x0b", "ipc.time.time.300",
    "ipc.fsp-srv.IFileSystem.8", "ipc.fsp-srv.IFile.0", "ipc.fsp-srv.IFile.4",
    "ipc.fsp-srv.IFile.close", "svc.0x08", "svc.0x09", "svc.0x0a",
    "svc.0x11", "svc.0x12", "svc.0x17", "svc.0x18", "svc.0x19", "svc.0x45", "svc.0x1a", "svc.0x1b",
}
MIXED_LEGACY = {"ipc.time.time.300", "svc.0x11", "svc.0x1b"}
FIXTURE_SEED = 0x4E484132
GUEST_FIXED = {
    "seed": FIXTURE_SEED, "tick_frequency": 19_200_000, "sleep_x0": 1_000_000,
    "yield_x0": 2**64 - 2, "file_size": 6, "file_read_count": 6,
    "file_read_buffer": int.from_bytes(b"hello\n\xa5\xa5", "little"), "closed_domain_rc": 0x1D60A,
    "event_index": 0, "poll_rc": 0xEA01, "cancel_rc": 0xEC01,
    "priority": 32, "ideal_core": 1, "affinity": 2, "exit_index": 0,
    "worker_seed": FIXTURE_SEED, "mutex_word": 0, "closed_thread_rc": 0xE401,
    "successful_closes": 12,
}
GUEST_FIELDS = (*GUEST_FIXED, "duplicate_start_rc", "sleep_ticks", "cntvct_delta",
                "unix_seconds", "system_clock_delta")


def clean_env():
    return {k: v for k, v in os.environ.items() if not k.upper().startswith(("SWR_NATIVE_", "SWR_VT"))}


def run(command, *, env=None, cwd=ROOT, log=None, timeout=120):
    command = [str(part) for part in command]
    if log:
        with log.open("w", encoding="utf-8") as stream:
            stream.write(json.dumps(command) + "\n")
            stream.flush()
            result = subprocess.run(command, env=env, cwd=cwd, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=timeout)
        if result.returncode:
            print(log.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
    else:
        result = subprocess.run(command, env=env, cwd=cwd, timeout=timeout)
    result.check_returncode()


def host_contracts(args, out, env):
    os_name = args.target
    exe = ".exe" if os_name == "win64" else ""
    system_libs = (["-static", "-lgdi32", "-luser32", "-lwinmm", "-lopengl32", "-lcomdlg32", "-lshell32"]
                   if exe else ["-pthread", "-lm", "-ldl"])
    compile_flags = ["-std=gnu17", "-O1", "-g", "-fno-strict-aliasing", "-DSWR_NO_H264",
                     "-DSWR_NO_OPUS", f"-I{ROOT / 'runtime'}"]
    if args.sanitize:
        compile_flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    # Link the same headless Runtime sources once, using a test executable's entry point.
    # MinGW also needs the entire set: PE unwind metadata roots functions in individual TUs.
    names = RUNTIME_SRC + ["gpu_nogl", "shader_proc", "host1x", "snxvk_glue", "backend_headless"]
    objects = out / "runtime-objects"
    objects.mkdir()
    run([args.cc, *compile_flags, "-c", *(ROOT / "runtime" / (name + ".c") for name in names)],
        env=env, cwd=objects, log=out / "build-test-runtime.log", timeout=300)
    linked_runtime = [objects / (name + ".o") for name in names]
    wraps = {
        "time": ["plat_ns", "plat_unix_time", "plat_sleep_ns", "plat_yield", "pc_wait"],
        "ipc_file": ["plat_file_open_read", "plat_file_read_at", "plat_file_size64", "plat_file_close", "fopen"],
        "thread": ["plat_thread_try", "plat_thread_forget"],
    }
    if exe:
        wraps["time"].append("__imp_SleepConditionVariableSRW")
    native = {"SWR_NATIVE_HOS": "1", "SWR_NATIVE_TRACE": "1"}
    variants = {
        "time": [("time", native | {"SWR_NATIVE_TIME": "1", "SWR_NATIVE_SYNC": "1"})],
        "fs": [("fs", {})],
        "ipc_file": [("ipc_file", native | {"SWR_NATIVE_FS": "1"})],
        "thread": [("thread", native | {"SWR_NATIVE_THREAD": "1", "SWR_NATIVE_SYNC": "1"})],
        "sync": [("sync", native | {"SWR_NATIVE_SYNC": "1"}),
                 ("sync-mixed", native | {"SWR_NATIVE_SYNC": "1", "SWR_NATIVE_APIS": "svc.0x1b=legacy"})],
    }
    evidence = {}
    for name in ("dispatch", "time", "fs", "ipc_file", "thread", "sync"):
        dest = out / f"test_{name}{exe}"
        sources = ["runtime/native_hos.c"] if name == "dispatch" else linked_runtime
        run([args.cc, *compile_flags, f"tests/nha/test_{name}.c", *sources,
             *(f"-Wl,--wrap={symbol}" for symbol in wraps.get(name, ())),
             *system_libs, "-o", dest], env=env, log=out / f"build-test-{name}.log")
        if args.compile_only or name == "dispatch":
            continue
        for case, settings in variants[name]:
            with tempfile.TemporaryDirectory(prefix="swr-nha-") as temp:
                log = out / f"test-{case}.log"
                run([dest, temp] if name == "fs" else [dest], env=env | settings, cwd=temp,
                    log=log, timeout=30)
            evidence[case] = {"exit_code": 0, "environment": settings, **check_host_log(case, log, args.target)}
    if args.compile_only:
        return
    host = ["SWR_NATIVE_HOS"]
    dispatch = out / f"test_dispatch{exe}"
    cases = [
        ("default", {}),
        ("default", {"SWR_NATIVE_TIME": "1", "SWR_NATIVE_TRACE": "1"}),
        ("time", dict.fromkeys(host + ["SWR_NATIVE_TIME"], "1")),
        ("override", {"SWR_NATIVE_HOS": "1", "SWR_NATIVE_APIS": "svc.0x1e=native"}),
        ("fs", dict.fromkeys(host + ["SWR_NATIVE_FS", "SWR_NATIVE_TRACE"], "1")),
        ("svc-errors", dict.fromkeys(host + ["SWR_NATIVE_THREAD", "SWR_NATIVE_SYNC"], "1")),
        ("invalid", {"SWR_NATIVE_APIS": "svc.0x1e=native,svc.0x1e=legacy"}),
        ("invalid", {"SWR_NATIVE_APIS": "unknown=native"}),
        ("invalid", {"SWR_NATIVE_APIS": "svc.0x1c=native"}),
        ("invalid", {"SWR_NATIVE_APIS": "svc.0x1e=native,"}),
        ("invalid", {"SWR_NATIVE_HOS": "1", "SWR_NATIVE_APIS": "ipc.fsp-srv.IFile.0=native"}),
        ("invalid", {"SWR_NATIVE_TRACE": "yes"}),
    ]
    for index, (name, settings) in enumerate(cases):
        run([dispatch, name], env=env | settings, cwd=out, log=out / f"test-dispatch-{index}.log", timeout=10)
    report = {"host": platform.platform(), "target": args.target, "sanitizers": args.sanitize,
              "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
              "cases": evidence, "dispatch_passed": len(cases)}
    (out / "host-results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Host contracts passed ({len(evidence)} host cases, {len(cases)} dispatch cases).", flush=True)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def input_tree(folder):
    return {p.relative_to(folder).as_posix(): digest(p)
            for p in sorted((folder / "fixture_data").rglob("*")) if p.is_file()}


def check_guest(log, name, wall_before, wall_after):
    output = log.read_text(encoding="utf-8", errors="replace")
    if output.count("[game] NHA contract passed") != 1 or "[game] NHA contract failed" in output:
        raise ValueError(output)
    samples = re.findall(r"^\[game\] NHA2((?: [0-9a-f]{16})+)$", output, re.MULTILINE)
    if len(samples) != 1 or len(samples[0].split()) != len(GUEST_FIELDS):
        raise ValueError(f"Missing or malformed guest observations in {log}")
    observed = dict(zip(GUEST_FIELDS, (int(word, 16) for word in samples[0].split())))
    expected = GUEST_FIXED | {"duplicate_start_rc": 0xFA01 if name in {"native", "mixed"} else 0}
    for field, value in expected.items():
        if observed[field] != value:
            raise ValueError(f"{name} {field}: got {observed[field]:#x}, expected {value:#x}")
    if observed["system_clock_delta"] >= 2**63:
        observed["system_clock_delta"] -= 2**64
    # 1 ms requested. Allow host scheduling latency up to the fixture's 10 s limit.
    if not (19_200 <= observed["sleep_ticks"] <= 192_000_000
            and 0 <= observed["cntvct_delta"] <= 192_000_000
            and min(wall_before, wall_after) - 1 <= observed["unix_seconds"] <= max(wall_before, wall_after) + 1
            and -5 <= observed["system_clock_delta"] <= 5):
        raise ValueError(f"Clock observations outside contract: {name} {observed}")
    return observed


def check_trace(log, expected_native):
    begin, end, summary, actual_native = {}, {}, None, set()
    trace_digest = hashlib.sha256()
    protocol_fields = {"schema_version", "call_id", "phase", "api_id", "route", "reason",
                       "guest_thread", "guest_pc", "outcome", "guest_result", "host_error", "duration_ns"}
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith('{"schema_version":'):
            continue
        trace_digest.update((line.rstrip() + "\n").encode("utf-8"))
        record = json.loads(line)
        if record["schema_version"] != 1 or summary is not None:
            raise ValueError(f"Unexpected trace record in {log}")
        phase = record["phase"]
        if phase == "trace_summary":
            summary = record
            continue
        if not protocol_fields <= record.keys():
            raise ValueError(f"Missing trace fields in {log}")
        call_id = record["call_id"]
        if phase == "begin" and call_id not in begin:
            begin[call_id] = record
        elif phase == "end" and call_id not in end and call_id in begin:
            end[call_id] = record
            expected_route = "native" if record["api_id"] in expected_native else "legacy"
            if record["route"] != expected_route:
                raise ValueError(f"Invalid route for {record['api_id']} in {log}: {record['route']}")
            if record["route"] == "native":
                actual_native.add(record["api_id"])
        else:
            raise ValueError(f"Invalid trace sequence for call {call_id} in {log}")
    if (not summary or not begin or begin.keys() != end.keys() or summary["calls"] != len(end)
            or any(summary[k] for k in ("incomplete_calls", "dropped_records", "truncated_ids"))):
        raise ValueError(f"Incomplete trace in {log}")
    for call_id, last in end.items():
        for key in ("api_id", "route", "reason", "guest_thread", "guest_pc"):
            if last[key] != begin[call_id][key]:
                raise ValueError(f"Mismatched trace pair for {call_id} in {log}")
        if last["outcome"] != "terminated" and last["api_id"] in {"svc.0x0a", "svc.0x07"}:
            raise ValueError(f"Missing termination record in {log}")
    if actual_native != expected_native:
        raise ValueError(f"Missing routes {expected_native - actual_native}, extra routes {actual_native - expected_native} in {log}")
    if any("one.txt" in json.dumps(r) or "hello" in json.dumps(r) for r in end.values()):
        raise ValueError(f"Guest file data leaked into {log}")
    return end, {"calls": len(end), "native_apis": sorted(actual_native),
                 "trace_records_sha256": trace_digest.hexdigest(),
                 "api_calls": dict(sorted(Counter(r["api_id"] for r in end.values()).items()))}


def check_host_log(case, log, target):
    counters = {}
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("NHA_TEST "):
            counters.update(json.loads(line[len("NHA_TEST "):]))
    if not counters:
        raise ValueError(f"Missing resource and test counts in {log}")
    if case == "fs":
        return {"counters": counters}

    sync = {f"svc.0x{number:02x}" for number in (0x11, 0x12, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x45)}
    expected_native = {
        "ipc_file": {api for api in NATIVE_GUEST_APIS if api.startswith("ipc.fsp-srv.")},
        "thread": {"svc.0x08", "svc.0x09", "svc.0x18"},
        "time": {"svc.0x0b", "svc.0x1e", "ipc.time.time.300", "svc.0x18"},
        "sync": sync, "sync-mixed": sync - {"svc.0x1b"},
    }[case]
    records, stats = check_trace(log, expected_native)
    faults = [r for r in records.values() if r["host_error"] is not None]
    observed = Counter()
    for record in faults:
        api, error, result = record["api_id"], record["host_error"], record["guest_result"]
        if record["route"] != "native" or record["outcome"] != "error" or result.get("transport_rc", 0):
            raise ValueError(f"Bad fault trace in {log}: {record}")
        observed[(api, error["domain"], error["code"], result.get("service_rc", result.get("x0")))] += 1

    expected = []
    if case == "ipc_file":
        domain = "win32" if target == "win64" else "errno"
        expected = [("ipc.fsp-srv.IFileSystem.8", domain, counters["access_error"], (6400 << 9) | 2)]
        expected += [(f"ipc.fsp-srv.IFile.{cmd}", domain, counters["io_error"], (5000 << 9) | 2)
                     for cmd in ("0", "4", "close")]
    if case == "thread":
        expected = [("svc.0x09", "errno", counters["start_errno"], 0xD001)]
    if case == "time":
        expected = [("ipc.time.time.300", "errno", counters["wall_errno"], (200 << 9) | 116)]
        waits = [r for r in records.values() if r["api_id"] == "svc.0x18"]
        if (len(waits) != 1 or waits[0]["guest_result"] != {"kind": "rc", "x0": 0xEA01, "x1": 0}
                or waits[0]["duration_ns"] != counters["host_wait_ns"] or waits[0]["outcome"] != "error"):
            raise ValueError(f"Invalid injected timeout trace in {log}: {waits}")
        if target == "win64" and counters.get("wait_conversion_cases") != 6:
            raise ValueError(f"Missing Win32 wait conversion checks in {log}")
    if observed != Counter(expected):
        raise ValueError(f"Wrong host fault records in {log}: {observed} != {Counter(expected)}")
    return stats | {"counters": counters, "host_faults": faults}


def check_guest_trace(records, observed):
    results = {}
    for record in records.values():
        results.setdefault(record["api_id"], []).append(record)
    closes = results["ipc.fsp-srv.IFile.close"]
    if len(closes) != 1 or closes[0]["guest_result"] != {"kind": "resource_close", "service_rc": 0}:
        raise ValueError("Domain IFile must release its resource exactly once")
    handles = [r["guest_result"]["x0"] for r in results["svc.0x16"]]
    if Counter(handles) != {0: observed["successful_closes"], observed["closed_thread_rc"]: 1}:
        raise ValueError(f"Unexpected handle close results: {handles}")
    starts = [r["guest_result"]["x0"] for r in results["svc.0x09"]]
    if starts != [0, observed["duplicate_start_rc"]]:
        raise ValueError(f"Unexpected thread start results: {starts}")
    if len(results["svc.0x0a"]) != 1:
        raise ValueError("Guest worker must exit exactly once")
    for api in ("ipc.time.time.300", "ipc.fsp-srv.IFileSystem.8", "ipc.fsp-srv.IFile.0", "ipc.fsp-srv.IFile.4"):
        replies = [r["guest_result"] for r in results[api]]
        if replies != [{"kind": "ipc", "transport_rc": 0, "service_rc": 0}]:
            raise ValueError(f"Unexpected IPC result for {api}: {replies}")


def guest_contract(args, out, env):
    fixture = out / "guest.nro"
    make_guest(fixture, args.assembler)
    shutil.copyfile(HERE / "guest.S", out / "guest.S")
    assembler_command = [args.assembler]
    if "clang" in Path(args.assembler).name:
        assembler_command += ["--target=aarch64-none-elf", "-c"]
    assembler_command += ["-o", "guest.o", str(HERE / "guest.S")]
    baseline = out / "original"
    exe = "fixture.exe" if args.target == "win64" else "fixture"
    module = "fixture_code.dll" if args.target == "win64" else "fixture_code.so"
    flags = ["--target", args.target, "--backend", "headless", "--cc", args.cc]
    archive = subprocess.check_output(["git", "-c", f"safe.directory={ROOT.as_posix()}", "archive", BASE],
                                      cwd=ROOT, env=env)
    with tempfile.TemporaryDirectory(prefix="swr-nha-baseline-") as base_dir:
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            # This archive comes from the pinned commit in the local repo.
            tar.extractall(base_dir)
        cli = Path(base_dir) / "switchrecomp.py"
        build_cmd = [sys.executable, str(cli), "--build-runtime", *flags]
        guest_cmd = [sys.executable, str(cli), str(fixture), *flags, "--jobs", "2",
                     "--outdir", str(baseline), "-o", "fixture"]
        run(build_cmd, env=env, log=out / "build-baseline-runtime.log", timeout=300)
        run(guest_cmd, env=env, log=out / "build-baseline-module.log", timeout=300)
    sd = baseline / "fixture_data" / "sdmc"
    sd.mkdir()
    (sd / "one.txt").write_bytes(b"hello\n")
    initial_files = input_tree(baseline)
    runtime_sha = {"baseline": digest(baseline / exe), "current": digest(args.runtime)}
    manifest = {
        "authoring": "Original SDK-free AArch64 fixture, under the repository license",
        "baseline": BASE,
        "seed": FIXTURE_SEED,
        "host": platform.platform(),
        "target": args.target,
        "backend": "headless",
        "cc": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
        "assembler_command": assembler_command,
        "runtime_command": build_cmd,
        "module_command": guest_cmd,
        "source_sha256": digest(out / "guest.S"),
        "nro_sha256": digest(fixture),
        "module": module,
        "module_sha256": digest(baseline / module),
        "ini_sha256": digest(baseline / "fixture.ini"),
        "runtime_sha256": runtime_sha,
        "initial_files": initial_files,
        "run_results": {},
    }
    if args.runtime_commit:
        manifest["runtime_commit"] = args.runtime_commit
    if not args.compile_only:
        native_env = dict.fromkeys(
            ["SWR_NATIVE_HOS", "SWR_NATIVE_TIME", "SWR_NATIVE_FS", "SWR_NATIVE_THREAD", "SWR_NATIVE_SYNC"], "1")
        scenarios = {
            "baseline": ({}, set()), "legacy": ({}, set()), "native": (native_env, NATIVE_GUEST_APIS),
            "mixed": (native_env | {"SWR_NATIVE_APIS": ",".join(f"{api}=legacy" for api in sorted(MIXED_LEGACY))},
                      NATIVE_GUEST_APIS - MIXED_LEGACY),
        }
        for name, (settings, expected) in scenarios.items():
            folder = out / name
            folder.mkdir()
            shutil.copytree(baseline / "fixture_data", folder / "fixture_data")
            shutil.copyfile(baseline / "fixture.ini", folder / "fixture.ini")
            shutil.copyfile(baseline / module, folder / module)
            executable = folder / exe
            shutil.copyfile(baseline / exe if name == "baseline" else args.runtime, executable)
            if os.name != "nt":
                executable.chmod(0o755)
            log = out / f"guest-{name}.log"
            settings = settings if name == "baseline" else settings | {"SWR_NATIVE_TRACE": "1"}
            expected_runtime = runtime_sha["baseline" if name == "baseline" else "current"]
            if input_tree(folder) != initial_files or digest(folder / "fixture.ini") != manifest["ini_sha256"]:
                raise ValueError(f"Guest inputs differ before {name}")
            if digest(folder / module) != manifest["module_sha256"] or digest(executable) != expected_runtime:
                raise ValueError(f"Guest module or Runtime differs before {name}")
            wall_before = int(time.time())
            run([executable, "--seconds=10"], env=env | settings, cwd=folder, log=log, timeout=20)
            wall_after = int(time.time())
            observed = check_guest(log, name, wall_before, wall_after)
            if digest(folder / module) != manifest["module_sha256"]:
                raise ValueError(f"Guest module changed during {name}")
            if input_tree(folder) != initial_files:
                raise ValueError(f"Guest input changed during {name}")
            result = {"exit_code": 0, "environment": settings, "runtime_sha256": expected_runtime,
                      "guest": observed, "host_wall_seconds": [wall_before, wall_after]}
            if name != "baseline":
                records, stats = check_trace(log, expected)
                check_guest_trace(records, observed)
                result.update(stats)
            manifest["run_results"][name] = result
    (out / "fixture-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"ABI 1 evidence saved to {out}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="new directory for binaries and evidence")
    parser.add_argument("--target", choices=["linux", "win64"], default="win64" if os.name == "nt" else "linux")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--assembler", default="clang" if os.name == "nt" else "aarch64-linux-gnu-as")
    parser.add_argument("--runtime", type=Path, help="path to the current target's headless Runtime")
    parser.add_argument("--runtime-commit", help="Git revision used to build the supplied Runtime")
    parser.add_argument("--sanitize", action="store_true", help="enable Linux ASan and UBSan in host tests")
    parser.add_argument("--compile-only", action="store_true", help="prepare the target binaries without running them")
    args = parser.parse_args()
    if args.sanitize and args.target != "linux":
        parser.error("--sanitize requires the Linux target")
    if args.runtime_commit:
        if not args.runtime:
            parser.error("--runtime-commit requires --runtime")
        revision = subprocess.run(
            ["git", "-c", f"safe.directory={ROOT.as_posix()}", "rev-parse", "--verify", "--end-of-options",
             f"{args.runtime_commit}^{{commit}}"], cwd=ROOT, text=True, capture_output=True)
        if revision.returncode:
            parser.error("--runtime-commit must resolve to a local Git commit")
        args.runtime_commit = revision.stdout.strip()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = clean_env() | {"SWR_NO_SNXVK": "1"}
    if args.sanitize:
        env |= {"ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1"}
    host_contracts(args, out, env)
    if args.runtime:
        guest_contract(args, out, env)


if __name__ == "__main__":
    main()
