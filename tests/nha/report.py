# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
"""Compare versioned NHA traces with the static interface inventory."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import sys


DEFAULT_INTERFACES = Path(__file__).resolve().parents[2] / "docs" / "nha" / "interfaces.json"
TRACE_LINE = re.compile(r'^\s*(?:\[\s*\d+(?:\.\d+)?\]\s+)?(\{\s*"schema_version\b.*)')
IDENTITY = ("api_id", "route", "reason", "guest_thread", "guest_pc")
SUMMARY_FIELDS = ("calls", "incomplete_calls", "dropped_records", "truncated_ids")
RESULT_FIELDS = {
    "rc": ("x0",), "value": ("x0",), "void": (),
    "ipc": ("transport_rc", "service_rc"), "resource_close": ("service_rc",),
}
OUTCOMES = {"ok", "error", "terminated", "legacy_stub", "unimplemented"}


def load_json(path):
    with path.open(encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def uint(value):
    return type(value) is int and 0 <= value < 2**64


def valid_event(event):
    if not all(uint(event.get(k)) for k in ("call_id", "guest_thread", "guest_pc", "duration_ns")):
        return False
    if not event["call_id"] or event.get("route") not in ("legacy", "native"):
        return False
    if not all(isinstance(event.get(k), str) and event[k] for k in ("api_id", "reason")):
        return False
    if not {"guest_result", "host_error"} <= event.keys():
        return False
    error = event["host_error"]
    if error is not None and not (isinstance(error, dict) and error.get("domain") in ("win32", "errno")
                                  and uint(error.get("code"))):
        return False
    result = event["guest_result"]
    outcome = event.get("outcome")
    if event.get("phase") == "begin":
        return outcome == "pending" and result is None and error is None
    if event.get("phase") != "end" or not isinstance(outcome, str) or outcome not in OUTCOMES:
        return False
    if outcome == "terminated":
        return result is None
    if not isinstance(result, dict) or not isinstance(result.get("kind"), str):
        return False
    required = RESULT_FIELDS.get(result["kind"])
    return required is not None and all(uint(result.get(k)) for k in required)


def missing_ranges(seen, expected):
    ranges = []
    previous = 0
    for call_id in sorted(seen):
        if call_id > previous + 1:
            ranges.append([previous + 1, call_id - 1])
        previous = call_id
    if previous < expected:
        ranges.append([previous + 1, expected])
    return ranges


def nested_relations(pairs):
    # Logger order defines nesting on a single guest thread. Thread id 0
    # means no guest-thread attribution, so it cannot establish an edge.
    events = []
    for start, end, call in pairs:
        if call["guest_thread"]:
            events.extend(((start, "begin", call), (end, "end", call)))
    stacks = defaultdict(list)
    edges = Counter()
    for line, phase, call in sorted(events):
        stack = stacks[call["guest_thread"]]
        if phase == "begin":
            if stack:
                edges[(stack[-1]["api_id"], call["api_id"])] += 1
            stack.append(call)
        elif stack[-1]["call_id"] == call["call_id"]:
            stack.pop()
        else:
            return [], line
    return [{"caller": a, "callee": b, "calls": n} for (a, b), n in sorted(edges.items())], None


def read_trace(path, run):
    begins, ends = {}, {}
    duplicates, truncated = set(), set()
    invalid_lines = []
    summary = None
    digest = hashlib.sha256()
    records_digest = hashlib.sha256()
    with path.open("rb") as stream:
        for line_no, raw in enumerate(stream, 1):
            digest.update(raw)
            match = TRACE_LINE.match(raw.decode("utf-8", errors="replace"))
            if not match:
                continue
            payload = match[1].rstrip()
            records_digest.update((payload + "\n").encode("utf-8"))
            try:
                event = json.loads(payload)
            except ValueError:
                event = {}
            if (not isinstance(event, dict) or type(event.get("schema_version")) is not int
                    or event["schema_version"] != 1 or summary is not None):
                invalid_lines.append(line_no)
            elif event.get("phase") == "trace_summary" and all(uint(event.get(k)) for k in SUMMARY_FIELDS):
                summary = {k: event[k] for k in SUMMARY_FIELDS}
            elif valid_event(event):
                call_id = event["call_id"]
                events = begins if event["phase"] == "begin" else ends
                if call_id in events:
                    duplicates.add(call_id)
                else:
                    events[call_id] = (line_no, event)
                if event["reason"] == "api_id_truncated":
                    truncated.add(call_id)
            else:
                invalid_lines.append(line_no)

    sha256 = digest.hexdigest()
    missing_ends = sorted(begins.keys() - ends.keys())
    missing_begins = sorted(ends.keys() - begins.keys())
    seen = begins.keys() | ends.keys()
    expected = summary["calls"] if summary else 0
    gaps = missing_ranges(seen, expected)
    mismatches, pairs = [], []
    for call_id in sorted((begins.keys() & ends.keys()) - duplicates):
        begin_line, begin = begins[call_id]
        end_line, end = ends[call_id]
        if end_line < begin_line or any(begin[k] != end[k] for k in IDENTITY):
            mismatches.append(call_id)
        elif call_id not in truncated:
            pairs.append((begin_line, end_line, end))

    issues = []
    if summary is None:
        issues.append("missing_summary")
    elif (expected != len(seen) or any(i > expected for i in seen)
          or any(summary[k] for k in SUMMARY_FIELDS[1:])):
        issues.append("summary_reports_loss_or_count_mismatch")
    if "calls" in run and run["calls"] != len(pairs):
        issues.append("manifest_count_mismatch")
    if "api_calls" in run and run["api_calls"] != Counter(end["api_id"] for _, _, end in pairs):
        issues.append("manifest_api_counts_mismatch")
    if "native_apis" in run and set(run["native_apis"]) != {
        end["api_id"] for _, _, end in pairs if end["route"] == "native"
    }:
        issues.append("manifest_native_apis_mismatch")
    if ("trace_records_sha256" in run
            and run["trace_records_sha256"] != records_digest.hexdigest()):
        issues.append("manifest_hash_mismatch")
    if run.get("exit_code") != 0:
        issues.append("sample_exit_unverified")
    edges = []
    degraded = any((issues, invalid_lines, gaps, missing_begins, missing_ends,
                    duplicates, mismatches, truncated))
    if not degraded:
        edges, crossing = nested_relations(pairs)
        if crossing is not None:
            issues.append("crossing_call_intervals")
            invalid_lines.append(crossing)
            degraded = True
    return {
        "status": "degraded" if degraded else "complete", "file": path.name, "sha256": sha256,
        "records_sha256": records_digest.hexdigest(),
        "complete_calls": len(pairs), "summary": summary, "issues": issues,
        "invalid_lines": invalid_lines, "missing_call_id_ranges": gaps,
        "missing_begin_ids": missing_begins, "missing_end_ids": missing_ends,
        "duplicate_call_ids": sorted(duplicates), "mismatched_call_ids": mismatches,
        "truncated_call_ids": sorted(truncated),
    }, edges, [end for _, _, end in pairs]


def result_counts(counter):
    return [{"count": count, **json.loads(key)} for key, count in sorted(counter.items())]


def summarize_calls(records, interfaces, modules):
    by_api = defaultdict(lambda: defaultdict(list))
    for record in records:
        by_api[record["api_id"]][record["route"]].append(record)
    calls, native, legacy = {}, defaultdict(list), {}
    unknown, placeholders, unclassified_native = [], [], []
    for api_id, routes in sorted(by_api.items()):
        interface = interfaces.get(api_id)
        if interface is None:
            unknown.append(api_id)
        if any(r["outcome"] == "legacy_stub" for r in routes.get("legacy", [])) or (
                interface and interface["implementation_status"] == "legacy_stub" and "legacy" in routes):
            placeholders.append(api_id)
        calls[api_id] = {"count": sum(map(len, routes.values())), "routes": {}}
        for route, entries in sorted(routes.items()):
            results, errors = Counter(), Counter()
            for entry in entries:
                result = entry["guest_result"]
                if result is not None:
                    # Retain protocol status and result kind. Values, returned
                    # handles and waiter indices can vary on every call.
                    result = {k: v for k, v in result.items()
                              if k in {"kind", "service_rc", "transport_rc"} or
                              (k == "x0" and result["kind"] == "rc")}
                    results[json.dumps(result, sort_keys=True)] += 1
                error = entry["host_error"]
                if error is not None:
                    errors[json.dumps({"domain": error["domain"], "code": error["code"]}, sort_keys=True)] += 1
            calls[api_id]["routes"][route] = {
                "count": len(entries), "outcomes": dict(Counter(r["outcome"] for r in entries)),
                "reasons": dict(Counter(r["reason"] for r in entries)),
                "guest_results": result_counts(results), "host_errors": result_counts(errors),
            }
            if route == "legacy":
                legacy[api_id] = {"calls": len(entries), "sources": interface["sources"] if interface else []}
            elif interface and interface["native_module"] in modules:
                native[interface["native_module"]].append(api_id)
            else:
                unclassified_native.append(api_id)
    needed = {name: {"api_ids": api_ids, **modules[name]} for name, api_ids in sorted(native.items())}
    return calls, needed, legacy, unknown, placeholders, unclassified_native


def run_report(path, expected, run, interfaces, modules):
    if path is not None:
        health, edges, records = read_trace(path, run)
    else:
        health = {"status": "missing" if expected else "not_collected", "complete_calls": 0, "summary": None}
        edges, records = [], []
    calls, native, legacy, unknown, placeholders, unclassified = summarize_calls(records, interfaces, modules)
    coverage = {"uncalled": [], "uncovered": [], "insufficient_evidence": []}
    state = health["status"]
    for api_id, entry in sorted(interfaces.items()):
        if api_id in calls:
            continue
        if not entry["traceable"] or state in {"missing", "not_collected"}:
            coverage["uncovered"].append(api_id)
        elif state == "complete":
            coverage["uncalled"].append(api_id)
        else:
            coverage["insufficient_evidence"].append(api_id)
    return {
        "exit_code": run.get("exit_code"), "environment": run.get("environment", {}),
        "runtime_sha256": run.get("runtime_sha256"), "trace_health": health, "calls": calls,
        "observed_relations": edges, "coverage": coverage,
        "unknown_operations": unknown, "observed_placeholders": placeholders,
        "unimplemented_operations": [
            api_id for api_id, data in calls.items()
            if (interfaces.get(api_id, {}).get("implementation_status") == "unimplemented"
                or any("unimplemented" in route["outcomes"] for route in data["routes"].values()))
        ],
        "native_modules": native, "unclassified_native": unclassified, "legacy_dependencies": legacy,
    }


def build_report(inventory_path, manifest_path, trace_paths=None):
    inventory = load_json(inventory_path)
    manifest = load_json(manifest_path)
    if type(inventory.get("schema_version")) is not int or inventory["schema_version"] != 1:
        raise ValueError("unsupported interface inventory schema")
    interfaces = {}
    for entry in inventory["interfaces"]:
        api_id = entry["api_id"]
        if api_id in interfaces:
            raise ValueError(f"duplicate inventory api_id: {api_id}")
        interfaces[api_id] = entry

    trace_paths = trace_paths or {}
    declared = manifest["run_results"]
    if not isinstance(declared, dict) or not declared:
        raise ValueError("manifest has no sample runs")
    if extra := trace_paths.keys() - declared.keys():
        raise ValueError(f"trace run absent from manifest: {', '.join(sorted(extra))}")
    runs = {}
    for name, run in sorted(declared.items()):
        if not re.fullmatch(r"[a-zA-Z0-9_-]+", name):
            raise ValueError(f"invalid run name: {name!r}")
        if not isinstance(run, dict) or not isinstance(run.get("environment", {}), dict):
            raise ValueError(f"invalid run in manifest: {name}")
        expected = run.get("environment", {}).get("SWR_NATIVE_TRACE") == "1"
        automatic = manifest_path.parent / f"guest-{name}.log"
        path = trace_paths.get(name)
        if path is None and expected and automatic.is_file():
            path = automatic
        runs[name] = run_report(path, expected, run, interfaces, inventory["native_modules"])

    static_evidence_gaps = sorted(api_id for api_id, row in interfaces.items()
                                  if row["test_status"] == "insufficient_evidence")
    candidates = []
    for candidate in inventory["pruning_candidates"]:
        evidence = {}
        for name, run in runs.items():
            coverage = {api: status for status, entries in run["coverage"].items() for api in entries}
            evidence[name] = {
                api_id: {"status": "observed", "calls": run["calls"][api_id]["count"]}
                if api_id in run["calls"] else {"status": coverage[api_id]}
                for api_id in candidate["api_ids"]
            }
        candidates.append({**candidate, "sample_evidence": evidence})

    return {
        "schema_version": 1,
        "inventory": {
            "schema_version": 1, "audited_commit": inventory["audited_commit"],
            "sha256": hashlib.sha256(inventory_path.read_bytes()).hexdigest(),
            "api_count": len(interfaces), "registered_service_count": len(inventory["registered_services"]),
        },
        "sample": {k: manifest[k] for k in (
            "authoring", "baseline", "runtime_commit", "seed", "host", "target", "backend", "cc",
            "source_sha256", "nro_sha256", "module", "module_sha256", "ini_sha256", "initial_files",
        ) if k in manifest},
        "runs": runs,
        "static_relations": {api_id: row["relations"] for api_id, row in sorted(interfaces.items())
                             if row["relations"]},
        "static_placeholders": sorted(api_id for api_id, row in interfaces.items()
                                      if row["implementation_status"] == "legacy_stub"),
        "static_evidence_gaps": static_evidence_gaps,
        "dynamic_dependencies": inventory["dynamic_dependencies"],
        "pruning_candidates": candidates,
        "license_review": inventory["license_review"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interfaces", type=Path, default=DEFAULT_INTERFACES)
    parser.add_argument("--manifest", required=True, type=Path, help="fixture-manifest.json from run.py")
    parser.add_argument("--trace", action="append", metavar="RUN=PATH", default=[],
                        help="override guest-RUN.log for a run listed in the manifest")
    parser.add_argument("--output", type=Path, help="JSON output file (default: stdout)")
    args = parser.parse_args()
    trace_paths = {}
    for spec in args.trace:
        name, sep, path = spec.partition("=")
        if not sep or not name or not path or name in trace_paths:
            parser.error(f"expected one RUN=PATH per run: {spec!r}")
        trace_paths[name] = Path(path)
    try:
        report = build_report(args.interfaces, args.manifest, trace_paths)
        data = (json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")
        if args.output:
            args.output.write_bytes(data)
        else:
            sys.stdout.buffer.write(data)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
