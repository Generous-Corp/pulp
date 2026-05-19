"""LLDB Python formatters for Pulp core types.

Load this from your ~/.lldbinit (or per-project .lldbinit):

    command script import /path/to/pulp/tools/debug/pulp_lldb.py

After loading, `p` / `frame variable` / `expr` on these types prints
a structured summary instead of the default raw-bytes view.

Mirrors tools/debug/pulp.natvis (which does the same job for MSVC).

Slice 9 of planning/2026-05-18-rt-safety-and-debug-dx.md, per sudara
"Big List of JUCE Tips" #21.
"""

from __future__ import annotations


def _atomic_float(valobj, name: str) -> str:
    """Extract a float from an std::atomic<float> child on libc++/libstdc++.

    libc++:  __a_.__a_value
    libstdc++:  _M_i
    """
    member = valobj.GetChildMemberWithName(name)
    if not member.IsValid():
        return "?"
    # libc++
    libcxx = member.GetChildMemberWithName("__a_")
    if libcxx.IsValid():
        v = libcxx.GetChildMemberWithName("__a_value")
        if v.IsValid():
            return f"{v.GetValueAsFloat():.4g}"
    # libstdc++
    libstd = member.GetChildMemberWithName("_M_i")
    if libstd.IsValid():
        return f"{libstd.GetValueAsFloat():.4g}"
    # Apple libc++ legacy
    fallback = member.GetChildMemberWithName("__val_")
    if fallback.IsValid():
        return f"{fallback.GetValueAsFloat():.4g}"
    return "?"


def param_value_summary(valobj, internal_dict):
    """Formatter for pulp::state::ParamValue."""
    value = _atomic_float(valobj, "value_")
    mod = _atomic_float(valobj, "mod_offset_")
    if mod == "0" or mod == "0.0000" or mod == "0.000" or mod == "?" or mod == "0":
        return f"{value}"
    return f"{value} (mod={mod})"


def param_info_summary(valobj, internal_dict):
    """Formatter for pulp::state::ParamInfo."""
    pid = valobj.GetChildMemberWithName("id").GetValueAsUnsigned()
    name = valobj.GetChildMemberWithName("name").GetSummary() or "?"
    unit = valobj.GetChildMemberWithName("unit").GetSummary() or ""
    return f"id={pid} name={name} unit={unit}"


def param_range_summary(valobj, internal_dict):
    """Formatter for pulp::state::ParamRange."""
    lo = valobj.GetChildMemberWithName("min").GetValueAsFloat()
    hi = valobj.GetChildMemberWithName("max").GetValueAsFloat()
    dv = valobj.GetChildMemberWithName("default_value").GetValueAsFloat()
    step = valobj.GetChildMemberWithName("step").GetValueAsFloat()
    step_part = f" step={step:.4g}" if step > 0 else ""
    return f"[{lo:.4g}..{hi:.4g}] default={dv:.4g}{step_part}"


def state_store_summary(valobj, internal_dict):
    """Formatter for pulp::state::StateStore — surfaces param count."""
    params = valobj.GetChildMemberWithName("params_")
    if not params.IsValid():
        return "StateStore"
    n = params.GetNumChildren()
    return f"StateStore ({n} params)"


def rect_summary(valobj, internal_dict):
    """Formatter for pulp::canvas::Rect.

    Fields per core/canvas/include/pulp/canvas/rectangle_list.hpp:12 —
    `x`, `y`, `width`, `height` (not `w`/`h`).
    """
    x = valobj.GetChildMemberWithName("x").GetValueAsFloat()
    y = valobj.GetChildMemberWithName("y").GetValueAsFloat()
    w = valobj.GetChildMemberWithName("width").GetValueAsFloat()
    h = valobj.GetChildMemberWithName("height").GetValueAsFloat()
    return f"{{ x={x:.3g} y={y:.3g} w={w:.3g} h={h:.3g} }}"


