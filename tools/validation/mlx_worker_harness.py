#!/usr/bin/env python3
"""Private MLX worker probe; never part of the public plugin ABI.

All MLX object creation, evaluation, and release happen on the worker that owns
that instance. The probe is intentionally synthetic: it measures thread and
scheduling behavior, not model quality or a shipped model's performance.
"""
from __future__ import annotations

import argparse
import json
import platform
import statistics
import sys
import threading
import time
from dataclasses import asdict, dataclass


@dataclass
class WorkerResult:
    worker: int
    thread_id: int
    blocks: int
    frames: int
    sample_rate: int
    service_us_p50: float
    service_us_p95: float
    service_us_p99: float
    max_service_us: float
    deadline_misses: int
    eval_count: int
    owner_thread_consistent: bool
    release_thread_id: int
    resident_weight_bytes: int
    error: str = ""


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    return statistics.quantiles(values, n=100, method="inclusive")[int(p) - 1]


def run_worker(worker: int, blocks: int, frames: int, sample_rate: int,
               paced: bool, out: list[WorkerResult], barrier: threading.Barrier,
               startup_timeout_s: float) -> None:
    try:
        import mlx.core as mx  # imported and first used on the owning thread

        owner = threading.get_ident()
        # Small deterministic stand-in for a block-parallel model. It is not a
        # quality or throughput claim for a production neural architecture.
        weights = mx.ones((frames, frames), dtype=mx.float32)
        bias = mx.zeros((frames,), dtype=mx.float32)
        mx.eval(weights, bias)
        weight_bytes = frames * frames * 4 + frames * 4
        barrier.wait(timeout=startup_timeout_s)
        period_ns = int(frames * 1_000_000_000 / sample_rate)
        services: list[float] = []
        misses = 0
        start_ns = time.perf_counter_ns()
        next_deadline = start_ns
        for index in range(blocks):
            if paced:
                next_deadline += period_ns
                sleep_ns = next_deadline - time.perf_counter_ns()
                if sleep_ns > 0:
                    time.sleep(sleep_ns / 1_000_000_000)
            begin = time.perf_counter_ns()
            signal = mx.ones((frames,), dtype=mx.float32)
            result = mx.tanh(mx.add(mx.matmul(weights, signal), bias))
            mx.eval(result)
            elapsed = (time.perf_counter_ns() - begin) / 1000.0
            services.append(elapsed)
            if elapsed * 1000 > period_ns:
                misses += 1
        # Delete/evaluate on the same owner thread. The Python binding has no
        # explicit release primitive; dropping references here is the proof.
        del result, signal, weights, bias
        release_thread = threading.get_ident()
        out.append(WorkerResult(
            worker=worker, thread_id=owner, blocks=blocks, frames=frames,
            sample_rate=sample_rate, service_us_p50=percentile(services, 50),
            service_us_p95=percentile(services, 95),
            service_us_p99=percentile(services, 99),
            max_service_us=max(services, default=0.0),
            deadline_misses=misses, eval_count=blocks + 1,
            owner_thread_consistent=owner == release_thread,
            release_thread_id=release_thread,
            resident_weight_bytes=weight_bytes))
    except Exception as exc:  # receipt must preserve a failed worker attempt
        # Release peers waiting for initialization; otherwise one failed
        # worker can strand the remaining workers in barrier.wait() forever.
        try:
            barrier.abort()
        except threading.BrokenBarrierError:
            pass
        out.append(WorkerResult(
            worker=worker, thread_id=threading.get_ident(), blocks=blocks,
            frames=frames, sample_rate=sample_rate, service_us_p50=0,
            service_us_p95=0, service_us_p99=0, max_service_us=0,
            deadline_misses=0, eval_count=0, owner_thread_consistent=False,
            release_thread_id=threading.get_ident(), resident_weight_bytes=0,
            error=f"{type(exc).__name__}: {exc}"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--blocks", type=int, default=100_000)
    parser.add_argument("--frames", type=int, default=32)
    parser.add_argument("--sample-rate", type=int, default=48_000)
    parser.add_argument("--instances", type=int, choices=(1, 2), default=2)
    parser.add_argument("--unpaced", action="store_true")
    parser.add_argument(
        "--worker-timeout-seconds", type=float, default=30.0,
        help="maximum startup/join wait before the private probe reports a failure",
    )
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.blocks <= 0 or args.frames <= 0:
        parser.error("--blocks and --frames must be positive")
    if args.worker_timeout_seconds <= 0:
        parser.error("--worker-timeout-seconds must be positive")
    try:
        import mlx.core as mx
        mlx_version = getattr(mx, "__version__", "unknown")
        device = str(mx.default_device())
    except Exception as exc:
        payload = {"status": "unavailable", "reason": f"{type(exc).__name__}: {exc}"}
        print(json.dumps(payload, sort_keys=True) if args.json else payload["reason"])
        return 2

    barrier = threading.Barrier(args.instances)
    results: list[WorkerResult] = []
    threads = [threading.Thread(target=run_worker,
                                args=(i, args.blocks, args.frames, args.sample_rate,
                                      not args.unpaced, results, barrier,
                                      args.worker_timeout_seconds),
                                name=f"mlx-worker-{i}", daemon=True)
               for i in range(args.instances)]
    started = time.perf_counter_ns()
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=args.worker_timeout_seconds)
    alive_workers = [thread.name for thread in threads if thread.is_alive()]
    if alive_workers:
        # Daemon workers let the private probe terminate even if a provider call
        # is stuck. The output is deliberately an error receipt: no partial
        # worker result may be mistaken for a successful bounded run.
        payload = {
            "status": "error",
            "reason": "worker_timeout",
            "alive_workers": alive_workers,
            "worker_timeout_seconds": args.worker_timeout_seconds,
            "synthetic": True,
            "mlx_version": mlx_version,
            "device": device,
            "host": {"machine": platform.machine(), "platform": platform.platform()},
            "instances": args.instances,
            "paced": not args.unpaced,
            "blocks": args.blocks,
            "workers": [asdict(result) for result in sorted(results, key=lambda r: r.worker)],
            "phase3_gate": "not_claimed: synthetic workload and no audio callback/model oracle",
        }
        print(json.dumps(payload, indent=2, sort_keys=True) if args.json else
              f"MLX worker timeout after {args.worker_timeout_seconds:.3f}s: "
              f"{', '.join(alive_workers)}")
        return 1
    elapsed_s = (time.perf_counter_ns() - started) / 1_000_000_000
    payload = {
        "status": "ok" if results and all(not r.error for r in results) else "error",
        "synthetic": True,
        "mlx_version": mlx_version,
        "device": device,
        "host": {"machine": platform.machine(), "platform": platform.platform()},
        "instances": args.instances,
        "paced": not args.unpaced,
        "blocks": args.blocks,
        "worker_timeout_seconds": args.worker_timeout_seconds,
        "elapsed_seconds": elapsed_s,
        "workers": [asdict(result) for result in sorted(results, key=lambda r: r.worker)],
        "phase3_gate": "not_claimed: synthetic workload and no audio callback/model oracle",
    }
    print(json.dumps(payload, indent=2, sort_keys=True) if args.json else
          f"MLX {mlx_version} {device}: {len(results)} worker(s), {args.blocks} blocks, "
          f"{elapsed_s:.3f}s")
    return 0 if payload["status"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
