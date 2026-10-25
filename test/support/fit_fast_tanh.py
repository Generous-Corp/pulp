#!/usr/bin/env python3
"""Derive FastMath::tanh's [5/4] constrained-minimax coefficients.

FastMath::tanh (core/signal/include/pulp/signal/fast_math.hpp) evaluates

    f(x) = x * (a0 + a1 t + a2 t^2) / (1 + b1 t + b2 t^2),   t = x^2,

on an argument clamped to [-c, c]. This script fits a0..b2 to minimise the
maximum absolute error against double-precision tanh on [0, c], subject to
f(c) = 1, so the clamp meets the saturation value with no step. The only
target is numpy's double-precision tanh.

Two stages, both deterministic:
  1. A linearised rational minimax (Cheney & Loeb, Numer. Math. 3, 1961):
     bisection on the error bound over a linear-programming feasibility
     problem, here with the extra constraint f'(c) = 0, which gives a
     well-conditioned starting point.
  2. Nelder-Mead on the true maximum error over a dense grid with only
     f(c) = 1 imposed (a0 follows from the other four coefficients), for each
     clamp in a sweep; the clamp with the smallest total error wins. Total
     error is max(fit error on [0, c], 1 - tanh(c)), the second term being
     what the clamp costs beyond c. Each clamp starts from the previous
     clamp's result; that warm start, carried up the sweep, is what reaches
     the optimum, so run the full default sweep rather than a single clamp.

At the chosen clamp the total error equals the tail 1 - tanh(c): the fit's
own error on [0, c] sits below it, so the optimum is a set rather than a
point, and Nelder-Mead lands on different members of that set depending on
its path. The coefficients fast_math.hpp carries (SHIPPED below) are one such
member; the script reports their total error on the same grid next to the
fit it finds, and both equal the tail. The exhaustive float-precision check
lives in test/test_fast_math.cpp.

Requires numpy and scipy. Offline tooling: nothing in the build runs it.

Usage: fit_fast_tanh.py [--clamps 5.10:5.30:0.01] [--grid 20000]
"""

from __future__ import annotations

import argparse

import numpy as np
from scipy.optimize import linprog, minimize


# FastMath::tanh's coefficients (a0, a1, a2, b1, b2) at clamp SHIPPED_CLAMP.
SHIPPED_CLAMP = 5.24
SHIPPED = (0.9997622141279201, 0.10092608875679396, 0.0006321601824242231,
           0.4335808440239807, 0.01240458813388444)


def lp_start(c: float, n: int = 1500) -> np.ndarray:
    """Stage 1: value- and slope-constrained linearised minimax at clamp c."""
    x = np.linspace(0.0, c, n)
    y = np.tanh(x)
    t = x * x
    numer = np.stack([x, x * t, x * t * t], 1)
    denom = np.stack([t, t * t], 1)
    tc = c * c
    a_eq = [[c, c * tc, c * tc * tc, -tc, -tc * tc], [1.0, 3 * tc, 5 * tc * tc, -2 * c, -4 * c * tc]]
    b_eq = [1.0, 0.0]
    lo, hi, best = 0.0, 1e-2, None
    for _ in range(40):
        e = 0.5 * (lo + hi)
        a_ub = np.vstack([
            np.hstack([numer, -(y + e)[:, None] * denom]),
            np.hstack([-numer, (y - e)[:, None] * denom]),
            np.hstack([np.zeros((n, 3)), -denom]),
        ])
        b_ub = np.concatenate([y + e, -(y - e), np.full(n, 0.9)])
        r = linprog(np.zeros(5), A_ub=a_ub, b_ub=b_ub, A_eq=a_eq, b_eq=b_eq,
                    bounds=[(None, None)] * 5, method="highs")
        if r.status == 0:
            hi, best = e, r.x
        else:
            lo = e
    if best is None:
        raise SystemExit("linear program found no feasible start")
    return best


def coefficients(p: np.ndarray, c: float) -> tuple[float, ...]:
    """(a0, a1, a2, b1, b2) with a0 chosen so that f(c) = 1."""
    a1, a2, b1, b2 = p
    tc = c * c
    a0 = (1 + b1 * tc + b2 * tc * tc) / c - a1 * tc - a2 * tc * tc
    return a0, a1, a2, b1, b2


def total_error(p: np.ndarray, c: float, x: np.ndarray) -> float:
    a0, a1, a2, b1, b2 = coefficients(p, c)
    t = x * x
    q = 1 + b1 * t + b2 * t * t
    if q.min() <= 0:
        return 1.0
    f = x * (a0 + a1 * t + a2 * t * t) / q
    return max(float(np.abs(f - np.tanh(x)).max()), 1.0 - float(np.tanh(c)))


def refine(p: np.ndarray, c: float, grid: int) -> tuple[float, np.ndarray]:
    x = np.linspace(0.0, c, grid)
    for _ in range(3):
        r = minimize(total_error, p, args=(c, x), method="Nelder-Mead",
                     options={"maxiter": 40000, "xatol": 1e-14, "fatol": 1e-14})
        p = r.x
    return float(r.fun), p


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--clamps", default="5.10:5.30:0.01", help="start:stop:step clamp sweep")
    ap.add_argument("--grid", type=int, default=20000, help="dense grid points on [0, c]")
    args = ap.parse_args()
    start, stop, step = (float(v) for v in args.clamps.split(":"))
    clamps = np.round(np.arange(start, stop + 0.5 * step, step), 6)

    a0, a1, a2, b1, b2 = lp_start(5.0)
    p = np.array([a1, a2, b1, b2])
    best = None
    for c in clamps:
        err, p = refine(p, float(c), args.grid)
        print(f"clamp {c:.2f}: total error {err:.4g}")
        if best is None or err < best[0]:
            best = (err, float(c), p.copy())
    err, c, p = best
    print(f"\nbest clamp {c:.2f}, total error {err:.4g} (tail 1 - tanh(c) = {1 - np.tanh(c):.4g})")
    for name, value in zip(("a0", "a1", "a2", "b1", "b2"), coefficients(p, c)):
        print(f"  {name} = {float(value)!r}  (float {float(np.float32(value))!r})")
    x = np.linspace(0.0, SHIPPED_CLAMP, args.grid)
    a0, a1, a2, b1, b2 = SHIPPED
    t = x * x
    f = x * (a0 + a1 * t + a2 * t * t) / (1 + b1 * t + b2 * t * t)
    shipped = max(float(np.abs(f - np.tanh(x)).max()), 1.0 - float(np.tanh(SHIPPED_CLAMP)))
    tc = SHIPPED_CLAMP * SHIPPED_CLAMP
    edge = SHIPPED_CLAMP * (a0 + a1 * tc + a2 * tc * tc) / (1 + b1 * tc + b2 * tc * tc)
    print(f"\nshipped coefficients at clamp {SHIPPED_CLAMP}: total error {shipped:.4g}, "
          f"f(c) - 1 = {edge - 1:.2e}")


if __name__ == "__main__":
    main()
