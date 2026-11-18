# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
import json
from pathlib import Path
import re
import tempfile
import unittest

from report import DEFAULT_INTERFACES, build_report


PARENT = "svc.0x21"
FIRST = "ipc.nfp:user.IUser.0"
SECOND = "ipc.nfc:user.IUser.0"
UNKNOWN = "ipc.nfp:user.IUser.9999"
TICK = "rtapi.ticks"
NATIVE = "svc.0x1e"


def record(call_id, api_id, phase, *, thread=11, route="legacy", reason="unclassified",
           outcome="ok", result=None, host_error=None):
    return json.dumps({
        "schema_version": 1, "call_id": call_id, "phase": phase, "api_id": api_id,
        "route": route, "reason": reason, "guest_thread": thread, "guest_pc": 40,
        "outcome": outcome if phase == "end" else "pending",
        "guest_result": result if phase == "end" else None,
        "host_error": host_error, "duration_ns": 4 if phase == "end" else 0,
    })


def summary(calls, incomplete=0, dropped=0, truncated=0):
    return json.dumps({"schema_version": 1, "phase": "trace_summary", "calls": calls,
                       "incomplete_calls": incomplete, "dropped_records": dropped, "truncated_ids": truncated})


class ReportTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.path = Path(temp.name)
        self.manifest = self.path / "fixture-manifest.json"
        self.manifest.write_text(json.dumps({
            "module": "fixture_code.so", "module_sha256": "a" * 64, "target": "linux",
            "host": "test host", "baseline": "b" * 40, "runtime_commit": "c" * 40,
            "run_results": {"baseline": {"exit_code": 0, "environment": {}},
                            "native": {"exit_code": 0, "environment": {"SWR_NATIVE_TRACE": "1"}}},
        }), encoding="utf-8")
        self.log = self.path / "guest-native.log"

    def report(self, *lines):
        self.log.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return build_report(DEFAULT_INTERFACES, self.manifest)

    def test_complete_calls_keep_ipc_origins_and_result_layers(self):
        ok = {"kind": "ipc", "transport_rc": 0, "service_rc": 0}
        failed = {"kind": "ipc", "transport_rc": 0, "service_rc": 7}
        transport_failure = {"kind": "ipc", "transport_rc": 0xCC01, "service_rc": 0}
        report = self.report(
            "[game] unrelated log", record(1, PARENT, "begin"), record(2, FIRST, "begin"),
            record(3, NATIVE, "begin", thread=12, route="native"),
            record(2, FIRST, "end", result=ok), record(4, SECOND, "begin"),
            record(4, SECOND, "end", outcome="error", result=failed, host_error={"domain": "errno", "code": 5}),
            record(1, PARENT, "end", result={"kind": "rc", "x0": 0}),
            record(3, NATIVE, "end", thread=12, route="native", result={"kind": "value", "x0": 7}),
            record(5, UNKNOWN, "begin"), record(5, UNKNOWN, "end", outcome="legacy_stub", result=ok),
            record(6, FIRST, "begin"), record(6, FIRST, "end", outcome="error", result=transport_failure),
            "[  0.42] " + summary(6),
        )
        run = report["runs"]["native"]
        self.assertEqual(run["trace_health"]["status"], "complete")
        self.assertEqual(run["trace_health"]["complete_calls"], 6)
        self.assertEqual(run["calls"][FIRST]["routes"]["legacy"]["guest_results"], [
            {"count": 1, "kind": "ipc", "service_rc": 0, "transport_rc": 0},
            {"count": 1, "kind": "ipc", "service_rc": 0, "transport_rc": 0xCC01},
        ])
        second = run["calls"][SECOND]["routes"]["legacy"]
        self.assertEqual(second["guest_results"], [{"count": 1, **failed}])
        self.assertEqual(second["host_errors"], [{"count": 1, "domain": "errno", "code": 5}])
        self.assertEqual(run["observed_relations"], [
            {"caller": PARENT, "callee": api, "calls": 1} for api in (SECOND, FIRST)
        ])
        self.assertEqual(run["unknown_operations"], [UNKNOWN])
        self.assertEqual(run["observed_placeholders"], [UNKNOWN])
        self.assertEqual(run["native_modules"]["time"]["api_ids"], [NATIVE])
        self.assertIn(PARENT, run["legacy_dependencies"])
        self.assertIn(TICK, run["coverage"]["uncovered"])
        self.assertIn("svc.0x09", run["coverage"]["uncalled"])
        self.assertEqual(report["runs"]["baseline"]["trace_health"]["status"], "not_collected")
        self.assertEqual(report["sample"]["runtime_commit"], "c" * 40)
        self.assertTrue(report["pruning_candidates"])
        self.assertTrue(report["license_review"])

    def test_loss_and_truncation_do_not_assert_absence_or_count_truncated_ids(self):
        rc = {"kind": "rc", "x0": 0}
        run = self.report(
            record(1, PARENT, "begin"),
            record(3, PARENT, "end", result=rc),
            record(4, FIRST, "begin", reason="api_id_truncated"),
            record(4, FIRST, "end", reason="api_id_truncated", result=rc),
            '{"schema_version":1,"phase":"begin"',
            summary(5, incomplete=1, dropped=4, truncated=1),
        )["runs"]["native"]
        health = run["trace_health"]
        self.assertEqual(health["status"], "degraded")
        self.assertEqual(health["missing_call_id_ranges"], [[2, 2], [5, 5]])
        self.assertEqual(health["missing_end_ids"], [1])
        self.assertEqual(health["missing_begin_ids"], [3])
        self.assertEqual(health["invalid_lines"], [5])
        self.assertEqual(health["truncated_call_ids"], [4])
        self.assertEqual(health["summary"]["dropped_records"], 4)
        self.assertEqual(run["calls"], {})
        self.assertEqual(run["observed_relations"], [])
        self.assertEqual(run["coverage"]["uncalled"], [])
        self.assertIn(FIRST, run["coverage"]["insufficient_evidence"])

    def test_pairs_cannot_cross_summary_or_merge_mismatched_identity(self):
        rc = {"kind": "rc", "x0": 0}
        health = self.report(
            record(1, FIRST, "begin"), record(1, SECOND, "end", result=rc),
            record(2, FIRST, "begin"), summary(2),
            record(2, FIRST, "end", result=rc), summary(1),
        )["runs"]["native"]["trace_health"]
        self.assertEqual(health["status"], "degraded")
        self.assertEqual(health["mismatched_call_ids"], [1])
        self.assertEqual(health["missing_end_ids"], [2])
        self.assertEqual(health["invalid_lines"], [5, 6])
        self.assertEqual(health["complete_calls"], 0)

    def test_missing_and_empty_trace_are_uncovered(self):
        run = build_report(DEFAULT_INTERFACES, self.manifest)["runs"]["native"]
        self.assertEqual(run["trace_health"]["status"], "missing")
        self.assertIn(PARENT, run["coverage"]["uncovered"])
        run = self.report()["runs"]["native"]
        self.assertEqual(run["trace_health"]["status"], "degraded")
        self.assertIn(PARENT, run["coverage"]["insufficient_evidence"])

    def test_report_rejects_evidence_from_a_different_run(self):
        manifest = json.loads(self.manifest.read_text(encoding="utf-8"))
        manifest["run_results"]["native"].update(calls=2, trace_records_sha256="0" * 64)
        self.manifest.write_text(json.dumps(manifest), encoding="utf-8")
        rc = {"kind": "rc", "x0": 0}
        run = self.report(record(1, PARENT, "begin"), record(1, PARENT, "end", result=rc),
                          summary(1))["runs"]["native"]
        self.assertEqual(run["trace_health"]["status"], "degraded")
        self.assertEqual(run["trace_health"]["issues"], ["manifest_count_mismatch", "manifest_hash_mismatch"])
        self.assertEqual(run["calls"][PARENT]["count"], 1)
        self.assertEqual(run["coverage"]["uncalled"], [])

    def test_older_manifest_catches_a_swapped_route_with_the_same_total(self):
        manifest = json.loads(self.manifest.read_text(encoding="utf-8"))
        manifest["run_results"]["native"].update(calls=1, native_apis=[NATIVE], api_calls={NATIVE: 1})
        self.manifest.write_text(json.dumps(manifest), encoding="utf-8")
        run = self.report(record(1, NATIVE, "begin"),
                          record(1, NATIVE, "end", result={"kind": "value", "x0": 19}),
                          summary(1))["runs"]["native"]
        self.assertEqual(run["trace_health"]["status"], "degraded")
        self.assertEqual(run["trace_health"]["issues"], ["manifest_native_apis_mismatch"])

    def test_zero_call_summary_can_prove_absence(self):
        run = self.report(summary(0))["runs"]["native"]
        self.assertEqual(run["trace_health"]["status"], "complete")
        self.assertEqual(run["trace_health"]["complete_calls"], 0)
        self.assertIn(NATIVE, run["coverage"]["uncalled"])

    def test_duplicate_event_cannot_complete_a_call(self):
        start = record(1, PARENT, "begin")
        end = record(1, PARENT, "end", result={"kind": "rc", "x0": 0})
        run = self.report(start, start, end, summary(1))["runs"]["native"]
        self.assertEqual(run["trace_health"]["duplicate_call_ids"], [1])
        self.assertEqual(run["calls"], {})


