// SPDX-License-Identifier: MIT

// The properties the capture asks Chromium to resolve, in request order.
//
// A property absent from this list is one no consumer can ever draw: the box
// arrives with the appearance silently defaulted, which renders as a plausible
// wrong picture rather than as an error. The list is therefore sized for
// reproducing a whole panel, not for describing a control -- collecting a
// property costs one string per node, while re-capturing a corpus to add one
// later costs every design.
//
// Order is load-bearing in a narrow sense only: `layout.styles` rows are
// positional, so the request order is written into the snapshot as
// `computedStyleNames` and every consumer keys by name from there. Inserting a
// property anywhere in this list is safe; hardcoding a parallel copy is not.
export const COMPUTED_STYLES = [
  "display",
  "visibility",
  "opacity",
  "position",
  "z-index",
  "background-color",
  "background-image",
  "background-blend-mode",
  // A gradient with a hard stop, tiled, is the standard CSS grid idiom. The
  // repeat length lives entirely in background-size: without it the same
  // declaration lowers to one hairline instead of eight columns, and nothing
  // downstream can tell that the value was never read rather than default.
  "background-size",
  "background-position",
  "background-repeat",
  "background-clip",
  "background-origin",
  "border-top-color",
  "border-right-color",
  "border-bottom-color",
  "border-left-color",
  "border-top-width",
  "border-right-width",
  "border-bottom-width",
  "border-left-width",
  // All four edges. Reading only the top edge and applying it to the box makes
  // a dashed left border vanish, and makes a mixed-edge box uniformly wrong in
  // a way that looks like a paint bug rather than a missing input.
  "border-top-style",
  "border-right-style",
  "border-bottom-style",
  "border-left-style",
  "border-radius",
  "box-shadow",
  "text-shadow",
  "outline-color",
  "outline-style",
  "outline-width",
  "outline-offset",
  "filter",
  "backdrop-filter",
  "transform",
  "transform-origin",
  "overflow",
  "clip-path",
  "mask-image",
  "mix-blend-mode",
  "isolation",
  "color",
  "font-family",
  "font-size",
  "font-weight",
  "font-style",
  "font-variation-settings",
  "text-align",
  "letter-spacing",
  "word-spacing",
  "line-height",
  "text-transform",
  "text-decoration-line",
  "text-decoration-color",
  "text-decoration-style",
  "text-decoration-thickness",
  "text-underline-offset",
  "white-space",
  "text-overflow",
  // Generated content. The snapshot reports ::before / ::after as their own
  // nodes with their own layout entries, so the text they inject is only
  // recoverable through this property -- there is no DOM text node to read.
  "content",
  "cursor",
  "pointer-events",
  // SVG paint. An inline `<svg>` icon carries its colour three different ways —
  // a presentation attribute, a stylesheet rule, or `currentColor` inherited
  // from the box around it — and only the browser knows which one won. Reading
  // the authored attribute back off the DOM gets a CSS-styled icon wrong and
  // gets `currentColor` wrong every time, so the resolved value is the only
  // usable input. `fill` / `stroke` are `none` on a non-SVG box, which costs a
  // shared string.
  "fill",
  "fill-opacity",
  "fill-rule",
  "stroke",
  "stroke-opacity",
  "stroke-width",
  // Not drawn by the vector lowering, but READ by it: a dashed or dotted stroke
  // rendered solid is a wrong picture, so its presence sends the subtree to the
  // element fallback instead.
  "stroke-dasharray",
];

