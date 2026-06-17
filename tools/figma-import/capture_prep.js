/* capture_prep.js — pre-capture DOM hardening for the HTML->Figma pipeline.
 *
 * html-to-design (generate_figma_design) captures the live DOM but cannot
 * faithfully reproduce two CSS features the Pulp design system relies on. This
 * script rewrites those into capture-safe equivalents IN THE PAGE before the
 * capture fires, with zero layout change. Two fixes:
 *
 *  1) Knob value ring — .pulp-knob .ring is a CSS conic-gradient masked to a
 *     6px outer annulus. The capture flattens conic-gradient+mask and drops the
 *     ring (track + teal value arc vanish). We replace it with an equivalent
 *     inline <svg> of polyline-sampled arcs that renders identically AND
 *     survives capture as crisp vector. Colors are read from live computed
 *     styles, so it is theme-correct (dark + light) automatically.
 *
 *  2) Text descender clipping — short text holders use overflow:hidden with a
 *     tight line-height; glyph descenders (g/p/y) bleed below the line box and
 *     the capture clips text frames at that box, cutting the tails. We set
 *     overflow:visible on text-only leaves whose text FITS horizontally (so no
 *     intended truncation is touched) — descenders then render fully, no reflow.
 *
 * Must run AFTER web fonts load (glyph metrics are wrong under the fallback
 * font); idempotent so the fonts.ready + timeout double-trigger is safe.
 */
