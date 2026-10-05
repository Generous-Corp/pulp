#!/usr/bin/env python3
"""Private, default-off MLX probe for the checked-in NAM WaveNet/A1 fixture.

This tool deliberately stays outside Pulp's public ABI and build graph.  It
loads the immutable fixture on one owning worker thread, evaluates an MLX copy
and a CPU shadow on identical samples, and emits evidence fields needed for a
future adapter.  It does not constitute Pulp transport or product evidence.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import statistics
import threading
import time
from dataclasses import asdict, dataclass
from pathlib import Path


def _activation(name: str, value: float) -> float:
    if name == "ReLU":
        return max(0.0, value)
    if name == "Hardtanh":
        return max(-1.0, min(1.0, value))
    if name == "Sigmoid":
        return 1.0 / (1.0 + math.exp(-value))
    if name == "Identity":
        return value
    return math.tanh(value)


class CpuConv:
    def __init__(self, inp: int, out: int, kernel: int, dilation: int, bias: bool):
        self.inp, self.out, self.kernel, self.dilation = inp, out, kernel, dilation
        self.bias = bias
        self.weights = [[[0.0] * inp for _ in range(out)] for _ in range(kernel)]
        self.biases = [0.0] * out if bias else []
        self.slots = (kernel - 1) * dilation + 1
        self.history = [[0.0] * inp for _ in range(self.slots)]
        self.head = 0

    def load(self, weights: list[float], cursor: int) -> int:
        for o in range(self.out):
            for i in range(self.inp):
                for k in range(self.kernel):
                    self.weights[k][o][i] = weights[cursor]
                    cursor += 1
        if self.bias:
            self.biases[:] = weights[cursor:cursor + self.out]
            cursor += self.out
        return cursor

    def step(self, x: list[float]) -> list[float]:
        self.history[self.head] = list(x)
        y = list(self.biases) if self.bias else [0.0] * self.out
        for k in range(self.kernel):
            slot = self.head - (self.kernel - 1 - k) * self.dilation
            while slot < 0:
                slot += self.slots
            past = self.history[slot]
            for o in range(self.out):
                y[o] += sum(self.weights[k][o][i] * past[i] for i in range(self.inp))
        self.head = (self.head + 1) % self.slots
        return y


class MlxConv:
    def __init__(self, mx, inp: int, out: int, kernel: int, dilation: int, bias: bool):
        self.mx, self.inp, self.out, self.kernel, self.dilation = mx, inp, out, kernel, dilation
        self.bias = bias
        self.weights = [None] * kernel
        self.biases = None
        self.slots = (kernel - 1) * dilation + 1
        self.history = [mx.zeros((inp,), dtype=mx.float32) for _ in range(self.slots)]
        self.head = 0

    def load(self, weights: list[float], cursor: int) -> int:
        rows = []
        for k in range(self.kernel):
            rows.append([[0.0] * self.inp for _ in range(self.out)])
        for o in range(self.out):
            for i in range(self.inp):
                for k in range(self.kernel):
                    rows[k][o][i] = weights[cursor]
                    cursor += 1
        self.weights = [self.mx.array(row, dtype=self.mx.float32) for row in rows]
        self.biases = self.mx.array(weights[cursor:cursor + self.out], dtype=self.mx.float32) if self.bias else self.mx.zeros((self.out,), dtype=self.mx.float32)
        if self.bias:
            cursor += self.out
        self.mx.eval(*self.weights, self.biases)
        return cursor

    def step(self, x):
        self.history[self.head] = x
        y = self.biases
        for k, weight in enumerate(self.weights):
            slot = self.head - (self.kernel - 1 - k) * self.dilation
            while slot < 0:
                slot += self.slots
            y = y + self.mx.matmul(weight, self.history[slot])
        self.head = (self.head + 1) % self.slots
        self.mx.eval(y)
        return y


class CpuNam:
    def __init__(self, doc: dict):
        self.scale = float(doc["config"]["head_scale"])
        self.arrays = []
        cursor = 0
        for cfg in doc["config"]["layers"]:
            channels = int(cfg["channels"])
            array = {
                "cfg": cfg,
                "rechannel": CpuConv(int(cfg["input_size"]), channels, 1, 1, False),
                "layers": [],
                "head": CpuConv(channels, int(cfg["head_size"]), 1, 1, bool(cfg.get("head_bias", False))),
            }
            for dilation in cfg["dilations"]:
                out = channels * (2 if cfg.get("gated", False) else 1)
                array["layers"].append({
                    "conv": CpuConv(channels, out, int(cfg["kernel_size"]), int(dilation), True),
                    "mixin": CpuConv(1, out, 1, 1, False),
                    "residual": CpuConv(channels, channels, 1, 1, True),
                })
            cursor = array["rechannel"].load(doc["weights"], cursor)
            for layer in array["layers"]:
                cursor = layer["conv"].load(doc["weights"], cursor)
                cursor = layer["mixin"].load(doc["weights"], cursor)
                cursor = layer["residual"].load(doc["weights"], cursor)
            cursor = array["head"].load(doc["weights"], cursor)
            self.arrays.append(array)
        if cursor != len(doc["weights"]) - 1 or abs(float(doc["weights"][cursor]) - self.scale) > 1e-6:
            raise ValueError("serialized head scale or weight cursor mismatch")

    def sample(self, value: float) -> float:
        current = [value]
        prior = None
        for array in self.arrays:
            current = array["rechannel"].step(current)
            acc = [0.0] * len(current)
            if prior is not None:
                acc[:len(prior)] = prior
            for layer in array["layers"]:
                z = layer["conv"].step(current)
                mix = layer["mixin"].step([value])
                z = [a + b for a, b in zip(z, mix)]
                channels = len(current)
                if array["cfg"].get("gated", False):
                    head = [_activation(array["cfg"].get("activation", "Tanh"), z[i]) * _activation("Sigmoid", z[channels + i]) for i in range(channels)]
                else:
                    head = [_activation(array["cfg"].get("activation", "Tanh"), x) for x in z]
                nxt = layer["residual"].step(head)
                current = [a + b for a, b in zip(current, nxt)]
                acc = [a + b for a, b in zip(acc, head)]
            prior = array["head"].step(acc)
        return self.scale * prior[0]


class MlxNam:
    def __init__(self, mx, doc: dict):
        self.mx, self.scale = mx, float(doc["config"]["head_scale"])
        self.arrays = []
        cursor = 0
        for cfg in doc["config"]["layers"]:
            channels = int(cfg["channels"])
            array = {"cfg": cfg, "rechannel": MlxConv(mx, int(cfg["input_size"]), channels, 1, 1, False), "layers": [], "head": MlxConv(mx, channels, int(cfg["head_size"]), 1, 1, bool(cfg.get("head_bias", False)))}
            for dilation in cfg["dilations"]:
                out = channels * (2 if cfg.get("gated", False) else 1)
                array["layers"].append({"conv": MlxConv(mx, channels, out, int(cfg["kernel_size"]), int(dilation), True), "mixin": MlxConv(mx, 1, out, 1, 1, False), "residual": MlxConv(mx, channels, channels, 1, 1, True)})
            cursor = array["rechannel"].load(doc["weights"], cursor)
            for layer in array["layers"]:
                cursor = layer["conv"].load(doc["weights"], cursor)
                cursor = layer["mixin"].load(doc["weights"], cursor)
                cursor = layer["residual"].load(doc["weights"], cursor)
            cursor = array["head"].load(doc["weights"], cursor)
            self.arrays.append(array)
        if cursor != len(doc["weights"]) - 1 or abs(float(doc["weights"][cursor]) - self.scale) > 1e-6:
            raise ValueError("serialized head scale or weight cursor mismatch")
        mx.eval(*[w for a in self.arrays for w in self._weights(a)])

    @staticmethod
    def _weights(array):
        for c in [array["rechannel"], array["head"]]:
            yield from c.weights
            yield c.biases
        for layer in array["layers"]:
            for c in [layer["conv"], layer["mixin"], layer["residual"]]:
                yield from c.weights
                yield c.biases

    def sample(self, value: float) -> float:
        x = self.mx.array([value], dtype=self.mx.float32)
        condition = x
        prior = None
        for array in self.arrays:
            current = array["rechannel"].step(x)
            acc = self.mx.zeros((len(current),), dtype=self.mx.float32)
            if prior is not None:
                acc = acc + prior
            for layer in array["layers"]:
                z = layer["conv"].step(current) + layer["mixin"].step(condition)
                channels = len(current)
                if array["cfg"].get("gated", False):
                    head = self.mx.tanh(z[:channels]) * (1.0 / (1.0 + self.mx.exp(-z[channels:])))
                else:
                    head = self.mx.tanh(z)
                current = current + layer["residual"].step(head)
                acc = acc + head
            prior = array["head"].step(acc)
            x = current
        result = self.scale * prior[0]
        self.mx.eval(result)
        return float(result.item())


@dataclass
class WorkerResult:
    worker: int
    owner_thread_id: int
    release_thread_id: int
    provider: str
    model_id: str
    eval_count: int
    max_residual: float
    parity_failures: int
    deadline_misses: int
    fallback_blocks: int
    active_memory_before: int
    active_memory_after_prepare: int
    active_memory_after_release: int
    peak_memory: int
    service_us_p50: float
    service_us_p95: float
    error: str = ""


def _percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    return statistics.quantiles(values, n=100, method="inclusive")[int(p) - 1] if len(values) > 1 else values[0]


def _worker(index: int, blocks: int, model_path: Path, out: list[WorkerResult], barrier: threading.Barrier, timeout: float) -> None:
    owner = threading.get_ident()
    try:
        import mlx.core as mx
        doc = json.loads(model_path.read_text())
        cpu = CpuNam(doc)
        before = int(mx.get_active_memory())
        mlx = MlxNam(mx, doc)
        after_prepare = int(mx.get_active_memory())
        barrier.wait(timeout=timeout)
        services, misses, residual, failures = [], 0, 0.0, 0
        period_ns = int(32 * 1_000_000_000 / int(doc["sample_rate"]))
        next_deadline = time.perf_counter_ns()
        for block in range(blocks):
            next_deadline += period_ns
            delay = next_deadline - time.perf_counter_ns()
            if delay > 0:
                time.sleep(delay / 1_000_000_000)
            begin = time.perf_counter_ns()
            for frame in range(32):
                sample = math.sin((block * 32 + frame) * 0.071) + 0.25 * math.cos((block * 32 + frame) * 0.013)
                expected = cpu.sample(sample)
                actual = mlx.sample(sample)
                delta = abs(actual - expected)
                residual = max(residual, delta)
                failures += int(delta > 1e-5)
            elapsed = (time.perf_counter_ns() - begin) / 1000.0
            services.append(elapsed)
            misses += int(elapsed * 1000 > period_ns)
        mx.eval()
        peak = int(mx.get_peak_memory())
        active = int(mx.get_active_memory())
        del mlx
        mx.clear_cache()
        after_release = int(mx.get_active_memory())
        out.append(WorkerResult(index, owner, threading.get_ident(), "mlx", str(doc.get("metadata", {}).get("name", "")), blocks * 32, residual, failures, misses, 0, before, after_prepare, after_release, peak, _percentile(services, 50), _percentile(services, 95)))
    except Exception as exc:
        try:
            barrier.abort()
        except threading.BrokenBarrierError:
            pass
        out.append(WorkerResult(index, owner, threading.get_ident(), "unavailable", "", 0, 0.0, 0, 0, 0, 0, 0, 0, 0, 0.0, 0.0, f"{type(exc).__name__}: {exc}"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--blocks", type=int, default=8)
    parser.add_argument("--instances", type=int, choices=(1, 2), default=2)
    parser.add_argument("--worker-timeout-seconds", type=float, default=30.0)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.blocks <= 0 or not args.model.is_file():
        parser.error("--blocks must be positive and --model must be a file")
    try:
        import mlx.core as mx
        version, device = getattr(mx, "__version__", "unknown"), str(mx.default_device())
    except Exception as exc:
        payload = {"status": "unavailable", "reason": f"{type(exc).__name__}: {exc}", "phase3_gate": "not_claimed"}
        print(json.dumps(payload, sort_keys=True) if args.json else payload["reason"])
        return 2
    barrier, results = threading.Barrier(args.instances), []
    threads = [threading.Thread(target=_worker, args=(i, args.blocks, args.model, results, barrier, args.worker_timeout_seconds), name=f"mlx-nam-worker-{i}", daemon=True) for i in range(args.instances)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(args.worker_timeout_seconds)
    alive = [thread.name for thread in threads if thread.is_alive()]
    payload = {
        "status": "error" if alive or any(r.error for r in results) else "ok",
        "model_id": "nam.example",
        "model_name": "Test Model",
        "model_sha256": hashlib.sha256(args.model.read_bytes()).hexdigest(),
        "mlx_version": version,
        "device": device,
        "host": {"machine": platform.machine(), "platform": platform.platform()},
        "instances": args.instances,
        "blocks": args.blocks,
        "workers": [asdict(r) for r in sorted(results, key=lambda r: r.worker)],
        "alive_workers": alive,
        "cpu_shadow": "private Python mirror of NamTcnArtifactAdapter arithmetic",
        "selected_provider": "mlx" if not alive and results else "unavailable",
        "fallback_contract": "cpu_control_available; Pulp transport fallback not exercised",
        "phase3_gate": "not_claimed: private harness has no Pulp transport or callback",
    }
    print(json.dumps(payload, indent=2, sort_keys=True) if args.json else f"{payload['status']}: {len(results)} worker(s)")
    return 1 if payload["status"] != "ok" else 0


if __name__ == "__main__":
    raise SystemExit(main())