export function semanticExpression(snapshotClickableIndexes = []) {
  return `(() => {
  const elements = [...document.querySelectorAll('*')];
  const snapshotClickable = new Set(
    ${JSON.stringify(snapshotClickableIndexes)});
  const roleKinds = new Map([
    ['button', 'button'],
    ['checkbox', 'toggle'],
    ['combobox', 'select'],
    ['link', 'action'],
    ['menuitem', 'action'],
    ['radio', 'toggle'],
    ['slider', 'slider'],
    ['spinbutton', 'number_input'],
    ['switch', 'toggle'],
    ['textbox', 'text_input']
  ]);
  const nativeKind = element => {
    const tag = element.tagName.toLowerCase();
    const type = (element.getAttribute('type') || '').toLowerCase();
    if (tag === 'button') return 'button';
    if (tag === 'select') return 'select';
    if (tag === 'textarea') return 'text_input';
    if (tag === 'a' && element.hasAttribute('href')) return 'action';
    if (tag !== 'input') return '';
    if (type === 'range') return 'slider';
    if (type === 'checkbox' || type === 'radio') return 'toggle';
    if (type === 'button' || type === 'submit' || type === 'reset') return 'button';
    if (type === 'number') return 'number_input';
    return 'text_input';
  };
  const dataPulp = element => {
    const result = {};
    for (const attribute of element.attributes) {
      if (attribute.name.startsWith('data-pulp-')) {
        result[attribute.name.slice('data-pulp-'.length)] = attribute.value;
      }
    }
    return result;
  };
  // The painted control box, when the author marked it with data-pulp-paint.
  // Null when unmarked: the component box is NOT a safe fallback for value
  // geometry, so a consumer must decide what to do rather than be handed a
  // plausible wrong rectangle. Bounds are page coordinates, same frame as the
  // candidate's own, so the two are directly comparable.
  const paintBox = element => {
    try {
      const painted = element.querySelector('[data-pulp-paint]');
      if (!painted) return null;
      const box = painted.getBoundingClientRect();
      return {
        left: box.left + window.scrollX,
        top: box.top + window.scrollY,
        width: box.width,
        height: box.height
      };
    } catch (e) {
      return null;
    }
  };
  // The design's OWN pointer, when the author marked it with
  // data-pulp-indicator. Same contract as paintBox above and for the same
  // reason: only the author knows which child is the pointer, and inferring it
  // (the thinnest child, the one whose class says "needle") would hard-code one
  // design system's vocabulary into the importer.
  //
  // Null when unmarked, which is the common case and NOT a defect: a design
  // that draws no pointer gets the widget's derived tick instead. Bounds are
  // page coordinates, the same frame as the candidate's own bounds and its
  // paint box, so a consumer can express the pointer relative to the dial
  // without a second coordinate system.
  //
  // A box with NO extent on either axis is treated as unmarked: it carries no
  // axis at all, so the lowering has no direction to sweep it along. A box with
  // extent on one axis is a different thing entirely and must not be dropped --
  // see strokePaintedExtent.
  //
  // The painted width of a stroked SVG line in CSS px, or 0 when that cannot be
  // recovered.
  //
  // getBoundingClientRect() does not include stroke, so an SVG <line> drawn
  // straight up, down, left or right reports ZERO extent across its own axis
  // however thick it is. Twelve o'clock is the resting position of any centred
  // bipolar parameter, so refusing a zero axis silently dropped the COMMONEST
  // pointer orientation and fell back to the derived tick -- a plausible
  // picture, which is why no render caught it.
  //
  // The stroke is in USER UNITS, not CSS px: a 2-unit stroke in a 24-unit
  // viewBox painted at 96px is 8px on screen. Scale is therefore read off
  // whichever axis has extent (client px per user unit) rather than assumed to
  // be 1. That ratio is only a pure scale for an axis-aligned element -- for a
  // rotated one it folds in the rotation -- which is exactly why it is computed
  // here and nowhere else: a rotated pointer has extent on both axes and never
  // reaches this path.
  const strokePaintedExtent = marked => {
    try {
      if (typeof marked.getBBox !== 'function') return 0;
      const stroke = parseFloat(window.getComputedStyle(marked).strokeWidth);
      if (!(stroke > 0)) return 0;
      const bb = marked.getBBox();
      const rect = marked.getBoundingClientRect();
      const scale = bb.height > 0 ? rect.height / bb.height
        : bb.width > 0 ? rect.width / bb.width : 0;
      if (!(scale > 0)) return 0;
      return stroke * scale;
    } catch (e) {
      return 0;
    }
  };
  // The element's own linear map -- rotation, scale, skew -- to its parent,
  // ignoring translation.
  //
  // CSS applies the individual transform properties before the transform
  // property, in the order translate, rotate, scale, transform, so they compose
  // in that order here. translate is deliberately absent: it cannot change an
  // extent, and the placement it does affect is read from the client rect,
  // which already has every transform in it.
  const cssAngleDegrees = value => {
    const match = String(value).trim().match(
      /^([+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:e[+-]?\\d+)?)\\s*(deg|grad|rad|turn)$/i);
    if (!match) return null;
    const amount = Number(match[1]);
    if (!Number.isFinite(amount)) return null;
    switch (match[2].toLowerCase()) {
      case 'deg': return amount;
      case 'grad': return amount * 0.9;
      case 'rad': return amount * 180 / Math.PI;
      case 'turn': return amount * 360;
      default: return null;
    }
  };
  // CSS rotate accepts an angle, a named axis plus angle, or a three-number
  // axis plus angle. Only a Z-axis rotation is a faithful 2D affine map. A
  // rotation around X/Y cannot be squeezed into the six-number contract, so
  // refuse oriented geometry instead of publishing an incomplete matrix that
  // would become authoritative downstream.
  const cssRotateDegrees = value => {
    const rotate = String(value || '').trim();
    if (!rotate || rotate === 'none') return 0;
    const tokens = rotate.split(/\\s+/);
    if (tokens.length === 1) return cssAngleDegrees(tokens[0]);
    if (tokens.length === 2) {
      const angle = cssAngleDegrees(tokens[1]);
      if (angle === null) return null;
      const axis = tokens[0].toLowerCase();
      return axis === 'z' ? angle : null;
    }
    if (tokens.length === 4) {
      const axis = tokens.slice(0, 3).map(Number);
      const angle = cssAngleDegrees(tokens[3]);
      if (angle === null || axis.some(component => !Number.isFinite(component)))
        return null;
      if (axis[0] !== 0 || axis[1] !== 0 || axis[2] === 0) return null;
      return axis[2] > 0 ? angle : -angle;
    }
    return null;
  };
  const localMatrix = el => {
    const style = window.getComputedStyle(el);
    // A motion path adds its own translation and tangent rotation between the
    // individual transforms and transform. Reconstructing arbitrary paths from
    // computed CSS is outside this affine payload; decline it rather than make
    // an incomplete matrix authoritative.
    if ((style.offsetPath || 'none').trim() !== 'none') return null;
    const matrix = new DOMMatrix();
    const rawZoom = (style.zoom || '').trim();
    let zoom = 1;
    if (rawZoom && rawZoom !== 'normal') {
      zoom = rawZoom.endsWith('%')
        ? parseFloat(rawZoom) / 100 : Number(rawZoom);
      if (!(zoom > 0) || !Number.isFinite(zoom)) return null;
    }
    matrix.scaleSelf(zoom);
    const angle = cssRotateDegrees(style.rotate);
    if (angle === null) return null;
    matrix.rotateSelf(angle);
    const scale = (style.scale || '').trim();
    if (scale && scale !== 'none') {
      const axes = scale.split(/\\s+/).map(Number);
      if (axes.length > 0 && axes.every(Number.isFinite))
        matrix.scaleSelf(axes[0], axes.length > 1 ? axes[1] : axes[0]);
    }
    const transform = (style.transform || '').trim();
    if (transform && transform !== 'none') {
      const transformed = new DOMMatrix(transform);
      if (!transformed.is2D) return null;
      matrix.multiplySelf(transformed);
    }
    // The perspective property belongs to an ancestor but projects its
    // children. That projection cannot be represented by [a,b,c,d,e,f].
    if ((style.perspective || 'none').trim() !== 'none') return null;
    return matrix;
  };
  // The marked pointer's size in its OWN coordinate space, plus the matrix that
  // carries that space to the page.
  //
  // The client rect cannot answer this. It is the axis-aligned bounding box of
  // whatever the element became, so a rotated needle reports the box its
  // diagonal sweeps -- a 4x38 needle at 38 degrees measures 26.5x32.4 -- and any
  // width taken from it is the diagonal's, roughly ten times the truth. The
  // element's own box is the only description of a rotated pointer that does not
  // fold the rotation into the answer.
  //
  // Deliberately NOT pre-multiplied into CSS px here. Baking one consumer's
  // assumption into the producer double-counts the moment a second consumer
  // reads the matrix, and a rotated element's scale cannot be recovered from
  // extents afterwards -- the ratio of transformed to untransformed extent folds
  // the rotation in. Element space plus the matrix is the only decomposition
  // that keeps both recoverable.
  //
  // UNITS. intrinsic is in the element's own space: SVG USER UNITS for an SVG
  // shape, CSS px for an HTML box. It is the matrix, not the number, that says
  // how large that is on screen -- a 2-unit stroke in a 24-unit viewBox drawn at
  // 96px paints 8 CSS px. transform is [a, b, c, d, e, f], mapping element space
  // to PAGE coordinates, the same frame as the bounds beside it.
  //
  // Two sources, because the two shapes answer to different APIs: an SVG shape
  // has getBBox() and a screen CTM, an HTML box has neither and offers
  // offsetWidth/offsetHeight instead.
  const indicatorGeometry = (marked, rect) => {
    try {
      let x = 0;
      let y = 0;
      let width = 0;
      let height = 0;
      let matrix = null;
      if (typeof marked.getBBox === 'function') {
        const markedStyle = window.getComputedStyle(marked);
        // A non-scaling line's outline is constructed in host coordinates, so
        // describe it there: an oriented rectangle whose width is the screen
        // stroke and whose length/direction are the transformed centreline.
        // This preserves the intrinsic-width contract without pretending the
        // element CTM scales a stroke that SVG explicitly keeps unscaled.
        const nonScalingStroke =
          (markedStyle.vectorEffect || '').trim() === 'non-scaling-stroke';
        if (nonScalingStroke &&
            (marked.tagName || '').toLowerCase() === 'line') {
          if (typeof marked.getScreenCTM !== 'function') return null;
          const ctm = marked.getScreenCTM();
          const stroke = parseFloat(markedStyle.strokeWidth);
          if (!ctm || !(stroke > 0)) return null;
          const first = {
            x: ctm.a * marked.x1.baseVal.value +
              ctm.c * marked.y1.baseVal.value + ctm.e + window.scrollX,
            y: ctm.b * marked.x1.baseVal.value +
              ctm.d * marked.y1.baseVal.value + ctm.f + window.scrollY
          };
          const second = {
            x: ctm.a * marked.x2.baseVal.value +
              ctm.c * marked.y2.baseVal.value + ctm.e + window.scrollX,
            y: ctm.b * marked.x2.baseVal.value +
              ctm.d * marked.y2.baseVal.value + ctm.f + window.scrollY
          };
          const dx = second.x - first.x;
          const dy = second.y - first.y;
          const length = Math.hypot(dx, dy);
          if (!(length > 0)) return null;
          const alongX = dx / length;
          const alongY = dy / length;
          const acrossX = -alongY;
          const acrossY = alongX;
          const centreX = (first.x + second.x) / 2;
          const centreY = (first.y + second.y) / 2;
          return {
            intrinsic: { x: 0, y: 0, width: stroke, height: length },
            transform: [acrossX, acrossY, alongX, alongY,
              centreX - acrossX * stroke / 2 - alongX * length / 2,
              centreY - acrossY * stroke / 2 - alongY * length / 2]
          };
        }
        const bb = marked.getBBox();
        x = bb.x;
        y = bb.y;
        width = bb.width;
        height = bb.height;
        // getBBox() is geometry only, so a <line> is 38 long and 0 wide however
        // thick its stroke: a line has no width in user space, only a stroke
        // width. The stroke is in USER UNITS too -- getComputedStyle reports it
        // with a px suffix that is not px -- so it inflates the element's own
        // box directly, with no scale applied. Only a zero axis is grown, which
        // is what the client-rect path does, so an axis-aligned pointer keeps
        // the number it has today.
        const stroke = parseFloat(markedStyle.strokeWidth);
        if (stroke > 0 && !nonScalingStroke) {
          if (!(width > 0)) { x -= stroke / 2; width = stroke; }
          if (!(height > 0)) { y -= stroke / 2; height = stroke; }
        }
        const ctm = typeof marked.getScreenCTM === 'function'
          ? marked.getScreenCTM() : null;
        if (ctm) {
          matrix = new DOMMatrix([ctm.a, ctm.b, ctm.c, ctm.d,
            ctm.e + window.scrollX, ctm.f + window.scrollY]);
        }
      } else {
        // Border box, matching what the client rect measures for an
        // untransformed element. offsetWidth/offsetHeight are the authoritative
        // fallback but round to integers, so prefer the browser's fractional
        // used CSS size when it can be reconstructed as a border box. A 0.4px
        // needle must not become zero-width merely because it was rotated.
        const style = window.getComputedStyle(marked);
        const borderBoxExtent = (axis, fallback) => {
          let extent = parseFloat(style[axis]);
          if (!Number.isFinite(extent)) return fallback;
          if (style.boxSizing !== 'border-box') {
            const horizontal = axis === 'width';
            const sides = horizontal
              ? ['paddingLeft', 'paddingRight', 'borderLeftWidth', 'borderRightWidth']
              : ['paddingTop', 'paddingBottom', 'borderTopWidth', 'borderBottomWidth'];
            for (const side of sides) {
              const value = parseFloat(style[side]);
              if (Number.isFinite(value)) extent += value;
            }
          }
          return extent;
        };
        width = borderBoxExtent('width', marked.offsetWidth);
        height = borderBoxExtent('height', marked.offsetHeight);
        let accumulated = new DOMMatrix();
        // Ancestor transforms scale this element too, and the dial they are
        // measured against is under the same ones, so the fraction is only right
        // if the whole chain is in the matrix.
        for (let el = marked; el && el.nodeType === 1; el = el.parentElement) {
          const local = localMatrix(el);
          if (!local) return null;
          accumulated = local.multiply(accumulated);
        }
        // The image of the border box's centre is the centre of the client rect,
        // because an affine map sends a rectangle's centre to the centre of the
        // parallelogram it becomes, and that parallelogram's bounding box shares
        // that centre. So the translation that places element space on the page
        // follows from the rect, with no transform-origin bookkeeping.
        accumulated.e = 0;
        accumulated.f = 0;
        const centre = accumulated.transformPoint(
          new DOMPoint(width / 2, height / 2));
        accumulated.e =
          rect.left + rect.width / 2 + window.scrollX - centre.x;
        accumulated.f =
          rect.top + rect.height / 2 + window.scrollY - centre.y;
        matrix = accumulated;
      }
      // Same predicate as the box beside it, and for the same reason: no extent
      // on EITHER axis carries no direction to sweep along, while one zero axis
      // is a stroke that could not be read and must still be placed. Written
      // against the box it actually guards -- a rotated line is fat in the
      // client rect and zero-width in its own space, so a guard inherited from
      // the other box passes exactly where it should refuse and refuses exactly
      // where it should pass.
      if (!(width > 0) && !(height > 0)) return null;
      if (!matrix || !Number.isFinite(matrix.a) || !Number.isFinite(matrix.b) ||
          !Number.isFinite(matrix.c) || !Number.isFinite(matrix.d))
        return null;
      return {
        intrinsic: { x, y, width, height },
        transform: [matrix.a, matrix.b, matrix.c, matrix.d,
          matrix.e, matrix.f]
      };
    } catch (e) {
      return null;
    }
  };
  // The page-space footprint of an SVG <line> stroke, used only to erase pixels
  // from the captured sprite. Geometry deliberately keeps stroke recovery out
  // of the radial extent; erasure has the opposite contract and must cover the
  // painted stroke exactly. Rotating a line gives its bare centreline positive
  // width and height, so the older zero-axis recovery cannot see this case.
  //
  // This is line-specific on purpose. Expanding a generic shape's getBBox by
  // half the stroke is destructive guesswork around joins; a pointer shape the
  // browser cannot describe exactly keeps the existing client rect instead.
  const svgStrokeFootprint = marked => {
    try {
      if (typeof marked.getBBox !== 'function' ||
          typeof marked.getScreenCTM !== 'function') return null;
      if ((marked.tagName || '').toLowerCase() !== 'line') return null;
      const style = window.getComputedStyle(marked);
      const stroke = parseFloat(style.strokeWidth);
      if (!(stroke > 0) || !style.stroke || style.stroke === 'none') return null;
      const ctm = marked.getScreenCTM();
      if (!ctm) return null;
      let x1 = marked.x1.baseVal.value;
      let y1 = marked.y1.baseVal.value;
      let x2 = marked.x2.baseVal.value;
      let y2 = marked.y2.baseVal.value;
      const dx = x2 - x1;
      const dy = y2 - y1;
      const length = Math.hypot(dx, dy);
      if (!(length > 0)) return null;
      const nonScaling = (style.vectorEffect || '').trim() ===
        'non-scaling-stroke';
      if (nonScaling) {
        let first = {
          x: ctm.a * x1 + ctm.c * y1 + ctm.e,
          y: ctm.b * x1 + ctm.d * y1 + ctm.f
        };
        let second = {
          x: ctm.a * x2 + ctm.c * y2 + ctm.e,
          y: ctm.b * x2 + ctm.d * y2 + ctm.f
        };
        const pageDx = second.x - first.x;
        const pageDy = second.y - first.y;
        const pageLength = Math.hypot(pageDx, pageDy);
        if (!(pageLength > 0)) return null;
        const pageUx = pageDx / pageLength;
        const pageUy = pageDy / pageLength;
        const pageHalfStroke = stroke / 2;
        const cap = (style.strokeLinecap || 'butt').trim();
        if (cap === 'square') {
          first = { x: first.x - pageUx * pageHalfStroke,
            y: first.y - pageUy * pageHalfStroke };
          second = { x: second.x + pageUx * pageHalfStroke,
            y: second.y + pageUy * pageHalfStroke };
        }
        const pagePx = -pageUy * pageHalfStroke;
        const pagePy = pageUx * pageHalfStroke;
        const points = [
          [first.x + pagePx, first.y + pagePy],
          [first.x - pagePx, first.y - pagePy],
          [second.x + pagePx, second.y + pagePy],
          [second.x - pagePx, second.y - pagePy]
        ];
        let left = Math.min(...points.map(point => point[0]));
        let top = Math.min(...points.map(point => point[1]));
        let right = Math.max(...points.map(point => point[0]));
        let bottom = Math.max(...points.map(point => point[1]));
        if (cap === 'round') {
          left = Math.min(left, first.x - pageHalfStroke,
            second.x - pageHalfStroke);
          top = Math.min(top, first.y - pageHalfStroke,
            second.y - pageHalfStroke);
          right = Math.max(right, first.x + pageHalfStroke,
            second.x + pageHalfStroke);
          bottom = Math.max(bottom, first.y + pageHalfStroke,
            second.y + pageHalfStroke);
        }
        return { left, top, width: right - left, height: bottom - top };
      }
      const ux = dx / length;
      const uy = dy / length;
      const halfStroke = stroke / 2;
      const cap = (style.strokeLinecap || 'butt').trim();
      if (cap === 'square') {
        x1 -= ux * halfStroke;
        y1 -= uy * halfStroke;
        x2 += ux * halfStroke;
        y2 += uy * halfStroke;
      }
      const px = -uy * halfStroke;
      const py = ux * halfStroke;
      const points = [
        [x1 + px, y1 + py], [x1 - px, y1 - py],
        [x2 + px, y2 + py], [x2 - px, y2 - py]
      ].map(
        ([x, y]) => ({
          x: ctm.a * x + ctm.c * y + ctm.e,
          y: ctm.b * x + ctm.d * y + ctm.f
        }));
      const xs = points.map(point => point.x);
      const ys = points.map(point => point.y);
      let left = Math.min(...xs);
      let top = Math.min(...ys);
      let right = Math.max(...xs);
      let bottom = Math.max(...ys);
      if (cap === 'round') {
        const radiusX = halfStroke * Math.hypot(ctm.a, ctm.c);
        const radiusY = halfStroke * Math.hypot(ctm.b, ctm.d);
        for (const [x, y] of [[x1, y1], [x2, y2]]) {
          const cx = ctm.a * x + ctm.c * y + ctm.e;
          const cy = ctm.b * x + ctm.d * y + ctm.f;
          left = Math.min(left, cx - radiusX);
          top = Math.min(top, cy - radiusY);
          right = Math.max(right, cx + radiusX);
          bottom = Math.max(bottom, cy + radiusY);
        }
      }
      return {
        left,
        top,
        width: right - left,
        height: bottom - top
      };
    } catch (e) {
      return null;
    }
  };
  const indicatorBox = element => {
    try {
      const marked = element.querySelector('[data-pulp-indicator]');
      if (!marked) return null;
      const box = marked.getBoundingClientRect();
      let { left, top, width, height } = box;
      if (!(width > 0) && !(height > 0)) return null;
      // The element's own geometry, recorded ALONGSIDE the client rect rather
      // than instead of it. The rect stays exactly what it was: it is the
      // pointer's painted footprint, which is what the sprite pass crops and
      // erases, and for that job a rotated needle's fat box is the right answer.
      const geometry = indicatorGeometry(marked, box);
      if (!(width > 0) || !(height > 0)) {
        // Grown about the line's own centre, so the box still describes what is
        // painted AND its centroid -- which is what the radial projection
        // downstream actually reads -- is left where it was.
        //
        // A stroke that cannot be recovered leaves the axis at zero rather than
        // dropping the box. The consumer places such a pointer correctly from
        // its radial extent and falls back to a default THICKNESS, which is a
        // thin pointer in the right place. Dropping it instead costs the whole
        // sprite lane: with no geometry the knob keeps its captured body, the
        // designed-body painter is reinstalled, and it comes back wearing the
        // value arc and track ring over a face whose pointer was never erased.
        const stroke = strokePaintedExtent(marked);
        if (stroke > 0) {
          if (!(width > 0)) { left -= stroke / 2; width = stroke; }
          if (!(height > 0)) { top -= stroke / 2; height = stroke; }
        }
      }
      const strokeBox = svgStrokeFootprint(marked);
      if (strokeBox) {
        const right = Math.max(left + width, strokeBox.left + strokeBox.width);
        const bottom = Math.max(top + height, strokeBox.top + strokeBox.height);
        left = Math.min(left, strokeBox.left);
        top = Math.min(top, strokeBox.top);
        width = right - left;
        height = bottom - top;
      }
      const bounds = {
        left: left + window.scrollX,
        top: top + window.scrollY,
        width,
        height
      };
      if (geometry) {
        bounds.intrinsic = geometry.intrinsic;
        bounds.transform = geometry.transform;
      }
      return bounds;
    } catch (e) {
      return null;
    }
  };
  // The pointer's resolved colour, read the same way the accent is: from
  // computed style, so a value set anywhere up the tree arrives resolved.
  //
  // background-color first (a dot or wedge is a filled box), then the border
  // and text colours (a hairline drawn as a border, or a glyph). An author
  // whose pointer is a gradient or a mask -- where no single computed colour is
  // the right answer -- states it as the attribute's own value, which wins.
  //
  // An SVG pointer is painted by NEITHER background nor border: a line or a
  // path carries its colour in stroke, a filled arrowhead in fill, and both
  // leave background-color transparent. Walking past them lands such a pointer
  // on the inherited text colour, and an authored #101014 needle arrived as
  // rgb(232, 232, 238) -- near-black reported as near-white, on a knob that
  // still rendered and still passed every pixel gate.
  //
  // The SVG branch sits BETWEEN background and border, and answers even when it
  // finds nothing, because both of its neighbours would otherwise answer for
  // it. An SVG element's border-color initial value is currentColor, so a
  // branch appended after borderTopColor never runs at all; and the color
  // fallback behind it is the original defect by another name.
  //
  // fill computes to opaque BLACK on every element -- a div, an svg root, a g,
  // a stroke-only line -- rather than to none. That is why the branch is
  // namespace-guarded AND why fill is consulted only on the shapes a fill
  // actually paints: reading it unguarded hands every borderless div pointer a
  // black it never had, and reading it on an svg root reports a black that
  // element paints nowhere. url(#...) paint is a gradient or a pattern rather
  // than a colour, and is absent for the same reason the vector lowering
  // refuses paint_reference.
  //
  // An unpainted pointer reports nothing, and the consumer draws the widget's
  // derived tick. That is the honest answer; a confident wrong colour is not.
  const svgNamespace = 'http://www.w3.org/2000/svg';
  const fillPaintedShapes =
    new Set(['path', 'polygon', 'circle', 'ellipse', 'rect']);
  const indicatorColor = element => {
    try {
      const marked = element.querySelector('[data-pulp-indicator]');
      if (!marked) return '';
      const declared = (marked.getAttribute('data-pulp-indicator') || '').trim();
      if (declared) return declared;
      const style = window.getComputedStyle(marked);
      const opaque = value => {
        const text = (value || '').trim();
        if (!text || text === 'transparent' || text === 'none') return '';
        // A paint server reference names a gradient or a pattern; no single
        // colour describes it, so it is reported as no colour at all.
        if (text.indexOf('url(') === 0) return '';
        // rgba(...) with a zero alpha is the computed form of "not painted".
        if (text.indexOf('rgba(') === 0 && text.replace(/ /g, '').indexOf(',0)') > 0)
          return '';
        return text;
      };
      const background = opaque(style.backgroundColor);
      if (background) return background;
      if (marked.namespaceURI === svgNamespace) {
        const stroked = opaque(style.stroke);
        if (stroked) return stroked;
        if (!fillPaintedShapes.has(marked.tagName.toLowerCase())) return '';
        return opaque(style.fill);
      }
      return opaque(style.borderTopColor) || opaque(style.color);
    } catch (e) {
      return '';
    }
  };
  // The accent this control is actually drawn in, resolved by the browser.
  //
  // Reading the PACK's accent token instead is wrong whenever a panel scopes or
  // overrides its palette -- which a good one does -- because the token set is
  // the pack default, not what this control ended up. That mismatch paints a
  // green value arc onto an orange knob: our render comes out worse than the
  // browser's, and every pixel gate still passes because the arc is ours alone.
  //
  // --accent-ring first: a pack that distinguishes the ring from the general
  // accent means the ring colour for the ring. Computed style is what makes
  // this correct -- it resolves inheritance and inline overrides alike, so an
  // accent set anywhere up the tree arrives here already resolved.
  const accentColor = element => {
    try {
      const painted = element.querySelector('[data-pulp-paint]') || element;
      const style = window.getComputedStyle(painted);
      const ring = style.getPropertyValue('--accent-ring').trim();
      if (ring) return ring;
      return style.getPropertyValue('--accent').trim();
    } catch (e) {
      return '';
    }
  };
  // A design-system component states what it is in its class. A pulp-knob is a
  // styled div whose drag handlers sit on an ancestor, so it has no native tag,
  // no ARIA role and no inline handler -- every signal below walks past it, and
  // a six-knob sampler imported as eleven candidates that were all buttons.
  //
  // NOTE: this whole region is inside the template literal returned by
  // semanticExpression() and is evaluated IN THE PAGE. No backticks, and no
  // backslash escapes: the template literal eats them, so a regex written with
  // one arrives stripped. Splitting on a plain space is deliberate -- a class
  // attribute is space separated and needs no escape.
  const componentKinds = new Map([
    ['pulp-knob', 'knob'], ['pulp-fader', 'fader'], ['pulp-slider', 'slider'],
    ['pulp-range', 'range'], ['pulp-switch', 'toggle'], ['pulp-check', 'toggle'],
    ['pulp-radio', 'radio'], ['pulp-stepper', 'stepper'],
    ['pulp-numbox', 'number_input'], ['pulp-numfield', 'number_input'],
    ['pulp-valedit', 'number_input'], ['pulp-meter', 'meter'],
    ['pulp-barmeter', 'meter'], ['pulp-ledmeter', 'meter'],
    ['pulp-spectrum', 'meter'], ['pulp-scope', 'meter'],
    ['pulp-progress', 'meter'], ['pulp-tab', 'tab'],
    ['pulp-combo', 'select'], ['pulp-select', 'select'],
  ]);
  const componentKind = element => {
    try {
      const raw = element && element.getAttribute ? element.getAttribute('class') : '';
      if (!raw) return '';
      const parts = String(raw).split(' ');
      for (let i = 0; i < parts.length; i++) {
        const hit = componentKinds.get(parts[i]);
        if (hit) return hit;
      }
    } catch (e) { return ''; }
    return '';
  };
  const strongSignal = element => {
    const data = dataPulp(element);
    const role = (element.getAttribute('role') || '').toLowerCase();
    return Object.keys(data).length > 0 || nativeKind(element) || componentKind(element) ||
      roleKinds.has(role) || element.hasAttribute('onclick') ||
      element.hasAttribute('onpointerdown') ||
      element.hasAttribute('onmousedown') ||
      element.hasAttribute('onwheel') ||
      element.hasAttribute('onkeydown') ||
      typeof element.onclick === 'function' ||
      typeof element.onpointerdown === 'function' ||
      typeof element.onmousedown === 'function' ||
      typeof element.onwheel === 'function' ||
      typeof element.onkeydown === 'function';
  };
  const strongAncestor = element => {
    for (let parent = element.parentElement; parent; parent = parent.parentElement) {
      if (strongSignal(parent)) return true;
    }
    return false;
  };
  const candidates = [];
  for (let domIndex = 0; domIndex < elements.length; domIndex++) {
    const element = elements[domIndex];
    const style = getComputedStyle(element);
    const rect = element.getBoundingClientRect();
    if (style.display === 'none' || style.visibility === 'hidden' ||
        Number(style.opacity || 1) <= 0.001 ||
        rect.width <= 0.25 || rect.height <= 0.25) continue;

    const data = dataPulp(element);
    const role = (element.getAttribute('role') || '').toLowerCase();
    const native = nativeKind(element);
    const explicitKind = data.kind || '';
    const roleKind = roleKinds.get(role) || '';
    const eventNames = [
      'onclick', 'onpointerdown', 'onmousedown', 'onwheel', 'onkeydown'
    ];
    const hasEvent = eventNames.some(name =>
      element.hasAttribute(name) || typeof element[name] === 'function');
    const snapshotClick = snapshotClickable.has(domIndex);
    const interactiveCursor = [
      'pointer', 'grab', 'grabbing', 'ew-resize', 'ns-resize',
      'col-resize', 'row-resize'
    ].includes(style.cursor);
    const pointerRoot = interactiveCursor &&
      (!element.parentElement ||
       getComputedStyle(element.parentElement).cursor !== style.cursor);
    // NOTE: this inline predicate -- not strongSignal() above, which only feeds
    // strongAncestor -- is the actual admission gate. A recogniser added to the
    // function alone has no effect; that cost one silent no-op round.
    const component = componentKind(element);
    const strong = Object.keys(data).length > 0 || native || roleKind ||
      component || hasEvent || snapshotClick;
    if (!strong && (!pointerRoot || strongAncestor(element))) continue;

    const evidence = [];
    if (Object.keys(data).length) {
      evidence.push({
        kind: 'data-pulp',
        value: JSON.stringify(data),
        strength: 'strong'
      });
    }
    if (native) {
      evidence.push({
        kind: 'native-element',
        value: element.tagName.toLowerCase() +
          (element.getAttribute('type') ? ':' + element.getAttribute('type') : ''),
        strength: 'strong'
      });
    }
    if (roleKind) {
      evidence.push({ kind: 'aria-role', value: role, strength: 'strong' });
    }
    if (hasEvent) {
      evidence.push({ kind: 'event-handler', value: 'present', strength: 'medium' });
    }
    if (snapshotClick) {
      evidence.push({
        kind: 'dom-snapshot-clickable',
        value: 'true',
        strength: 'medium'
      });
    }
    if (!strong && pointerRoot) {
      evidence.push({ kind: 'cursor', value: 'pointer', strength: 'weak' });
    }

    const kind = explicitKind || native || roleKind || component || 'unknown';
    const conflicts = [];
    if (explicitKind && native && explicitKind !== native) {
      conflicts.push('explicit kind differs from native element kind');
    }
    const name = element.getAttribute('aria-label') ||
      element.getAttribute('title') ||
      (element.innerText || element.textContent || '').replace(/\\s+/g, ' ').trim()
        .slice(0, 200);
    // A component class is as declarative as an ARIA role -- the author named
    // the control -- so it scores alongside one rather than falling to the
    // weak-signal floor, which is where these landed when the recogniser fed
    // the gate and the kind but not the score.
    const confidence = explicitKind ? 1 :
      native ? 0.95 : roleKind ? 0.9 : component ? 0.9 : hasEvent ? 0.7 : 0.35;
    candidates.push({
      id: 'browser-node:' + domIndex,
      dom_index: domIndex,
      backend_node_id: null,
      source_node_id: data.node || element.id || '',
      tag: element.tagName.toLowerCase(),
      role,
      name,
      kind,
      bounds: {
        left: rect.left + window.scrollX,
        top: rect.top + window.scrollY,
        width: rect.width,
        height: rect.height
      },
      // The box a live widget must paint its value geometry into, which is NOT
      // the component box: a knob's component includes its caption (measured
      // 116x139.9 where the dial is 116x116), and a fader's track is inset and
      // far narrower (633,274.9,26x150 vs 643,274.9,6x150). Painting a value
      // ring into the component box puts it ~12px below the dial centre -- it
      // renders, it looks almost right, and no gate can see it.
      //
      // The author declares it, exactly as they declare the binding, because
      // only the author knows which child is the painted control. Inferring it
      // would hard-code one design system's class vocabulary into the importer.
      paint_bounds: paintBox(element),
      // The design's own pointer geometry, when it declared one. The lowering
      // turns it into the same knob_ind_* vocabulary the Figma lane already
      // writes, so the consumer has ONE name for the thing rather than one per
      // importer.
      indicator_bounds: indicatorBox(element),
      indicator_color: indicatorColor(element),
      accent: accentColor(element),
      data_pulp: data,
      evidence,
      confidence,
      resolved: kind !== 'unknown',
      binding_status: data.param || data.meter || data.action ? 'bound' : 'unbound',
      conflicts
    });
  }
  return candidates;
})()`;
}

