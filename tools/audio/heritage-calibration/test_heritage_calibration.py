from __future__ import annotations

import json
import math
import os
import tempfile
import unittest
from pathlib import Path

import heritage_calibration as hc


class HeritageCalibrationTest(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("PULP_HERITAGE_C1_C5_RENDER_WAV"),
                         "configured CTest supplies the Pulp C1-C5 renderer")
    def test_00_product_c1_through_c5_bootstrap_recovers_declared_behavior(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = hc.bootstrap_c1_c5(
                Path(temp) / "c1-c5",
                Path(os.environ["PULP_HERITAGE_C1_C5_RENDER_WAV"]))
            self.assertTrue(report["passed"])
            self.assertEqual(set(report["declared_vs_recovered"]),
                             {"C1", "C2", "C3", "C4", "C5"})
            self.assertTrue(all(item["matched"] for item in
                                report["declared_vs_recovered"].values()))
            self.assertTrue(all(item["rejected"] for item in
                                report["negative_controls"].values()))

    def test_capture_plan_expands_every_literal_required_grid_row(self) -> None:
        plan_path = (Path(__file__).parents[3] / "docs/validation/sample-heritage/capture-plan.json")
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        rows = hc.expand_capture_plan(plan)
        self.assertEqual(len(rows), 575)
        adaptive = [row for row in rows.values()
                    if row["protocol_id"] == "C6" and row["parameters"]["mode"] == "adaptive"]
        self.assertEqual(len(adaptive), 486)
        self.assertTrue(all(row["parameters"]["stimulus"] == "impulse-train" or
                            row["parameters"]["factor_percent"] == 100
                            for row in adaptive))
        self.assertFalse(any(row["protocol_id"] == "C6" and
                             row["parameters"]["stimulus"].startswith("licensed-")
                             for row in rows.values()))

    def test_capture_readiness_reconciles_rows_roles_and_exact_plan_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            plan = {
                "schema": hc.CAPTURE_PLAN_SCHEMA,
                "protocols": [
                    {
                        "id": "C4",
                        "axes": {"state": ["active", "idle"]},
                        "variants": [{"fixed": {}, "vary": ["state"],
                                      "required_artifact_roles": ["raw_capture", "session_log"]}],
                        "required_selectors": [{}],
                    },
                    {
                        "id": "C6",
                        "applicability": {"capability": "offline-stretch",
                                          "allow_omit_when_absent": True},
                        "axes": {"mode": ["cyclic"]},
                        "variants": [{"fixed": {}, "vary": ["mode"],
                                      "required_artifact_roles": ["raw_capture"]}],
                        "required_selectors": [{}],
                    },
                ],
            }
            plan_path = root / "plan.json"
            hc.write_json(plan_path, plan)
            target = {"manufacturer": "M", "model": "X", "serial": "S",
                      "revision": "R"}
            applicability_evidence = root / "offline-stretch-applicability.json"
            hc.write_json(applicability_evidence, {
                "schema": hc.APPLICABILITY_EVIDENCE_SCHEMA,
                "capture_plan_sha256": hc.sha256_file(plan_path),
                "capability": "offline-stretch",
                "session_id": "readiness-fixture",
                "target": target,
                "conclusion": "not-applicable",
                "finding": "The target has no offline stretch function.",
                "sources": ["target operator manual capability inventory"],
            })
            artifacts = []
            rows = []
            for state in ("active", "idle"):
                row_id = f"c4-{state}"
                rows.append({"row_id": row_id, "protocol_id": "C4",
                             "parameters": {"state": state}})
                for role in ("raw_capture", "session_log"):
                    artifact_path = root / f"{row_id}-{role}.bin"
                    artifact_path.write_bytes(f"{row_id}:{role}".encode())
                    artifacts.append({
                        "path": artifact_path.name,
                        "sha256": hc.sha256_file(artifact_path),
                        "bytes": artifact_path.stat().st_size,
                        "test_id": row_id,
                        "row_id": row_id,
                        "role": role,
                    })
            manifest = {
                "schema": hc.CAPTURE_SCHEMA,
                "session_id": "readiness-fixture",
                "captured_at": "2026-07-19T20:00:00Z",
                "operator_id": "operator-a",
                "target": target,
                "conditions": {"psu_and_calibration_state": "ok", "temperature_c": 22.0,
                               "gain_staging": "documented"},
                "capture_chain": [{"stage": "capture", "description": "fixture"}],
                "capture_plan": {"schema": hc.CAPTURE_PLAN_SCHEMA,
                                 "sha256": hc.sha256_file(plan_path)},
                "target_applicability": {
                    "schema": hc.TARGET_APPLICABILITY_SCHEMA,
                    "capture_plan_sha256": hc.sha256_file(plan_path),
                    "declarations": [{
                        "capability": "offline-stretch",
                        "supported": False,
                        "evidence_path": applicability_evidence.name,
                        "evidence_sha256": hc.sha256_file(applicability_evidence),
                    }],
                },
                "rows": rows,
                "artifacts": artifacts,
                "trademark_notice": hc.TRADEMARK_NOTICE,
            }
            manifest_path = root / "session.json"
            hc.write_json(manifest_path, manifest)
            report = hc.audit_capture_readiness(manifest_path, plan_path)
            self.assertEqual(report["required_rows"], 2)
            self.assertEqual(report["canonical_required_rows"], 3)
            self.assertEqual(report["omitted_rows"], 1)
            self.assertEqual(report["absent_capabilities"], ["offline-stretch"])
            self.assertEqual(report["verified_artifacts"], 4)

            hc.write_json(applicability_evidence, {
                "finding": "An opaque file must not waive a protocol.",
            })
            manifest["target_applicability"]["declarations"][0][
                "evidence_sha256"] = hc.sha256_file(applicability_evidence)
            hc.write_json(manifest_path, manifest)
            with self.assertRaisesRegex(hc.HeritageCalibrationError,
                                        "applicability evidence schema"):
                hc.audit_capture_readiness(manifest_path, plan_path)

            hc.write_json(applicability_evidence, {
                "schema": hc.APPLICABILITY_EVIDENCE_SCHEMA,
                "capture_plan_sha256": hc.sha256_file(plan_path),
                "capability": "offline-stretch",
                "session_id": "readiness-fixture",
                "target": target,
                "conclusion": "not-applicable",
                "finding": "The target has no offline stretch function.",
                "sources": ["target operator manual capability inventory"],
            })
            manifest["target_applicability"]["declarations"][0][
                "evidence_sha256"] = hc.sha256_file(applicability_evidence)

            manifest["artifacts"] = [artifact for artifact in artifacts
                                     if not (artifact["row_id"] == "c4-idle" and
                                             artifact["role"] == "session_log")]
            hc.write_json(manifest_path, manifest)
            with self.assertRaisesRegex(hc.HeritageCalibrationError,
                                        "rows missing artifact roles"):
                hc.audit_capture_readiness(manifest_path, plan_path)

            manifest["capture_plan"]["sha256"] = "0" * 64
            hc.write_json(manifest_path, manifest)
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "does not match the plan"):
                hc.audit_capture_readiness(manifest_path, plan_path)

    def test_c1_through_c5_analysis_recovers_neutral_measurements(self) -> None:
        sample_rate = 48000
        frames = 1024
        tone = [0.4 * math.sin(2.0 * math.pi * 1000.0 * frame / sample_rate)
                for frame in range(frames)]
        loaded = hc.WavData(sample_rate, 1, tone)
        recorded = hc.WavData(sample_rate, 1, [value * 0.5 for value in tone])
        c1 = hc.analyze_c1(recorded, loaded)
        self.assertLess(c1["measurements"]["residual_db_relative_to_reference"], -250.0)
        self.assertAlmostEqual(c1["measurements"]["gain_applied_to_measured"], 2.0, places=10)

        c2 = hc.analyze_c2([(0.0, "fixed-period-tone", loaded, loaded)])
        self.assertAlmostEqual(c2["rows"][0]["dominant_frequency_hz"], 984.375, places=3)

        c3 = hc.analyze_c3(loaded, hc.WavData(sample_rate, 1, [value * 0.1 for value in tone]))
        self.assertLess(c3["measurements"]["residual_db_relative_to_reference"], -250.0)

        active = hc.WavData(sample_rate, 1, [0.01 * math.sin(
            2.0 * math.pi * 200.0 * frame / sample_rate) for frame in range(frames)])
        idle = hc.WavData(sample_rate, 1, [value * 0.1 for value in active.samples])
        c4 = hc.analyze_c4(active, idle)
        self.assertAlmostEqual(c4["active_minus_idle_rms_db"], 20.0, places=6)

        impulse = [0.0] * frames
        impulse[7] = 1.0
        step = [0.0] * 10 + [0.75] * (frames - 10)
        c5 = hc.analyze_c5([
            (0.0, "unit-impulse", hc.WavData(sample_rate, 1, impulse)),
            (0.0, "unit-step", hc.WavData(sample_rate, 1, step)),
        ])
        impulse_row = next(row for row in c5["rows"] if row["stimulus"] == "unit-impulse")
        step_row = next(row for row in c5["rows"] if row["stimulus"] == "unit-step")
        self.assertEqual(impulse_row["peak_frame"], 7)
        self.assertAlmostEqual(step_row["final_value"], 0.75)

    def test_c2_swept_sine_tracks_the_full_fold_evolution_and_rejects_wrong_order(self) -> None:
        sample_rate = 48000
        frames = 8192
        phase = 0.0
        sweep = []
        for frame in range(frames):
            frequency = 300.0 + 9000.0 * frame / (frames - 1)
            phase += 2.0 * math.pi * frequency / sample_rate
            sweep.append(0.5 * math.sin(phase))
        reference = hc.WavData(sample_rate, 1, sweep)
        positive = hc.analyze_swept_fold_evolution(reference, reference, 0.0)
        negative = hc.analyze_swept_fold_evolution(
            reference, hc.WavData(sample_rate, 1, list(reversed(sweep))), 0.0)
        self.assertEqual(positive["first_window_start_frame"], 0)
        self.assertEqual(positive["last_window_end_frame"], frames)
        self.assertEqual(positive["maximum_fold_error_hz"], 0.0)
        self.assertGreater(negative["median_fold_error_hz"], 1000.0)

    def test_adaptive_c6_recovers_variable_segments_factor_and_splices(self) -> None:
        source = hc.make_indexed_impulse_basis(16384)
        segments = [64, 80, 72, 96, 56, 88, 68, 92]
        widths = [0, 8, 12, 6, 10, 14, 4, 16]
        capture = hc.oracle_adaptive(source, segments, 1.75, widths)
        recovered = hc.recover_adaptive(source, capture)
        self.assertEqual(recovered["segment_frames"][:7], segments[:7])
        self.assertEqual(recovered["splice_frames"][:7], widths[1:])
        self.assertAlmostEqual(recovered["factor"], 1.75, delta=0.02)
        self.assertTrue(recovered["variable_segment_lengths"])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            hc.write_float_wav(root / "source.wav", hc.WavData(48000, 1, source))
            hc.write_float_wav(root / "capture.wav", hc.WavData(48000, 1, capture))
            request = {
                "schema": hc.ANALYSIS_REQUEST_SCHEMA,
                "analyses": [{
                    "id": "adaptive-grid",
                    "protocol": "C6-adaptive",
                    "inputs": {
                        "source": "source.wav",
                        "captures": [{
                            "path": "capture.wav",
                            "factor_percent": 175,
                            "cycle_ms": "auto",
                            "adaptive_quality": 50,
                            "adaptive_width": 25,
                        }],
                    },
                }],
            }
            request_path = root / "request.json"
            hc.write_json(request_path, request)
            report = hc.run_analysis_request(request_path)
            row = report["analyses"][0]["rows"][0]
            self.assertAlmostEqual(row["measurements"]["factor"], 1.75, delta=0.02)
            self.assertTrue(row["declared_vs_recovered"]["matched"])
            self.assertEqual(set(report["analyses"][0]["input_sha256"]),
                             {"source.wav", "capture.wav"})
            request["analyses"][0]["inputs"]["captures"][0]["factor_percent"] = 300
            hc.write_json(request_path, request)
            with self.assertRaisesRegex(hc.HeritageCalibrationError,
                                        "declared/recovered mismatch: factor"):
                hc.run_analysis_request(request_path)

    def test_analysis_request_is_path_bounded_and_machine_readable(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            samples = [0.2 * math.sin(2.0 * math.pi * frame / 32.0) for frame in range(256)]
            hc.write_float_wav(root / "loaded.wav", hc.WavData(48000, 1, samples))
            hc.write_float_wav(root / "recorded.wav", hc.WavData(
                48000, 1, [value * 0.75 for value in samples]))
            request = {
                "schema": hc.ANALYSIS_REQUEST_SCHEMA,
                "analyses": [{"id": "record-null", "protocol": "C1", "inputs": {
                    "recorded_replay": "recorded.wav", "loaded_replay": "loaded.wav"}}],
            }
            request_path = root / "analysis.json"
            hc.write_json(request_path, request)
            report = hc.run_analysis_request(request_path)
            self.assertEqual(report["schema"], hc.ANALYSIS_REPORT_SCHEMA)
            self.assertEqual(report["analyses"][0]["protocol"], "C1")
            request["analyses"][0]["inputs"]["loaded_replay"] = "../outside.wav"
            hc.write_json(request_path, request)
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "normalized relative path"):
                hc.run_analysis_request(request_path)
            request["analyses"][0]["inputs"]["loaded_replay"] = str(root / "loaded.wav")
            hc.write_json(request_path, request)
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "normalized relative path"):
                hc.run_analysis_request(request_path)

    def test_cyclic_recovery_math_recovers_parameters_and_rejects_wrong_law(self) -> None:
        source = hc.make_indexed_impulse_basis(8192)
        capture = hc.oracle_cyclic(source, 1536, 1.75, 64, 8)
        recovered = hc.recover_cyclic(source, capture)
        self.assertAlmostEqual(recovered["factor"], 1.75, delta=0.02)
        self.assertEqual(recovered["cycle_frames"], 64)
        self.assertEqual(recovered["splice_frames"], 8)
        sparse = hc.make_sparse_impulse_fixture(8192)
        correct = hc.oracle_cyclic(sparse, 1536, 1.75, 64, 8)
        wrong = hc.oracle_cyclic(sparse, 1536, 1.75, 64, 8, wrong_law=True)
        self.assertGreater(max(abs(a - b) for a, b in zip(correct, wrong)), 0.1)

    @unittest.skipUnless(os.environ.get("PULP_HERITAGE_CYCLIC_RENDER_WAV"),
                         "configured CTest supplies the Pulp product renderer")
    def test_product_cyclic_bootstrap_covers_c6_range(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            renderer = Path(os.environ["PULP_HERITAGE_CYCLIC_RENDER_WAV"])
            for factor in (0.5, 1.75, 20.0):
                for cycle_frames in (240, 3120):
                    for splice_frames in (8, 64):
                        with self.subTest(factor=factor, cycle_frames=cycle_frames,
                                          splice_frames=splice_frames):
                            report = hc.bootstrap_cyclic(
                                Path(temp) / f"f{factor}-c{cycle_frames}-s{splice_frames}",
                                factor, cycle_frames, splice_frames, renderer)
                            self.assertTrue(report["passed"])
                            self.assertAlmostEqual(
                                report["recovered"]["factor"], factor,
                                delta=max(0.02, factor * 0.005))
                            self.assertEqual(
                                report["recovered"]["cycle_frames"], cycle_frames)
                            self.assertEqual(
                                report["recovered"]["splice_frames"], splice_frames)
                            self.assertTrue(report["impulse_holdout"]["matched"])
                            self.assertTrue(report["negative_control"]["rejected"])

    @unittest.skipUnless(os.environ.get("PULP_HERITAGE_CYCLIC_RENDER_WAV"),
                         "configured CTest supplies the Pulp product renderer")
    def test_product_cyclic_unity_is_bit_transparent(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            renderer = Path(os.environ["PULP_HERITAGE_CYCLIC_RENDER_WAV"])
            _, _, source, capture = hc._product_render(
                renderer, Path(temp), "sparse", 1.0, 240, 64, 240 * 24)
            self.assertEqual(source, capture)

    @unittest.skipUnless(os.environ.get("PULP_HERITAGE_ADAPTIVE_RENDER_WAV"),
                         "configured CTest supplies the Pulp adaptive renderer")
    def test_product_adaptive_bootstrap_recovers_declared_settings(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = hc.bootstrap_adaptive(
                Path(temp) / "adaptive", 1.75, 64, 8, 1, 8,
                Path(os.environ["PULP_HERITAGE_ADAPTIVE_RENDER_WAV"]), 8192)
            self.assertTrue(report["passed"])
            self.assertTrue(report["declared_vs_recovered"]["matched"])
            self.assertLessEqual(
                report["declared_vs_recovered"]["comparisons"]
                ["decision_hop_samples"]["maximum_error"], 1)
            self.assertLessEqual(
                report["declared_vs_recovered"]["comparisons"]
                ["crossfade_samples"]["maximum_error"], 1)

    def test_capture_manifest_verifies_hashes_and_rejects_host_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            artifact = root / "impulse.wav"
            hc.write_float_wav(artifact, hc.WavData(48000, 1, [0.0, 1.0, 0.0]))
            manifest = {
                "schema": hc.CAPTURE_SCHEMA,
                "session_id": "session-2026-07-a",
                "captured_at": "2026-07-19T20:00:00Z",
                "operator_id": "operator-a",
                "target": {
                    "manufacturer": "Example Audio",
                    "model": "Sampler 1",
                    "serial": "redacted-public-copy",
                    "revision": "rev-a",
                },
                "conditions": {
                    "psu_and_calibration_state": "warmed 30 minutes; calibrated",
                    "temperature_c": 22.5,
                    "gain_staging": "0 dBFS stimulus; unity replay",
                },
                "capture_chain": [{"stage": "converter", "description": "line input at 48 kHz"}],
                "artifacts": [{
                    "path": artifact.name,
                    "sha256": hc.sha256_file(artifact),
                    "bytes": artifact.stat().st_size,
                    "test_id": "C6-impulse-factor-175",
                    "role": "capture",
                }],
                "trademark_notice": hc.TRADEMARK_NOTICE,
            }
            manifest_path = root / "session.json"
            hc.write_json(manifest_path, manifest)
            self.assertTrue(hc.verify_capture_manifest(manifest_path)["ok"])

            manifest["artifacts"][0]["sha256"] = manifest["artifacts"][0]["sha256"].upper()
            hc.write_json(manifest_path, manifest)
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "lowercase SHA-256"):
                hc.verify_capture_manifest(manifest_path)
            manifest["artifacts"][0]["sha256"] = hc.sha256_file(artifact)

            manifest["recording_host"] = "development-mac"
            hc.write_json(manifest_path, manifest)
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "development-host"):
                hc.verify_capture_manifest(manifest_path)

            manifest.pop("recording_host")
            manifest["conditions"]["temperature_c"] = None
            manifest["capture_chain"] = [None]
            hc.write_json(manifest_path, manifest)
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "temperature_c"):
                hc.verify_capture_manifest(manifest_path)

    def test_capture_manifest_detects_artifact_tampering(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            artifact = root / "capture.bin"
            artifact.write_bytes(b"original")
            manifest = {
                "schema": hc.CAPTURE_SCHEMA,
                "session_id": "session-a",
                "captured_at": "2026-07-19T20:00:00Z",
                "operator_id": "operator-a",
                "target": {"manufacturer": "M", "model": "X", "serial": "S", "revision": "R"},
                "conditions": {"psu_and_calibration_state": "ok", "temperature_c": 22.0,
                               "gain_staging": "documented"},
                "capture_chain": [{"stage": "adc", "description": "capture"}],
                "artifacts": [{"path": artifact.name, "sha256": hc.sha256_file(artifact),
                               "bytes": 8, "test_id": "C1", "role": "capture"}],
                "trademark_notice": hc.TRADEMARK_NOTICE,
            }
            path = root / "session.json"
            hc.write_json(path, manifest)
            artifact.write_bytes(b"tampered")
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "SHA-256 mismatch"):
                hc.verify_capture_manifest(path)

    def test_listening_pack_is_keyed_blinded_level_matched_and_verifiable(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            pairs = []
            for index in range(4):
                ref = root / f"ref-{index}.wav"
                cand = root / f"cand-{index}.wav"
                wave = [0.2 * ((frame % 19) - 9) / 9 for frame in range(512)]
                hc.write_float_wav(ref, hc.WavData(48000, 1, wave))
                hc.write_float_wav(cand, hc.WavData(48000, 1, [value * (0.3 + 0.1 * index) for value in wave]))
                pairs.append({"pair_id": f"recipe-{index}", "reference": ref.name,
                              "candidate": cand.name})
            pairs_path = root / "pairs.json"
            hc.write_json(pairs_path, {"schema": "pulp.heritage.listening-pairs.v1", "pairs": pairs})
            key_path = root / "key.bin"
            key_path.write_bytes(b"deterministic-test-key-material")
            answers_path = root / "answers.json"
            pack_dir = root / "pack"

            first = hc.generate_listening_pack(pairs_path, pack_dir, answers_path, key_path)
            first_bytes = (pack_dir / "manifest.json").read_bytes()
            second_dir = root / "pack-second"
            second_answers = root / "answers-second.json"
            second = hc.generate_listening_pack(
                pairs_path, second_dir, second_answers, key_path)
            self.assertEqual(first, second)
            self.assertEqual(first_bytes, (second_dir / "manifest.json").read_bytes())
            public = json.loads(first_bytes)
            self.assertTrue(all("pair_id" not in pair for pair in public["pairs"]))
            self.assertTrue(all("role" not in artifact
                                for pair in public["pairs"] for artifact in pair["artifacts"]))
            result = hc.verify_listening_pack(pack_dir / "manifest.json", answers_path, key_path)
            self.assertEqual(result["pairs"], 4)

            first_wav = pack_dir / first["pairs"][0]["artifacts"][0]["path"]
            first_wav.write_bytes(first_wav.read_bytes() + b"x")
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "hash mismatch"):
                hc.verify_listening_pack(pack_dir / "manifest.json", answers_path, key_path)

            arbitrary = hc.WavData(48000, 1, [0.125, -0.125] * 256)
            public_manifest = json.loads(first_bytes)
            for artifact_record in public_manifest["pairs"][0]["artifacts"]:
                path = pack_dir / artifact_record["path"]
                hc.write_float_wav(path, arbitrary)
                artifact_record["sha256"] = hc.sha256_file(path)
            hc.write_json(pack_dir / "manifest.json", public_manifest)
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "keyed role"):
                hc.verify_listening_pack(pack_dir / "manifest.json", answers_path, key_path)

    def test_listening_key_is_not_embedded_in_manifests(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            ref = root / "r.wav"
            cand = root / "c.wav"
            hc.write_float_wav(ref, hc.WavData(48000, 1, [0.1, -0.1] * 64))
            hc.write_float_wav(cand, hc.WavData(48000, 1, [0.2, -0.2] * 64))
            pairs = root / "pairs.json"
            hc.write_json(pairs, {"schema": "pulp.heritage.listening-pairs.v1", "pairs": [
                {"pair_id": "one", "reference": "r.wav", "candidate": "c.wav"}]})
            secret = b"this-key-must-remain-outside-artifacts"
            key = root / "key"
            key.write_bytes(secret)
            answers = root / "answers.json"
            pack = root / "pack"
            hc.generate_listening_pack(pairs, pack, answers, key)
            self.assertNotIn(secret, (pack / "manifest.json").read_bytes())
            self.assertNotIn(secret, answers.read_bytes())
            self.assertNotIn("answers_sha256", json.loads((pack / "manifest.json").read_text()))

    def test_listener_pack_refuses_answer_or_key_leakage(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            ref = root / "r.wav"
            cand = root / "c.wav"
            hc.write_float_wav(ref, hc.WavData(48000, 1, [0.1, -0.1] * 64))
            hc.write_float_wav(cand, hc.WavData(48000, 1, [0.2, -0.2] * 64))
            pairs = root / "pairs.json"
            hc.write_json(pairs, {"schema": "pulp.heritage.listening-pairs.v1", "pairs": [
                {"pair_id": "one", "reference": "r.wav", "candidate": "c.wav"}]})
            key = root / "key"
            key.write_bytes(b"private-listening-key")
            pack = root / "pack"
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "outside"):
                hc.generate_listening_pack(pairs, pack, pack / "answers.json", key)

    def test_listener_pack_is_peak_safe_and_float_wav_has_fact_chunk(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            reference = root / "reference.wav"
            candidate = root / "candidate.wav"
            hc.write_float_wav(reference, hc.WavData(48000, 1, [0.2, -0.2] * 128))
            hc.write_float_wav(candidate, hc.WavData(48000, 1, [1.0] + [0.0] * 255))
            self.assertIn(b"fact", reference.read_bytes())
            pairs = root / "pairs.json"
            hc.write_json(pairs, {"schema": "pulp.heritage.listening-pairs.v1", "pairs": [
                {"pair_id": "peak", "reference": reference.name, "candidate": candidate.name}]})
            key = root / "key"
            key.write_bytes(b"peak-safe-listening-key")
            pack = root / "pack"
            answers = root / "answers.json"
            manifest = hc.generate_listening_pack(pairs, pack, answers, key)
            peaks = []
            for artifact in manifest["pairs"][0]["artifacts"]:
                wav = hc.read_wav(pack / artifact["path"])
                peaks.append(max(abs(value) for value in wav.samples))
            self.assertLessEqual(max(peaks), 0.981)
            self.assertTrue(hc.verify_listening_pack(pack / "manifest.json", answers, key)["ok"])

    def test_pair_source_paths_must_stay_beside_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            inside = root / "inside"
            inside.mkdir()
            outside = root / "outside.wav"
            local = inside / "local.wav"
            hc.write_float_wav(outside, hc.WavData(48000, 1, [0.1, -0.1] * 64))
            hc.write_float_wav(local, hc.WavData(48000, 1, [0.1, -0.1] * 64))
            pairs = inside / "pairs.json"
            hc.write_json(pairs, {"schema": "pulp.heritage.listening-pairs.v1", "pairs": [
                {"pair_id": "escape", "reference": "../outside.wav", "candidate": "local.wav"}]})
            key = root / "key"
            key.write_bytes(b"path-containment-key")
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "normalized relative path"):
                hc.generate_listening_pack(pairs, root / "pack", root / "answers.json", key)

    def test_answer_path_cannot_overwrite_key_or_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            reference = root / "reference.wav"
            candidate = root / "candidate.wav"
            hc.write_float_wav(reference, hc.WavData(48000, 1, [0.1, -0.1] * 64))
            hc.write_float_wav(candidate, hc.WavData(48000, 1, [0.1, -0.1] * 64))
            pairs = root / "pairs.json"
            hc.write_json(pairs, {"schema": "pulp.heritage.listening-pairs.v1", "pairs": [
                {"pair_id": "collision", "reference": reference.name,
                 "candidate": candidate.name}]})
            key = root / "key"
            original_key = b"collision-safe-key"
            key.write_bytes(original_key)
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "distinct"):
                hc.generate_listening_pack(pairs, root / "pack", key, key)
            self.assertEqual(key.read_bytes(), original_key)


if __name__ == "__main__":
    unittest.main()
