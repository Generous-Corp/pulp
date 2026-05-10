// ═══════════════════════════════════════════════════════════════════════════════
// CSSStyleDeclaration
// ═══════════════════════════════════════════════════════════════════════════════

function CSSStyleDeclaration(el) {
    this._el = el;
    this._props = {};
    // pulp #1148 (slice b) — auto-overlay heuristic state. Tracks whether
    // we've called `claimOverlay` for this element via the CSS-shape
    // detector so we can release exactly once on the inverse transition
    // (position -> static/relative, z-index -> below threshold,
    // data-overlay -> not "true"). The C++ release_overlay is idempotent
    // and the @pulp/react `overlay` prop path uses the same bridge calls,
    // so the two paths converge on `View::active_overlay_` without
    // double-claim/double-release surprises.
    this._autoOverlayClaimed = false;
}

// pulp #1148 (slice b) — z-index threshold above which an absolutely
// positioned element is treated as a popover/overlay candidate. Web
// authors typically use values like 1000 / 9999 for popovers and 1-3
// for stacking-context shuffles within layouts; 10 is comfortably
// above the in-flow stacking range and well below conventional
// popover values, so it is a conservative gate against false
// positives (decorative absolutely-positioned badges with z-index 1
// must NOT auto-claim because a claim hijacks click routing).
var _PULP_AUTO_OVERLAY_Z_INDEX_THRESHOLD = 10;

// pulp #1148 (slice b) — re-evaluate the auto-overlay heuristic for
// this element. Called whenever `position`, `zIndex`, or the
// `data-overlay` hint changes. Conservative by design: opt-in only
// when the CSS shape strongly signals a popover (position:absolute +
// high z-index) OR the author explicitly hints `data-overlay="true"`.
// Mirrors what the @pulp/react prop-applier does for `<View overlay>`
// — both paths call `claimOverlay` / `releaseOverlay` on the same
// bridge so the single `View::active_overlay_` slot stays consistent.
CSSStyleDeclaration.prototype._reevaluateOverlay = function() {
    var el = this._el;
    if (!el || !el._nativeCreated) return;

    // 1. Explicit hint wins (HTML data-overlay="true" or CSS data-overlay
    //    style; both surface through Element._dataset.overlay).
    var hint = el._dataset && el._dataset.overlay;
    var hinted = (hint === "true" || hint === true);

    // 2. CSS shape: position:absolute + z-index above threshold. We
    //    require BOTH — `position:absolute` alone catches tooltips,
    //    decorations, and absolutely-laid-out cards that should NOT
    //    steal clicks. A high z-index alone (with position:relative or
    //    static) doesn't reorder hit-testing in the same popover sense.
    var posResolved = _resolveVar(String(this._props.position || ""));
    var zRaw = this._props.zIndex;
    var zResolved = (zRaw == null || zRaw === "") ? "" : _resolveVar(String(zRaw));
    var zVal = parseInt(zResolved, 10);
    if (isNaN(zVal)) zVal = 0;
    var shapeClaim = (posResolved === "absolute" &&
                      zVal >= _PULP_AUTO_OVERLAY_Z_INDEX_THRESHOLD);

    var shouldClaim = hinted || shapeClaim;

    if (shouldClaim && !this._autoOverlayClaimed) {
        if (typeof claimOverlay === "function") claimOverlay(el._id);
        this._autoOverlayClaimed = true;
    } else if (!shouldClaim && this._autoOverlayClaimed) {
        if (typeof releaseOverlay === "function") releaseOverlay(el._id);
        this._autoOverlayClaimed = false;
    }
};

// Flush all stored properties to the bridge
CSSStyleDeclaration.prototype._flushAll = function() {
    for (var key in this._props) {
        this._applyProperty(key, this._props[key]);
    }
};