// The snapshot node indexes that correspond, one for one and in order, to the
// elements `document.querySelectorAll('*')` returns in the page. Everything a
// candidate carries from the snapshot -- its backend id, its paint order -- is
// looked up through this correspondence, so a single extra node shifts every
// later element onto its neighbour's data.
//
// nodeType === 1 alone is NOT that correspondence. The snapshot also emits
// pseudo-element boxes (::before / ::after) as element nodes, and
// querySelectorAll cannot return them, so one ::before anywhere on the page
// re-points every control after it at the node before its own -- a control
// that still resolves, still has plausible data, and is simply the wrong node.
// Shadow-tree content is excluded for the same reason: querySelectorAll does
// not pierce a shadow root.
//
// parentIndex is emitted in document order, so a node's parent always appears
// before it and the shadow-tree flag is settled by the time a child is read.
function snapshotElementNodes(snapshot) {
  const nodes = snapshot.documents?.[0]?.nodes;
  if (!nodes) return [];
  const total = nodes.nodeType?.length ?? 0;
  const pseudoNodes = new Set(nodes.pseudoType?.index ?? []);
  const shadowRoots = new Set(nodes.shadowRootType?.index ?? []);
  const parents = nodes.parentIndex ?? [];
  const inShadowTree = new Array(total).fill(false);
  const result = [];
  for (let index = 0; index < total; index++) {
    const parent = parents[index] ?? -1;
    inShadowTree[index] = shadowRoots.has(index) ||
      (parent >= 0 && parent < index && inShadowTree[parent]);
    if (nodes.nodeType[index] !== 1) continue;
    if (pseudoNodes.has(index) || inShadowTree[index]) continue;
    result.push(index);
  }
  return result;
}

