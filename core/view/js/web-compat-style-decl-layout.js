// ═══════════════════════════════════════════════════════════════════════════════
// CSSStyleDeclaration — layout domain handler (P5-5 split of _applyProperty)
// ═══════════════════════════════════════════════════════════════════════════════
//
// Handles the Yoga flex / grid / box-model / dimensions / positioning
// CSS properties. `_applyLayoutProp(decl, id, key, resolved, value)`
// returns true if it claimed the key, false otherwise — exactly the
// per-domain dispatcher contract used by the @pulp/react prop-applier
// split (prop-applier-layout.ts). Each `case` body below is byte-
// identical to the matching arm of the pre-split `_applyProperty`
// switch — same bridge calls, same order.
//
// `decl` is the CSSStyleDeclaration `this`; the display case reads
// `decl._props` and the position/zIndex cases call
// `decl._reevaluateOverlay()`, so the instance must be threaded in
// explicitly rather than relying on `this`-binding.
//
// Embed order: loaded AFTER web-compat-style-decl.js (the dispatcher).
// This is a plain function declaration so it hoists across embed
// boundaries; the dispatcher only references it at call time.

function _applyLayoutProp(decl, id, key, resolved, value) {
    switch (key) {
        // Display / flex direction
        case "display":
            if (resolved === "none") { setVisible(id, false); }
            else if (resolved === "flex" || resolved === "block" ||
                     resolved === "inline-block" || resolved === "inline-flex") {
                // pulp #1420 — `inline-block` ≡ `block` and `inline-flex`
                // ≡ `flex` in pulp's non-text-flowing layout system. This
                // matches react-native semantics and CSS spec for
                // formatting contexts that don't have inline flow.
                setVisible(id, true);
                // CSS web-compat: `display: flex` defaults to flex-direction: row.
                // Pulp's underlying widgets default to FlexDirection::column (RN
                // convention), so we explicitly emit a row-direction here unless
                // the consumer ALSO declared flexDirection / flex-direction —
                // in which case the explicit declaration overrides this default
                // either later in this flush (individual setters apply in order)
                // or via _flushAll's iteration. See pulp #1147.
                if (resolved === "flex" || resolved === "inline-flex") {
                    var hasExplicitDirection =
                        Object.prototype.hasOwnProperty.call(decl._props, "flexDirection") ||
                        Object.prototype.hasOwnProperty.call(decl._props, "flex-direction");
                    // flex-flow shorthand only counts as explicit when it
                    // includes a direction token. `flexFlow: "wrap"` (or
                    // "nowrap") leaves direction omitted, which per CSS
                    // also defaults to row — so the display:flex default
                    // still applies.
                    if (!hasExplicitDirection) {
                        var ff = decl._props.flexFlow;
                        if (typeof ff === "string" && /\b(row|column)\b/.test(ff)) {
                            hasExplicitDirection = true;
                        }
                    }
                    if (!hasExplicitDirection) {
                        setFlex(id, "direction", "row");
                    }
                }
            }
            else if (resolved === "grid") { /* grid mode set via gridTemplateColumns */ }
            return true;
        case "flexDirection":
            // pulp #1434 (rn batch B) — forward all four CSS values
            // verbatim. Bridge dispatches to YGFlexDirectionRow /
            // RowReverse / Column / ColumnReverse. Previously only
            // "row" survived; "row-reverse"/"column-reverse" silently
            // collapsed to "col".
            setFlex(id, "direction",
                resolved === "row" ? "row" :
                resolved === "row-reverse" ? "row-reverse" :
                resolved === "column-reverse" ? "column-reverse" :
                "col");
            return true;
        case "flexWrap":
            // pulp #1434 Triage #14 — forward the keyword verbatim so the
            // bridge can route `wrap-reverse` through Yoga's
            // YGWrapWrapReverse path. Previous behavior coerced to 0/1
            // and silently dropped wrap-reverse to plain wrap.
            setFlex(id, "flex_wrap", resolved);
            return true;
        case "flexGrow":
            setFlex(id, "flex_grow", parseFloat(resolved) || 0);
            return true;
        case "flexShrink":
            setFlex(id, "flex_shrink", parseFloat(resolved) || 0);
            return true;
        case "flexBasis":
            var fb = resolveCSSLength(resolved);
            if (fb) setFlex(id, "flex_basis", fb.value);
            return true;
        case "flex": {
            // Shorthand: flex: <grow> [<shrink>] [<basis>]
            // pulp DIVERGE→PASS sweep — accept the CSS shorthand
            // keywords `auto` / `none` / `initial`. Per the CSS Flexible
            // Box spec these expand to:
            //   flex: auto    ≡ 1 1 auto
            //   flex: none    ≡ 0 0 auto
            //   flex: initial ≡ 0 1 auto   (the CSS default)
            // Without this branch the keyword fell through to
            // parseFloat() → NaN → 0 and silently zeroed flex_grow,
            // making `flex: auto` equivalent to `flex: 0` — the wrong
            // semantics for the "fill remaining space" idiom.
            var fkw = String(resolved).trim().toLowerCase();
            if (fkw === "auto" || fkw === "none" || fkw === "initial") {
                var grow   = (fkw === "auto") ? 1 : 0;
                var shrink = (fkw === "none") ? 0 : 1;
                setFlex(id, "flex_grow",   grow);
                setFlex(id, "flex_shrink", shrink);
                setFlex(id, "flex_basis",  "auto");
                return true;
            }
            var parts = resolved.split(/\s+/);
            setFlex(id, "flex_grow", parseFloat(parts[0]) || 0);
            if (parts[1]) setFlex(id, "flex_shrink", parseFloat(parts[1]) || 0);
            if (parts[2]) {
                // Accept `<basis>` token = `auto` / length / percentage.
                var basisTok = String(parts[2]).trim().toLowerCase();
                if (basisTok === "auto") {
                    setFlex(id, "flex_basis", "auto");
                } else {
                    var b = resolveCSSLength(parts[2]);
                    if (b) setFlex(id, "flex_basis", b.unit === "%" ? (b.value + "%") : b.value);
                }
            }
            return true;
        }
        case "justifyContent":
            setFlex(id, "justify_content", _cssToFlex(resolved));
            return true;
        case "alignItems":
            setFlex(id, "align_items", _cssToFlex(resolved));
            return true;
        case "alignSelf":
            setFlex(id, "align_self", _cssToFlex(resolved));
            return true;
        case "order":
            setFlex(id, "order", parseInt(resolved) || 0);
            return true;
        case "gap": {
            // pulp Wave 2 css.2 — CSS shorthand `gap: <row-gap> [<col-gap>]`.
            // When two tokens are present, fan out to setRowGap +
            // setColumnGap so each axis gets its own value (matching the
            // CSS spec). Single-token form writes the shared `gap` slot.
            //
            // Codex #1616 P1 on #1638 — single-token writes were ignoring
            // any prior 2-token state, leaving stale row_gap/column_gap
            // (which FlexStyle::effective_gap prefers when ≥0). The fix
            // resets per-axis to the -1 sentinel before writing the
            // shared slot so the new shorthand value actually wins.
            var gapToks = String(resolved).trim().split(/\s+/);
            if (gapToks.length >= 2) {
                var grRow = resolveCSSLength(gapToks[0]);
                var grCol = resolveCSSLength(gapToks[1]);
                if (grRow) setFlex(id, "row_gap",
                    grRow.unit === "%" ? (grRow.value + "%") : grRow.value);
                if (grCol) setFlex(id, "column_gap",
                    grCol.unit === "%" ? (grCol.value + "%") : grCol.value);
            } else {
                // Codex P2 followup on #1700 (#1707): parse FIRST,
                // then reset per-axis only if the new value is valid.
                // The earlier ordering cleared row_gap/column_gap
                // unconditionally, which silently nuked prior 2-token
                // state when the new value was malformed.
                var g = resolveCSSLength(resolved);
                if (g) {
                    // Reset per-axis (-1 = "consult shared gap") so
                    // the new single-token value isn't shadowed by a
                    // prior 2-token write.
                    setFlex(id, "row_gap", -1);
                    setFlex(id, "column_gap", -1);
                    setFlex(id, "gap",
                        g.unit === "%" ? (g.value + "%") : g.value);
                }
            }
            return true;
        }
        case "rowGap": {
            // pulp Wave 2 css.2 — forward `'NN%'` verbatim so the bridge
            // stores the percent value on the FlexStyle.row_gap slot
            // (best-effort: Yoga has no row-gap percent API yet, so the
            // value is treated as px until the Yoga update lands).
            var rg = resolveCSSLength(resolved);
            if (rg) setFlex(id, "row_gap",
                rg.unit === "%" ? (rg.value + "%") : rg.value);
            return true;
        }
        case "columnGap": {
            // pulp Wave 2 css.2 — forward `'NN%'` verbatim (same caveat
            // as rowGap above).
            var cg = resolveCSSLength(resolved);
            if (cg) setFlex(id, "column_gap",
                cg.unit === "%" ? (cg.value + "%") : cg.value);
            return true;
        }

        // Dimensions
        // pulp #1423 — pass the resolved string verbatim for width/height
        // when it is a percent value. The bridge's setFlex(width|height,
        // ...) inspects the third arg as a string and detects '%' suffix.
        // This keeps the existing px path numeric (no JS-side regression)
        // while letting "100%" survive through to Yoga's native
        // YGNodeStyleSet{Width,Height}Percent path.
        case "width": {
            // pulp #1434 (sub-agent #12 follow-up) — forward `'auto'`
            // verbatim so the bridge can route to YGNodeStyleSetWidthAuto
            // ("hug contents"). Mirrors the percent path.
            if (resolved === "auto") { setFlex(id, "width", "auto"); return true; }
            var w = resolveCSSLength(resolved);
            if (!w) return true;
            if (w.unit === "auto") setFlex(id, "width", "auto");
            else if (w.unit === "%") setFlex(id, "width", w.value + "%");
            else setFlex(id, "width", w.value);
            return true;
        }
        case "height": {
            if (resolved === "auto") { setFlex(id, "height", "auto"); return true; }
            var h = resolveCSSLength(resolved);
            if (!h) return true;
            if (h.unit === "auto") setFlex(id, "height", "auto");
            else if (h.unit === "%") setFlex(id, "height", h.value + "%");
            else setFlex(id, "height", h.value);
            return true;
        }
        // pulp #1576 — these four min/max length properties previously
        // had a dual-path: a `/^(calc|min|max|clamp)\(/` guard that
        // routed calc-family values into resolveCSSLength's number-
        // return shape, and a parseCSSLength branch for the px/%/auto
        // case. After #1576 restored resolveCSSLength to the unified
        // {value, unit} shape (drop-in replacement for parseCSSLength),
        // both paths collapse into a single resolveCSSLength call.
        case "minWidth": {
            var mw = resolveCSSLength(resolved);
            if (mw) setFlex(id, "min_width",
                mw.unit === "%" ? (mw.value + "%") : mw.value);
            return true;
        }
        case "minHeight": {
            var mh = resolveCSSLength(resolved);
            if (mh) setFlex(id, "min_height",
                mh.unit === "%" ? (mh.value + "%") : mh.value);
            return true;
        }
        case "maxWidth": {
            var xw = resolveCSSLength(resolved);
            if (xw) setFlex(id, "max_width",
                xw.unit === "%" ? (xw.value + "%") : xw.value);
            return true;
        }
        case "maxHeight": {
            var xh = resolveCSSLength(resolved);
            if (xh) setFlex(id, "max_height",
                xh.unit === "%" ? (xh.value + "%") : xh.value);
            return true;
        }

        // Margin (individual) — pulp #1434 cross-surface mega-batch:
        // forward `'NN%'` and `'auto'` strings verbatim so the bridge can
        // route through Yoga's YGNodeStyleSetMargin{Percent,Auto} APIs.
        // Numeric values flow through parseCSSLength as before. `auto`
        // is the canonical centering idiom (e.g. `marginLeft: auto;
        // marginRight: auto`) — Yoga supports it on margin only.
        case "marginTop": {
            if (resolved === "auto") { setFlex(id, "margin_top", "auto"); return true; }
            var mt = resolveCSSLength(resolved);
            if (!mt) return true;
            if (mt.unit === "%") setFlex(id, "margin_top", mt.value + "%");
            else setFlex(id, "margin_top", mt.value);
            return true;
        }
        case "marginRight": {
            if (resolved === "auto") { setFlex(id, "margin_right", "auto"); return true; }
            var mr = resolveCSSLength(resolved);
            if (!mr) return true;
            if (mr.unit === "%") setFlex(id, "margin_right", mr.value + "%");
            else setFlex(id, "margin_right", mr.value);
            return true;
        }
        case "marginBottom": {
            if (resolved === "auto") { setFlex(id, "margin_bottom", "auto"); return true; }
            var mb = resolveCSSLength(resolved);
            if (!mb) return true;
            if (mb.unit === "%") setFlex(id, "margin_bottom", mb.value + "%");
            else setFlex(id, "margin_bottom", mb.value);
            return true;
        }
        case "marginLeft": {
            if (resolved === "auto") { setFlex(id, "margin_left", "auto"); return true; }
            var ml = resolveCSSLength(resolved);
            if (!ml) return true;
            if (ml.unit === "%") setFlex(id, "margin_left", ml.value + "%");
            else setFlex(id, "margin_left", ml.value);
            return true;
        }
        // Margin shorthand
        case "margin": {
            // pulp Wave 2 css.2 — per-token `auto` / `%` support in the
            // shorthand. Mirrors the per-edge `marginTop` etc. behavior.
            // expandShorthand collapses all tokens to numeric values
            // (lossy for auto / percent), so we re-tokenize here and
            // dispatch each side via the same string-aware setFlex
            // pathway used by per-edge keys. CSS shorthand convention:
            //   1 token  → all four
            //   2 tokens → vertical / horizontal
            //   3 tokens → top / horizontal / bottom
            //   4 tokens → top / right / bottom / left
            var mTokens = String(resolved).trim().split(/\s+/);
            var mEdges = [mTokens[0], mTokens[1] || mTokens[0],
                          mTokens[2] || mTokens[0], mTokens[3] || mTokens[1] || mTokens[0]];
            var mNames = ["margin_top", "margin_right", "margin_bottom", "margin_left"];
            for (var mi = 0; mi < 4; mi++) {
                var tok = String(mEdges[mi]).trim().toLowerCase();
                if (tok === "auto") {
                    setFlex(id, mNames[mi], "auto");
                } else {
                    var mp = resolveCSSLength(tok);
                    if (!mp) continue;
                    setFlex(id, mNames[mi],
                        mp.unit === "%" ? (mp.value + "%") : mp.value);
                }
            }
            return true;
        }
        // pulp #1434 batch 4 — React Native shorthand aliases. RN code
        // commonly writes `style={{ marginHorizontal: 8 }}` which CSS
        // doesn't recognize, but the DOM-lite el.style adapter sees the
        // raw key when consumers port RN snippets verbatim. Fan out to
        // the same per-edge bridge calls the CSS marginInline / margin
        // shorthand uses so the behavior is identical regardless of the
        // entry surface. `auto` is a no-op for now (parseCSSLength returns
        // null for non-numeric input); numeric and percent paths route
        // through the same setFlex per-edge dispatch as marginLeft etc.
        case "marginHorizontal": {
            // pulp #1434 cross-surface mega-batch — forward %/auto through
            // the per-edge fan-out so RN snippets like
            // `style={{ marginHorizontal: '5%' }}` or
            // `style={{ marginHorizontal: 'auto' }}` route correctly.
            if (resolved === "auto") {
                setFlex(id, "margin_left",  "auto");
                setFlex(id, "margin_right", "auto");
                return true;
            }
            var mhv = resolveCSSLength(resolved);
            if (!mhv) return true;
            var mhArg = mhv.unit === "%" ? mhv.value + "%" : mhv.value;
            setFlex(id, "margin_left",  mhArg);
            setFlex(id, "margin_right", mhArg);
            return true;
        }
        case "marginVertical": {
            if (resolved === "auto") {
                setFlex(id, "margin_top",    "auto");
                setFlex(id, "margin_bottom", "auto");
                return true;
            }
            var mvv = resolveCSSLength(resolved);
            if (!mvv) return true;
            var mvArg = mvv.unit === "%" ? mvv.value + "%" : mvv.value;
            setFlex(id, "margin_top",    mvArg);
            setFlex(id, "margin_bottom", mvArg);
            return true;
        }

        // Padding (individual) — pulp #1434 cross-surface mega-batch:
        // forward `'NN%'` strings verbatim (Yoga's
        // YGNodeStyleSetPaddingPercent). Yoga's padding does NOT support
        // `auto` (only margin does), so the keyword is silently dropped
        // here.
        case "paddingTop": {
            var pt = resolveCSSLength(resolved);
            if (!pt) return true;
            if (pt.unit === "%") setFlex(id, "padding_top", pt.value + "%");
            else setFlex(id, "padding_top", pt.value);
            return true;
        }
        case "paddingRight": {
            var pr = resolveCSSLength(resolved);
            if (!pr) return true;
            if (pr.unit === "%") setFlex(id, "padding_right", pr.value + "%");
            else setFlex(id, "padding_right", pr.value);
            return true;
        }
        case "paddingBottom": {
            var pb = resolveCSSLength(resolved);
            if (!pb) return true;
            if (pb.unit === "%") setFlex(id, "padding_bottom", pb.value + "%");
            else setFlex(id, "padding_bottom", pb.value);
            return true;
        }
        case "paddingLeft": {
            var pl = resolveCSSLength(resolved);
            if (!pl) return true;
            if (pl.unit === "%") setFlex(id, "padding_left", pl.value + "%");
            else setFlex(id, "padding_left", pl.value);
            return true;
        }
        // Padding shorthand
        case "padding": {
            // pulp Wave 2 css.2 — per-token `%` support in the shorthand.
            // Yoga's padding doesn't accept `auto` (only margin does), so
            // the keyword is silently dropped per token. Otherwise the
            // tokenizing logic mirrors the `margin` shorthand above.
            var pTokens = String(resolved).trim().split(/\s+/);
            var pEdges = [pTokens[0], pTokens[1] || pTokens[0],
                          pTokens[2] || pTokens[0], pTokens[3] || pTokens[1] || pTokens[0]];
            var pNames = ["padding_top", "padding_right", "padding_bottom", "padding_left"];
            for (var pi = 0; pi < 4; pi++) {
                var ptok = String(pEdges[pi]).trim().toLowerCase();
                if (ptok === "auto") continue; // Yoga padding has no auto
                var pp = resolveCSSLength(ptok);
                if (!pp) continue;
                setFlex(id, pNames[pi],
                    pp.unit === "%" ? (pp.value + "%") : pp.value);
            }
            return true;
        }
        // pulp #1434 batch 4 — React Native shorthand aliases for padding.
        // Same fan-out pattern as marginHorizontal / marginVertical above
        // — paddingHorizontal sets padding_left + padding_right to the
        // same value, paddingVertical sets padding_top + padding_bottom.
        case "paddingHorizontal": {
            // pulp #1434 cross-surface mega-batch — forward percent through
            // the per-edge fan-out so RN snippets like
            // `style={{ paddingHorizontal: '5%' }}` route correctly.
            // Yoga's padding does NOT support 'auto', so the keyword is
            // a no-op (unlike the marginHorizontal alias).
            var phv = resolveCSSLength(resolved);
            if (!phv) return true;
            var phArg = phv.unit === "%" ? phv.value + "%" : phv.value;
            setFlex(id, "padding_left",  phArg);
            setFlex(id, "padding_right", phArg);
            return true;
        }
        case "paddingVertical": {
            var pvv = resolveCSSLength(resolved);
            if (!pvv) return true;
            var pvArg = pvv.unit === "%" ? pvv.value + "%" : pvv.value;
            setFlex(id, "padding_top",    pvArg);
            setFlex(id, "padding_bottom", pvArg);
            return true;
        }

        // pulp #1434 Phase A2-3 — CSS `direction: ltr | rtl`. Maps to
        // View::WritingDirection via setDirection bridge fn; Yoga
        // honors at layout, Skia paragraph_style at text shape.
        case "direction": {
            if (typeof setDirection !== "undefined") {
                setDirection(id, resolved);
            }
            return true;
        }

        // Overflow
        case "overflow":
            setOverflow(id, resolved);
            return true;
        // pulp #1434 A4 Bundle 4 — overflow per-axis. Pulp's View has a
        // single Overflow enum (visible|hidden) that clips both axes
        // together; CSS `overflow-x` / `overflow-y` ask for axis-tied
        // clipping which the layout engine doesn't model. Forward the
        // value to the same setOverflow bridge — last-write-wins across
        // the two axes, which is the conservative interpretation
        // (`overflow-x: hidden` clips both axes; `overflow-x: visible`
        // unclips both). Catalog status is `partial` with the axis-tied
        // gotcha documented in unsupportedValues.
        case "overflowX":
        case "overflowY":
            setOverflow(id, resolved);
            return true;

        // Position
        case "position":
            setPosition(id, resolved);
            // pulp #1148 (slice b) — auto-overlay heuristic. Re-evaluate
            // whenever `position` changes; switching to `absolute` with
            // a sufficient z-index claims the global overlay slot, and
            // switching back to `static` / `relative` releases it.
            decl._reevaluateOverlay();
            return true;
        // pulp #1434 batch 6 — pass the resolved string verbatim for
        // top/right/bottom/left when the unit is %. The bridge's setTop /
        // setRight / setBottom / setLeft inspect arg index 1 as a string
        // and detect '%' suffix, routing the value through Yoga's native
        // YGNodeStyleSetPositionPercent path. Mirrors PR #1426 for the
        // View positional fields.
        // pulp Wave 2 css.2 — relative-unit resolution for top/right/
        // bottom/left. em/rem are resolved against the default 14px
        // font-size (no per-element cascade context here); vh/vw are
        // resolved against an 800x600 default viewport (matches
        // resolveLength in css-parser.js). Numeric px and `%` paths
        // are unchanged.
        case "top": {
            var tv = resolveCSSLength(resolved); if (!tv) return true;
            if (tv.unit === "%") setTop(id, tv.value + "%");
            else if (tv.unit === "em" || tv.unit === "rem") setTop(id, tv.value * 14);
            else if (tv.unit === "vh") setTop(id, tv.value / 100 * 600);
            else if (tv.unit === "vw") setTop(id, tv.value / 100 * 800);
            else setTop(id, tv.value);
            return true;
        }
        case "right": {
            var rv = resolveCSSLength(resolved); if (!rv) return true;
            if (rv.unit === "%") setRight(id, rv.value + "%");
            else if (rv.unit === "em" || rv.unit === "rem") setRight(id, rv.value * 14);
            else if (rv.unit === "vh") setRight(id, rv.value / 100 * 600);
            else if (rv.unit === "vw") setRight(id, rv.value / 100 * 800);
            else setRight(id, rv.value);
            return true;
        }
        case "bottom": {
            var bv = resolveCSSLength(resolved); if (!bv) return true;
            if (bv.unit === "%") setBottom(id, bv.value + "%");
            else if (bv.unit === "em" || bv.unit === "rem") setBottom(id, bv.value * 14);
            else if (bv.unit === "vh") setBottom(id, bv.value / 100 * 600);
            else if (bv.unit === "vw") setBottom(id, bv.value / 100 * 800);
            else setBottom(id, bv.value);
            return true;
        }
        case "left": {
            var lv = resolveCSSLength(resolved); if (!lv) return true;
            if (lv.unit === "%") setLeft(id, lv.value + "%");
            else if (lv.unit === "em" || lv.unit === "rem") setLeft(id, lv.value * 14);
            else if (lv.unit === "vh") setLeft(id, lv.value / 100 * 600);
            else if (lv.unit === "vw") setLeft(id, lv.value / 100 * 800);
            else setLeft(id, lv.value);
            return true;
        }

        // z-index
        case "zIndex":
            setZIndex(id, parseInt(resolved) || 0);
            // pulp #1148 (slice b) — z-index moving above/below the
            // popover threshold flips the auto-overlay heuristic.
            decl._reevaluateOverlay();
            return true;

        // CSS `grid` shorthand — fans out into the existing grid
        // longhands. The full spec syntax is complex (auto-flow forms,
        // grid-template-areas embedded between row tracks); this shim
        // parses the common `<rows> / <cols>` form which covers the
        // bulk of imported designs. Other forms are deferred — authors
        // can use the longhand grid-template-{rows,columns,areas} +
        // grid-auto-{rows,columns,flow} entries directly.
        case "grid": {
            var gv = String(resolved).trim();
            if (gv === "" || gv === "none") {
                if (typeof setGrid === "function") {
                    setGrid(id, "template_rows", "none");
                    setGrid(id, "template_columns", "none");
                }
                return true;
            }
            // Detect "<rows> / <cols>" form. Split on first unparenthesized "/".
            var depth = 0, slashIdx = -1;
            for (var gi = 0; gi < gv.length; ++gi) {
                var gc = gv[gi];
                if (gc === "(") depth++;
                else if (gc === ")") depth--;
                else if (gc === "/" && depth === 0) { slashIdx = gi; break; }
            }
            if (slashIdx > 0 && typeof setGrid === "function") {
                var rows = gv.slice(0, slashIdx).trim();
                var cols = gv.slice(slashIdx + 1).trim();
                if (rows) setGrid(id, "template_rows", rows);
                if (cols) setGrid(id, "template_columns", cols);
            } else if (typeof setGrid === "function") {
                // Single-track form: treat as template_rows for spec-most
                // common interpretation; cols default to 1fr.
                setGrid(id, "template_rows", gv);
            }
            return true;
        }

        // Grid
        case "gridTemplateColumns":
            setGrid(id, "template_columns", resolved);
            return true;
        case "gridTemplateRows":
            setGrid(id, "template_rows", resolved);
            return true;
        case "gridColumn": {
            var gcol = resolved.split("/").map(function(s) { return parseInt(s.trim()); });
            if (gcol[0]) setGrid(id, "column_start", gcol[0]);
            if (gcol[1]) setGrid(id, "column_end", gcol[1]);
            return true;
        }
        case "gridRow": {
            var grow = resolved.split("/").map(function(s) { return parseInt(s.trim()); });
            if (grow[0]) setGrid(id, "row_start", grow[0]);
            if (grow[1]) setGrid(id, "row_end", grow[1]);
            return true;
        }
        // pulp #1434 Phase A2-2 — extended grid surface
        case "gridAutoColumns":
            setGrid(id, "auto_columns", resolved);
            return true;
        case "gridAutoRows":
            setGrid(id, "auto_rows", resolved);
            return true;
        case "gridAutoFlow":
            setGrid(id, "auto_flow", resolved);
            return true;
        case "gridTemplateAreas":
            setGrid(id, "template_areas", resolved);
            return true;
        case "gridArea":
            // Pass through verbatim — bridge distinguishes name vs.
            // numeric "row / col / row / col" form.
            setGrid(id, "grid_area", resolved);
            return true;

        // aspect-ratio: "16/9", "1.5", or "auto" (pulp #1434).
        // Three value forms accepted — RN exports use the plain number form,
        // CSS exports use the `width / height` form, "auto" clears the slot.
        // Both `aspectRatio` (camelCase, set via `style.aspectRatio = ...`)
        // and `aspect-ratio` (kebab-case via `style.setProperty(...)`) reach
        // this branch — `setProperty` converts kebab to camel before
        // dispatching through the descriptor setter.
        case "aspectRatio": {
            var trimmed = String(resolved).trim();
            if (trimmed === "" || trimmed === "auto") {
                // Clear: bridge interprets non-positive as "unset".
                setFlex(id, "aspect_ratio", 0);
                return true;
            }
            var arParts = trimmed.split("/");
            var num = parseFloat(arParts[0]);
            if (!isFinite(num) || num <= 0) {
                setFlex(id, "aspect_ratio", 0);
                return true;
            }
            var ratio = num;
            if (arParts[1] !== undefined) {
                var den = parseFloat(arParts[1]);
                if (!isFinite(den) || den <= 0) {
                    setFlex(id, "aspect_ratio", 0);
                    return true;
                }
                ratio = num / den;
            }
            setFlex(id, "aspect_ratio", ratio);
            return true;
        }

        // box-sizing
        case "boxSizing":
            if (typeof setBoxSizing === "function") setBoxSizing(id, resolved);
            return true;

        // flex-flow shorthand
        case "flexFlow": {
            // pulp #1434 Triage #14 — recognize the full direction +
            // wrap vocabulary including `row-reverse` / `column-reverse`
            // (already-wired but missing from this shorthand path) and
            // `wrap-reverse` (newly wired through the bridge).
            var ffp = resolved.split(/\s+/);
            for (var ffi = 0; ffi < ffp.length; ffi++) {
                var fftok = ffp[ffi];
                if (fftok === "row" || fftok === "column"
                        || fftok === "row-reverse" || fftok === "column-reverse") {
                    setFlex(id, "direction", fftok === "row" ? "row" : fftok);
                }
                else if (fftok === "wrap" || fftok === "nowrap"
                        || fftok === "no-wrap" || fftok === "wrap-reverse") {
                    setFlex(id, "flex_wrap", fftok);
                }
            }
            return true;
        }

        // place-items shorthand (align-items + justify-items)
        case "placeItems": {
            var pip = resolved.split(/\s+/);
            setFlex(id, "align_items", _cssToFlex(pip[0]));
            if (pip[1]) setFlex(id, "justify_content", _cssToFlex(pip[1]));
            return true;
        }

        // place-content shorthand
        case "placeContent": {
            var pcp = resolved.split(/\s+/);
            setFlex(id, "align_content", _cssToFlex(pcp[0]));
            if (pcp[1]) setFlex(id, "justify_content", _cssToFlex(pcp[1]));
            return true;
        }

        // align-content (multi-line flex cross-axis)
        case "alignContent":
            setFlex(id, "align_content", _cssToFlex(resolved));
            return true;

        // pulp #1434 A4 Bundle 3 — logical-edge fan-out. Every logical
        // edge maps to the LTR / horizontal-tb physical edge:
        //   inline-start → left,  inline-end → right
        //   block-start  → top,   block-end  → bottom
        // RTL writing direction would swap inline-start/end; #1434 still
        // tracks this as a follow-up. The bridge stores the value on the
        // physical edge today, which is correct for LTR (the overwhelming
        // common case). When direction-aware mapping lands, only this
        // dispatch table needs to consult the View's writing direction.
        case "marginInline": {
            var mi = expandShorthand(resolved);
            setFlex(id, "margin_left", mi[0]); setFlex(id, "margin_right", mi[1]);
            return true;
        }
        case "marginInlineStart": {
            // LTR fast path — inline-start ≡ left.
            if (resolved === "auto") { setFlex(id, "margin_left", "auto"); return true; }
            var mis = resolveCSSLength(resolved);
            if (!mis) return true;
            setFlex(id, "margin_left", mis.unit === "%" ? mis.value + "%" : mis.value);
            return true;
        }
        case "marginInlineEnd": {
            // LTR fast path — inline-end ≡ right.
            if (resolved === "auto") { setFlex(id, "margin_right", "auto"); return true; }
            var mie = resolveCSSLength(resolved);
            if (!mie) return true;
            setFlex(id, "margin_right", mie.unit === "%" ? mie.value + "%" : mie.value);
            return true;
        }
        case "marginBlock": {
            var mb2 = expandShorthand(resolved);
            setFlex(id, "margin_top", mb2[0]); setFlex(id, "margin_bottom", mb2[1]);
            return true;
        }
        case "marginBlockStart": {
            // horizontal-tb fast path — block-start ≡ top.
            if (resolved === "auto") { setFlex(id, "margin_top", "auto"); return true; }
            var mbs = resolveCSSLength(resolved);
            if (!mbs) return true;
            setFlex(id, "margin_top", mbs.unit === "%" ? mbs.value + "%" : mbs.value);
            return true;
        }
        case "marginBlockEnd": {
            // horizontal-tb fast path — block-end ≡ bottom.
            if (resolved === "auto") { setFlex(id, "margin_bottom", "auto"); return true; }
            var mbe = resolveCSSLength(resolved);
            if (!mbe) return true;
            setFlex(id, "margin_bottom", mbe.unit === "%" ? mbe.value + "%" : mbe.value);
            return true;
        }
        case "paddingInline": {
            var pi2 = expandShorthand(resolved);
            setFlex(id, "padding_left", pi2[0]); setFlex(id, "padding_right", pi2[1]);
            return true;
        }
        case "paddingInlineStart": {
            // LTR fast path — inline-start ≡ left. Yoga's padding doesn't
            // support `auto` (only margin does); keyword silently dropped.
            var pis = resolveCSSLength(resolved);
            if (!pis) return true;
            setFlex(id, "padding_left", pis.unit === "%" ? pis.value + "%" : pis.value);
            return true;
        }
        case "paddingInlineEnd": {
            // LTR fast path — inline-end ≡ right.
            var pie = resolveCSSLength(resolved);
            if (!pie) return true;
            setFlex(id, "padding_right", pie.unit === "%" ? pie.value + "%" : pie.value);
            return true;
        }
        case "paddingBlock": {
            var pb2 = expandShorthand(resolved);
            setFlex(id, "padding_top", pb2[0]); setFlex(id, "padding_bottom", pb2[1]);
            return true;
        }
        case "paddingBlockStart": {
            // horizontal-tb fast path — block-start ≡ top.
            var pbs = resolveCSSLength(resolved);
            if (!pbs) return true;
            setFlex(id, "padding_top", pbs.unit === "%" ? pbs.value + "%" : pbs.value);
            return true;
        }
        case "paddingBlockEnd": {
            // horizontal-tb fast path — block-end ≡ bottom.
            var pbe = resolveCSSLength(resolved);
            if (!pbe) return true;
            setFlex(id, "padding_bottom", pbe.unit === "%" ? pbe.value + "%" : pbe.value);
            return true;
        }
        case "inset": {
            var ins = expandShorthand(resolved);
            var tv2 = resolveCSSLength(String(ins[0])); if (tv2) setTop(id, tv2.value);
            var rv2 = resolveCSSLength(String(ins[1])); if (rv2) setRight(id, rv2.value);
            var bv2 = resolveCSSLength(String(ins[2])); if (bv2) setBottom(id, bv2.value);
            var lv2 = resolveCSSLength(String(ins[3])); if (lv2) setLeft(id, lv2.value);
            return true;
        }

        default:
            return false;
    }
}
