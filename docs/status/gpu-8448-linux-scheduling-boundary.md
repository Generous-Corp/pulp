# Linux embedded GPU frame scheduling: design boundary

Status: design checkpoint; no platform source edits in this slice.

## Scope and non-overlap

- Issue: #8448 (Linux embedded plugin host frame advancement)
- Base: `365c0f449bdb7fb529af1791c875c09061115d15` (`origin/main` at audit time)
- Worktree: `feature/8448-linux-scheduling-20260923`
- Separate from #8457 capture, #8458/#8628 observability, and closed #8449 Windows scheduling.
- Candidate implementation surface: `core/view/platform/linux/plugin_view_host_linux.cpp`.
- No `core/render` or Vellum-transferred capture changes are proposed.

## Verified current behavior

The Linux X11 plugin host has an idle callback and an X event pump, but no
`HostFramePump`, `FrameClock`, timer, display-link equivalent, or expose-driven
continuous scheduler. `repaint()` is called by the embedding host and renders
one frame; it cannot advance animations continuously when the DAW stops issuing
paint requests.

The shared `HostFramePump` contract requires a host-owned presentation timestamp
and explicitly distinguishes an embedded host from a standalone window. The
Linux plugin host cannot assume ownership of the DAW loop or compositor.

## Design decision required before code

The safe default is an **explicit embedding-host tick contract**:

1. The DAW/format adapter remains the scheduler and supplies a monotonic
   presentation timestamp on the UI thread.
2. The Linux plugin host exposes one idempotent `advance_frame(timestamp)` (or
   equivalent) entry point that runs `should_dispatch_host_frame`,
   `begin_host_frame`, and `advance_host_frame` exactly once for that tick.
3. `repaint()` remains the presentation request and never fabricates a timer or
   background thread.
4. The host may coalesce ticks while the editor is closed; the pump records a
   skipped/resume boundary so idle time does not become animation time.
5. A host that cannot provide ticks keeps the current honest behavior: static
   and input-triggered frames work, continuous animation is unavailable and is
   reported as such.

An internal X11 `XTimer`, `usleep` loop, or worker thread is rejected: it would
compete with the DAW event loop, have no compositor-vsync truth, and risk
running while the editor is detached. A fixed `1/60` tick is also rejected by
the existing measured-delta contract.

## Required acknowledgement and receipts

Before implementation, record the owning format/embedding adapter and its
timestamp source. Acceptance must include:

- headless tests proving one tick advances `FrameClock` and continuous
  subscribers, skipped ticks create a resume, and duplicate timestamps do not
  rewind time;
- a Linux X11 smoke receipt showing the real host invokes the tick path while
  attached and stops it while detached;
- an unavailable-host receipt for adapters that do not provide a scheduler;
- governed macOS build/validation and a Vellum exact-boundary acknowledgement
  if any transferred path is touched.

Until the embedding owner and timestamp source are named, source edits would be
guesswork and must not be presented as #8448 completion.
