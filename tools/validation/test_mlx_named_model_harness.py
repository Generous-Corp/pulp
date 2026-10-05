import json
import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mlx_named_model_harness import CpuNam, MlxNam  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
