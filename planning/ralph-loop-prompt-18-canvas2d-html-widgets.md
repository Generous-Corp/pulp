"Implement Canvas 2D API parity + HTML widget parity for Pulp.

References:
- HTML Living Standard — Canvas 2D Context
- HTML Living Standard — form elements
Tracking: planning/w3c-css-support-matrix.md
GitHub Issues: #32 (Canvas 2D), #33 (HTML Elements)

CANVAS 2D API TO IMPLEMENT:
1. JS bridge for CanvasWidget draw commands that queue and replay:
   - canvasBeginPath/canvasMoveTo/canvasLineTo/canvasQuadTo/canvasCubicTo/canvasClosePath
   - canvasFill/canvasStroke
   - canvasFillRect/canvasStrokeRect/canvasFillCircle
   - canvasSetFillColor/canvasSetStrokeColor/canvasSetLineWidth
   - canvasSetFont/canvasFillText/canvasMeasureText
   - canvasSave/canvasRestore/canvasTranslate/canvasScale/canvasRotate
   - canvasClipRect
2. canvasDrawImage(id, imageData, x, y, w, h) — pixel data to texture
3. canvasSetLineDash(id, [dash, gap]) — dashed line support
4. canvasCreateLinearGradient / canvasCreateRadialGradient
5. canvasClear(id) — clear the canvas

HTML WIDGETS TO IMPLEMENT:
1. createImage(id, parent) + setImageSource(id, path) — image display widget
2. createDetails(id, summaryText, parent) — collapsible disclosure section
3. createDialog(id, parent) — modal dialog with backdrop blur
4. Progress widget enhancements: indeterminate state (animated spinner)
5. Multi-line TextEditor improvements (textarea equivalent)

ARCHITECTURE:
- CanvasWidget: store draw commands as a vector of DrawCommand structs
- On paint(), replay commands via the canvas API
- JS bridge queues commands, layout() triggers repaint
- Image: load via stb_image or Skia image decode, cache as SkImage
- Details: expandable Row with arrow + content Col, click toggles visibility
- Dialog: overlay view with backdrop blur + centered content panel

TESTING:
- Canvas: draw shapes from JS, verify screenshot
- Image: load test PNG, verify display
- Details: expand/collapse test
- Dialog: show/hide with backdrop

EACH ITERATION: build, test, screenshot, commit.
COMPLETION: Output 'CANVAS AND WIDGETS COMPLETE'" --completion-promise "CANVAS AND WIDGETS COMPLETE" --max-iterations 20
