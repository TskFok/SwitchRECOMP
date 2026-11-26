# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Compare a saved Phase 1 ABI 1 fixture across Runtime routes on one backend."""

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import time

from run import BASE, NATIVE_GUEST_APIS, clean_env, digest, input_tree, check_trace


def scenarios():
    yield "baseline", {}, set()
    yield "legacy-untraced", {}, set()
    yield "legacy", {"SWR_NATIVE_TRACE": "1"}, set()
    enabled = {"SWR_NATIVE_TRACE": "1", "SWR_NATIVE_HOS": "1"}
    groups = dict.fromkeys(("SWR_NATIVE_TIME", "SWR_NATIVE_FS",
                            "SWR_NATIVE_THREAD", "SWR_NATIVE_SYNC"), "1")
    yield "native", enabled | groups, NATIVE_GUEST_APIS
    files = {api for api in NATIVE_GUEST_APIS if api.startswith("ipc.fsp-srv.")}
    singles = [(api, {api}) for api in sorted(NATIVE_GUEST_APIS - files)]
    for name, apis in [*singles, ("file-lifetime", files)]:
        overrides = ",".join(f"{api}=native" for api in sorted(apis))
        yield f"only-{name.replace('.', '-')}", enabled | {"SWR_NATIVE_APIS": overrides}, apis


def trace_results(records):
    counts = Counter()
    for record in records.values():
        api = record["api_id"]
        result = dict(record["guest_result"] or {})
        # The guest checks tick order, and uses the returned handles through
        # waiting and closing. Compare their subsequent results across routes.
        if api == "svc.0x1e":
            result.pop("x0")
        if api in {"svc.0x08", "svc.0x45"}:
            result.pop("x1", None)
            result.pop("x2", None)
        counts[api, record["guest_pc"], record["outcome"],
               json.dumps(result, sort_keys=True), json.dumps(record["host_error"], sort_keys=True)] += 1
    return counts


