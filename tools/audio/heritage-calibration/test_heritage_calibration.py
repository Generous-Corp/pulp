from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path

import heritage_calibration as hc


class HeritageCalibrationTest(unittest.TestCase):
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
            with self.assertRaisesRegex(hc.HeritageCalibrationError, "escapes"):
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
