# Partial Repaint (experimental)

Partial repaint lets a GPU host repaint **only the region that changed**
instead of the whole window, so a single live sub-view — a meter, a value
readout, a knob strip — updating on an otherwise static panel no longer forces
every pixel of chrome to re-composite.

Three hosts implement it: the macOS GPU **window** host, and the Windows and
Linux **plug-in** hosts. The plug-in hosts share one implementation
(`PluginFrameRenderer`), so their damage-clipping behaviour is the same code,
not two descriptions of the same intent.

It is **off by default** and gated behind an environment variable while it
soaks. With the flag unset there is zero behavior change: the host paints the
whole window every frame exactly as before.

```bash
PULP_PARTIAL_REPAINT=1 ./build/examples/…/YourApp
```

## What it does

When enabled, the host does three things per frame:

1. **Tracks the damaged rect.** A widget that calls
   `View::request_repaint(rect)` (rather than the rect-less `repaint()`)
   announces the sub-rect it changed. The producer maps that rect to root/window
   space and escalates to a full repaint if the view or any ancestor carries a
   render transform, a pixel-spreading filter, or a scroll offset (the plain
   offset mapping would be wrong under any of those).

2. **Decides whether the rect can be clipped losslessly.** `compute_effective_damage`
   walks the view tree once and escalates to a full repaint if any *sampling
   hazard* reaches the damage — a `backdrop-filter` view, a blurred / filter-chain
   view, a compositing effect that samples, a masked view, or a render-transformed
   view whose painted box is unknown. Otherwise it returns the (device-pixel-snapped)
   damaged rect. This guarantees the clipped repaint is **pixel-identical** to a
   full repaint.

3. **Repaints only that rect into a retained scene.** The Dawn swapchain does not
   preserve content between frames, so the surface runs in **persistent-scene
   mode**: a window-sized Graphite `SkSurface` holds the last frame, the host
   clips the repaint (background fill included) to the damaged rect, and the
   scene is blitted 1:1 onto the drawable each frame. Everything outside the clip
   is the previous frame's pixels.

   The negotiation is not optional: if the backend refuses to retain a scene,
   the host turns partial repaint **off** rather than clipping anyway. Clipping
   a non-preserving swapchain leaves whatever the driver last had outside the
   clip, which reads as flicker or stale garbage rather than as a bug in the
   damage model.

The first frame after launch, a resize, or a backing-scale (DPI) change is always
a full repaint. Partial repaint only engages when the pending damage is bounded:
the animation pump and any widget that calls the rect-less `repaint()` mark the
whole surface dirty (`invalidate_all`), so an animating or full-invalidating UI
falls back to a full repaint for free — there is no separate "is an animation
running" check, just the bounded-vs-full damage state.

## Getting the win in your UI

Partial repaint is opportunistic: it costs nothing and changes nothing unless a
widget opts in by calling `request_repaint(rect)` with the sub-rect it repaints,
**including any halo it draws** (a meter's peak-hold overscan, a glow). Rect-less
`repaint()` keeps the full-frame behavior. See the built-in meters
(`core/view/src/widgets/visualizers.cpp`) for the overscan pattern.

## Interaction with subtree caching

`View::set_subtree_cached(true)` and partial repaint are independent
optimizations that **do not compound** in this version. A bounded repaint request
inside a cached subtree invalidates the whole subtree cache (so the cached pixels
stay correct), which forces a full re-record of that subtree — the partial-repaint
win is nullified there. This is deliberate for correctness; caching and partial
repaint are best used on different parts of the tree (cache the static panels,
partial-repaint the live meters).

## Design-viewport (letterboxed) mode

Plug-in editors always set a design viewport, so this used to exclude them
entirely. Damage is produced in ROOT space, but the clip is installed in
SURFACE space — *before* the paint body applies its letterbox
`translate(tx, ty) + scale(sx, sy)`. An unmapped root-space rect therefore
clipped the wrong part of the surface.

`compute_frame_clip()` now maps the rect through the same transform and re-snaps
the result OUT to whole surface pixels (a fractional edge would clip a partially
covered pixel). Letterboxed editors get partial repaint like anything else.

## Limitations

- The macOS **plug-in** host (as distinct from the macOS window host) still
  full-repaints.
- The damage model **escalates** on any hazard rather than growing the clip to
  cover it. A grow-to-fixpoint expansion is possible future work.
- A host that draws into the Pulp surface out of band while the flag is on would
  need to invalidate through the host API — the persistent scene target is
  authoritative in that mode.