def compare(args):
    source = args.saved / "original"
    saved = json.loads((args.saved / "fixture-manifest.json").read_text(encoding="utf-8"))
    windows = args.backend in {"win32", "win64-headless"}
    windowed = args.backend in {"x11", "win32"}
    exe, module = ("fixture.exe", "fixture_code.dll") if windows else ("fixture", "fixture_code.so")
    target = "win64" if windows else "linux"
    if (saved["baseline"] != BASE or saved["target"] != target or saved["module"] != module
            or saved["module_sha256"] != digest(source / module) or saved["initial_files"] != input_tree(source)):
        raise ValueError("Saved Phase 1 fixture does not match its manifest")

    ini_sha = digest(source / "fixture.ini")
    runtime_sha = {"baseline": digest(args.baseline_runtime), "current": digest(args.runtime)}
    report = {
        "baseline": BASE, "host": platform.platform(), "target": target,
        "backend": "headless" if args.backend == "win64-headless" else args.backend,
        "runtime_commit": args.runtime_commit, "cc": args.compiler,
        "launcher": args.launcher, "authoring": saved["authoring"],
        "source_sha256": saved["source_sha256"], "nro_sha256": saved["nro_sha256"],
        "module": module, "module_sha256": saved["module_sha256"],
        "initial_files": saved["initial_files"], "ini_sha256": ini_sha,
        "runtime_sha256": runtime_sha, "run_results": {},
    }
    if args.runtime_patch:
        report["runtime_patch_sha256"] = digest(args.runtime_patch)
    env = clean_env() | {"SWR_NO_SNXVK": "1"}
    report["host_settings"] = {key: env[key] for key in (
        "DISPLAY", "SDL_AUDIODRIVER", "LIBGL_ALWAYS_SOFTWARE", "MESA_SHADER_CACHE_DISABLE",
        "LP_NUM_THREADS", "SWR_NO_GLTHREAD", "mesa_glthread", "WINEDEBUG", "WINEDLLOVERRIDES",
    ) if key in env}
    out = args.output
    out.mkdir(parents=True, exist_ok=False)
    legacy_results = None
    for name, settings, expected in scenarios():
        settings = {"SWR_NO_SNXVK": "1"} | settings
        folder = out / name
        folder.mkdir()
        shutil.copytree(source / "fixture_data", folder / "fixture_data")
        shutil.copyfile(source / "fixture.ini", folder / "fixture.ini")
        shutil.copyfile(source / module, folder / module)
        baseline = name == "baseline"
        runtime = folder / exe
        shutil.copyfile(args.baseline_runtime if baseline else args.runtime, runtime)
        if os.name != "nt":
            runtime.chmod(0o755)
        expected_runtime = runtime_sha["baseline" if baseline else "current"]
        if digest(runtime) != expected_runtime or digest(folder / "fixture.ini") != ini_sha:
            raise ValueError(f"Runtime or settings changed while copying for {name}")
        executable = str(runtime.resolve()) if os.name == "nt" else f"./{exe}"
        # Check guest-driven process exit. A frontend timer could hide a hang
        # after the guest printed its success marker.
        command = [*args.launcher, executable]
        log = out / f"guest-{name}.log"
        result = {"environment": settings, "runtime_sha256": expected_runtime,
                  "command": command, "validation_errors": []}
        errors = result["validation_errors"]
        started = time.monotonic()
        with log.open("wb") as stream:
            try:
                result["exit_code"] = subprocess.run(
                    command, cwd=folder, env=env | settings, stdout=stream,
                    stderr=subprocess.STDOUT, timeout=30,
                ).returncode
            except (OSError, subprocess.TimeoutExpired) as error:
                result["exit_code"] = None
                errors.append(str(error))
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)
        output = log.read_text(encoding="utf-8", errors="replace")
        result["observed"] = {
            "guest_passes": output.count("[game] NHA contract passed"),
            "guest_failures": output.count("[game] NHA contract failed"),
            "opengl": re.findall(r"^OpenGL: (.+)$", output, re.MULTILINE),
            "audio": re.findall(r"^audio: (.+)$", output, re.MULTILINE),
            "frames": re.findall(r"^frames=(\d+) hash=(\w+).*$", output, re.MULTILINE),
        }
        if result["exit_code"] != 0:
            errors.append(f"Runtime exit status: {result['exit_code']}")
        if result["observed"]["guest_passes"] != 1 or result["observed"]["guest_failures"]:
            errors.append("Missing successful ABI 1 guest assertions")
        if bool(result["observed"]["opengl"]) != windowed:
            errors.append(f"Unexpected OpenGL startup evidence for {args.backend}")
        if re.search(r"^FATAL:|^\*\*\* Crash", output, flags=re.MULTILINE):
            errors.append("Runtime logged a crash or fatal error")
        if digest(folder / module) != saved["module_sha256"] or input_tree(folder) != saved["initial_files"]:
            errors.append("Guest module or inputs changed")

        if "SWR_NATIVE_TRACE" in settings:
            try:
                records, stats = check_trace(log, expected)
                result.update(stats)
                observed = trace_results(records)
                if name == "legacy":
                    legacy_results = observed
                elif legacy_results is None:
                    errors.append("No valid Legacy trace to compare")
                elif observed != legacy_results:
                    result["trace_diff"] = {
                        "added": [[*key, count] for key, count in sorted((observed - legacy_results).items())],
                        "removed": [[*key, count] for key, count in sorted((legacy_results - observed).items())],
                    }
                    errors.append("Guest call counts, outcomes or results differ from Legacy")
            except (KeyError, ValueError) as error:
                errors.append(str(error))
        elif '{"schema_version":' in output:
            errors.append("Unexpected trace with tracing disabled")
        report["run_results"][name] = result
        (out / "fixture-manifest.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"{name}: exit={result['exit_code']} {'; '.join(errors) if errors else 'PASS'}", flush=True)
    return int(any(run["validation_errors"] for run in report["run_results"].values()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--saved", required=True, type=Path, help="Phase 1 output containing original/ and its manifest")
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--baseline-runtime", required=True, type=Path, help="Phase 0 Runtime for the same backend")
    parser.add_argument("--runtime-commit", required=True, help="Git revision used for the Runtime build")
    parser.add_argument("--runtime-patch", type=Path, help="optional source patch applied to that revision")
    parser.add_argument("--compiler", required=True, help="compiler version used for the Runtime build")
    parser.add_argument("--backend", choices=["x11", "headless", "win32", "win64-headless"], required=True)
    parser.add_argument("--launcher", action="append", default=[], help="optional compatibility launcher, such as Wine")
    parser.add_argument("--output", required=True, type=Path, help="new directory for evidence")
    return compare(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
