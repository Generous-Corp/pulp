#!/usr/bin/env python3
"""Unit tests for tools/debug/pulp_lldb.py formatters."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import sys
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent.parent / "debug" / "pulp_lldb.py"

spec = importlib.util.spec_from_file_location("pulp_lldb", SCRIPT)
assert spec and spec.loader
pulp_lldb = importlib.util.module_from_spec(spec)
sys.modules["pulp_lldb"] = pulp_lldb
spec.loader.exec_module(pulp_lldb)


class FakeValue:
    def __init__(
        self,
        *,
        valid: bool = True,
        children: dict[str, "FakeValue"] | None = None,
        indexed: list["FakeValue"] | None = None,
        value_float: float = 0.0,
        value_signed: int = 0,
        value_unsigned: int = 0,
        summary: str | None = None,
        num_children: int | None = None,
    ) -> None:
        self.valid = valid
        self.children = children or {}
        self.indexed = indexed or []
        self.value_float = value_float
        self.value_signed = value_signed
        self.value_unsigned = value_unsigned
        self.summary = summary
        self.num_children = num_children

    def IsValid(self) -> bool:
        return self.valid

    def GetChildMemberWithName(self, name: str) -> "FakeValue":
        return self.children.get(name, FakeValue(valid=False))

    def GetValueAsFloat(self) -> float:
        return self.value_float

    def GetValueAsSigned(self) -> int:
        return self.value_signed

    def GetValueAsUnsigned(self) -> int:
        return self.value_unsigned

    def GetSummary(self) -> str | None:
        return self.summary

    def GetNumChildren(self) -> int:
        return self.num_children if self.num_children is not None else len(self.indexed)

    def GetChildAtIndex(self, index: int) -> "FakeValue":
        return self.indexed[index]

    def Cast(self, _type):
        return self

    def GetType(self):
        return object()


class FakeSampleChannel(FakeValue):
    def __init__(self, samples: list[float]) -> None:
        super().__init__()
        self.samples = samples

    def GetPointeeData(self, index: int, _count: int):
        return FakeSample(self.samples[index])


class FakeSample:
    def __init__(self, value: float) -> None:
        self.value = value

    def GetFloat(self, _err, _offset: int) -> float:
        return self.value


class FakeError:
    def Success(self) -> bool:
        return True


class FakeFailError:
    def Success(self) -> bool:
        return False


class FakeLldb:
    SBError = FakeError


class FakeFailLldb:
    SBError = FakeFailError


def atomic_member(*, libcxx=None, libstd=None, legacy=None) -> FakeValue:
    children: dict[str, FakeValue] = {}
    if libcxx is not None:
        children["__a_"] = FakeValue(
            children={"__a_value": FakeValue(value_float=libcxx)}
        )
    if libstd is not None:
        children["_M_i"] = FakeValue(value_float=libstd)
    if legacy is not None:
        children["__val_"] = FakeValue(value_float=legacy)
    return FakeValue(children=children)


class FormatterTests(unittest.TestCase):
    def test_atomic_float_variants_and_param_value_summary(self) -> None:
        value = FakeValue(
            children={
                "value_": atomic_member(libcxx=0.5),
                "mod_offset_": atomic_member(libstd=0.25),
            }
        )
        self.assertEqual(pulp_lldb._atomic_float(value, "missing"), "?")
        self.assertEqual(pulp_lldb._atomic_float(FakeValue(children={"x": atomic_member()}), "x"), "?")
        self.assertEqual(
            pulp_lldb._atomic_float(FakeValue(children={"x": FakeValue(children={"__a_": FakeValue()})}), "x"),
            "?",
        )
        self.assertEqual(pulp_lldb._atomic_float(FakeValue(children={"x": atomic_member(legacy=1.25)}), "x"), "1.25")
        self.assertEqual(pulp_lldb.param_value_summary(value, {}), "0.5 (mod=0.25)")

        unmodulated = FakeValue(
            children={
                "value_": atomic_member(libcxx=0.75),
                "mod_offset_": atomic_member(libstd=0.0),
            }
        )
        self.assertEqual(pulp_lldb.param_value_summary(unmodulated, {}), "0.75")

    def test_param_info_range_geometry_color_and_listener_summaries(self) -> None:
        info = FakeValue(children={
            "id": FakeValue(value_unsigned=7),
            "name": FakeValue(summary='"gain"'),
            "unit": FakeValue(summary='"dB"'),
        })
        self.assertEqual(pulp_lldb.param_info_summary(info, {}), 'id=7 name="gain" unit="dB"')

        info_missing_strings = FakeValue(children={
            "id": FakeValue(value_unsigned=8),
            "name": FakeValue(summary=None),
            "unit": FakeValue(summary=None),
        })
        self.assertEqual(pulp_lldb.param_info_summary(info_missing_strings, {}), "id=8 name=? unit=")

        range_with_step = FakeValue(children={
            "min": FakeValue(value_float=0.0),
            "max": FakeValue(value_float=1.0),
            "default_value": FakeValue(value_float=0.5),
            "step": FakeValue(value_float=0.1),
        })
        range_without_step = FakeValue(children={
            "min": FakeValue(value_float=-1.0),
            "max": FakeValue(value_float=1.0),
            "default_value": FakeValue(value_float=0.0),
            "step": FakeValue(value_float=0.0),
        })
        self.assertEqual(pulp_lldb.param_range_summary(range_with_step, {}), "[0..1] default=0.5 step=0.1")
        self.assertEqual(pulp_lldb.param_range_summary(range_without_step, {}), "[-1..1] default=0")

        rect = FakeValue(children={
            "x": FakeValue(value_float=1.25),
            "y": FakeValue(value_float=2.5),
            "width": FakeValue(value_float=3.75),
            "height": FakeValue(value_float=4.0),
        })
        point = FakeValue(children={"x": FakeValue(value_float=5.0), "y": FakeValue(value_float=6.5)})
        color = FakeValue(children={
            "r": FakeValue(value_unsigned=1),
            "g": FakeValue(value_unsigned=2),
            "b": FakeValue(value_unsigned=3),
            "a": FakeValue(value_unsigned=255),
        })
        self.assertEqual(pulp_lldb.rect_summary(rect, {}), "{ x=1.25 y=2.5 w=3.75 h=4 }")
        self.assertEqual(pulp_lldb.point_summary(point, {}), "(5, 6.5)")
        self.assertEqual(pulp_lldb.color_summary(color, {}), "rgba(1, 2, 3, 255)")

        self.assertEqual(pulp_lldb.listener_token_summary(FakeValue(children={"id_": FakeValue(value_unsigned=0)}), {}), "empty")
        self.assertEqual(pulp_lldb.listener_token_summary(FakeValue(children={"id_": FakeValue(value_unsigned=42)}), {}), "token #42")

    def test_state_store_summary_counts_valid_params(self) -> None:
        self.assertEqual(pulp_lldb.state_store_summary(FakeValue(), {}), "StateStore")
        params = FakeValue(num_children=3)
        self.assertEqual(
            pulp_lldb.state_store_summary(FakeValue(children={"params_": params}), {}),
            "StateStore (3 params)",
        )

    def test_buffer_view_summary_handles_empty_and_sparkline_paths(self) -> None:
        empty = FakeValue(children={
            "num_channels_": FakeValue(value_signed=0),
            "num_samples_": FakeValue(value_signed=12),
        })
        self.assertEqual(pulp_lldb.buffer_view_summary(empty, {}), "BufferView 0ch x 12 samples")

        old_lldb = getattr(pulp_lldb, "lldb", None)
        pulp_lldb.lldb = FakeLldb()
        try:
            channel = FakeSampleChannel([-1.0, -0.5, 0.0, 0.5, 1.0])
            view = FakeValue(children={
                "num_channels_": FakeValue(value_signed=1),
                "num_samples_": FakeValue(value_signed=5),
                "channels_": FakeValue(indexed=[channel]),
            })
            summary = pulp_lldb.buffer_view_summary(view, {})
        finally:
            if old_lldb is None:
                delattr(pulp_lldb, "lldb")
            else:
                pulp_lldb.lldb = old_lldb

        self.assertTrue(summary.startswith("BufferView 1ch x 5 samples ["))

    def test_buffer_view_summary_swallows_unreadable_sample_data(self) -> None:
        class BrokenChannel(FakeValue):
            def Cast(self, _type):
                raise RuntimeError("unreadable")

        invalid_channels = FakeValue(children={
            "num_channels_": FakeValue(value_signed=1),
            "num_samples_": FakeValue(value_signed=8),
            "channels_": FakeValue(valid=False),
        })
        invalid_first_channel = FakeValue(children={
            "num_channels_": FakeValue(value_signed=1),
            "num_samples_": FakeValue(value_signed=8),
            "channels_": FakeValue(indexed=[FakeValue(valid=False)]),
        })
        unreadable = FakeValue(children={
            "num_channels_": FakeValue(value_signed=1),
            "num_samples_": FakeValue(value_signed=8),
            "channels_": FakeValue(indexed=[BrokenChannel()]),
        })
        failed_samples = FakeValue(children={
            "num_channels_": FakeValue(value_signed=1),
            "num_samples_": FakeValue(value_signed=2),
            "channels_": FakeValue(indexed=[FakeSampleChannel([0.0, 0.5])]),
        })

        old_lldb = getattr(pulp_lldb, "lldb", None)
        pulp_lldb.lldb = FakeFailLldb()
        try:
            self.assertEqual(pulp_lldb.buffer_view_summary(failed_samples, {}), "BufferView 1ch x 2 samples")
        finally:
            if old_lldb is None:
                delattr(pulp_lldb, "lldb")
            else:
                pulp_lldb.lldb = old_lldb

        self.assertEqual(pulp_lldb.buffer_view_summary(invalid_channels, {}), "BufferView 1ch x 8 samples")
        self.assertEqual(pulp_lldb.buffer_view_summary(invalid_first_channel, {}), "BufferView 1ch x 8 samples")
        self.assertEqual(pulp_lldb.buffer_view_summary(unreadable, {}), "BufferView 1ch x 8 samples")

    def test_lldb_init_registers_all_summaries(self) -> None:
        class Debugger:
            def __init__(self) -> None:
                self.commands: list[str] = []

            def HandleCommand(self, command: str) -> None:
                self.commands.append(command)

        debugger = Debugger()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            getattr(pulp_lldb, "__lldb_init_module")(debugger, {})

        self.assertIn("type category define -l c++ pulp", debugger.commands)
        self.assertIn("type category enable pulp", debugger.commands)
        self.assertTrue(any("ParamValue" in command for command in debugger.commands))
        self.assertTrue(any("BufferView" in command for command in debugger.commands))
        self.assertIn("formatters registered", out.getvalue())


if __name__ == "__main__":
    unittest.main()