function elementBackendIds(snapshot) {
  const nodes = snapshot.documents?.[0]?.nodes;
  if (!nodes) return [];
  return snapshotElementNodes(snapshot).map(
    (index) => nodes.backendNodeId?.[index] ?? null);
}

// Lowercased element names in the same order, so a candidate can be checked
// against the node it was matched to instead of trusting the correspondence.
function elementTagNames(snapshot) {
  const nodes = snapshot.documents?.[0]?.nodes;
  const strings = snapshot.strings;
  if (!nodes || !Array.isArray(strings)) return [];
  return snapshotElementNodes(snapshot).map((index) => {
    const name = strings[nodes.nodeName?.[index]];
    return typeof name === "string" ? name.toLowerCase() : "";
  });
}

// Chromium's own answer to "what paints on top of what", one integer per laid
// out node, taken from the paint order the capture already requests with
// includePaintOrder.
//
// This is consumed, never recomputed. Re-deriving order from z-index is the
// same mistake as re-solving layout from computed styles: stacking contexts,
// opacity, transforms, filters, will-change and position all create or escape
// contexts, so a z-index sort agrees with Chromium on simple pages and diverges
// silently on exactly the layered panels this exists for.
//
// Returns an array parallel to the element ordering the candidate walk uses,
// holding null for elements with no layout entry.
function elementPaintOrders(snapshot) {
  const document = snapshot.documents?.[0];
  const nodes = document?.nodes;
  const layout = document?.layout;
  if (!nodes) return [];
  const nodeIndex = layout?.nodeIndex;
  const paintOrders = layout?.paintOrders;
  // A capture that asked for paint order and did not get it must not resolve to
  // a tree of nulls that reads as "this page has no layering". Chromium omits
  // paintOrders only when includePaintOrder was not requested, so a laid out
  // document without it means the request and the reader have drifted apart.
  if (Array.isArray(nodeIndex) && nodeIndex.length > 0) {
    if (!Array.isArray(paintOrders)) {
      const error = new Error(
        "DOMSnapshot returned layout without paintOrders; the capture must " +
        "request includePaintOrder");
      error.code = "capture-paint-order-missing";
      throw error;
    }
    if (paintOrders.length !== nodeIndex.length) {
      const error = new Error(
        `DOMSnapshot paintOrders length ${paintOrders.length} does not match ` +
        `layout nodeIndex length ${nodeIndex.length}`);
      error.code = "capture-paint-order-mismatch";
      throw error;
    }
  }
  // Layout entries are not one per node: a box that also lays out an inline
  // text box contributes two entries for the same node index (a ::before with
  // generated text does exactly this). The first entry is the node's own box,
  // so keeping it makes the choice defined rather than last-write-wins.
  const orderByNode = new Map();
  for (let entry = 0; entry < (nodeIndex?.length ?? 0); entry++) {
    const order = paintOrders[entry];
    if (typeof order !== "number") continue;
    if (!orderByNode.has(nodeIndex[entry])) {
      orderByNode.set(nodeIndex[entry], order);
    }
  }
  return snapshotElementNodes(snapshot).map(
    (index) => (orderByNode.has(index) ? orderByNode.get(index) : null));
}