class InventoryTest(unittest.TestCase):
    def test_explicit_svc_and_service_registrations_are_indexed(self):
        inventory = json.loads(DEFAULT_INTERFACES.read_text(encoding="utf-8"))
        root = DEFAULT_INTERFACES.parents[2]
        runtime = root / "runtime"
        registration_files = ("svc_core.c", "svc_fs.c", "svc_hid.c", "svc_vi.c", "nvdrv.c", "main.c")
        registry = {
            name for file in registration_files
            for name in re.findall(r'\bsvc_register\("([^"]+)"', (runtime / file).read_text(encoding="utf-8"))
        }
        self.assertEqual(set(inventory["registered_services"]), registry)
        kernel = (runtime / "kernel.c").read_text(encoding="utf-8").split("  switch(imm){")[-1]
        legacy = kernel.split("  default:\n    *status=NHA_UNIMPLEMENTED", 1)[0]
        numbers = {int(value, 16) for value in re.findall(r"\bcase 0x([0-9A-Fa-f]+):", legacy)}
        self.assertEqual({row["origin"]["number"] for row in inventory["interfaces"]
                          if row["origin"]["kind"] == "svc"}, numbers)

    def test_static_relations_resolve_and_trace_blind_spots_are_explicit(self):
        inventory = json.loads(DEFAULT_INTERFACES.read_text(encoding="utf-8"))
        rows = {row["api_id"]: row for row in inventory["interfaces"]}
        self.assertEqual(len(rows), len(inventory["interfaces"]))
        for api in ("rtapi.ticks", "ipc.control.domain_close", "nvdrv.ioctl.gpu.0x08",
                    "nvdrv.ioctl.nvdec.0x00.0x01", "vi.binder.7"):
            self.assertFalse(rows[api]["traceable"], api)
        for row in rows.values():
            self.assertTrue(all(relation["target"] in rows for relation in row["relations"]), row["api_id"])
        for candidate in inventory["pruning_candidates"]:
            self.assertTrue(all(api in rows for api in candidate["api_ids"]), candidate["symbol"])


if __name__ == "__main__":
    unittest.main()
