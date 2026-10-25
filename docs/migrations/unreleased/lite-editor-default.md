---
breaking = true
summary = "macOS plug-ins now link the lite editor stack (no Skia, Dawn or Tint) unless they declare UI_SCRIPT, GPU_UI or FULL_EDITOR"
---

## What changed

On macOS, `pulp_add_plugin()` now builds a plug-in's VST3, CLAP, AU v2 and
Standalone binaries with the lite editor stack unless the plug-in declares
`UI_SCRIPT`, `GPU_UI` or `FULL_EDITOR`. The lite stack leaves out the GPU
plug-in and window hosts and links lite replacements for the library
functions that reached Skia unconditionally, so the VST3, CLAP and AU v2
modules carry no Skia, Dawn or Tint. An AutoUi plug-in module drops from about
19 MB to about 2.1 MB stripped per format, and its first load in a host gets
faster in proportion. The Standalone app opens the same lite editor but still
links Dawn through the standalone host's own render code.

Editors that already opened on the CoreGraphics host (Pulp's generated AutoUi
editor, and C++ views that never set `requires_gpu_host()`) render the same.
That host paints text through CoreText; the lite stack also measures through
CoreText, and it answers `resolved_face_identity()` with the face CoreText
paints, so a design-imported panel still reuses the line breaks its capture
recorded. Measured against the full stack: PulpGain, Pulp Delay and
PulpDesignSynth are pixel-identical. PulpDesignArp differs in one 9 px label,
a bold "07", whose glyphs are identical but sit 0.5 logical px lower in lite.

That label and any text like it can differ by up to 1 logical pixel, because
the face it asks for is not one Pulp bundles, and Skia and CoreText fall back
to different faces for it. The capture resolved "JetBrains Mono" at weight 700
to JetBrainsMono-Bold. Pulp bundles only JetBrains Mono Regular. Skia skips the
Regular face for a bold request and falls through the stack to Menlo-Bold
(ascent 9.47, descent 3.55, leading 0.12 at 9 px); CoreText resolves the
bundled JetBrains Mono (ascent 10.08, descent 3.60, no leading). The
CoreGraphics host paints with CoreText in both builds, so the full build places
JetBrains Mono glyphs using Menlo-Bold's metrics, while lite measures the face
it paints. Advances agree (5.40 px per digit in both), so widths and line
breaks do not change, only the baseline. Neither stack's face matches the
capture's JetBrainsMono-Bold, so neither reuses that label's captured breaks.
The same holds for system families at non-400 weights ("Helvetica Neue" 600 at
12 px: ascent 13.92 and width 116.7 in Skia, 13.69 and 120.0 for the painted
face). To get identical placement in both builds, bundle or register
(`register_font`) the exact faces a design uses.

Two kinds of plug-in need the full stack and must now say so:

- A scripted UI (`UI_SCRIPT`) keeps the full stack automatically.
- A C++ editor with a view that returns `requires_gpu_host()` true needs
  `GPU_UI`. Without it, the editor logs

  ```
  [plugin-gpu-host] this editor asked for the GPU host, but the plug-in was built with the lite editor stack ...
  ```

  once and opens on the CoreGraphics host, where GPU-only drawing (Skia SVG,
  shaders, Dawn textures, 3D) is blank.

`FULL_EDITOR` links the full stack for any other reason. Other platforms are
unchanged: they always build the full stack.

## Migration steps

1. Search your plug-in's editor code for `set_requires_gpu_host(true)` or an
   override that returns true. If you find one, add `GPU_UI` to that
   plug-in's `pulp_add_plugin(...)` call.
2. If the editor draws through Skia-only APIs on purpose (custom
   `SkiaCanvas` code, `draw_svg` that must composite, GPU shaders) without
   setting the flag, add `GPU_UI` and set the flag on the editor's root view
   so the host is chosen correctly.
3. If you need the full stack for another reason, add `FULL_EDITOR`.
4. Rebuild and open the editor once. A `[plugin-gpu-host] ... lite editor
   stack` line in the log means a view still asks for the GPU host; declare
   `GPU_UI` for that plug-in.
5. `LITE_EDITOR` on its own still works and states the default. If a plug-in
   that declares it also gets `GPU_UI` or `FULL_EDITOR`, remove `LITE_EDITOR`:
   the combination is a configure error.