function clickableElementIndexes(snapshot) {
  const clickableNodeIndexes = new Set(
    snapshot.documents?.[0]?.nodes?.isClickable?.index ?? []);
  const result = [];
  snapshotElementNodes(snapshot).forEach((nodeIndex, elementIndex) => {
    if (clickableNodeIndexes.has(nodeIndex)) result.push(elementIndex);
  });
  return result;
}

export async function evaluateSemantics(cdp, snapshot, viewport) {
  const result = await cdp.call("Runtime.evaluate", {
    expression: semanticExpression(clickableElementIndexes(snapshot)),
    returnByValue: true,
  });
  const candidates = result.result?.value ?? [];
  const backendIds = elementBackendIds(snapshot);
  const paintOrders = elementPaintOrders(snapshot);
  const tagNames = elementTagNames(snapshot);
  for (const candidate of candidates) {
    // The page walk and the snapshot walk have to agree on which element index
    // N is, and when they disagree every value below is a neighbour's. The
    // element name is the cheap witness both sides already carry, so the
    // mismatch surfaces here rather than as a control wearing another node's
    // paint order.
    const snapshotTag = tagNames[candidate.dom_index];
    if (snapshotTag && candidate.tag && snapshotTag !== candidate.tag) {
      const error = new Error(
        `page element ${candidate.dom_index} is <${candidate.tag}> but the ` +
        `DOM snapshot has <${snapshotTag}> at that index; the element walks ` +
        "have drifted apart");
      error.code = "capture-node-alignment-mismatch";
      throw error;
    }
    candidate.backend_node_id = backendIds[candidate.dom_index] ?? null;
    // Explicitly null rather than absent when the element has no layout entry,
    // so a consumer can distinguish "not painted" from "order not collected".
    candidate.paint_order = paintOrders[candidate.dom_index] ?? null;
    candidate.source_index = candidate.dom_index;
    delete candidate.dom_index;
  }
  const resolved = candidates.filter((candidate) => candidate.resolved).length;
  return {
    schema: "pulp-browser-semantics-v1",
    version: 1,
    viewport,
    summary: {
      candidates: candidates.length,
      resolved,
      unresolved: candidates.length - resolved,
      bound: candidates.filter(
        (candidate) => candidate.binding_status === "bound").length,
      conflicted: candidates.filter(
        (candidate) => candidate.conflicts.length > 0).length,
      // Visible in the artifact so a wholesale paint-order miss shows up as a
      // count of zero next to a non-zero candidate count, rather than as a
      // field every reader quietly treats as optional.
      paint_ordered: candidates.filter(
        (candidate) => candidate.paint_order !== null).length,
    },
    limitations: [
      "Canvas and WebGL internal hit regions are not recoverable from the DOM; " +
      "their host surface is reported as one unresolved candidate when clickable.",
    ],
    candidates,
  };
}
