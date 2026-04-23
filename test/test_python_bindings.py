#!/usr/bin/env python3
"""Smoke-test the built pybind11 extension directly from the build tree."""

from __future__ import annotations

import importlib.util
import math
import pathlib
import sys


def load_module(module_path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("pulp", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not create import spec for {module_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_python_bindings.py <built-module-path>")

    module_path = pathlib.Path(sys.argv[1]).resolve()
    pulp = load_module(module_path)

    assert pathlib.Path(pulp.__file__).resolve() == module_path

    param_range = pulp.ParamRange(0.0, 1.0, 0.5)
    assert math.isclose(param_range.normalize(0.5), 0.5)
    assert math.isclose(param_range.denormalize(0.25), 0.25)

    info = pulp.ParamInfo()
    info.id = 101
    info.name = "Gain"
    info.unit = "dB"
    info.range = param_range
    assert info.id == 101
    assert info.name == "Gain"
    assert info.unit == "dB"
    assert math.isclose(info.range.default_value, 0.5)

    midi = pulp.MidiBuffer()
    assert midi.empty()
    midi.add(pulp.MidiEvent.note_on(1, 60, 100))
    assert midi.size() == 1
    assert not midi.empty()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