(function () {
  function deg(v, fb) { const n = parseFloat(v); return isNaN(n) ? fb : n; }
  // conic angle (from top, clockwise) -> svg point (y-down)
  function pt(cx, cy, R, a) {
    const r = (a * Math.PI) / 180;
    return [cx + R * Math.sin(r), cy - R * Math.cos(r)];
  }
  function arcPath(cx, cy, R, a0, a1) {
    // sample every ~2deg; guarantees correct direction without SVG flag pitfalls
    const step = 2, pts = [];
    const n = Math.max(1, Math.ceil(Math.abs(a1 - a0) / step));
    for (let i = 0; i <= n; i++) {
      const a = a0 + ((a1 - a0) * i) / n;
      const [x, y] = pt(cx, cy, R, a);
      pts.push((i ? 'L' : 'M') + x.toFixed(2) + ' ' + y.toFixed(2));
    }
    return pts.join(' ');
  }
  function fixKnob(k) {
    const ring = k.querySelector('.ring');
    const dial = k.querySelector('.dial');
    if (!ring || !dial) return;
    if (ring.querySelector('[data-knob-ring]')) return;    // idempotent
    const cs = getComputedStyle(k);
    const accent = cs.getPropertyValue('--accent').trim() || '#16DAC2';
    const track = cs.getPropertyValue('--line-strong').trim() || 'rgba(220,232,250,0.22)';
    const D = dial.getBoundingClientRect().width;         // dial diameter
    const S = D + 12;                                      // ring box (inset -6)
    const c = S / 2;                                       // center
    const R = D / 2 + 3;                                   // mid-annulus radius
    const sw = 6;                                          // ring thickness
    const isPan = k.classList.contains('pan');
    const _l = deg(dial.style.getPropertyValue('--_l'), 0);
    const _r = deg(dial.style.getPropertyValue('--_r'), 0);
    const _deg = deg(dial.style.getPropertyValue('--_deg'), 160);
    // Layer 1: one continuous faint TRACK arc (no seam) — 280deg with an 80deg
    // gap centered at the bottom (140..220). Drawn 220 -> 500 (== 140 mod 360).
    // Layer 2: the teal VALUE arc(s) painted on top, identical radius/width.
    const trackSeg = [220, 500, track, false];
    let valSegs;
    if (isPan) {
      // bipolar from the top: right value 0..r, left value (360-l)..360
      valSegs = [
        [0, _r, accent, true],
        [360 - _l, 360, accent, true],
      ];
    } else {
      valSegs = [[220, 220 + _deg, accent, true]];
    }
    const segs = [trackSeg, ...valSegs];
    const paths = segs
      .filter((s) => Math.abs(s[1] - s[0]) > 0.01)
      .map((s) => {
        const glow = s[3] ? ' style="filter:drop-shadow(0 0 3px ' + s[2] + ')"' : '';
        return (
          '<path d="' + arcPath(c, c, R, s[0], s[1]) + '" fill="none" stroke="' +
          s[2] + '" stroke-width="' + sw + '" stroke-linecap="butt"' + glow + '/>'
        );
      })
      .join('');
    // The .ring host is ALREADY inset:-6px on the dial (so it is S px wide and
    // centered on the dial). The SVG must FILL that host (inset:0), not add a
    // second offset, or it scales/shifts outward.
    const svg =
      '<svg width="' + S + '" height="' + S + '" viewBox="0 0 ' + S + ' ' + S +
      '" style="position:absolute;inset:0;width:100%;height:100%;overflow:visible" data-knob-ring="1">' +
      paths + '</svg>';
    // neutralize the original conic-gradient + mask, keep the element as host
    ring.style.background = 'none';
    ring.style.webkitMaskImage = 'none';
    ring.style.maskImage = 'none';
    ring.style.filter = 'none';
    ring.innerHTML = svg;
  }
  // ---- text de-clip ------------------------------------------------------
  // Short text holders in these cards use overflow:hidden with a tight
  // line-height, so glyph descenders (g, p, y) get clipped ~1px in CSS and
  // MORE by the html-to-design capture (it sizes text frames to the line box).
  // Setting overflow:visible on a text-only leaf whose content actually
  // overflows restores full descenders WITHOUT changing layout (the descender
  // just bleeds into the padding it already sits over). We never touch tall
  // scroll/clip containers or elements with element children.
  function declipText() {
    // Glyph descender INK can extend below the CSS line box (Jost g/p/y), and
    // the capture clips text frames at that line box — so a Range/box measure
    // underestimates the real clip. The safe, precise rule: declip any short
    // text-only leaf that is NOT horizontally overflowing (scrollWidth fits),
    // so we never disturb intended horizontal truncation, and overflow:visible
    // causes no reflow — descenders simply bleed into the padding they overlap.
    const leaves = Array.from(document.querySelectorAll('*')).filter((e) => {
      if (e.children.length) return false;                 // text-only leaf
      if (!e.textContent.trim()) return false;
      const cs = getComputedStyle(e);
      if (cs.overflowY === 'visible' && cs.overflowX === 'visible') return false;
      const r = e.getBoundingClientRect();
      if (r.height > 40) return false;                     // not a scroll box
      // scrollWidth <= clientWidth means the text FITS — no horizontal
      // truncation is actually happening, so declipping is safe even if the
      // element declares text-overflow:ellipsis (a no-op when text fits).
      // Truly-overflowing ellipsis text fails this and stays clipped.
      return e.scrollWidth <= e.clientWidth + 1;
    });
    leaves.forEach((e) => { e.style.overflow = 'visible'; });
    return leaves.length;
  }

  // ---- waveform vertical centering --------------------------------------
  // The waveform <svg> (.wscope) uses viewBox "0 0 1000 200" +
  // preserveAspectRatio="none" but is NOT height-constrained, so it renders at
  // its intrinsic aspect-ratio height (taller than the track), top-aligned and
  // overflowing — pushing the zero-axis (viewBox y=100) below the track's true
  // center. The canonical design fills the track. height:100% makes the
  // stretched viewBox map y=100 to the track midline → vertically centered.
  // NOTE: an earlier waveform-centering backstop lived here. The off-center
  // waveform was a Claude design *export* bug (svg had no height constraint);
  // it was fixed upstream (Pulp Design System-4 / Components-5), where the svg
  // now carries height:100% itself and the recorder mini-scopes intentionally
  // use height:calc(100% - 24px) for label room. A blanket .wscope svg{height
  // :100%!important} override REINTRODUCED a 12px offset on those mini-scopes,
  // so it was removed. Fix capture issues at the source when the source owns
  // the bug; only compensate in capture_prep for true capture-engine gaps.
  // ---- stepper crisp edges ----------------------------------------------
  // The active (green) .nb-step has radius 0 and relies on the parent
  // .pulp-numbox's border-radius + overflow:hidden to round its OUTER corners.
  // html-to-design softens that rounded clip, leaving a fuzzy/haloed edge.
  // Giving the active stepper its OWN matching radius on the outer side makes
  // the green a crisp vector rounded rect that the capture renders cleanly.
  // (Capture-engine gap, not a source bug — so we compensate here.)
  function crispSteppers() {
    let n = 0;
    document.querySelectorAll('.nb-step.is-active').forEach((act) => {
      const parent = act.parentElement;
      if (!parent || !/numbox/i.test(parent.className)) return;
      const pcs = getComputedStyle(parent);
      const pr = parseFloat(pcs.borderTopLeftRadius) || 0;
      const bw = parseFloat(pcs.borderTopWidth) || 0;
      const r = Math.max(0, pr - bw); // inner radius
      if (!r) return;
      const kids = Array.from(parent.children);
      const idx = kids.indexOf(act);
      if (idx === 0) {                          // left stepper → round left side
        act.style.borderTopLeftRadius = r + 'px';
        act.style.borderBottomLeftRadius = r + 'px';
      } else if (idx === kids.length - 1) {     // right stepper → round right side
        act.style.borderTopRightRadius = r + 'px';
        act.style.borderBottomRightRadius = r + 'px';
      }
      n++;
    });
    return n;
  }

  function run() {
    declipText();
    crispSteppers();
    document.querySelectorAll('.pulp-knob').forEach(fixKnob);
  }
  // Must run AFTER web fonts load — glyph descender metrics (used to detect
  // clipping) are wrong while the fallback font is active. fonts.ready resolves
  // once Jost/JetBrains Mono are applied; fall back to a timeout just in case.
  function start() {
    if (document.fonts && document.fonts.ready) {
      document.fonts.ready.then(run);
      setTimeout(run, 1200); // belt-and-suspenders; run() is idempotent
    } else {
      setTimeout(run, 400);
    }
  }
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', start);
  } else {
    start();
  }
})();