// Apply a single CSS property to the bridge
CSSStyleDeclaration.prototype._applyProperty = function(key, value) {
    var id = this._el._id;
    if (!this._el._nativeCreated) return;

    var resolved = _resolveVar(String(value));

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
                        Object.prototype.hasOwnProperty.call(this._props, "flexDirection") ||
                        Object.prototype.hasOwnProperty.call(this._props, "flex-direction");
                    // flex-flow shorthand only counts as explicit when it
                    // includes a direction token. `flexFlow: "wrap"` (or
                    // "nowrap") leaves direction omitted, which per CSS
                    // also defaults to row — so the display:flex default
                    // still applies.
                    if (!hasExplicitDirection) {
                        var ff = this._props.flexFlow;
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
            break;
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
            break;
        case "flexWrap":
            // pulp #1434 Triage #14 — forward the keyword verbatim so the
            // bridge can route `wrap-reverse` through Yoga's
            // YGWrapWrapReverse path. Previous behavior coerced to 0/1
            // and silently dropped wrap-reverse to plain wrap.
            setFlex(id, "flex_wrap", resolved);
            break;
        case "flexGrow":
            setFlex(id, "flex_grow", parseFloat(resolved) || 0);
            break;
        case "flexShrink":
            setFlex(id, "flex_shrink", parseFloat(resolved) || 0);
            break;
        case "flexBasis":
            var fb = parseCSSLength(resolved);
            if (fb) setFlex(id, "flex_basis", fb.value);
            break;
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
                break;
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
                    var b = parseCSSLength(parts[2]);
                    if (b) setFlex(id, "flex_basis", b.unit === "%" ? (b.value + "%") : b.value);
                }
            }
            break;
        }
        case "justifyContent":
            setFlex(id, "justify_content", _cssToFlex(resolved));
            break;
        case "alignItems":
            setFlex(id, "align_items", _cssToFlex(resolved));
            break;
        case "alignSelf":
            setFlex(id, "align_self", _cssToFlex(resolved));
            break;
        case "order":
            setFlex(id, "order", parseInt(resolved) || 0);
            break;
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
                var grRow = parseCSSLength(gapToks[0]);
                var grCol = parseCSSLength(gapToks[1]);
                if (grRow) setFlex(id, "row_gap",
                    grRow.unit === "%" ? (grRow.value + "%") : grRow.value);
                if (grCol) setFlex(id, "column_gap",
                    grCol.unit === "%" ? (grCol.value + "%") : grCol.value);
            } else {
                // Reset per-axis (-1 = "consult shared gap") so the new
                // single-token value isn't shadowed by a prior 2-token
                // write.
                setFlex(id, "row_gap", -1);
                setFlex(id, "column_gap", -1);
                var g = parseCSSLength(resolved);
                if (g) setFlex(id, "gap",
                    g.unit === "%" ? (g.value + "%") : g.value);
            }
            break;
        }
        case "rowGap": {
            // pulp Wave 2 css.2 — forward `'NN%'` verbatim so the bridge
            // stores the percent value on the FlexStyle.row_gap slot
            // (best-effort: Yoga has no row-gap percent API yet, so the
            // value is treated as px until the Yoga update lands).
            var rg = parseCSSLength(resolved);
            if (rg) setFlex(id, "row_gap",
                rg.unit === "%" ? (rg.value + "%") : rg.value);
            break;
        }
        case "columnGap": {
            // pulp Wave 2 css.2 — forward `'NN%'` verbatim (same caveat
            // as rowGap above).
            var cg = parseCSSLength(resolved);
            if (cg) setFlex(id, "column_gap",
                cg.unit === "%" ? (cg.value + "%") : cg.value);
            break;
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
            if (resolved === "auto") { setFlex(id, "width", "auto"); break; }
            var w = parseCSSLength(resolved);
            if (!w) break;
            if (w.unit === "auto") setFlex(id, "width", "auto");
            else if (w.unit === "%") setFlex(id, "width", w.value + "%");
            else setFlex(id, "width", w.value);
            break;
        }
        case "height": {
            if (resolved === "auto") { setFlex(id, "height", "auto"); break; }
            var h = parseCSSLength(resolved);
            if (!h) break;
            if (h.unit === "auto") setFlex(id, "height", "auto");
            else if (h.unit === "%") setFlex(id, "height", h.value + "%");
            else setFlex(id, "height", h.value);
            break;
        }
        case "minWidth": {
            // pulp Wave 2 css.2 — accept `calc()` / `min()` / `max()` /
            // `clamp()` via the existing resolveCSSLength helper. Numeric
            // px and % paths route through the legacy parseCSSLength
            // dispatch (which the bridge then forwards to Yoga).
            if (/^(calc|min|max|clamp)\(/.test(String(resolved).trim())) {
                setFlex(id, "min_width", resolveCSSLength(resolved, null));
                break;
            }
            var mw = parseCSSLength(resolved);
            if (mw) setFlex(id, "min_width",
                mw.unit === "%" ? (mw.value + "%") : mw.value);
            break;
        }
        case "minHeight": {
            if (/^(calc|min|max|clamp)\(/.test(String(resolved).trim())) {
                setFlex(id, "min_height", resolveCSSLength(resolved, null));
                break;
            }
            var mh = parseCSSLength(resolved);
            if (mh) setFlex(id, "min_height",
                mh.unit === "%" ? (mh.value + "%") : mh.value);
            break;
        }
        case "maxWidth": {
            if (/^(calc|min|max|clamp)\(/.test(String(resolved).trim())) {
                setFlex(id, "max_width", resolveCSSLength(resolved, null));
                break;
            }
            var xw = parseCSSLength(resolved);
            if (xw) setFlex(id, "max_width",
                xw.unit === "%" ? (xw.value + "%") : xw.value);
            break;
        }
        case "maxHeight": {
            if (/^(calc|min|max|clamp)\(/.test(String(resolved).trim())) {
                setFlex(id, "max_height", resolveCSSLength(resolved, null));
                break;
            }
            var xh = parseCSSLength(resolved);
            if (xh) setFlex(id, "max_height",
                xh.unit === "%" ? (xh.value + "%") : xh.value);
            break;
        }

        // Margin (individual) — pulp #1434 cross-surface mega-batch:
        // forward `'NN%'` and `'auto'` strings verbatim so the bridge can
        // route through Yoga's YGNodeStyleSetMargin{Percent,Auto} APIs.
        // Numeric values flow through parseCSSLength as before. `auto`
        // is the canonical centering idiom (e.g. `marginLeft: auto;
        // marginRight: auto`) — Yoga supports it on margin only.
        case "marginTop": {
            if (resolved === "auto") { setFlex(id, "margin_top", "auto"); break; }
            var mt = parseCSSLength(resolved);
            if (!mt) break;
            if (mt.unit === "%") setFlex(id, "margin_top", mt.value + "%");
            else setFlex(id, "margin_top", mt.value);
            break;
        }
        case "marginRight": {
            if (resolved === "auto") { setFlex(id, "margin_right", "auto"); break; }
            var mr = parseCSSLength(resolved);
            if (!mr) break;
            if (mr.unit === "%") setFlex(id, "margin_right", mr.value + "%");
            else setFlex(id, "margin_right", mr.value);
            break;
        }
        case "marginBottom": {
            if (resolved === "auto") { setFlex(id, "margin_bottom", "auto"); break; }
            var mb = parseCSSLength(resolved);
            if (!mb) break;
            if (mb.unit === "%") setFlex(id, "margin_bottom", mb.value + "%");
            else setFlex(id, "margin_bottom", mb.value);
            break;
        }
        case "marginLeft": {
            if (resolved === "auto") { setFlex(id, "margin_left", "auto"); break; }
            var ml = parseCSSLength(resolved);
            if (!ml) break;
            if (ml.unit === "%") setFlex(id, "margin_left", ml.value + "%");
            else setFlex(id, "margin_left", ml.value);
            break;
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
                    var mp = parseCSSLength(tok);
                    if (!mp) continue;
                    setFlex(id, mNames[mi],
                        mp.unit === "%" ? (mp.value + "%") : mp.value);
                }
            }
            break;
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
                break;
            }
            var mhv = parseCSSLength(resolved);
            if (!mhv) break;
            var mhArg = mhv.unit === "%" ? mhv.value + "%" : mhv.value;
            setFlex(id, "margin_left",  mhArg);
            setFlex(id, "margin_right", mhArg);
            break;
        }
        case "marginVertical": {
            if (resolved === "auto") {
                setFlex(id, "margin_top",    "auto");
                setFlex(id, "margin_bottom", "auto");
                break;
            }
            var mvv = parseCSSLength(resolved);
            if (!mvv) break;
            var mvArg = mvv.unit === "%" ? mvv.value + "%" : mvv.value;
            setFlex(id, "margin_top",    mvArg);
            setFlex(id, "margin_bottom", mvArg);
            break;
        }

        // Padding (individual) — pulp #1434 cross-surface mega-batch:
        // forward `'NN%'` strings verbatim (Yoga's
        // YGNodeStyleSetPaddingPercent). Yoga's padding does NOT support
        // `auto` (only margin does), so the keyword is silently dropped
        // here.
        case "paddingTop": {
            var pt = parseCSSLength(resolved);
            if (!pt) break;
            if (pt.unit === "%") setFlex(id, "padding_top", pt.value + "%");
            else setFlex(id, "padding_top", pt.value);
            break;
        }
        case "paddingRight": {
            var pr = parseCSSLength(resolved);
            if (!pr) break;
            if (pr.unit === "%") setFlex(id, "padding_right", pr.value + "%");
            else setFlex(id, "padding_right", pr.value);
            break;
        }
        case "paddingBottom": {
            var pb = parseCSSLength(resolved);
            if (!pb) break;
            if (pb.unit === "%") setFlex(id, "padding_bottom", pb.value + "%");
            else setFlex(id, "padding_bottom", pb.value);
            break;
        }
        case "paddingLeft": {
            var pl = parseCSSLength(resolved);
            if (!pl) break;
            if (pl.unit === "%") setFlex(id, "padding_left", pl.value + "%");
            else setFlex(id, "padding_left", pl.value);
            break;
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
                var pp = parseCSSLength(ptok);
                if (!pp) continue;
                setFlex(id, pNames[pi],
                    pp.unit === "%" ? (pp.value + "%") : pp.value);
            }
            break;
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
            var phv = parseCSSLength(resolved);
            if (!phv) break;
            var phArg = phv.unit === "%" ? phv.value + "%" : phv.value;
            setFlex(id, "padding_left",  phArg);
            setFlex(id, "padding_right", phArg);
            break;
        }
        case "paddingVertical": {
            var pvv = parseCSSLength(resolved);
            if (!pvv) break;
            var pvArg = pvv.unit === "%" ? pvv.value + "%" : pvv.value;
            setFlex(id, "padding_top",    pvArg);
            setFlex(id, "padding_bottom", pvArg);
            break;
        }

        // Colors
        case "backgroundColor": {
            var bgColor = parseCSSColor(resolved);
            if (bgColor) setBackground(id, bgColor);
            break;
        }
        case "color": {
            var txtColor = parseCSSColor(resolved);
            if (txtColor) setTextColor(id, txtColor);
            break;
        }

        // Typography
        case "fontSize": {
            // pulp Wave 2 css.2 — relative-unit & keyword expansion.
            //   • em/rem/%   → resolve against parent / root font-size.
            //                  We don't have a real cascade context here
            //                  (the CSS shim is per-element with no
            //                  ancestor walk), so we use the CSS default
            //                  of 14px (matches Pulp's default Label
            //                  font-size and the resolveCSSLength ctx
            //                  fallback) as the inherited size. This is
            //                  imperfect for nested fontSize cascades but
            //                  matches the existing webview default and
            //                  unblocks the common case (single-level
            //                  rem/em).
            //   • smaller    → 0.83x the inherited size (CSS spec).
            //   • larger     → 1.2x the inherited size  (CSS spec).
            var fsResolved = String(resolved).trim().toLowerCase();
            var inherited = 14; // CSS default + pulp Label default
            if (fsResolved === "smaller") {
                setFontSize(id, inherited * 0.83);
                break;
            }
            if (fsResolved === "larger") {
                setFontSize(id, inherited * 1.2);
                break;
            }
            var fs = parseCSSLength(resolved);
            if (!fs) break;
            if (fs.unit === "em" || fs.unit === "rem") {
                setFontSize(id, fs.value * inherited);
            } else if (fs.unit === "%") {
                setFontSize(id, fs.value / 100 * inherited);
            } else {
                setFontSize(id, fs.value);
            }
            break;
        }
        case "fontWeight":
            // pulp #1434 (batch 3) — translate CSS keyword forms to
            // numeric weight before reaching the bridge. Numeric values
            // ("400", "500") still flow through unchanged. The previous
            // `parseInt` path returned NaN for keywords, which fell back
            // to the `|| 400` default — silently mapping `"bold"` to
            // `normal`. CSS spec values:
            //   normal  → 400
            //   bold    → 700
            //   lighter → 300 (relative to inherited; pulp has no font
            //                  inheritance cascade today, so a fixed
            //                  "one step lighter than normal" is the
            //                  closest safe default)
            //   bolder  → 700 (likewise: "one step bolder than normal")
            // Numeric keywords ("100".."900") parseInt cleanly.
            var fwResolved = String(resolved).trim().toLowerCase();
            var fwNumeric;
            if (fwResolved === "normal") fwNumeric = 400;
            else if (fwResolved === "bold") fwNumeric = 700;
            else if (fwResolved === "lighter") fwNumeric = 300;
            else if (fwResolved === "bolder") fwNumeric = 700;
            else fwNumeric = parseInt(fwResolved, 10) || 400;
            setFontWeight(id, fwNumeric);
            break;
        case "fontStyle":
            // pulp Wave 2 css.4 — `oblique` (and `oblique <angle>`) aliases
            // to `italic`. Skia distinguishes italic-vs-oblique only when
            // the font has a slant (`slnt`) variation axis, which most
            // bundled fonts don't. The previous default-case behavior
            // forwarded `oblique` verbatim, which the bridge silently
            // dropped (only `italic` flips the Label slot). Aliasing
            // upgrades a silent no-op to the closest visual approximation.
            if (/^oblique\b/i.test(String(resolved).trim())) {
                setFontStyle(id, "italic");
            } else {
                setFontStyle(id, resolved);
            }
            break;
        case "letterSpacing": {
            // pulp Wave 2 css.2 — `normal` keyword + `em` unit.
            //   • normal → 0 (CSS spec — no extra spacing)
            //   • em     → resolved against the same default font-size as
            //              fontSize above (14px).
            var lsResolved = String(resolved).trim().toLowerCase();
            if (lsResolved === "normal") {
                setLetterSpacing(id, 0);
                break;
            }
            var ls = parseCSSLength(resolved);
            if (!ls) break;
            if (ls.unit === "em" || ls.unit === "rem") {
                setLetterSpacing(id, ls.value * 14);
            } else {
                setLetterSpacing(id, ls.value);
            }
            break;
        }
        case "lineHeight": {
            // pulp Wave 2 css.2 — accept three additional value forms:
            //   • unitless multiplier ("1.5") → multiply by font-size
            //     (CSS spec — most common form). Pulp's Label expects a
            //     line-height in *pixels*; we resolve the multiplier
            //     against the default font-size of 14px (same caveat as
            //     fontSize re: single-level cascade).
            //   • `%`  → percent of font-size (parses to value/100 * 14)
            //   • `em` → multiplier (same math as unitless)
            //   • `normal` → spec default 1.2 × font-size.
            var lhResolved = String(resolved).trim().toLowerCase();
            if (lhResolved === "normal") {
                setLineHeight(id, 14 * 1.2);
                break;
            }
            // Unitless number (no `px` / `em` / `%` suffix). parseCSSLength
            // treats bare numbers as px, so we have to detect this case
            // before falling through — a value of `1.5` should multiply,
            // not be set as 1.5 px.
            if (/^-?[\d.]+$/.test(lhResolved)) {
                setLineHeight(id, parseFloat(lhResolved) * 14);
                break;
            }
            var lh = parseCSSLength(resolved);
            if (!lh) break;
            if (lh.unit === "em" || lh.unit === "rem") {
                setLineHeight(id, lh.value * 14);
            } else if (lh.unit === "%") {
                setLineHeight(id, lh.value / 100 * 14);
            } else {
                setLineHeight(id, lh.value);
            }
            break;
        }
        case "textAlign":
            setTextAlign(id, resolved);
            break;
        case "textTransform":
            setTextTransform(id, resolved);
            break;
        case "textDecoration":
            setTextDecoration(id, resolved);
            break;
        // pulp #1434 (batch 3) — text-decoration longhands. CSS exposes
        // the shorthand `text-decoration` plus three independent
        // longhands: `-line` / `-color` / `-style`. Routing each to its
        // own bridge setter (instead of coalescing into a shorthand
        // string) means a previously-set sibling longhand is preserved
        // — matching the per-attribute border-color/width fix from PR
        // #1166 finding #4. Same pattern, same reasoning.
        case "textDecorationLine":
            // Reuse the shorthand setter — same line keyword surface
            // (underline / line-through / overline / none).
            setTextDecoration(id, resolved);
            break;
        case "textDecorationColor": {
            var tdc = parseCSSColor(resolved);
            if (tdc && typeof setTextDecorationColor === "function")
                setTextDecorationColor(id, tdc);
            break;
        }
        case "textDecorationStyle":
            if (typeof setTextDecorationStyle === "function")
                setTextDecorationStyle(id, resolved);
            break;
        case "textOverflow":
            setTextOverflow(id, resolved);
            break;

        // Border
        // pulp #1027 (audit PR #1166 finding #4) — these used to lower onto
        // the unified `setBorder(id, color, width, radius)` which clobbers
        // ALL three slots on every call. Setting `borderRadius` then
        // `borderColor` would silently drop the radius back to 0. The
        // per-attribute bridge setters `setBorderColor` / `setBorderWidth`
        // / `setBorderRadius` mutate exactly one field on the View, so
        // routing to them preserves the unset siblings — matching CSS
        // semantics.
        case "borderRadius": {
            // pulp Wave 2 css.2 — accept `%` values. Pulp's setBorderRadius
            // is scalar (no percent unit on the View slot), so we treat
            // the percent value as a px-equivalent best-effort: 50% on a
            // 200x100 box would historically be 100/50px, but we don't
            // have the box size here. The catalog flips %/elliptical to
            // `partial` honest. Users wanting a circle should use a
            // numeric radius >= half their min(width, height).
            var br = parseCSSLength(resolved);
            if (br) setBorderRadius(id, br.value);
            break;
        }
        case "border": {
            // "1px solid #333" — CSS `border` shorthand sets width/style/color
            // but NOT border-radius (per CSS Backgrounds & Borders L3). We
            // route to the per-attribute setters so a previously-set
            // border-radius is preserved across a `border:` shorthand assignment.
            var bp = resolved.match(/([\d.]+)px\s+\w+\s+(.+)/);
            if (bp) {
                var bc = parseCSSColor(bp[2].trim());
                setBorderColor(id, bc || bp[2].trim());
                setBorderWidth(id, parseFloat(bp[1]));
            }
            break;
        }
        case "borderColor": {
            var bcc = parseCSSColor(resolved);
            if (bcc) setBorderColor(id, bcc);
            break;
        }
        case "borderWidth": {
            // pulp Wave 2 css.2 — keyword expansion for `thin` / `medium` /
            // `thick` (CSS Backgrounds & Borders L3 named widths). Browser
            // defaults vary slightly but the canonical values are 1 / 3 /
            // 5px (Chromium / WebKit ship 1 / 3 / 5; Firefox uses 1 / 3 /
            // 5 too). We pick 1 / 2 / 4 to match the original Wave 2 plan
            // — slightly thinner than browsers but a reasonable visual
            // ladder for our 1x DPI default. Authors who want exact
            // browser parity can pass numeric px values.
            var bwResolved = String(resolved).trim().toLowerCase();
            if (bwResolved === "thin")   { setBorderWidth(id, 1); break; }
            if (bwResolved === "medium") { setBorderWidth(id, 2); break; }
            if (bwResolved === "thick")  { setBorderWidth(id, 4); break; }
            var bw = parseCSSLength(resolved);
            if (bw) setBorderWidth(id, bw.value);
            break;
        }
        // pulp #1434 Triage #10 — borderStyle keyword passes verbatim to
        // setBorderStyle. The bridge maps to View::BorderStyle. Skia
        // installs SkDashPathEffect for dashed/dotted; other named
        // styles currently degrade to solid (paint-side gap).
        case "borderStyle": {
            if (typeof setBorderStyle !== "undefined") {
                setBorderStyle(id, resolved);
            }
            break;
        }

        // pulp #1434 Phase A2-3 — CSS `direction: ltr | rtl`. Maps to
        // View::WritingDirection via setDirection bridge fn; Yoga
        // honors at layout, Skia paragraph_style at text shape.
        case "direction": {
            if (typeof setDirection !== "undefined") {
                setDirection(id, resolved);
            }
            break;
        }

        // pulp #1514 — list-style cluster. Pulp doesn't model
        // <li>/<ul>/<ol> semantics; the bridge stores the values
        // verbatim. Marker glyph rendering is deferred — flipping
        // the catalog from `missing` to `partial` documents the
        // stored-but-not-painted state honestly.
        //
        // CSS spec: `list-style: <type> || <position> || <image>`
        // (any order, space-separated). Detect each token by shape:
        //   - matches the type keyword set → setListStyleType
        //   - matches "inside" / "outside" → setListStylePosition
        //   - starts with "url(" or "none" → setListStyleImage
        case "listStyle": {
            // Parse the space-separated shorthand into the 3 longhands.
            var lsTokens = String(resolved).trim().split(/\s+/);
            var lsTypes = { "none": 1, "disc": 1, "circle": 1, "square": 1, "decimal": 1 };
            var lsPos = { "inside": 1, "outside": 1 };
            var sawType = false;
            var sawPos = false;
            var sawImage = false;
            for (var li = 0; li < lsTokens.length; li++) {
                var tok = lsTokens[li];
                if (tok.indexOf("url(") === 0) {
                    if (typeof setListStyleImage !== "undefined") setListStyleImage(id, tok);
                    sawImage = true;
                } else if (lsPos[tok]) {
                    if (typeof setListStylePosition !== "undefined") setListStylePosition(id, tok);
                    sawPos = true;
                } else if (lsTypes[tok]) {
                    // "none" matches both type and image. CSS spec: "none"
                    // applies to whichever is unset; if neither, type wins.
                    // We bias to type — `list-style: none` is overwhelmingly
                    // a type-reset, not an image-reset.
                    if (tok === "none" && sawType && !sawImage) {
                        if (typeof setListStyleImage !== "undefined") setListStyleImage(id, "none");
                        sawImage = true;
                    } else {
                        if (typeof setListStyleType !== "undefined") setListStyleType(id, tok);
                        sawType = true;
                    }
                }
                // Unknown tokens silently dropped.
            }
            break;
        }
        case "listStyleType": {
            if (typeof setListStyleType !== "undefined") setListStyleType(id, resolved);
            break;
        }
        case "listStyleImage": {
            if (typeof setListStyleImage !== "undefined") setListStyleImage(id, resolved);
            break;
        }
        case "listStylePosition": {
            if (typeof setListStylePosition !== "undefined") setListStylePosition(id, resolved);
            break;
        }

        // Opacity
        case "opacity":
            setOpacity(id, parseFloat(resolved) || 0);
            break;

        // Overflow
        case "overflow":
            setOverflow(id, resolved);
            break;
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
            break;

        // Cursor
        case "cursor":
            setCursor(id, resolved);
            break;

        // Touch behavior (W3C touch-action)
        case "touch-action":
        case "touchAction":
            // Store on element for pointer event handling
            // Values: auto, none, pan-x, pan-y, pinch-zoom, manipulation
            this._touchAction = resolved;
            break;

        // Transform
        case "transform": {
            // pulp #1434 Triage #9 — full CSS transform-function fan-out.
            // Walk-once accumulator (mirrors the @pulp/react prop-applier
            // walker) so the within-string order produces a single set
            // of consolidated bridge calls instead of multiple
            // axis-clobbering ones. translateX(10) translateY(20)
            // produces ONE setTranslate(10, 20). scaleX/scaleY share the
            // uniform setScale slot (last-write-wins; bridge gap).
            // skewX(α) skewY(β) → ONE setSkew(α, β).
            //
            // Deferred (silent no-op + TODO):
            //   • rotateX / rotateY — pulp's 2D View has no 3D rotation
            //     storage; rotateZ aliases to setRotation.
            //   • matrix3d / perspective — ditto, no 3D model.
            //   • matrix(a b c d tx ty) — 2D affine. Per Codex P1 audit,
            //     dispatched directly to setTransform(id, a, b, c, d, e, f)
            //     to preserve all 6 components verbatim. The earlier
            //     decomposition to translate+uniform-scale+rotate dropped
            //     the c/d skew components on rotation matrices like
            //     `matrix(0.866, 0.5, -0.5, 0.866, 100, 50)` and could
            //     mask zero-scale collapses (a=b=0 was silently rounded
            //     to scl=1).
            var transforms = parseTransform(resolved);
            var tx = 0, ty = 0;
            var rotZ = 0;
            var scl = 1;
            var skewX = 0, skewY = 0;
            var haveT = false, haveR = false, haveS = false, haveK = false;
            var matrixCall = null; // {a,b,c,d,e,f} for matrix() entries
            for (var i = 0; i < transforms.length; i++) {
                var t = transforms[i];
                var a0 = t.args[0] || 0;
                var a1 = t.args[1] || 0;
                if (t.fn === "translate")        { tx = a0; ty = a1; haveT = true; }
                else if (t.fn === "translateX") { tx = a0;          haveT = true; }
                else if (t.fn === "translateY") { ty = a0;          haveT = true; }
                else if (t.fn === "rotate")     { rotZ = a0;        haveR = true; }
                else if (t.fn === "rotateZ")    { rotZ = a0;        haveR = true; }
                else if (t.fn === "scale")      { scl = a0;         haveS = true; }
                else if (t.fn === "scaleX")     { scl = a0;         haveS = true; }
                else if (t.fn === "scaleY")     { scl = a0;         haveS = true; }
                else if (t.fn === "skewX")      { skewX = a0;       haveK = true; }
                else if (t.fn === "skewY")      { skewY = a0;       haveK = true; }
                else if (t.fn === "matrix") {
                    // matrix(a b c d tx ty) — preserve full 6-component
                    // 2D affine. The bridge already exposes setTransform
                    // with the same 6-arg signature; we pass through
                    // verbatim. Note: when matrix() coexists with
                    // translate/scale/rotate ops in the same string,
                    // matrix() takes precedence (its 6 components encode
                    // the full affine — applying the others on top would
                    // be ambiguous).
                    matrixCall = {
                        a: t.args[0] !== undefined ? t.args[0] : 1,
                        b: t.args[1] !== undefined ? t.args[1] : 0,
                        c: t.args[2] !== undefined ? t.args[2] : 0,
                        d: t.args[3] !== undefined ? t.args[3] : 1,
                        e: t.args[4] !== undefined ? t.args[4] : 0,
                        f: t.args[5] !== undefined ? t.args[5] : 0,
                    };
                }
                // rotateX / rotateY / matrix3d / perspective: 2D View has
                // no 3D rotation storage; silently dropped. Tracked for
                // a follow-up issue (3D model on View).
            }
            if (matrixCall && typeof setTransform !== "undefined") {
                // Full-matrix path — 6-component bridge call. Skips the
                // decomposed translate/rotate/scale dispatchers since
                // matrix() already encodes them in a/b/c/d/e/f.
                setTransform(id, matrixCall.a, matrixCall.b, matrixCall.c,
                             matrixCall.d, matrixCall.e, matrixCall.f);
            } else {
                if (haveT) setTranslate(id, tx, ty);
                if (haveR) setRotation(id, rotZ);
                if (haveS) setScale(id, scl);
                if (haveK && typeof setSkew !== "undefined") setSkew(id, skewX, skewY);
            }
            break;
        }
        case "transformOrigin": {
            // "center", "left top", "50% 50%", "10px 20px"
            var ox = 0.5, oy = 0.5;
            var op = resolved.split(/\s+/);
            function _parseOrigin(v) {
                if (v === "center") return 0.5;
                if (v === "left" || v === "top") return 0;
                if (v === "right" || v === "bottom") return 1;
                var l = parseCSSLength(v);
                if (l && l.unit === "%") return l.value / 100;
                return 0.5;
            }
            ox = _parseOrigin(op[0] || "center");
            oy = _parseOrigin(op[1] || op[0] || "center");
            setTransformOrigin(id, ox, oy);
            break;
        }

        // Transition (pulp #1434 Phase A2-1) — pass the full shorthand
        // string to the bridge, which parses it into a list of
        // TransitionSpecs (one per comma-separated entry; supports
        // duration / delay / easing / property + cubic-bezier + steps).
        // The legacy parseTransition / setTransitionDuration path is
        // kept as a fallback for older runtimes.
        case "transition": {
            if (typeof setTransition !== "undefined") {
                setTransition(id, resolved);
            } else {
                var tr = parseTransition(resolved);
                setTransitionDuration(id, tr.duration);
            }
            break;
        }
        case "transitionDuration": {
            var td = parseFloat(resolved);
            if (resolved.indexOf("ms") >= 0) td /= 1000;
            setTransitionDuration(id, td);
            break;
        }
        case "transitionProperty": {
            if (typeof setTransitionProperty !== "undefined") {
                setTransitionProperty(id, resolved);
            }
            break;
        }
        case "transitionTimingFunction": {
            if (typeof setTransitionTimingFunction !== "undefined") {
                setTransitionTimingFunction(id, resolved);
            }
            break;
        }
        case "transitionDelay": {
            var dly = parseFloat(resolved);
            if (resolved.indexOf("ms") >= 0) dly /= 1000;
            if (typeof setTransitionDelay !== "undefined") {
                setTransitionDelay(id, dly);
            }
            break;
        }

        // Position
        case "position":
            setPosition(id, resolved);
            // pulp #1148 (slice b) — auto-overlay heuristic. Re-evaluate
            // whenever `position` changes; switching to `absolute` with
            // a sufficient z-index claims the global overlay slot, and
            // switching back to `static` / `relative` releases it.
            this._reevaluateOverlay();
            break;
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
            var tv = parseCSSLength(resolved); if (!tv) break;
            if (tv.unit === "%") setTop(id, tv.value + "%");
            else if (tv.unit === "em" || tv.unit === "rem") setTop(id, tv.value * 14);
            else if (tv.unit === "vh") setTop(id, tv.value / 100 * 600);
            else if (tv.unit === "vw") setTop(id, tv.value / 100 * 800);
            else setTop(id, tv.value);
            break;
        }
        case "right": {
            var rv = parseCSSLength(resolved); if (!rv) break;
            if (rv.unit === "%") setRight(id, rv.value + "%");
            else if (rv.unit === "em" || rv.unit === "rem") setRight(id, rv.value * 14);
            else if (rv.unit === "vh") setRight(id, rv.value / 100 * 600);
            else if (rv.unit === "vw") setRight(id, rv.value / 100 * 800);
            else setRight(id, rv.value);
            break;
        }
        case "bottom": {
            var bv = parseCSSLength(resolved); if (!bv) break;
            if (bv.unit === "%") setBottom(id, bv.value + "%");
            else if (bv.unit === "em" || bv.unit === "rem") setBottom(id, bv.value * 14);
            else if (bv.unit === "vh") setBottom(id, bv.value / 100 * 600);
            else if (bv.unit === "vw") setBottom(id, bv.value / 100 * 800);
            else setBottom(id, bv.value);
            break;
        }
        case "left": {
            var lv = parseCSSLength(resolved); if (!lv) break;
            if (lv.unit === "%") setLeft(id, lv.value + "%");
            else if (lv.unit === "em" || lv.unit === "rem") setLeft(id, lv.value * 14);
            else if (lv.unit === "vh") setLeft(id, lv.value / 100 * 600);
            else if (lv.unit === "vw") setLeft(id, lv.value / 100 * 800);
            else setLeft(id, lv.value);
            break;
        }

        // z-index
        case "zIndex":
            setZIndex(id, parseInt(resolved) || 0);
            // pulp #1148 (slice b) — z-index moving above/below the
            // popover threshold flips the auto-overlay heuristic.
            this._reevaluateOverlay();
            break;

        // Box shadow: "2px 4px 8px rgba(0,0,0,0.3)" or "inset 2px 4px 8px ..." (issue-925)
        case "boxShadow": {
            if (resolved === "none" || resolved === "" || resolved == null) {
                if (typeof clearBoxShadow === "function") clearBoxShadow(id);
                break;
            }
            var work = String(resolved).trim();
            var inset = false;
            if (/^inset\s+/i.test(work)) {
                inset = true;
                work = work.replace(/^inset\s+/i, "");
            } else if (/\s+inset\s*$/i.test(work)) {
                inset = true;
                work = work.replace(/\s+inset\s*$/i, "");
            }
            var sm = work.match(/(-?[\d.]+)px\s+(-?[\d.]+)px\s+([\d.]+)px(?:\s+(-?[\d.]+)px)?\s+(.*)/);
            if (sm) {
                var sc = parseCSSColor(sm[5].trim());
                setBoxShadow(id, parseFloat(sm[1]), parseFloat(sm[2]),
                            parseFloat(sm[3]), parseFloat(sm[4] || 0),
                            sc || sm[5].trim(), inset);
            }
            break;
        }

        // Filter
        case "filter":
            setFilter(id, resolved);
            break;

        // pulp #1434 (batch 3) — backdrop-filter route. The bridge
        // setter is numeric (`setBackdropFilter(id, blur_px)`), so we
        // parse a `blur(Npx)` substring out of the CSS value. This
        // matches what `setFilter` already does on the bridge side
        // (see widget_bridge.cpp::setFilter — same blur-only surface).
        // Any other filter function is intentionally ignored here;
        // matching the `unsupportedValues: ["other filter functions"]`
        // entry in compat.json. `none` / empty / 0 clears the slot.
        case "backdropFilter": {
            if (typeof setBackdropFilter !== "function") break;
            var bdf = String(resolved).trim().toLowerCase();
            if (bdf === "" || bdf === "none") {
                setBackdropFilter(id, 0);
                break;
            }
            // Match `blur(Npx)` or `blur(N)` (treat unitless as px).
            var bdm = bdf.match(/blur\(\s*([\d.]+)\s*(px)?\s*\)/);
            if (bdm) {
                setBackdropFilter(id, parseFloat(bdm[1]) || 0);
            }
            break;
        }

        // pulp #1515 — CSS `clip-path` cluster. The bridge only honors
        // the `path("...")` form (Skia parses via SkPath::FromSVGString
        // and installs the clip on paint). URL refs (`url(#id)`) and
        // named shape forms (`circle()`, `inset()`, `polygon()`,
        // `ellipse()`) are deferred — for those we set an empty slot
        // so the partial coverage is honest. `none` / empty clears.
        case "clipPath": {
            if (typeof setClipPath !== "function") break;
            var cpv = String(resolved).trim();
            if (cpv === "" || cpv === "none") {
                setClipPath(id, "");
                break;
            }
            // path("M 0 0 L 100 0 ...") or path('M 0 0 ...').
            var cpm = cpv.match(/^path\(\s*['"]([^'"]+)['"]\s*\)$/);
            if (cpm) {
                setClipPath(id, cpm[1]);
            } else {
                // url() / circle() / inset() / polygon() — deferred;
                // clear the slot so a previous path() doesn't linger.
                setClipPath(id, "");
            }
            break;
        }

        // pulp #1515 — CSS `mask-image`. Storage-only today; the
        // paint pipeline does not yet composite a shader mask onto a
        // saveLayer. Forwarding the value through to the bridge keeps
        // the slot round-trippable so harness tests can assert the
        // shim accepts the value, and so a future paint slice can
        // honor it without a JS-side change.
        case "maskImage": {
            if (typeof setMaskImage !== "function") break;
            var miv = String(resolved).trim();
            if (miv === "none") miv = "";
            setMaskImage(id, miv);
            break;
        }

        // pulp #1515 — CSS `mask` shorthand. Parse the image
        // sub-property out (it's the only longhand we support today)
        // and forward both the shorthand verbatim (so View::mask()
        // round-trips) and the extracted image to setMaskImage.
        // The remaining longhands (mode / repeat / position / size /
        // origin / clip / composite) are deferred — the saveLayer +
        // SkBlendMode::kDstIn paint slice is the follow-up.
        case "mask": {
            if (typeof setMask === "function") {
                setMask(id, String(resolved));
            }
            if (typeof setMaskImage === "function") {
                var mv = String(resolved).trim();
                if (mv === "" || mv === "none") {
                    setMaskImage(id, "");
                } else {
                    // Pull the first url(...) / linear-gradient(...) /
                    // radial-gradient(...) substring out and treat the
                    // rest as deferred sub-properties. Solid-color
                    // masks (`mask: black`) flow through verbatim too;
                    // the bridge stores the value but doesn't paint it
                    // yet.
                    var imgm = mv.match(/(url\([^)]*\)|(?:linear|radial|conic)-gradient\([^)]*\))/);
                    setMaskImage(id, imgm ? imgm[1] : mv);
                }
            }
            break;
        }

        // Background gradient
        case "backgroundImage":
        case "background": {
            if (resolved.indexOf("gradient") >= 0) {
                setBackgroundGradient(id, resolved);
            } else {
                var bgc2 = parseCSSColor(resolved);
                if (bgc2) setBackground(id, bgc2);
            }
            break;
        }

        // pulp #1517 — background sub-props.
        // - backgroundAttachment: only `scroll` is the conformant default in
        //   pulp's non-scrolling layout model. `fixed` / `local` need a
        //   scroll-context coupling we don't model — accept verbatim and
        //   no-op so consumers don't crash. Catalog is `noop`.
        // - backgroundClip: `text` is the only interesting form (paint-time
        //   SkBlendMode::kSrcIn against text glyphs). Others are no-ops on
        //   our solid-bg surface. The bridge slot stores the keyword so
        //   future paint logic can honor it; catalog is `partial` because
        //   `text` isn't fully wired through the paint chain yet.
        // - backgroundOrigin: positions the bg-paint origin relative to the
        //   border / padding / content box. Pulp paints bg edge-to-edge,
        //   so all three keywords no-op for a solid color and matter only
        //   for repeating gradients (deferred). Catalog is `noop`.
        case "backgroundAttachment":
            // Stored on the View's bg-attachment slot via a thin bridge
            // setter that just records the keyword — no paint impact today.
            if (typeof setBackgroundAttachment === "function") {
                setBackgroundAttachment(id, resolved);
            }
            break;
        case "backgroundClip":
            if (typeof setBackgroundClip === "function") {
                setBackgroundClip(id, resolved);
            }
            break;
        case "backgroundOrigin":
            if (typeof setBackgroundOrigin === "function") {
                setBackgroundOrigin(id, resolved);
            }
            break;

        // Grid
        case "gridTemplateColumns":
            setGrid(id, "template_columns", resolved);
            break;
        case "gridTemplateRows":
            setGrid(id, "template_rows", resolved);
            break;
        case "gridColumn": {
            var gc = resolved.split("/").map(function(s) { return parseInt(s.trim()); });
            if (gc[0]) setGrid(id, "column_start", gc[0]);
            if (gc[1]) setGrid(id, "column_end", gc[1]);
            break;
        }
        case "gridRow": {
            var gr = resolved.split("/").map(function(s) { return parseInt(s.trim()); });
            if (gr[0]) setGrid(id, "row_start", gr[0]);
            if (gr[1]) setGrid(id, "row_end", gr[1]);
            break;
        }
        // pulp #1434 Phase A2-2 — extended grid surface
        case "gridAutoColumns":
            setGrid(id, "auto_columns", resolved);
            break;
        case "gridAutoRows":
            setGrid(id, "auto_rows", resolved);
            break;
        case "gridAutoFlow":
            setGrid(id, "auto_flow", resolved);
            break;
        case "gridTemplateAreas":
            setGrid(id, "template_areas", resolved);
            break;
        case "gridArea":
            // Pass through verbatim — bridge distinguishes name vs.
            // numeric "row / col / row / col" form.
            setGrid(id, "grid_area", resolved);
            break;

        // ── P1: New CSS properties ──────────────────────────────────────

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
                break;
            }
            var arParts = trimmed.split("/");
            var num = parseFloat(arParts[0]);
            if (!isFinite(num) || num <= 0) {
                setFlex(id, "aspect_ratio", 0);
                break;
            }
            var ratio = num;
            if (arParts[1] !== undefined) {
                var den = parseFloat(arParts[1]);
                if (!isFinite(den) || den <= 0) {
                    setFlex(id, "aspect_ratio", 0);
                    break;
                }
                ratio = num / den;
            }
            setFlex(id, "aspect_ratio", ratio);
            break;
        }

        // visibility: "hidden" vs display:none — hidden preserves layout space
        case "visibility":
            if (typeof setVisibility === "function") setVisibility(id, resolved);
            else if (resolved === "hidden") setOpacity(id, 0);
            else setOpacity(id, 1);
            break;

        // outline: "2px solid blue" — fan-out to the per-attribute
        // bridge fns introduced in pulp #1519 (setOutlineColor /
        // setOutlineStyle / setOutlineWidth). Falls back to legacy
        // setOutline if the new ones aren't registered (older bridge).
        case "outline": {
            var op = resolved.match(/([\d.]+)px\s+(\w+)\s+(.+)/);
            if (op) {
                var oc = parseCSSColor(op[3].trim());
                if (typeof setOutlineWidth === "function") {
                    setOutlineWidth(id, parseFloat(op[1]));
                    if (typeof setOutlineStyle === "function") setOutlineStyle(id, op[2]);
                    if (typeof setOutlineColor === "function") setOutlineColor(id, oc || op[3].trim());
                } else if (typeof setOutline === "function") {
                    setOutline(id, parseFloat(op[1]), oc || op[3].trim());
                }
            }
            break;
        }
        case "outlineWidth": {
            var ow = parseCSSLength(resolved);
            if (ow) {
                if (typeof setOutlineWidth === "function") setOutlineWidth(id, ow.value);
                else if (typeof setOutline === "function") setOutline(id, ow.value, "");
            }
            break;
        }
        case "outlineColor": {
            var occ = parseCSSColor(resolved);
            if (occ) {
                if (typeof setOutlineColor === "function") setOutlineColor(id, occ);
                else if (typeof setOutline === "function") setOutline(id, 0, occ);
            }
            break;
        }
        // pulp #1519 — outline-offset / outline-style now have dedicated
        // bridge setters. Outline doesn't take Yoga layout space, so the
        // CSS path mirrors borderStyle keyword set verbatim.
        case "outlineOffset": {
            var oo = parseCSSLength(resolved);
            if (oo && typeof setOutlineOffset === "function") setOutlineOffset(id, oo.value);
            break;
        }
        case "outlineStyle": {
            if (typeof setOutlineStyle === "function") setOutlineStyle(id, resolved);
            break;
        }

        // white-space: "nowrap", "pre", "normal"
        case "whiteSpace":
            if (typeof setWhiteSpace === "function") setWhiteSpace(id, resolved);
            break;

        // word-break / overflow-wrap
        case "wordBreak":
        case "overflowWrap":
        case "wordWrap":
            if (typeof setWordBreak === "function") setWordBreak(id, resolved);
            break;

        // text-shadow: "2px 2px 4px rgba(0,0,0,0.5)"
        case "textShadow": {
            var tsm = resolved.match(/(-?[\d.]+)px\s+(-?[\d.]+)px\s+([\d.]+)px\s+(.*)/);
            if (tsm && typeof setTextShadow === "function") {
                var tsc = parseCSSColor(tsm[4].trim());
                setTextShadow(id, parseFloat(tsm[1]), parseFloat(tsm[2]), parseFloat(tsm[3]), tsc || tsm[4].trim());
            }
            break;
        }

        // user-select: "none", "text", "all"
        case "userSelect":
            if (typeof setUserSelect === "function") setUserSelect(id, resolved);
            break;

        // pointer-events: "none", "auto"
        case "pointerEvents":
            if (typeof setPointerEvents === "function") setPointerEvents(id, resolved);
            break;

        // font-family — pulp #1151
        // CSS font-family is a comma-separated fallback list, e.g.
        //   font-family: 'JetBrains Mono', ui-monospace, SFMono-Regular, monospace;
        // Splitting must respect quoted multi-word names ('JetBrains Mono'
        // contains a space but is one family). Real CSS doesn't put commas
        // inside font names, so a plain split is sufficient. We trim each
        // token, strip matching outer single/double quotes, and hand the
        // first non-empty parsed family to setFontFamily — Pulp's bundled
        // fonts (#932) plus the public registration API (#1150) decide if
        // it resolves; generic fallbacks like "monospace" / "ui-monospace"
        // never resolved before either, so picking the first author-named
        // family is the right behavior.
        //
        // Before this fix, the entire raw list (commas + spaces) was passed
        // verbatim to SkFontMgr::matchFamilyStyle which never matched
        // anything → silent fallback to platform default.
        case "fontFamily":
            if (typeof setFontFamily === "function") {
                var ffParts = String(resolved).split(",");
                for (var ffi = 0; ffi < ffParts.length; ffi++) {
                    var ffName = ffParts[ffi].replace(/^\s+|\s+$/g, "");
                    if (ffName.length >= 2) {
                        var first = ffName.charAt(0);
                        var last = ffName.charAt(ffName.length - 1);
                        if ((first === "'" && last === "'") || (first === '"' && last === '"')) {
                            ffName = ffName.substring(1, ffName.length - 1);
                        }
                    }
                    if (ffName.length > 0) {
                        setFontFamily(id, ffName);
                        break;
                    }
                }
            }
            break;

        // background-size: "cover", "contain", "100px 200px"
        case "backgroundSize":
            if (typeof setBackgroundSize === "function") setBackgroundSize(id, resolved);
            break;

        // background-position: "center", "top left", "50% 50%"
        case "backgroundPosition":
            if (typeof setBackgroundPosition === "function") setBackgroundPosition(id, resolved);
            break;

        // align-content (multi-line flex cross-axis)
        case "alignContent":
            setFlex(id, "align_content", _cssToFlex(resolved));
            break;

        // ── Tier 2: Per-side borders ────────────────────────────────────

        case "borderTop": case "borderRight": case "borderBottom": case "borderLeft": {
            var side = key.replace("border", "").toLowerCase();
            var bsp = resolved.match(/([\d.]+)px\s+\w+\s+(.+)/);
            if (bsp) {
                var bsc = parseCSSColor(bsp[2].trim());
                if (typeof setBorderSide === "function")
                    setBorderSide(id, side, parseFloat(bsp[1]), bsc || bsp[2].trim());
            }
            break;
        }
        // pulp #1027 (audit PR #1166 finding #4) — per-side flat props
        // (RN parity). Route to setBorderTop/Right/Bottom/Left{Color,Width}
        // which preserve the OTHER attribute on the View (see
        // applyBorderSide in widget_bridge.cpp). Calling setBorderSide
        // with a placeholder 0/"" for the unset slot would clobber it.
        case "borderTopWidth": {
            var bwT = parseCSSLength(resolved);
            if (bwT && typeof setBorderTopWidth === "function") setBorderTopWidth(id, bwT.value);
            break;
        }
        case "borderRightWidth": {
            var bwR = parseCSSLength(resolved);
            if (bwR && typeof setBorderRightWidth === "function") setBorderRightWidth(id, bwR.value);
            break;
        }
        case "borderBottomWidth": {
            var bwB = parseCSSLength(resolved);
            if (bwB && typeof setBorderBottomWidth === "function") setBorderBottomWidth(id, bwB.value);
            break;
        }
        case "borderLeftWidth": {
            var bwL = parseCSSLength(resolved);
            if (bwL && typeof setBorderLeftWidth === "function") setBorderLeftWidth(id, bwL.value);
            break;
        }
        case "borderTopColor": {
            var bcT = parseCSSColor(resolved);
            if (bcT && typeof setBorderTopColor === "function") setBorderTopColor(id, bcT);
            break;
        }
        case "borderRightColor": {
            var bcR = parseCSSColor(resolved);
            if (bcR && typeof setBorderRightColor === "function") setBorderRightColor(id, bcR);
            break;
        }
        case "borderBottomColor": {
            var bcB = parseCSSColor(resolved);
            if (bcB && typeof setBorderBottomColor === "function") setBorderBottomColor(id, bcB);
            break;
        }
        case "borderLeftColor": {
            var bcL = parseCSSColor(resolved);
            if (bcL && typeof setBorderLeftColor === "function") setBorderLeftColor(id, bcL);
            break;
        }

        // Per-corner border-radius
        case "borderTopLeftRadius": case "borderTopRightRadius":
        case "borderBottomLeftRadius": case "borderBottomRightRadius": {
            var corner = key.replace("border", "").replace("Radius", "");
            var cr = parseCSSLength(resolved);
            if (cr && typeof setCornerRadius === "function")
                setCornerRadius(id, corner, cr.value);
            break;
        }

        // ── Tier 3: Layout keywords ─────────────────────────────────────

        // box-sizing
        case "boxSizing":
            if (typeof setBoxSizing === "function") setBoxSizing(id, resolved);
            break;

        // flex-flow shorthand
        case "flexFlow": {
            // pulp #1434 Triage #14 — recognize the full direction +
            // wrap vocabulary including `row-reverse` / `column-reverse`
            // (already-wired but missing from this shorthand path) and
            // `wrap-reverse` (newly wired through the bridge).
            var ffp = resolved.split(/\s+/);
            for (var ffi = 0; ffi < ffp.length; ffi++) {
                var tok = ffp[ffi];
                if (tok === "row" || tok === "column"
                        || tok === "row-reverse" || tok === "column-reverse") {
                    setFlex(id, "direction", tok === "row" ? "row" : tok);
                }
                else if (tok === "wrap" || tok === "nowrap"
                        || tok === "no-wrap" || tok === "wrap-reverse") {
                    setFlex(id, "flex_wrap", tok);
                }
            }
            break;
        }

        // place-items shorthand (align-items + justify-items)
        case "placeItems": {
            var pip = resolved.split(/\s+/);
            setFlex(id, "align_items", _cssToFlex(pip[0]));
            if (pip[1]) setFlex(id, "justify_content", _cssToFlex(pip[1]));
            break;
        }

        // place-content shorthand
        case "placeContent": {
            var pcp = resolved.split(/\s+/);
            setFlex(id, "align_content", _cssToFlex(pcp[0]));
            if (pcp[1]) setFlex(id, "justify_content", _cssToFlex(pcp[1]));
            break;
        }

        // ── Tier 4: Animation properties ────────────────────────────────

        case "animationName":
            if (typeof setAnimation === "function") setAnimation(id, "name", resolved);
            break;
        case "animationDuration": {
            var ad = parseFloat(resolved);
            if (resolved.indexOf("ms") >= 0) ad /= 1000;
            if (typeof setAnimation === "function") setAnimation(id, "duration", ad);
            break;
        }
        case "animationTimingFunction":
            if (typeof setAnimation === "function") setAnimation(id, "easing", resolved);
            break;
        case "animationDelay": {
            var adl = parseFloat(resolved);
            if (resolved.indexOf("ms") >= 0) adl /= 1000;
            if (typeof setAnimation === "function") setAnimation(id, "delay", adl);
            break;
        }
        case "animationIterationCount":
            if (typeof setAnimation === "function")
                setAnimation(id, "iterations", resolved === "infinite" ? -1 : parseFloat(resolved) || 1);
            break;
        case "animationDirection":
            if (typeof setAnimation === "function") setAnimation(id, "direction", resolved);
            break;
        case "animationFillMode":
            if (typeof setAnimation === "function") setAnimation(id, "fill", resolved);
            break;
        // pulp #1434 A4 Bundle 2 — animation-play-state. Forwards the
        // CSS keyword (`running` | `paused`) through the existing
        // setAnimation control-token ABI so the bridge can route it to
        // the staged_animation slot. The full pause/resume of the
        // active_animations playback driver is the follow-up; storing
        // the keyword today is enough for the catalog to claim partial
        // and for round-trip validation.
        case "animationPlayState":
            if (typeof setAnimation === "function") setAnimation(id, "play_state", resolved);
            break;
        case "animation": {
            // Shorthand: "name duration easing delay iterations direction fill"
            var atr = parseTransition(resolved); // reuse transition parser for timing
            if (typeof setAnimation === "function") {
                setAnimation(id, "name", atr.property);
                setAnimation(id, "duration", atr.duration);
                setAnimation(id, "easing", atr.easing);
                setAnimation(id, "delay", atr.delay);
            }
            break;
        }

        // ── Tier 6: Additional filter functions ─────────────────────────

        // filter already handled above — extend for multi-function
        // (the existing setFilter bridge parses "blur(4px)" — additional functions pass through)

        // ── Tier 7: CSS logical properties ──────────────────────────────
        //
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
            break;
        }
        case "marginInlineStart": {
            // LTR fast path — inline-start ≡ left.
            if (resolved === "auto") { setFlex(id, "margin_left", "auto"); break; }
            var mis = parseCSSLength(resolved);
            if (!mis) break;
            setFlex(id, "margin_left", mis.unit === "%" ? mis.value + "%" : mis.value);
            break;
        }
        case "marginInlineEnd": {
            // LTR fast path — inline-end ≡ right.
            if (resolved === "auto") { setFlex(id, "margin_right", "auto"); break; }
            var mie = parseCSSLength(resolved);
            if (!mie) break;
            setFlex(id, "margin_right", mie.unit === "%" ? mie.value + "%" : mie.value);
            break;
        }
        case "marginBlock": {
            var mb2 = expandShorthand(resolved);
            setFlex(id, "margin_top", mb2[0]); setFlex(id, "margin_bottom", mb2[1]);
            break;
        }
        case "marginBlockStart": {
            // horizontal-tb fast path — block-start ≡ top.
            if (resolved === "auto") { setFlex(id, "margin_top", "auto"); break; }
            var mbs = parseCSSLength(resolved);
            if (!mbs) break;
            setFlex(id, "margin_top", mbs.unit === "%" ? mbs.value + "%" : mbs.value);
            break;
        }
        case "marginBlockEnd": {
            // horizontal-tb fast path — block-end ≡ bottom.
            if (resolved === "auto") { setFlex(id, "margin_bottom", "auto"); break; }
            var mbe = parseCSSLength(resolved);
            if (!mbe) break;
            setFlex(id, "margin_bottom", mbe.unit === "%" ? mbe.value + "%" : mbe.value);
            break;
        }
        case "paddingInline": {
            var pi2 = expandShorthand(resolved);
            setFlex(id, "padding_left", pi2[0]); setFlex(id, "padding_right", pi2[1]);
            break;
        }
        case "paddingInlineStart": {
            // LTR fast path — inline-start ≡ left. Yoga's padding doesn't
            // support `auto` (only margin does); keyword silently dropped.
            var pis = parseCSSLength(resolved);
            if (!pis) break;
            setFlex(id, "padding_left", pis.unit === "%" ? pis.value + "%" : pis.value);
            break;
        }
        case "paddingInlineEnd": {
            // LTR fast path — inline-end ≡ right.
            var pie = parseCSSLength(resolved);
            if (!pie) break;
            setFlex(id, "padding_right", pie.unit === "%" ? pie.value + "%" : pie.value);
            break;
        }
        case "paddingBlock": {
            var pb2 = expandShorthand(resolved);
            setFlex(id, "padding_top", pb2[0]); setFlex(id, "padding_bottom", pb2[1]);
            break;
        }
        case "paddingBlockStart": {
            // horizontal-tb fast path — block-start ≡ top.
            var pbs = parseCSSLength(resolved);
            if (!pbs) break;
            setFlex(id, "padding_top", pbs.unit === "%" ? pbs.value + "%" : pbs.value);
            break;
        }
        case "paddingBlockEnd": {
            // horizontal-tb fast path — block-end ≡ bottom.
            var pbe = parseCSSLength(resolved);
            if (!pbe) break;
            setFlex(id, "padding_bottom", pbe.unit === "%" ? pbe.value + "%" : pbe.value);
            break;
        }
        case "inset": {
            var ins = expandShorthand(resolved);
            var tv2 = parseCSSLength(String(ins[0])); if (tv2) setTop(id, tv2.value);
            var rv2 = parseCSSLength(String(ins[1])); if (rv2) setRight(id, rv2.value);
            var bv2 = parseCSSLength(String(ins[2])); if (bv2) setBottom(id, bv2.value);
            var lv2 = parseCSSLength(String(ins[3])); if (lv2) setLeft(id, lv2.value);
            break;
        }

        // line-clamp (-webkit-line-clamp)
        case "webkitLineClamp":
        case "lineClamp":
            if (typeof setLineClamp === "function") setLineClamp(id, parseInt(resolved) || 0);
            break;

        // background-repeat
        case "backgroundRepeat":
            if (typeof setBackgroundRepeat === "function") setBackgroundRepeat(id, resolved);
            break;

        // ── Tier 8: pulp #1434 A4 Bundles 5–7 closure ───────────────────
        // The remaining cases route value-bearing CSS properties to the
        // bridge surface even when the bridge fn is absent today (the
        // typeof guard makes the call a no-op on older bridges, which
        // matches the convention used elsewhere in this file). Each case
        // is paired with a catalog entry that documents the implementation
        // depth (`partial` for storage-only, `noop` for accept-and-ignore,
        // `wontfix` for architecturally out-of-scope).

        // text-indent — first-line indent. Storage-only; SkParagraph
        // setTextIndent integration is the follow-up.
        case "textIndent": {
            var ti = parseCSSLength(resolved);
            if (ti && typeof setTextIndent === "function") setTextIndent(id, ti.value);
            break;
        }

        // vertical-align — line-box vertical alignment. Maps the four
        // canvas::TextVerticalAlign slots (top|middle|bottom|baseline);
        // sub/super and length values fall back to baseline.
        case "verticalAlign":
            if (typeof setVerticalAlign === "function") setVerticalAlign(id, resolved);
            break;

        // mix-blend-mode — already wired via setMixBlendMode bridge fn
        // (#1549). Mirroring the RN surface so CSS authors get the same
        // 16-keyword set.
        case "mixBlendMode":
            if (typeof setMixBlendMode === "function") setMixBlendMode(id, resolved);
            break;

        // direction — already supported by the case at line 534 above
        // (Tier 1 layout). Listed here as documentation that the catalog
        // logical-edge story routes through the same setDirection bridge.

        // font-variant — RN-style font-feature setting. Storage-only; the
        // HarfBuzz feature wiring is deferred. Catalog: partial.
        case "fontVariant":
            if (typeof setFontVariant === "function") setFontVariant(id, resolved);
            break;

        // ── Architecturally out-of-scope or no-op CSS surface entries ───
        // These cases exist purely so the harness sees them as `wired`
        // (case-arm present) and the catalog status rules the verdict.
        // They intentionally do NOT call a bridge fn because the
        // underlying capability is either out of scope (Pulp is 2D / no
        // scroll viewports / no z-index isolation) or not yet modeled.

        // 3D — Pulp's pipeline is 2D; perspective is silently accepted.
        case "perspective":
        case "perspectiveOrigin":
            // intentional no-op — see compat.json css/perspective entry.
            break;

        // Vertical writing — Pulp text flows horizontally only.
        case "writingMode":
            // intentional no-op — see compat.json css/writingMode.
            break;

        // Scroll-related CSS — Pulp doesn't render scroll viewports
        // through CSS (ScrollView intrinsic owns scroll state).
        case "scrollBehavior":
        case "scrollMargin":
        case "scrollPadding":
        case "scrollSnapType":
            // intentional no-op — see compat.json css/scroll* entries.
            break;

        // Stacking-context isolation — Pulp has no z-buffer or layer
        // isolation; entry exists for catalog completeness.
        case "isolation":
            // intentional no-op — see compat.json css/isolation.
            break;

        // textarea resize handle — Pulp doesn't render OS-style resize
        // handles. Stored for round-trip; no paint impact.
        case "resize":
            // intentional no-op — see compat.json css/resize.
            break;
    }
};

