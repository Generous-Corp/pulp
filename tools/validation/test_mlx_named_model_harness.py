import json
import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mlx_named_model_harness import BoundedSamples, CpuNam, MlxNam, validate_model_document  # noqa: E402


ROOT = Path(__file__).parents[2]
FIXTURE = ROOT / "test/fixtures/neural/example.nam"


class NamedModelHarnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = json.loads(FIXTURE.read_text())

    def test_cpu_shadow_matches_serialized_oracle_samples(self):
        model = CpuNam(self.doc)
        got = [model.sample(math.sin(i * 0.071) + 0.25 * math.cos(i * 0.013)) for i in range(4)]
        expected = [-0.01314363, -0.012158165, -0.014329166, -0.016419834]
        for actual, want in zip(got, expected):
            self.assertAlmostEqual(actual, want, places=6)

    def test_cpu_reset_replays_the_same_prefix(self):
        model = CpuNam(self.doc)
        samples = [math.sin(i * 0.071) + 0.25 * math.cos(i * 0.013) for i in range(8)]
        first = [model.sample(sample) for sample in samples]
        model.reset()
        replay = [model.sample(sample) for sample in samples]
        self.assertEqual(first, replay)

    def test_malformed_model_is_rejected_before_state_allocation(self):
        malformed = dict(self.doc)
        malformed.pop("config")
        with self.assertRaisesRegex(ValueError, "config"):
            validate_model_document(malformed)

        malformed = json.loads(json.dumps(self.doc))
        malformed["config"]["layers"][0]["condition_size"] = 2
        with self.assertRaisesRegex(ValueError, "conditioning"):
            CpuNam(malformed)

        malformed = json.loads(json.dumps(self.doc))
        malformed["weights"] = malformed["weights"][:-1]
        with self.assertRaisesRegex(ValueError, "weights length"):
            CpuNam(malformed)

    def test_timing_samples_are_bounded_and_deterministic(self):
        first = BoundedSamples(4)
        second = BoundedSamples(4)
        for value in range(100):
            first.append(float(value))
            second.append(float(value))
        self.assertEqual(first.count, 100)
        self.assertEqual(len(first.values), 4)
        self.assertEqual(first.values, second.values)
        self.assertTrue(all(0.0 <= value < 100.0 for value in first.values))

    def test_mlx_named_model_parity_when_runtime_is_available(self):
        try:
            import mlx.core as mx
        except ModuleNotFoundError:
            self.skipTest("MLX is optional and not installed in the default Python")
        cpu = CpuNam(self.doc)
        mlx = MlxNam(mx, self.doc)
        residual = 0.0
        for i in range(32):
            sample = math.sin(i * 0.071) + 0.25 * math.cos(i * 0.013)
            residual = max(residual, abs(cpu.sample(sample) - mlx.sample(sample)))
        self.assertLessEqual(residual, 1.0e-5)

    def test_mlx_reset_replays_the_same_prefix_when_runtime_is_available(self):
        try:
            import mlx.core as mx
        except ModuleNotFoundError:
            self.skipTest("MLX is optional and not installed in the default Python")
        model = MlxNam(mx, self.doc)
        samples = [math.sin(i * 0.071) + 0.25 * math.cos(i * 0.013) for i in range(8)]
        first = [model.sample(sample) for sample in samples]
        model.reset()
        replay = [model.sample(sample) for sample in samples]
        for actual, want in zip(replay, first):
            self.assertAlmostEqual(actual, want, places=6)


if __name__ == "__main__":
    unittest.main()
