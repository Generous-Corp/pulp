# Standard MIDI File interop

`import_smf` / `export_smf` (`core/smf/src/`) convert between SMF bytes and a
timeline `Project` through the tempo and meter maps. The public header is
`core/timeline/include/pulp/timeline/smf.hpp`; the implementation lives in the
dedicated `pulp::smf-interop` target, not in `pulp::timeline`.

The consent-gated surface is a separate installed target and header:
`pulp::smf-interchange` / `<pulp/smf/interchange.hpp>`. It exposes only
`pulp::smf::writer()`, a format-bound `FormatBoundExportWriter` handle. Callers
must create `Format::Smf` plans and execute them through `run_export()`; the plan owns the
immutable project snapshot and the central runner owns the reserved, versioned
`pulp-loss-manifest.json` artifact. Do not add project arguments to the adapter
or let it serialize a caller-captured document.

- Keep the conversion in the musical domain end to end. An SMF has its own
  timebase — a header division in ticks per quarter note plus Set Tempo and
  Time Signature meta-events — so scale those ticks onto
  `timebase::kTicksPerQuarter` and rebuild `TempoMap`/`MeterMap` from the
  meta-events. Never route positions through seconds: that is what makes a
  mid-file tempo change survive as a tempo point instead of pre-multiplied
  wall-clock positions.
- State exactness, do not assume it. `kTicksPerQuarter` is
  2^6·3^2·5^2·7^2 = 705600, so a division divides it only when the division's
  prime powers fit — 96, 120, 192, 240, 480, and 960 do; 384 and 1920 do not.
  Import reports `exact_tick_conversion` plus a rounding bound; export refuses a
  non-representable tick unless the caller sets `allow_lossy_tick_rounding`.
- Do not reuse `choc::midi::File` for this path. It throws `std::runtime_error`
  (the importer target compiles `-fno-exceptions`), skips unknown chunks instead
  of failing closed, terminates a system-exclusive block by scanning for the
  next high-bit byte rather than honouring its declared length, applies running
  status to meta and system-exclusive events when writing, and resolves events
  to seconds. `pulp::midi::read_midi_file` is the seconds-domain convenience
  path over that reader and is likewise the wrong layer here.
- Fail closed on the byte stream. Meta and system-exclusive events cancel
  running status; a data byte with no status in effect, a missing end-of-track,
  events after end-of-track, trailing bytes past the declared track count, an
  SMPTE division, format 2, and a non-`MTrk` chunk are all errors.
  `SmfUnsupportedEventPolicy::IgnoreNonNote` is the caller's explicit opt-in to
  discard out-of-subset events; the default drops nothing.
- Apply `SmfImportLimits` before the matching state grows — file bytes before
  decoding, track count before the track loop, event/note/concurrent-note
  counts before each push, payload bytes before reading a meta or sysex body.
- Match Note On to Note Off first-in-first-out per `(channel, pitch)`. Stacked
  identical pitches are legal input and only FIFO preserves their durations. A
  Note On with velocity 0 is a Note Off; a zero-length note is rejected because
  the model requires a positive duration.
- Velocity crosses a 7/16-bit boundary. The module keeps its own copy of the
  MIDI 2.0 scaling rather than including the CHOC-backed `pulp::midi` headers
  under `-fno-exceptions`; `test_timeline_smf.cpp` links `pulp::midi` and
  round-trips every 7-bit velocity so the two provably agree. Export rejects a
  velocity that scales to zero instead of rewriting it, because a zero-velocity
  Note On reads as a Note Off.
- A meter change that is not on a bar boundary is rejected by
  `MeterMap::create`, and a tempo below roughly 3.58 bpm has no Set Tempo
  representation (the event's period field is 24 bits of microseconds). Surface
  both as typed errors rather than clamping.
- Keep raw and consented behavior distinct. Raw `export_smf` strictly rejects
  unsupported clip, event, and grid shapes it visits, but it does not census
  unrelated mixer, device, take, asset, or marker state.
  The interchange adapter may, after exact concept consent, omit unsupported
  clips/state, strip note modifiers, step continuous tempo ramps at their
  authored points, and quantize non-representable 16-bit velocity to the nearest
  nonzero 7-bit value. Each policy branch must be driven from the plan's concrete
  losses; there is no blanket lossy mode.
