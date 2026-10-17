import stat
import tempfile
import unittest
import json
from datetime import datetime, timezone
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from unittest.mock import patch

import gpu_audio_p2_campaign as campaign

PINNED_DAWN = "b" * 40
PINNED_ASSET = "a" * 64
PLATFORM = "darwin-arm64"


def _write_pinned_manifest(path, dawn=PINNED_DAWN, asset=PINNED_ASSET):
    """A dependency manifest pinning one Skia asset and Dawn revision."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"dependencies": [{
        "name": "Skia",
        "determinism": {"built_dawn": dawn,
                        "release_assets": {"mac-arm64": {"sha256": asset}}},
    }]}) + "\n", encoding="utf-8")


class P2CampaignContractTests(unittest.TestCase):
    def test_host_preflight_requires_fresh_source_bound_quiet_receipt(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "host-preflight.json"
            source = "a" * 40
            build = Path(root) / "build"
            executable = build / "pulp"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"pulp-fixture")
            executable.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
            manifest = build / "core/runtime/generated/Release/pulp/runtime/build_info.hpp"
            manifest.parent.mkdir(parents=True)
            manifest.write_text("kGitSha = \"" + source + "\"\n", encoding="utf-8")
            receipt = {
                "schema": "pulp.gpu-audio.p2.host-preflight.v1",
                "status": "passed", "source_revision": source, "host_id": "m5-studio",
                "sampled_at": datetime.now(timezone.utc).isoformat(),
                "quiet_host": True, "host_vitals_level": "green",
                "gpu_contention": False, "ui_contention": False,
                "thermal_state": "nominal",
                "gpu_health_observation": {"status": "valid", "observation": {
                    "status": "valid", "schema": "pulp.gpu-health-result.v2",
                    "run_id": "fixture", "probe_id": "gpu-compute-magnitude",
                    "measured_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
                    "adapter": {"status": "authentic", "class": "hardware",
                                 "name": "Apple M5", "backend": "Metal", "device": "m5"},
                    "measurements": {"compute_initialized": True,
                                     "compute_oracle_passed": True, "device_lost": False},
                }},
                "gpu_health_source": {"status": "valid", "path": str(executable),
                                       "sha256": campaign.sha256(executable),
                                       "source_revision": source,
                                       "manifest_path": str(manifest),
                                       "manifest_sha256": campaign.sha256(manifest)},
            }
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with patch.object(campaign, "REPO_ROOT", Path(root)):
                receipt = campaign.validate_host_preflight(path, source)
            self.assertEqual(receipt["host_id"], "m5-studio")
            receipt["thermal_state"] = "serious"
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with patch.object(campaign, "REPO_ROOT", Path(root)):
                with self.assertRaisesRegex(RuntimeError, "thermal"):
                    campaign.validate_host_preflight(path, source)
            receipt["thermal_state"] = "nominal"
            receipt["gpu_health_observation"]["observation"]["measured_at_utc"] = "2000-01-01T00:00:00Z"
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with patch.object(campaign, "REPO_ROOT", Path(root)):
                with self.assertRaisesRegex(RuntimeError, "gpu_health_fresh"):
                    campaign.validate_host_preflight(path, source)
            receipt["gpu_health_observation"]["observation"]["measured_at_utc"] = datetime.now(
                timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
            receipt["gpu_health_observation"]["observation"]["measurements"]["compute_oracle_passed"] = False
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with patch.object(campaign, "REPO_ROOT", Path(root)):
                with self.assertRaisesRegex(RuntimeError, "gpu_health"):
                    campaign.validate_host_preflight(path, source)
            receipt["gpu_health_observation"]["observation"]["measurements"]["compute_oracle_passed"] = True
            receipt["gpu_health_source"]["path"] = "/tmp/arbitrary-pulp"
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with patch.object(campaign, "REPO_ROOT", Path(root)):
                with self.assertRaisesRegex(RuntimeError, "gpu_build_identity"):
                    campaign.validate_host_preflight(path, source)
            receipt["gpu_health_source"]["path"] = str(executable)
            receipt["sampled_at"] = "2000-01-01T00:00:00Z"
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with patch.object(campaign, "REPO_ROOT", Path(root)):
                with self.assertRaisesRegex(RuntimeError, "fresh"):
                    campaign.validate_host_preflight(path, source)

    def test_host_preflight_rejects_contention_or_source_mismatch(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "host-preflight.json"
            path.write_text(json.dumps({
                "schema": "pulp.gpu-audio.p2.host-preflight.v1",
                "status": "passed", "source_revision": "b" * 40, "host_id": "m5",
                "sampled_at": datetime.now(timezone.utc).isoformat(),
                "quiet_host": True, "host_vitals_level": "green",
                "gpu_contention": True, "ui_contention": False,
                "thermal_state": "nominal",
            }), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "source_revision|contention"):
                campaign.validate_host_preflight(path, "a" * 40)

    def test_host_preflight_rejects_missing_gpu_health_binding(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "host-preflight.json"
            source = "a" * 40
            path.write_text(json.dumps({
                "schema": "pulp.gpu-audio.p2.host-preflight.v1", "status": "passed",
                "source_revision": source, "host_id": "m5", "sampled_at": datetime.now(timezone.utc).isoformat(),
                "quiet_host": True, "host_vitals_level": "green", "gpu_contention": False,
                "ui_contention": False, "thermal_state": "nominal",
            }), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "gpu_health|gpu_build_identity"):
                campaign.validate_host_preflight(path, source)

    def test_host_preflight_mutation_is_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "host-preflight.json"
            path.write_text('{"status":"passed"}\n', encoding="utf-8")
            digest = campaign.sha256(path)
            campaign.require_unchanged_host_preflight(path, digest)
            path.write_text('{"status":"changed"}\n', encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "changed"):
                campaign.require_unchanged_host_preflight(path, digest)
    @staticmethod
    def _rows():
        bindings = {
            "provider_asset_sha256": "a" * 64,
            "dawn_archive_sha256": "b" * 64,
            "provider_asset_manifest_sha256": "c" * 64,
            "dawn_archive_manifest_sha256": "d" * 64,
        }
        provenance = {
            "kind": "provenance", "schema": "pulp.gpu-audio.p2.raw.v1",
            "engine_id": 7,
            "provider_identity_status": "passed", "provider_observed_identity": "passed",
            "provider_revision": "e" * 40, "adapter_name": "Apple GPU",
            "adapter_backend": "metal", "adapter_vendor_id": 0x106B,
            "adapter_device_id": 1, "native_runtime_identity_status": "passed",
            "native_runtime_name": "Metal", "native_runtime_backend": "metal",
            "run_kind": "cold",
            "executable_observed_sha256": "f" * 64,
            "provider_asset_sha256": bindings["provider_asset_sha256"],
            "dawn_archive_sha256": bindings["dawn_archive_sha256"],
            "manifest_bindings": bindings,
        }
        provenance["provenance_manifest_sha256"] = campaign.provenance_manifest_sha256(provenance)
        admission = {"kind": "admission", "engine_id": 7, "generation": 1, "sequence": 2}
        terminal = {"kind": "record", "engine_id": 7, "trace_kind": 0, "generation": 1,
                    "sequence": 2, "valid_stages": 1, "gpu_terminal": 1,
                    "admission_identity_matched": True}
        delivery = {"kind": "record", "engine_id": 7, "trace_kind": 2, "generation": 1,
                    "sequence": 2, "delivery": 1, "callback_timing_available": True,
                    "callback_end_ns": 10, "result_visible_ns": 11,
                    "admitted": True, "callback_only": False}
        return [provenance, admission, terminal, delivery]

    def test_identity_rows_reject_duplicate_or_missing_terminals(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        campaign.validate_identity_rows(rows, expected_sha)
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(rows + [rows[1]], expected_sha)
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(rows[:-1], expected_sha)
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(rows, "different-probe")
        missing_native = [dict(row) for row in rows]
        missing_native[0].pop("native_runtime_identity_status")
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(missing_native, expected_sha)
        mismatch = [dict(row) for row in rows]
        mismatch[0]["provider_identity_status"] = "mismatch"
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(mismatch, expected_sha)
        for field in ("provider_asset_sha256", "dawn_archive_sha256",
                      "provider_asset_manifest_sha256", "dawn_archive_manifest_sha256"):
            invalid = [dict(row) for row in rows]
            if field in ("provider_asset_sha256", "dawn_archive_sha256"):
                invalid[0][field] = "unknown"
            else:
                invalid[0]["manifest_bindings"] = dict(invalid[0]["manifest_bindings"])
                invalid[0]["manifest_bindings"][field] = "unknown"
            with self.assertRaises(RuntimeError):
                campaign.validate_identity_rows(invalid, expected_sha)

    def test_lossless_observer_retains_per_admission_terminal_and_delivery(self):
        rows = self._rows()
        observer = campaign.LosslessLifecycleObserver()
        for row in rows:
            observer.observe(row)
        observer.validate(rows[0]["executable_observed_sha256"])
        self.assertEqual(observer.rows, tuple(rows))
        identities = observer.identities()
        self.assertEqual(set(identities), {(7, 1, 2)})
        self.assertEqual(set(identities[(7, 1, 2)]), {"terminal", "delivery"})

    def test_observe_jsonl_rejects_blank_or_malformed_rows(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "raw.jsonl"
            path.write_text('{"kind":"provenance"}\n\n', encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "blank line"):
                campaign.observe_jsonl(path)
            path.write_text('{not-json}\n', encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "invalid JSON"):
                campaign.observe_jsonl(path)

    def test_manifest_digest_binds_asset_and_archive_hashes(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        digest = rows[0]["provenance_manifest_sha256"]
        campaign.validate_identity_rows(rows, expected_sha, digest)
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(rows, expected_sha, "0" * 64)
        changed = [dict(row) for row in rows]
        changed[0]["manifest_bindings"] = dict(changed[0]["manifest_bindings"])
        changed[0]["manifest_bindings"]["dawn_archive_sha256"] = "1" * 64
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(changed, expected_sha)

    def test_provider_identity_fixture_binds_raw_rows_independently(self):
        rows = self._rows()
        fixture = {
            "schema": campaign.PROVIDER_IDENTITY_SCHEMA, "status": "passed",
            "provider_identity_status": "passed",
            "expected_asset_sha256": rows[0]["provider_asset_sha256"],
            "validated_asset_sha256": rows[0]["provider_asset_sha256"],
            "expected_dawn_revision": rows[0]["provider_revision"],
            "header_dawn_revision": rows[0]["provider_revision"],
            "linked_dawn_archive_sha256": rows[0]["dawn_archive_sha256"],
            "manifest_sha256": "1" * 64,
            "generation_receipt_sha256": "2" * 64,
            "executable_sha256": rows[0]["executable_observed_sha256"],
            "configured_receipt_sha256": "3" * 64,
            "prelink_receipt_sha256": "4" * 64,
        }
        rows[0]["manifest_bindings"]["provider_asset_manifest_sha256"] = fixture["manifest_sha256"]
        rows[0]["manifest_bindings"]["dawn_archive_manifest_sha256"] = fixture["manifest_sha256"]
        rows[0]["provenance_manifest_sha256"] = campaign.provenance_manifest_sha256(rows[0])
        campaign.validate_identity_rows(rows, rows[0]["executable_observed_sha256"],
                                        expected_provider_identity=fixture)
        invalid = [dict(row) for row in rows]
        invalid[0]["provider_revision"] = "f" * 40
        with self.assertRaisesRegex(RuntimeError, "provider revision"):
            campaign.validate_identity_rows(
                invalid, invalid[0]["executable_observed_sha256"],
                expected_provider_identity=fixture)

    def test_provider_identity_fixture_requires_current_manifest(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            manifest = root_path / "tools/deps/manifest.json"
            _write_pinned_manifest(manifest)
            fixture = root_path / "provider.json"
            payload = {
                "schema": campaign.PROVIDER_IDENTITY_SCHEMA, "status": "passed",
                "provider_identity_status": "passed",
                "expected_asset_sha256": "a" * 64, "validated_asset_sha256": "a" * 64,
                "expected_dawn_revision": "b" * 40, "header_dawn_revision": "b" * 40,
                "linked_dawn_archive_sha256": "c" * 64,
                "manifest_sha256": campaign.sha256(manifest),
                "generation_receipt_sha256": "d" * 64,
                "executable_sha256": "e" * 64,
                "configured_receipt_sha256": "f" * 64,
                "prelink_receipt_sha256": "0" * 64,
                "configured_receipt_path": str(root_path / "configured.json"),
                "prelink_receipt_path": str(root_path / "prelink.json"),
            }
            (root_path / "configured.json").write_text('{"configured":true}\n', encoding="utf-8")
            (root_path / "prelink.json").write_text('{"prelink":true}\n', encoding="utf-8")
            payload["configured_receipt_sha256"] = campaign.sha256(root_path / "configured.json")
            payload["prelink_receipt_sha256"] = campaign.sha256(root_path / "prelink.json")
            fixture.write_text(json.dumps(payload), encoding="utf-8")
            with patch.object(campaign, "REPO_ROOT", root_path):
                self.assertEqual(
                    campaign.validate_provider_identity_fixture(fixture, PLATFORM), payload)
                manifest.write_text("changed\n", encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "current provider manifest"):
                    campaign.validate_provider_identity_fixture(fixture, PLATFORM)

    def _bound_rows_and_fixture(self):
        rows = self._rows()
        fixture = {
            "schema": campaign.PROVIDER_IDENTITY_SCHEMA, "status": "passed",
            "provider_identity_status": "passed",
            "expected_asset_sha256": rows[0]["provider_asset_sha256"],
            "validated_asset_sha256": rows[0]["provider_asset_sha256"],
            "expected_dawn_revision": rows[0]["provider_revision"],
            "header_dawn_revision": rows[0]["provider_revision"],
            "linked_dawn_archive_sha256": rows[0]["dawn_archive_sha256"],
            "manifest_sha256": "1" * 64,
            "generation_receipt_sha256": "2" * 64,
            "executable_sha256": rows[0]["executable_observed_sha256"],
            "configured_receipt_sha256": "3" * 64,
            "prelink_receipt_sha256": "4" * 64,
        }
        rows[0]["manifest_bindings"]["provider_asset_manifest_sha256"] = fixture["manifest_sha256"]
        rows[0]["manifest_bindings"]["dawn_archive_manifest_sha256"] = fixture["manifest_sha256"]
        rows[0]["provenance_manifest_sha256"] = campaign.provenance_manifest_sha256(rows[0])
        return rows, fixture

    def _assert_binding_refused(self, rows, fixture, message):
        rows[0]["provenance_manifest_sha256"] = campaign.provenance_manifest_sha256(rows[0])
        with self.assertRaisesRegex(RuntimeError, message):
            campaign.validate_identity_rows(rows, rows[0]["executable_observed_sha256"],
                                            expected_provider_identity=fixture)

    def test_raw_rows_must_carry_the_receipts_dawn_archive(self):
        rows, fixture = self._bound_rows_and_fixture()
        rows[0]["dawn_archive_sha256"] = "9" * 64
        rows[0]["manifest_bindings"]["dawn_archive_sha256"] = "9" * 64
        self._assert_binding_refused(rows, fixture, "Dawn archive disagrees")

    def test_raw_rows_must_bind_each_manifest_field_to_the_receipt(self):
        for field in ("provider_asset_manifest_sha256", "dawn_archive_manifest_sha256"):
            with self.subTest(field=field):
                rows, fixture = self._bound_rows_and_fixture()
                rows[0]["manifest_bindings"][field] = "9" * 64
                self._assert_binding_refused(rows, fixture, "manifest bindings disagree")

    def test_control_matching_raw_rows_bind_to_the_receipt(self):
        rows, fixture = self._bound_rows_and_fixture()
        campaign.validate_identity_rows(rows, rows[0]["executable_observed_sha256"],
                                        expected_provider_identity=fixture)

    def _receipt(self, root_path, **changes):
        manifest = root_path / "tools/deps/manifest.json"
        _write_pinned_manifest(manifest)
        configured = root_path / "configured.json"
        prelink = root_path / "prelink.json"
        configured.write_text('{"status":"passed","stage":"configured"}\n', encoding="utf-8")
        prelink.write_text('{"status":"passed","stage":"prelink"}\n', encoding="utf-8")
        payload = {
            "schema": campaign.PROVIDER_IDENTITY_SCHEMA, "status": "passed",
            "provider_identity_status": "passed",
            "expected_asset_sha256": "a" * 64, "validated_asset_sha256": "a" * 64,
            "expected_dawn_revision": "b" * 40, "header_dawn_revision": "b" * 40,
            "linked_dawn_archive_sha256": "c" * 64,
            "manifest_sha256": campaign.sha256(manifest),
            "generation_receipt_sha256": "d" * 64,
            "executable_sha256": "e" * 64,
            "configured_receipt_sha256": "f" * 64,
            "prelink_receipt_sha256": "0" * 64,
            "configured_receipt_path": str(configured),
            "prelink_receipt_path": str(prelink),
        }
        payload["configured_receipt_sha256"] = campaign.sha256(configured)
        payload["prelink_receipt_sha256"] = campaign.sha256(prelink)
        payload.update(changes)
        receipt = root_path / "provider.json"
        receipt.write_text(json.dumps(payload), encoding="utf-8")
        return receipt, payload

    def test_receipt_with_inconsistent_identity_pairs_is_refused(self):
        cases = {"asset pair": {"validated_asset_sha256": "e" * 64},
                 "revision pair": {"header_dawn_revision": "f" * 40}}
        for name, change in cases.items():
            with self.subTest(pair=name), tempfile.TemporaryDirectory() as root:
                receipt, _ = self._receipt(Path(root), **change)
                with patch.object(campaign, "REPO_ROOT", Path(root)):
                    with self.assertRaisesRegex(RuntimeError, "inconsistent provider identity"):
                        campaign.validate_provider_identity_fixture(receipt)

    def test_load_receipt_requires_authoritative_verification_match(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            receipt, payload = self._receipt(root_path)
            verification = campaign.argparse.Namespace(marker="authoritative")
            with patch.object(campaign, "REPO_ROOT", root_path), \
                    patch.object(campaign.identity_contract, "verify", return_value=dict(payload)) as verify:
                loaded, _ = campaign.load_provider_identity_receipt(
                    receipt, PLATFORM, verification=verification)
            self.assertEqual(loaded, payload)
            verify.assert_called_once_with(verification)

            mismatched = dict(payload)
            mismatched["configured_receipt_path"] = str(root_path / "other-configured.json")
            with patch.object(campaign, "REPO_ROOT", root_path), \
                    patch.object(campaign.identity_contract, "verify", return_value=mismatched):
                with self.assertRaisesRegex(RuntimeError, "configured_receipt_path disagrees"):
                    campaign.load_provider_identity_receipt(
                        receipt, PLATFORM, verification=verification)

    def test_raw_rows_must_carry_the_receipts_provider_asset(self):
        rows, fixture = self._bound_rows_and_fixture()
        rows[0]["provider_asset_sha256"] = "9" * 64
        rows[0]["manifest_bindings"]["provider_asset_sha256"] = "9" * 64
        self._assert_binding_refused(rows, fixture, "provider asset disagrees")

    def test_raw_rows_must_carry_the_bound_executable(self):
        rows, fixture = self._bound_rows_and_fixture()
        rows[0]["executable_observed_sha256"] = "9" * 64
        with self.assertRaisesRegex(RuntimeError, "executable disagrees"):
            campaign.validate_identity_rows(rows, rows[0]["executable_observed_sha256"],
                                            expected_provider_identity=fixture)

    def test_receipt_that_did_not_pass_is_refused(self):
        cases = {"schema": {"schema": "pulp.gpu-audio-provider-identity.v0"},
                 "status": {"status": "failed"},
                 "provider status": {"provider_identity_status": "failed"}}
        for name, change in cases.items():
            with self.subTest(field=name), tempfile.TemporaryDirectory() as root:
                receipt, _ = self._receipt(Path(root), **change)
                with patch.object(campaign, "REPO_ROOT", Path(root)):
                    with self.assertRaisesRegex(RuntimeError, "not a passed exact-provider receipt"):
                        campaign.validate_provider_identity_fixture(receipt, PLATFORM)

    def test_receipt_with_a_malformed_hash_field_is_refused(self):
        # Fields no later check compares, so only the format check can refuse them.
        cases = {"generation_receipt_sha256": "not-a-digest",
                 "linked_dawn_archive_sha256": "c" * 63,
                 "executable_sha256": "e" * 63,
                 "configured_receipt_sha256": "f" * 63,
                 "prelink_receipt_sha256": "0" * 63}
        for field, value in cases.items():
            with self.subTest(field=field), tempfile.TemporaryDirectory() as root:
                receipt, _ = self._receipt(Path(root), **{field: value})
                with patch.object(campaign, "REPO_ROOT", Path(root)):
                    with self.assertRaisesRegex(RuntimeError, f"{field} is invalid"):
                        campaign.validate_provider_identity_fixture(receipt, PLATFORM)

    def test_receipt_requires_bound_build_receipt_hashes(self):
        for field in ("executable_sha256", "configured_receipt_sha256", "prelink_receipt_sha256"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as root:
                receipt, payload = self._receipt(Path(root))
                payload.pop(field)
                receipt.write_text(json.dumps(payload), encoding="utf-8")
                with patch.object(campaign, "REPO_ROOT", Path(root)):
                    with self.assertRaisesRegex(RuntimeError, field + " is invalid"):
                        campaign.validate_provider_identity_fixture(receipt, PLATFORM)

    def test_receipt_rejects_mismatched_configured_or_prelink_bytes(self):
        for field, filename in (("configured_receipt_sha256", "configured.json"),
                                ("prelink_receipt_sha256", "prelink.json")):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as root:
                root_path = Path(root)
                receipt, _ = self._receipt(root_path)
                (root_path / filename).write_text('{"tampered":true}\n', encoding="utf-8")
                with patch.object(campaign, "REPO_ROOT", root_path):
                    with self.assertRaisesRegex(RuntimeError, field + " does not match"):
                        campaign.validate_provider_identity_fixture(receipt, PLATFORM)

    def test_receipt_must_name_the_provider_the_manifest_pins(self):
        # Each receipt is self-consistent and bound to the manifest's hash; only
        # the asset or revision differs from what the manifest pins.
        cases = {"asset": {"expected_asset_sha256": "e" * 64, "validated_asset_sha256": "e" * 64},
                 "revision": {"expected_dawn_revision": "f" * 40, "header_dawn_revision": "f" * 40}}
        for name, change in cases.items():
            with self.subTest(pin=name), tempfile.TemporaryDirectory() as root:
                receipt, _ = self._receipt(Path(root), **change)
                with patch.object(campaign, "REPO_ROOT", Path(root)):
                    with self.assertRaisesRegex(RuntimeError, "manifest does not pin"):
                        campaign.validate_provider_identity_fixture(receipt, PLATFORM)

    def test_control_receipt_naming_the_pinned_provider_is_accepted(self):
        with tempfile.TemporaryDirectory() as root:
            receipt, payload = self._receipt(Path(root))
            with patch.object(campaign, "REPO_ROOT", Path(root)):
                self.assertEqual(
                    campaign.validate_provider_identity_fixture(receipt, PLATFORM), payload)

    def test_receipt_is_checked_against_the_pins_for_its_platform(self):
        # The manifest pins no asset for this platform, so nothing can match.
        with tempfile.TemporaryDirectory() as root:
            receipt, _ = self._receipt(Path(root))
            with patch.object(campaign, "REPO_ROOT", Path(root)):
                with self.assertRaisesRegex(RuntimeError, "does not pin a provider identity"):
                    campaign.validate_provider_identity_fixture(receipt, "darwin-x64")

    def test_host_provider_platform_follows_the_macos_machine(self):
        with patch.object(campaign.sys, "platform", "darwin"):
            for machine, expected in (("arm64", "darwin-arm64"), ("x86_64", "darwin-x64")):
                with self.subTest(machine=machine), \
                        patch.object(campaign.platform, "machine", return_value=machine):
                    self.assertEqual(campaign.host_provider_platform(), expected)
        with patch.object(campaign.sys, "platform", "linux"):
            with self.assertRaisesRegex(RuntimeError, "macOS provider platform"):
                campaign.host_provider_platform()

    def test_run_checks_every_raw_census_against_the_receipt(self):
        # run() must hand the validated receipt to both the negative control's
        # and the measured runs' validate(); without it the raw rows are never
        # compared with the receipt at all.
        identity = {"marker": "validated-receipt"}
        seen = []

        class Stop(Exception):
            pass

        class Observer:
            rows = [{"kind": "provenance", "provenance_manifest_sha256": "1" * 64},
                    {"kind": "record"}]

            def validate(self, expected_probe_sha256=None, expected_manifest_sha256=None,
                         receipt=None, expected_provider_identity=None):
                seen.append(expected_provider_identity)
                if len(seen) == 2:
                    raise Stop()

        def fake_probe(argv, **_kwargs):
            out = Path(next(a for a in argv if a.startswith("--output-dir=")).split("=", 1)[1])
            out.mkdir(parents=True, exist_ok=True)
            (out / "receipt.json").write_text("{}", encoding="utf-8")
            (out / "raw.jsonl").write_text("", encoding="utf-8")
            return campaign.subprocess.CompletedProcess(argv, 0, "", "")

        # Production hardware admission remains macOS-only. This
        # receipt-propagation test runs on every CI host, so provide a
        # validated platform fixture instead of weakening that guard.
        with tempfile.TemporaryDirectory() as root, \
                patch.object(campaign, "_source_provenance", return_value=("a" * 40, "b" * 64)), \
                patch.object(campaign, "validate_host_preflight", return_value={}), \
                patch.object(campaign, "host_provider_platform", return_value=PLATFORM), \
                patch.object(campaign, "load_provider_identity_receipt",
                             return_value=(identity, "d" * 64)) as load_receipt, \
                patch.object(campaign.subprocess, "run", side_effect=fake_probe), \
                patch.object(campaign, "validate_negative_control_receipt"), \
                patch.object(campaign, "validate_receipt"), \
                patch.object(campaign, "observe_jsonl", return_value=Observer()):
            args = self._hardware_args(root, "--expected-source-revision", "a" * 40,
                                       "--provider-identity-receipt", str(Path(root) / "p.json"))
            with self.assertRaises(Stop):
                campaign.run(args)
        self.assertEqual(seen, [identity, identity])
        verification = load_receipt.call_args.kwargs["verification"]
        self.assertEqual(verification.executable, str(args.probe))
        self.assertEqual(verification.receipt, str(args.provider_identity_receipt))

    def test_a_symlinked_receipt_is_refused(self):
        with tempfile.TemporaryDirectory() as root:
            receipt, payload = self._receipt(Path(root))
            link = Path(root) / "linked.json"
            link.symlink_to(receipt)
            with patch.object(campaign, "REPO_ROOT", Path(root)):
                self.assertEqual(
                    campaign.validate_provider_identity_fixture(receipt, PLATFORM), payload)
                with self.assertRaisesRegex(RuntimeError, "must be a regular file"):
                    campaign.validate_provider_identity_fixture(link)

    def test_receipt_digest_is_of_the_bytes_that_were_validated(self):
        # The path holds different bytes from the ones the validator reads, as
        # if the file were replaced between the check and a later re-read. The
        # recorded digest must be of what was validated.
        with tempfile.TemporaryDirectory() as root:
            receipt, payload = self._receipt(Path(root))
            validated = receipt.read_bytes()
            receipt.write_text(json.dumps(dict(payload, status="swapped")), encoding="utf-8")
            real_read_bytes = Path.read_bytes

            def read_bytes(path):
                return validated if path == receipt else real_read_bytes(path)

            with patch.object(campaign, "REPO_ROOT", Path(root)), \
                    patch.object(Path, "read_bytes", read_bytes):
                fixture, digest = campaign.load_provider_identity_receipt(receipt, PLATFORM)
            self.assertEqual(fixture, payload)
            self.assertEqual(digest, campaign.hashlib.sha256(validated).hexdigest())
            self.assertNotEqual(digest, campaign.sha256(receipt))

    def test_the_campaign_records_the_validated_receipt_digest(self):
        # A full hardware campaign cannot run here; pin that run() records the
        # loader's digest rather than hashing the receipt path again.
        import inspect
        source = inspect.getsource(campaign.run)
        self.assertIn("load_provider_identity_receipt(", source)
        self.assertIn('"provider_identity_receipt_sha256": provider_identity_receipt_sha256', source)
        self.assertNotIn("sha256(args.provider_identity_receipt)", source)

    def _hardware_args(self, root, *extra, omit_provider=None):
        probe = Path(root) / "probe"
        probe.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        probe.chmod(probe.stat().st_mode | stat.S_IXUSR)
        preflight = Path(root) / "preflight.json"
        preflight.write_text("{}", encoding="utf-8")
        manifest = Path(root) / "manifest.json"
        skia = Path(root) / "skia"
        skia.mkdir()
        configured = Path(root) / "configured.json"
        prelink = Path(root) / "prelink.json"
        dawn_header = Path(root) / "dawn_version.h"
        dawn_library = Path(root) / "libdawn.a"
        for path in (manifest, configured, prelink, dawn_header, dawn_library):
            path.write_text("{}\n", encoding="utf-8")
        provider_values = {
            "provider_configured_receipt": configured,
            "provider_prelink_receipt": prelink,
            "provider_manifest": manifest,
            "provider_skia_dir": skia,
            "provider_dawn_header": dawn_header,
            "provider_dawn_library": dawn_library,
        }
        provider_args = []
        for name, value in provider_values.items():
            if name != omit_provider:
                provider_args.extend((f"--{name.replace('_', '-')}", str(value)))
        return campaign.parse_args([
            "--probe", str(probe), "--output-dir", str(Path(root) / "out"),
            "--host-preflight", str(preflight),
            *provider_args, *extra])

    def test_a_hardware_campaign_requires_the_provider_identity_receipt(self):
        with tempfile.TemporaryDirectory() as root, \
                patch.object(campaign, "_source_provenance", return_value=("a" * 40, "b" * 64)):
            args = self._hardware_args(root, "--expected-source-revision", "a" * 40)
            with self.assertRaisesRegex(RuntimeError, "--provider-identity-receipt is required"):
                campaign.run(args)

    def test_a_hardware_campaign_requires_every_authoritative_provider_input(self):
        provider_fields = (
            "provider_configured_receipt", "provider_prelink_receipt",
            "provider_manifest", "provider_skia_dir", "provider_dawn_header",
            "provider_dawn_library",
        )
        for missing in provider_fields:
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as root, \
                    patch.object(campaign, "_source_provenance", return_value=("a" * 40, "b" * 64)):
                    args = self._hardware_args(
                        root, "--expected-source-revision", "a" * 40, omit_provider=missing)
                    args.provider_identity_receipt = Path(root) / "provider.json"
                    with self.assertRaisesRegex(RuntimeError, "all authoritative provider verification inputs are required"):
                        campaign.run(args)

    def test_control_a_hardware_campaign_requires_the_source_revision_first(self):
        with tempfile.TemporaryDirectory() as root, \
                patch.object(campaign, "_source_provenance", return_value=("a" * 40, "b" * 64)):
            args = self._hardware_args(root)
            with self.assertRaisesRegex(RuntimeError, "--expected-source-revision is required"):
                campaign.run(args)

    def test_observed_device_id_zero_is_allowed(self):
        rows = self._rows()
        rows[0]["adapter_device_id"] = 0
        campaign.validate_identity_rows(rows, rows[0]["executable_observed_sha256"])

    def test_provider_and_native_backends_must_be_matching_metal(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        non_metal = [dict(row) for row in rows]
        non_metal[0]["adapter_backend"] = "vulkan"
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(non_metal, expected_sha)
        mismatch = [dict(row) for row in rows]
        mismatch[0]["native_runtime_backend"] = "vulkan"
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(mismatch, expected_sha)

    def test_provenance_engine_identity_must_match_every_row(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        mismatch = [dict(row) for row in rows]
        mismatch[0]["engine_id"] = 8
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(mismatch, expected_sha)
        mismatch = [dict(row) for row in rows]
        mismatch[1]["engine_id"] = 8
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(mismatch, expected_sha)

    def test_raw_census_requires_admission_and_receipt_counter_match(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        callback_only = {
            "kind": "record", "engine_id": 7, "trace_kind": 2, "generation": 1,
            "sequence": 3, "delivery": 2, "callback_timing_available": True,
            "callback_end_ns": 20, "result_visible_ns": 21,
            "admitted": False, "callback_only": True,
        }
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(rows[:1] + [callback_only], expected_sha)
        receipt = {
            "warmup_blocks": 0, "measured_blocks": 1, "declared_lead_blocks": 0,
            "total_callbacks": 1, "callback_count": 1, "measured_callback_count": 1,
            "admission_granularity": "one_per_callback", "admission_record_count": 1,
            "delivery_record_count": 1, "admissions_enqueued": 1,
            "terminal_record_count": 1, "authenticated_terminal_records": 1,
        }
        campaign.validate_identity_rows(rows, expected_sha, receipt=receipt)
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(rows, expected_sha,
                                            receipt={**receipt, "admissions_enqueued": 2})

    def test_raw_census_rejects_truncated_callback_stream(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        receipt = {
            "warmup_blocks": 0, "measured_blocks": 1, "declared_lead_blocks": 0,
            "total_callbacks": 1, "callback_count": 1, "measured_callback_count": 1,
            "admission_granularity": "one_per_callback", "admission_record_count": 1,
            "delivery_record_count": 1, "admissions_enqueued": 1,
            "terminal_record_count": 1, "authenticated_terminal_records": 1,
        }
        campaign.validate_identity_rows(rows, expected_sha, receipt=receipt)
        with self.assertRaisesRegex(RuntimeError, "delivery census|identity multisets"):
            campaign.validate_identity_rows(rows[:-1], expected_sha, receipt=receipt)
        with self.assertRaisesRegex(RuntimeError, "callback geometry|callback count"):
            campaign.validate_identity_rows(
                rows, expected_sha, receipt={**receipt, "callback_count": 2,
                                             "total_callbacks": 2,
                                             "delivery_record_count": 2,
                                             "admission_record_count": 2,
                                             "admissions_enqueued": 2,
                                             "terminal_record_count": 2,
                                             "authenticated_terminal_records": 2})

    def test_negative_control_requires_observed_oracle_failure(self):
        valid = {"status": "failed", "negative_control": True,
                 "oracle_failed_blocks": campaign.NEGATIVE_CONTROL_FAILED_BLOCKS}
        campaign.validate_negative_control_receipt(valid)
        for invalid in (
                {"status": "failed", "negative_control": True, "oracle_failed_blocks": 0},
                {"status": "failed", "negative_control": True},
                {"status": "completed", "negative_control": True,
                 "oracle_failed_blocks": campaign.NEGATIVE_CONTROL_FAILED_BLOCKS},
                {"status": "failed", "negative_control": False,
                 "oracle_failed_blocks": campaign.NEGATIVE_CONTROL_FAILED_BLOCKS},
        ):
            with self.assertRaisesRegex(RuntimeError, "negative control"):
                campaign.validate_negative_control_receipt(invalid)

    def test_delivery_disposition_must_match_terminal_census(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        invalid = [dict(row) for row in rows]
        invalid[-1]["delivery"] = 2  # CPU fallback is a typed disposition.
        # The fallback row remains valid because terminal and delivery are
        # intentionally orthogonal; a late GPU result may still fall back.
        campaign.validate_identity_rows(invalid, expected_sha)
        missing_timing = [dict(row) for row in rows]
        missing_timing[-1]["callback_timing_available"] = False
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(missing_timing, expected_sha)
        bad_gpu_pair = [dict(row) for row in rows]
        bad_gpu_pair[-1]["delivery"] = 1
        bad_gpu_pair[-2]["gpu_terminal"] = 2
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(bad_gpu_pair, expected_sha)

    def test_callback_only_delivery_rows_are_retained_and_classified(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        callback_only = {
            "kind": "record", "engine_id": 7, "trace_kind": 2, "generation": 1,
            "sequence": 3, "delivery": 2, "callback_timing_available": True,
            "callback_end_ns": 20, "result_visible_ns": 21,
            "admitted": False, "callback_only": True,
        }
        campaign.validate_identity_rows(rows + [callback_only], expected_sha)
        wrong_classification = list(rows) + [dict(callback_only, admitted=True)]
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(wrong_classification, expected_sha)

    def test_identity_rows_retain_known_lifecycle_kinds_and_reject_unknown_or_malformed(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        unknown = [dict(row) for row in rows]
        unknown.append(dict(rows[-1], trace_kind=99))
        with self.assertRaisesRegex(RuntimeError, "unknown trace kind"):
            campaign.validate_identity_rows(unknown, expected_sha)
        eligible = dict(rows[-1], trace_kind=1, generation=1, sequence=3,
                        valid_stages=1, gpu_work_admitted=False,
                        output_eligible=False, gpu_terminal=0, delivery=0)
        recovery = dict(eligible, trace_kind=3, next_generation=2)
        campaign.validate_identity_rows(rows + [eligible, recovery], expected_sha)
        zero_stage_eligible = dict(eligible, sequence=4, valid_stages=0)
        campaign.validate_identity_rows(rows + [zero_stage_eligible], expected_sha)
        malformed = dict(eligible, valid_stages=-1)
        with self.assertRaisesRegex(RuntimeError, "malformed known trace kind"):
            campaign.validate_identity_rows(rows + [malformed], expected_sha)
        malformed_recovery = dict(recovery, next_generation=1)
        with self.assertRaisesRegex(RuntimeError, "malformed Recovery"):
            campaign.validate_identity_rows(rows + [malformed_recovery], expected_sha)

    def test_steady_receipt_requires_same_process_residency(self):
        base = {
            "schema": "pulp.gpu-audio-paced-convolution.v1", "status": "completed",
            "gpu_receipt_authenticated": True, "declared_slots": 2,
            "declared_lead_blocks": 1, "run_kind": "steady",
            "authenticated_terminal_records": 500001, "terminal_record_count": 500001,
            "retired_success": 500001, "retired_failure": 0, "admissions_attempted": 500001,
            "admissions_enqueued": 1, "admissions_dropped": 0, "trace_attempted": 1,
            "trace_enqueued": 1, "trace_dropped": 0, "trace_sampled_out": 0,
            "trace_invalid": 0, "high_water_in_flight": 1,
            "measured_blocks": campaign.REQUIRED_MEASURED_BLOCKS * campaign.RUNS_PER_KIND,
            "measured_blocks_per_repetition": campaign.REQUIRED_MEASURED_BLOCKS,
            "steady_repetitions": campaign.RUNS_PER_KIND,
            "warmup_blocks": 0, "total_callbacks": campaign.REQUIRED_MEASURED_BLOCKS * campaign.RUNS_PER_KIND + 1,
            "callback_count": campaign.REQUIRED_MEASURED_BLOCKS * campaign.RUNS_PER_KIND + 1,
            "measured_callback_count": campaign.REQUIRED_MEASURED_BLOCKS * campaign.RUNS_PER_KIND,
            "admission_granularity": "one_per_callback",
            "admission_record_count": campaign.REQUIRED_MEASURED_BLOCKS * campaign.RUNS_PER_KIND + 1,
            "delivery_record_count": campaign.REQUIRED_MEASURED_BLOCKS * campaign.RUNS_PER_KIND + 1,
            "provider_identity_status": "passed", "native_runtime_identity_status": "passed",
            "adapter_vendor_id": 1, "adapter_device_id": 1,
        }
        base["admissions_enqueued"] = base["admission_record_count"]
        base["admissions_attempted"] = base["admission_record_count"]
        base["trace_attempted"] = base["trace_enqueued"] = base["admission_record_count"]
        invalid = dict(base)
        with self.assertRaises(RuntimeError):
            campaign.validate_receipt(invalid, 2, 1, "steady")
        valid = dict(base, steady_semantics="same_process_resident",
                     same_process_resident=True, process_id=42,
                     residency_session_id="session-1", prepared_sessions=1,
                     reprepare_count=0)
        campaign.validate_receipt(valid, 2, 1, "steady")
    def test_defaults_to_required_100k_blocks(self):
        args = campaign.parse_args(["--probe", "/bin/true", "--output-dir", "/tmp/p2-contract-test"])
        self.assertEqual(args.blocks, campaign.REQUIRED_MEASURED_BLOCKS)

    def test_source_provenance_rejects_tracked_dirty_tree(self):
        dirty = type("Result", (), {"returncode": 0, "stdout": " M tools/scripts/gpu_audio_p2_campaign.py\n"})()
        with patch.object(campaign.subprocess, "run", return_value=dirty):
            with self.assertRaisesRegex(RuntimeError, "tracked modifications"):
                campaign._source_provenance()

    def test_manifest_provenance_rejects_driver_or_source_mismatch(self):
        source = "a" * 40
        driver = "b" * 64
        manifest = {"source_revision": source, "driver_sha256": driver,
                    "source_tree_clean": True}
        campaign.validate_manifest_provenance(manifest, source, driver)
        with self.assertRaisesRegex(RuntimeError, "source revision"):
            campaign.validate_manifest_provenance({**manifest, "source_revision": "c" * 40}, source, driver)
        with self.assertRaisesRegex(RuntimeError, "driver hash"):
            campaign.validate_manifest_provenance({**manifest, "driver_sha256": "d" * 64}, source, driver)
        with self.assertRaisesRegex(RuntimeError, "clean tracked"):
            campaign.validate_manifest_provenance({**manifest, "source_tree_clean": False}, source, driver)

    def test_post_trial_provenance_recheck_rejects_mid_run_mutation(self):
        with patch.object(campaign, "_source_provenance",
                          side_effect=[("a" * 40, "b" * 64), ("c" * 40, "b" * 64)]):
            initial = campaign._source_provenance()
            with self.assertRaisesRegex(RuntimeError, "provenance changed"):
                campaign._require_unchanged_source(initial)

    def test_source_provenance_rejects_untracked_driver_copy(self):
        with tempfile.TemporaryDirectory() as root:
            copied = Path(root) / "gpu_audio_p2_campaign.py"
            copied.write_text("# copied driver\n", encoding="utf-8")
            with patch.object(campaign, "DRIVER_PATH", copied):
                clean = type("Result", (), {"returncode": 0, "stdout": ""})()
                with patch.object(campaign.subprocess, "run", return_value=clean):
                    with self.assertRaisesRegex(RuntimeError, "canonical tracked file"):
                        campaign._source_provenance()

    def test_plan_only_declares_complete_matrix(self):
        args = campaign.parse_args(["--plan-only"])
        self.assertEqual(campaign.run(args), 0)

    def test_plan_only_accepts_a_bounded_matrix_subset(self):
        args = campaign.parse_args(["--plan-only", "--slots", "16,2", "--leads", "8,1"])
        self.assertEqual(args.slots, (2, 16))
        self.assertEqual(args.leads, (1, 8))
        output = StringIO()
        with redirect_stdout(output):
            self.assertEqual(campaign.run(args), 0)
        plan = json.loads(output.getvalue())
        self.assertEqual(plan["cells"], [
            {"slots": 2, "lead": 1}, {"slots": 2, "lead": 8},
            {"slots": 16, "lead": 1}, {"slots": 16, "lead": 8},
        ])
        self.assertEqual(plan["trial_count"], 4 * 2 * campaign.RUNS_PER_KIND)

    def test_process_receipts_expand_to_logical_trials_once(self):
        self.assertEqual(campaign.logical_trial_repetitions("cold", 4), (4,))
        self.assertEqual(
            campaign.logical_trial_repetitions("steady", 1),
            tuple(range(1, campaign.RUNS_PER_KIND + 1)),
        )
        with self.assertRaises(ValueError):
            campaign.logical_trial_repetitions("other", 1)

    def test_matrix_axis_rejects_duplicates_and_unknown_values(self):
        with self.assertRaises(SystemExit):
            campaign.parse_args(["--plan-only", "--slots", "2,2"])
        with self.assertRaises(SystemExit):
            campaign.parse_args(["--plan-only", "--leads", "3"])

    def test_lower_block_count_rejected_before_probe(self):
        with tempfile.TemporaryDirectory() as root:
            probe = Path(root) / "probe"
            probe.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            probe.chmod(probe.stat().st_mode | stat.S_IXUSR)
            args = campaign.parse_args([
                "--probe", str(probe), "--output-dir", str(Path(root) / "out"), "--blocks", "1024"
            ])
            with self.assertRaises(RuntimeError):
                campaign.run(args)


if __name__ == "__main__":
    unittest.main()
