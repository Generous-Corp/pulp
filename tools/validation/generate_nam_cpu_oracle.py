#!/usr/bin/env python3
"""Independent scalar WaveNet-A1 oracle for the pinned example fixture.

This intentionally has no Pulp imports. It is a tiny reference runner used to
regenerate the checked-in vector in the NAM CPU receipt.
"""
import hashlib
import json
import math
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "test/fixtures/neural/example.nam"

def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]

class Conv:
    def __init__(self, ins, outs, kernel, dilation, bias):
        self.ins, self.outs, self.kernel, self.dilation = ins, outs, kernel, dilation
        self.slots = (kernel - 1) * dilation + 1
        self.bias = bias
        self.w = [[[0.0] * ins for _ in range(outs)] for _ in range(kernel)]
        self.b = [0.0] * outs
        self.history = [[0.0] * ins for _ in range(self.slots)]
        self.head = 0
    def load(self, weights, cursor):
        for o in range(self.outs):
            for i in range(self.ins):
                for k in range(self.kernel):
                    self.w[k][o][i] = weights[cursor[0]]; cursor[0] += 1
        if self.bias:
            self.b = weights[cursor[0]:cursor[0] + self.outs]; cursor[0] += self.outs
    def step(self, x):
        y = list(self.b)
        if self.kernel == 1:
            for o in range(self.outs):
                y[o] += sum(self.w[0][o][i] * x[i] for i in range(self.ins))
            return y
        self.history[self.head] = list(x)
        for k in range(self.kernel):
            slot = self.head - (self.kernel - 1 - k) * self.dilation
            slot %= self.slots
            for o in range(self.outs):
                y[o] += sum(self.w[k][o][i] * self.history[slot][i] for i in range(self.ins))
        self.head = (self.head + 1) % self.slots
        return y
    def reset(self):
        self.history = [[0.0] * self.ins for _ in range(self.slots)]; self.head = 0

class Array:
    def __init__(self, cfg):
        self.cfg = cfg; c, k = cfg["channels"], cfg["kernel_size"]
        self.rechannel = Conv(cfg["input_size"], c, 1, 1, False)
        self.layers = []
        for d in cfg["dilations"]:
            z = 2 * c if cfg.get("gated", False) else c
            layer = (Conv(c, z, k, d, True), Conv(1, z, 1, 1, False), Conv(c, c, 1, 1, True))
            self.layers.append(layer)
        self.head = Conv(c, cfg["head_size"], 1, 1, cfg.get("head_bias", False))
    def load(self, weights, cursor):
        self.rechannel.load(weights, cursor)
        c = self.cfg["channels"]
        for conv, mix, residual in self.layers:
            conv.load(weights, cursor); mix.load(weights, cursor); residual.load(weights, cursor)
        self.head.load(weights, cursor)
    def step(self, x, condition, prior):
        current = self.rechannel.step(x); acc = list(prior or [0.0] * self.cfg["channels"])
        skip = None
        for conv, mix, residual in self.layers:
            z = [a + b for a, b in zip(conv.step(current), mix.step(condition))]
            if self.cfg.get("gated", False):
                c = self.cfg["channels"]; h = [math.tanh(z[i]) * (1.0 / (1.0 + math.exp(-z[c+i]))) for i in range(c)]
            else: h = [math.tanh(v) for v in z[:self.cfg["channels"]]]
            nxt = residual.step(h); current = [a + b for a, b in zip(current, nxt)]
            acc = [a + b for a, b in zip(acc, h)]; skip = h
        return current, self.head.step(acc)

def main():
    raw = FIXTURE.read_bytes(); model = json.loads(raw); weights = model["weights"]
    arrays = [Array(c) for c in model["config"]["layers"]]; cursor = [0]
    for a in arrays: a.load(weights, cursor)
    scale = weights[cursor[0]]; assert cursor[0] + 1 == len(weights)
    values = [math.sin(i * 0.071) + 0.25 * math.cos(i * 0.013) for i in range(128)]
    out = []
    for value in values:
        x, prior = [value], None
        for a in arrays: x, prior = a.step(x, [value], prior)
        out.append(scale * prior[0])
    vector = ",".join(f"{v:.9g}" for v in out)
    print(f"fixture_sha256={hashlib.sha256(raw).hexdigest()}")
    print(f"vector_sha256={hashlib.sha256(vector.encode()).hexdigest()}")
    print(vector)

if __name__ == "__main__": main()