// Convert CSS flex alignment names to Pulp bridge names
function _cssToFlex(v) {
    if (v === "flex-start") return "start";
    if (v === "flex-end") return "end";
    if (v === "space-between") return "space-between";
    if (v === "space-around") return "space-around";
    if (v === "space-evenly") return "space-evenly";
    return v; // center, stretch pass through
}

// Define style property getters/setters via Proxy-like approach
// Since QuickJS supports Proxy, use it for the style object
// But for safety and compatibility, we use defineProperty on the prototype
var __cssProperties__ = [
    "display", "flexDirection", "flexWrap", "flexGrow", "flexShrink", "flexBasis", "flex",
    "flexFlow", "justifyContent", "alignItems", "alignSelf", "alignContent", "order",
    "placeItems", "placeContent",
    "gap", "rowGap", "columnGap",
    "width", "height", "minWidth", "minHeight", "maxWidth", "maxHeight",
    "aspectRatio", "boxSizing",
    "margin", "marginTop", "marginRight", "marginBottom", "marginLeft",
    "marginInline", "marginBlock",
    // pulp #1434 A4 Bundle 3 — CSS logical-edge longhands.
    "marginInlineStart", "marginInlineEnd", "marginBlockStart", "marginBlockEnd",
    "paddingInlineStart", "paddingInlineEnd", "paddingBlockStart", "paddingBlockEnd",
    // pulp #1434 batch 4 — React Native shorthand aliases.
    "marginHorizontal", "marginVertical",
    "padding", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
    "paddingInline", "paddingBlock",
    "paddingHorizontal", "paddingVertical",
    "backgroundColor", "color",
    "fontSize", "fontWeight", "fontStyle", "fontFamily", "letterSpacing", "lineHeight",
    "textAlign", "textTransform",
    // pulp #1434 (batch 3) — text-decoration shorthand + 3 longhands.
    "textDecoration", "textDecorationLine", "textDecorationColor", "textDecorationStyle",
    "textOverflow", "textShadow",
    "whiteSpace", "wordBreak", "overflowWrap", "wordWrap",
    "border", "borderColor", "borderWidth", "borderRadius",
    "borderTop", "borderRight", "borderBottom", "borderLeft",
    "borderTopWidth", "borderRightWidth", "borderBottomWidth", "borderLeftWidth",
    "borderTopColor", "borderRightColor", "borderBottomColor", "borderLeftColor",
    "borderTopLeftRadius", "borderTopRightRadius", "borderBottomLeftRadius", "borderBottomRightRadius",
    "outline", "outlineWidth", "outlineColor", "outlineOffset", "outlineStyle",
    "opacity", "overflow", "cursor", "visibility",
    // pulp #1434 A4 Bundle 4 — overflow per-axis (axis-tied gotcha).
    "overflowX", "overflowY",
    "userSelect", "pointerEvents",
    "transform", "transformOrigin",
    "transition", "transitionDuration",
    "animation", "animationName", "animationDuration", "animationTimingFunction",
    "animationDelay", "animationIterationCount", "animationDirection", "animationFillMode",
    // pulp #1434 A4 Bundle 2 — animation-play-state.
    "animationPlayState",
    "position", "top", "right", "bottom", "left", "zIndex", "inset",
    "boxShadow", "filter", "backdropFilter", "background", "backgroundImage",
    "backgroundSize", "backgroundPosition", "backgroundRepeat",
    // pulp #1517 — background sub-props (mostly noop / partial in pulp's
    // layout model; see _applyProperty for the per-prop semantics).
    "backgroundAttachment", "backgroundClip", "backgroundOrigin",
    "gridTemplateColumns", "gridTemplateRows", "gridColumn", "gridRow",
    "lineClamp", "webkitLineClamp",
    // pulp #1434 A4 Bundles 5–7 closure — text rendering tail, isolation,
    // clip/mask cluster, direction, resize, fontVariant.
    "textIndent", "verticalAlign", "writingMode",
    "scrollBehavior", "scrollMargin", "scrollPadding", "scrollSnapType",
    "isolation", "mixBlendMode", "clipPath", "mask", "maskImage", "direction",
    "resize", "fontVariant",
    "perspective", "perspectiveOrigin"
];

