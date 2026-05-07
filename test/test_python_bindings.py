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


def assert_param_range_bindings(pulp) -> None:
    default_range = pulp.ParamRange()
    assert math.isclose(default_range.min, 0.0)
    assert math.isclose(default_range.max, 1.0)
    assert math.isclose(default_range.default_value, 0.0)
    assert math.isclose(default_range.normalize(-10.0), 0.0)
    assert math.isclose(default_range.normalize(10.0), 1.0)
    assert math.isclose(default_range.denormalize(-1.0), 0.0)
    assert math.isclose(default_range.denormalize(2.0), 1.0)

    param_range = pulp.ParamRange(0.0, 1.0, 0.5)
    assert math.isclose(param_range.normalize(0.5), 0.5)
    assert math.isclose(param_range.denormalize(0.25), 0.25)

    param_range.min = -60.0
    param_range.max = 24.0
    param_range.default_value = -12.0
    assert math.isclose(param_range.normalize(-60.0), 0.0)
    assert math.isclose(param_range.normalize(24.0), 1.0)
    assert math.isclose(param_range.denormalize(0.5), -18.0)
    assert math.isclose(param_range.default_value, -12.0)

    equal_range = pulp.ParamRange(3.0, 3.0, 3.0)
    assert math.isclose(equal_range.normalize(3.0), 0.0)
    assert math.isclose(equal_range.denormalize(0.75), 3.0)

    clamped_range = pulp.ParamRange(-1.0, 1.0, 0.0)
    assert math.isclose(clamped_range.normalize(-2.0), 0.0)
    assert math.isclose(clamped_range.normalize(2.0), 1.0)

    info = pulp.ParamInfo()
    info.id = 101
    info.name = "Gain"
    info.unit = "dB"
    info.range = param_range
    info.group_id = 7
    assert info.id == 101
    assert info.name == "Gain"
    assert info.unit == "dB"
    assert info.group_id == 7
    assert math.isclose(info.range.min, -60.0)
    assert math.isclose(info.range.max, 24.0)
    assert math.isclose(info.range.default_value, -12.0)

    info.range = pulp.ParamRange(-1.0, 1.0, 0.0)
    assert math.isclose(info.range.denormalize(0.75), 0.5)
    info.range.default_value = 0.25
    assert math.isclose(info.range.default_value, 0.25)


def assert_midi_bindings(pulp) -> None:
    midi = pulp.MidiBuffer()
    assert midi.empty()
    assert midi.size() == 0

    note_on = pulp.MidiEvent.note_on(1, 60, 100)
    note_on.sample_offset = 3
    midi.add(note_on)
    midi.add(pulp.MidiEvent.note_off(1, 60))
    midi.add(pulp.MidiEvent.cc(1, 74, 64))
    midi.add(pulp.MidiEvent.pitch_bend(1, 8192))
    assert midi.size() == 4
    midi.clear()
    assert midi.empty()

    midi.add(note_on)
    midi.add(pulp.MidiEvent.note_off(1, 60))
    assert midi.size() == 2
    assert not midi.empty()
    midi.clear()
    assert midi.empty()

    note_on = pulp.MidiEvent.note_on(1, 60, 100)
    assert note_on.sample_offset == 0
    note_on.sample_offset = 3
    midi.add(note_on)
    assert midi.size() == 1
    assert not midi.empty()

    note_off_default_velocity = pulp.MidiEvent.note_off(channel=1, note=60)
    note_off_default_velocity.sample_offset = 4
    midi.add(note_off_default_velocity)
    midi.add(pulp.MidiEvent.note_off(1, 60, 64))
    midi.add(pulp.MidiEvent.cc(2, 74, 127))
    midi.add(pulp.MidiEvent.pitch_bend(3, 8192))
    assert midi.size() == 5

    midi.clear()
    assert midi.empty()
    assert midi.size() == 0
    midi.add(pulp.MidiEvent.cc(15, 1, 0))
    assert midi.size() == 1
    midi.clear()
    midi.clear()
    assert midi.empty()

    try:
        pulp.MidiEvent.note_on(1, 60)
    except TypeError:
        pass
    else:
        raise AssertionError("note_on should require an explicit velocity")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_python_bindings.py <built-module-path>")

    module_path = pathlib.Path(sys.argv[1]).resolve()
    pulp = load_module(module_path)

    assert pathlib.Path(pulp.__file__).resolve() == module_path

    assert_param_range_bindings(pulp)
    assert_midi_bindings(pulp)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
