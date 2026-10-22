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
editor, and C++ views that never set `requires_gpu_host()`) render the same,
with one correction. That host paints text through CoreText, but before this
release it measured text through Skia, and the two can resolve different faces
for the same family and weight. "Helvetica Neue" at weight 600 and 12 px, for
example, measured with ascent 13.92 and width 116.7 but painted a face with
ascent 13.69 and width 120.0; JetBrains Mono also picked up 0.12 px of leading
at 9 px that the painted face does not have. The lite stack measures the face
it paints, so labels in such editors can move by up to 1 logical pixel, now
placed by the metrics of the glyphs actually drawn. Pulp's AutoUi editors and
Inter text are unaffected.

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