def point_summary(valobj, internal_dict):
    """Formatter for pulp::canvas::Point."""
    x = valobj.GetChildMemberWithName("x").GetValueAsFloat()
    y = valobj.GetChildMemberWithName("y").GetValueAsFloat()
    return f"({x:.3g}, {y:.3g})"


def color_summary(valobj, internal_dict):
    """Formatter for pulp::canvas::Color (RGBA)."""
    r = valobj.GetChildMemberWithName("r").GetValueAsUnsigned()
    g = valobj.GetChildMemberWithName("g").GetValueAsUnsigned()
    b = valobj.GetChildMemberWithName("b").GetValueAsUnsigned()
    a = valobj.GetChildMemberWithName("a").GetValueAsUnsigned()
    return f"rgba({r}, {g}, {b}, {a})"


def buffer_view_summary(valobj, internal_dict):
    """Formatter for pulp::audio::BufferView<T>.

    Real members per core/audio/include/pulp/audio/buffer.hpp:70-74 —
    `channels_` (SampleType* const*), `num_channels_`, `num_samples_`.
    Includes a 16-sample sparkline of the first channel when reachable.
    """
    ch = valobj.GetChildMemberWithName("num_channels_").GetValueAsSigned()
    samples_n = valobj.GetChildMemberWithName("num_samples_").GetValueAsSigned()
    if ch <= 0 or samples_n <= 0:
        return f"BufferView {ch}ch x {samples_n} samples"

    spark = ""
    try:
        data = valobj.GetChildMemberWithName("channels_")
        if data.IsValid():
            first_ch = data.GetChildAtIndex(0)
            if first_ch.IsValid():
                blocks = "▁▂▃▄▅▆▇█"
                stride = max(1, samples_n // 16)
                samples = []
                for i in range(0, min(samples_n, 16 * stride), stride):
                    sample_obj = first_ch.Cast(first_ch.GetType()).GetPointeeData(i, 1)
                    err = lldb.SBError()
                    raw = sample_obj.GetFloat(err, 0)
                    if err.Success():
                        # Map [-1, 1] to [0, 7]
                        idx = max(0, min(7, int((raw + 1.0) * 3.5)))
                        samples.append(blocks[idx])
                if samples:
                    spark = " [" + "".join(samples) + "]"
    except Exception:
        pass

    return f"BufferView {ch}ch x {samples_n} samples{spark}"


def listener_token_summary(valobj, internal_dict):
    """Formatter for pulp::state::ListenerToken."""
    tid = valobj.GetChildMemberWithName("id_").GetValueAsUnsigned()
    if tid == 0:
        return "empty"
    return f"token #{tid}"


# ─── Registration ───────────────────────────────────────────────────────────

def __lldb_init_module(debugger, internal_dict):
    """Wire each formatter to its LLDB type. Called by LLDB on import."""
    cat = "pulp"
    debugger.HandleCommand(
        f'type category define -l c++ {cat}'
    )

    def reg(typename, func):
        debugger.HandleCommand(
            f'type summary add -F pulp_lldb.{func} '
            f'-w {cat} -x "^{typename}$"'
        )

    reg(r"pulp::state::ParamValue", "param_value_summary")
    reg(r"pulp::state::ParamInfo", "param_info_summary")
    reg(r"pulp::state::ParamRange", "param_range_summary")
    reg(r"pulp::state::StateStore", "state_store_summary")
    reg(r"pulp::state::ListenerToken", "listener_token_summary")
    reg(r"pulp::canvas::Rect", "rect_summary")
    reg(r"pulp::canvas::Point", "point_summary")
    reg(r"pulp::canvas::Color", "color_summary")
    reg(r"pulp::audio::BufferView<.+>", "buffer_view_summary")

    debugger.HandleCommand(f'type category enable {cat}')
    print(f"[pulp_lldb] Pulp LLDB formatters registered "
          f"(category '{cat}', 9 types).")


# Import lldb lazily so `python3 pulp_lldb.py --help`-style discovery
# from CI scripts doesn't fail on systems without lldb installed.
try:
    import lldb  # noqa: F401  (used by buffer_view_summary)
except ImportError:
    pass