(function() {
    for (var i = 0; i < __cssProperties__.length; i++) {
        (function(prop) {
            Object.defineProperty(CSSStyleDeclaration.prototype, prop, {
                get: function() { return this._props[prop] || ""; },
                set: function(v) {
                    this._props[prop] = v;
                    this._applyProperty(prop, v);
                },
                enumerable: true, configurable: true
            });
        })(__cssProperties__[i]);
    }
})();

// setProperty / getPropertyValue for CSS variable support
CSSStyleDeclaration.prototype.setProperty = function(name, value) {
    // --custom-property -> set as theme token
    if (name.indexOf("--") === 0) {
        var tokenName = name.slice(2);
        var parsed = parseCSSLength(value);
        if (parsed) {
            setMotionToken(tokenName, parsed.value);
        } else {
            // Color token? Store as a theme color override
            var color = parseCSSColor(value);
            if (color) {
                // Use applyTokenDiff for color tokens
                applyTokenDiff('{"colors":{"' + tokenName + '":"' + color + '"}}');
            }
        }
    } else {
        // Convert CSS property name to camelCase
        var camel = name.replace(/-([a-z])/g, function(_, c) { return c.toUpperCase(); });
        this[camel] = value;
    }
};

CSSStyleDeclaration.prototype.getPropertyValue = function(name) {
    if (name.indexOf("--") === 0) {
        var tokenName = name.slice(2);
        return String(getMotionToken(tokenName));
    }
    var camel = name.replace(/-([a-z])/g, function(_, c) { return c.toUpperCase(); });
    return this._props[camel] || "";
};

CSSStyleDeclaration.prototype.removeProperty = function(name) {
    var camel = name.replace(/-([a-z])/g, function(_, c) { return c.toUpperCase(); });
    var old = this._props[camel] || "";
    delete this._props[camel];
    return old;
};
