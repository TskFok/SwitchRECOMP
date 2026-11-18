# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Build and run NHA Phase 1 host contracts and the original ABI 1 fixture."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile

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


def clean_env():
    return {k: v for k, v in os.environ.items() if not k.upper().startswith("SWR_NATIVE_")}


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
        "time": ["plat_ns", "plat_unix_time", "plat_sleep_ns", "plat_yield"],
        "thread": ["plat_thread_try"],
    }
    for name in ("dispatch", "time", "fs", "ipc_file", "thread", "sync"):
        dest = out / f"test_{name}{exe}"
        sources = ["runtime/native_hos.c"] if name == "dispatch" else linked_runtime
        run([args.cc, *compile_flags, f"tests/nha/test_{name}.c", *sources,
             *(f"-Wl,--wrap={symbol}" for symbol in wraps.get(name, ())),
             *system_libs, "-o", dest], env=env, log=out / f"build-test-{name}.log")
        if args.compile_only or name == "dispatch":
            continue
        with tempfile.TemporaryDirectory(prefix="swr-nha-") as temp:
            settings = {"SWR_NATIVE_HOS": "1", "SWR_NATIVE_FS": "1"} if name == "ipc_file" else {}
            run([dest, temp] if name == "fs" else [dest], env=env | settings, cwd=temp,
                log=out / f"test-{name}.log", timeout=20)
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
        ("invalid", {"SWR_NATIVE_APIS": "svc.0x1e=native,svc.0x1e=legacy"}),
        ("invalid", {"SWR_NATIVE_APIS": "unknown=native"}),
        ("invalid", {"SWR_NATIVE_APIS": "svc.0x1c=native"}),
        ("invalid", {"SWR_NATIVE_APIS": "svc.0x1e=native,"}),
        ("invalid", {"SWR_NATIVE_HOS": "1", "SWR_NATIVE_APIS": "ipc.fsp-srv.IFile.0=native"}),
        ("invalid", {"SWR_NATIVE_TRACE": "yes"}),
    ]
    for index, (name, settings) in enumerate(cases):
        run([dispatch, name], env=env | settings, cwd=out, log=out / f"test-dispatch-{index}.log", timeout=10)
    print(f"Host contracts passed (5 binaries, {len(cases)} dispatch cases).", flush=True)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_trace(log, expected_native):
    begin, end, summary, actual_native = {}, {}, None, set()
    protocol_fields = {"schema_version", "call_id", "phase", "api_id", "route", "reason",
                       "guest_thread", "guest_pc", "outcome", "guest_result", "host_error", "duration_ns"}
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith('{"schema_version":'):
            continue
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
    return {"calls": len(end), "native_apis": sorted(actual_native)}


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
    initial_files = {p.relative_to(baseline).as_posix(): digest(p)
                     for p in sorted((baseline / "fixture_data").rglob("*")) if p.is_file()}
    manifest = {
        "authoring": "Original SDK-free AArch64 fixture, under the repository license",
        "baseline": BASE,
        "host": platform.platform(),
        "target": args.target,
        "cc": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
        "assembler_command": assembler_command,
        "runtime_command": build_cmd,
        "module_command": guest_cmd,
        "source_sha256": digest(out / "guest.S"),
        "nro_sha256": digest(fixture),
        "module": module,
        "module_sha256": digest(baseline / module),
        "initial_files": initial_files,
        "run_results": {},
    }
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
            run([executable, "--seconds=10"], env=env | settings, cwd=folder, log=log, timeout=20)
            output = log.read_text(encoding="utf-8", errors="replace")
            if output.count("[game] NHA contract passed") != 1 or "[game] NHA contract failed" in output:
                raise ValueError(output)
            if digest(folder / module) != manifest["module_sha256"]:
                raise ValueError(f"Guest module changed during {name}")
            if any(digest(folder / f) != sha for f, sha in initial_files.items()):
                raise ValueError(f"Guest input changed during {name}")
            manifest["run_results"][name] = {"exit_code": 0, "environment": settings}
            if name != "baseline":
                manifest["run_results"][name].update(check_trace(log, expected))
    (out / "fixture-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"ABI 1 evidence saved to {out}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="new directory for binaries and evidence")
    parser.add_argument("--target", choices=["linux", "win64"], default="win64" if os.name == "nt" else "linux")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--assembler", default="clang" if os.name == "nt" else "aarch64-linux-gnu-as")
    parser.add_argument("--runtime", type=Path, help="path to the current target's headless Runtime")
    parser.add_argument("--sanitize", action="store_true", help="enable Linux ASan and UBSan in host tests")
    parser.add_argument("--compile-only", action="store_true", help="prepare the target binaries without running them")
    args = parser.parse_args()
    if args.sanitize and args.target != "linux":
        parser.error("--sanitize requires the Linux target")
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
