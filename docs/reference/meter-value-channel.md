# Meter & Value Channels

The paint-safe way to get a live number from a plugin's audio/host thread onto a
native view: gain reduction, output levels, a readout, a modulation ring's
position. Without this seam every plugin UI that shows a meter re-rolls the same
reader-polled-on-the-frame-clock plumbing.

Headers: [`value_source.hpp`](../../core/view/include/pulp/view/value_source.hpp)
(the channels) and
[`value_source_binding.hpp`](../../core/view/include/pulp/view/value_source_binding.hpp)
(the view-side bindings).

## The model

```
audio / host thread            UI thread
───────────────────            ─────────────────────────────────────
source->publish(frame)  ──►    binding snapshots once per FrameClock tick
                               paint() reads the snapshot  (no lock, no alloc)
```

Two channels, both lock-free, one writer and one reader (the `TripleBuffer`
contract):

| Type | Carries | Use for |
|------|---------|---------|
| `MeterSource` | a `MeterFrame`: RMS + peak for up to `MeterFrame::kMaxChannels` (8) channels | level meters, gain-reduction meters, multi-channel strips |
| `ScalarSource` | one `float` | a readout value, a modulation ring's modulated position |

`MeterFrame` is fixed-capacity and trivially copyable on purpose: that is what
makes `publish()` allocation-free on the audio thread and the snapshot a plain
copy on the UI thread. Do not grow it into something that allocates.

## Binding a source to a view

Any `View` can bind. The subscription attaches whenever a `FrameClock` becomes
reachable, so it does not matter whether you bind before or after the view is
hosted.

```cpp
class OutputPanel : public pulp::view::DesignFrameView {
public:
    void paint(pulp::canvas::Canvas& c) override {
        const auto& f = meter_frame();          // paint-safe: a cached copy
        const int n = std::min(f.channels, pulp::view::MeterFrame::kMaxChannels);
        for (int ch = 0; ch < n; ++ch)
            draw_bar(c, ch, f.peak[ch], f.rms[ch]);
    }
};

// UI thread, at setup:
auto levels = std::make_shared<pulp::view::MeterSource>();
panel->set_meter_source(levels);

// Audio thread, per block:
pulp::view::MeterFrame f;
f.channels = 2;
f.rms[0] = l_rms; f.peak[0] = l_peak;
f.rms[1] = r_rms; f.peak[1] = r_peak;
levels->publish(f);                             // alloc-free, non-blocking
```

One view can paint several meters from one source — that is what the frame's
channels are for. Bound your channel index by
`min(frame.channels, MeterFrame::kMaxChannels)`: `publish()` stores the frame
verbatim and never trusts the count to gate an array access.

`set_scalar_source` / `scalar_value()` are the same shape for a single number.
Passing `nullptr` to either unbinds and drops the last snapshot, so a view never
paints a reading the plugin has stopped producing.

### Widgets

`Meter` binds through this seam (`Meter::set_source` is `set_meter_source`) and
overrides `on_meter_frame` to drive its ballistics. Override `on_meter_frame`
when your view must advance per-frame state from the reading (smoothing,
ballistics); if you only need to draw the latest value, read `meter_frame()` in
`paint()` and ignore the hook.

### Per-element values on a `DesignFrameView`

A `DesignFrameView` is one view painting many elements, so its own single scalar
cannot carry a value per element (a modulation ring per macro knob, say). Bind
those with `set_element_scalar_source(param_key, source)` and read them with
`element_scalar(i)` in `paint()`.

Bindings are keyed by **`param_key`, not element index**, because
`set_active_frame` replaces the element list wholesale — index `0` is a different
control in a different frame. A binding therefore follows its key across a frame
swap; a key no frame declares reads `0`.

## Paint-safety

A `paint()` getter must not allocate, lock, or block. The snapshot-at-tick design
is what buys that: `meter_frame()`, `scalar_value()`, and `element_scalar(i)` all
read a cached member, never the source. It is the same discipline
`HostParamSurface` enforces — *snapshot at tick, paint from the snapshot*.

Two consequences worth knowing:

- A value published mid-frame is not visible until the next tick. That is
  deliberate: every read within one frame is coherent, so a meter and a readout
  fed by the same source cannot disagree.
- Reading a `TripleBuffer` directly from `paint()` would defeat both properties.
  Bind a source instead.

`test_meter_source.cpp` asserts the read path is allocation-free with the shared
`RtAllocationProbe` interposer (including a positive control, so a zero is never
a probe that was silently not linked).

## Lifetime

A bound-and-attached view is a `FrameClock` subscriber, so it keeps an editor's
frames alive (see `needs_continuous_frames`) while a source is bound. Unbind when
nothing feeds it.

The bound `FrameClock` must outlive the view, **or** the host must clear it
(`root->set_frame_clock(nullptr)`) or detach the view before destroying the
clock — both drop the subscription first. Pulp's GPU hosts clear the root clock
during teardown, so this holds for them; a custom host that owns a clock must
observe the same order.

## What this is not

These are latest-value channels. There is **no rolling sample-history channel**
for a scrolling waveform scope: a `MeterSource` carries a reading, not a signal.
A view that needs rolling history (a limiter scope redrawn each frame) still owns
that buffer itself. `WaveformView::set_data` takes a complete window per call and
replaces it — the caller supplies the history.
