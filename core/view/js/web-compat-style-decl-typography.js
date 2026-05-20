// ═══════════════════════════════════════════════════════════════════════════════
// CSSStyleDeclaration — typography domain handler (P5-5 split of _applyProperty)
// ═══════════════════════════════════════════════════════════════════════════════
//
// Handles the text / font CSS properties: font-size / weight / style /
// family, letter-spacing, line-height, text-align / transform /
// decoration / overflow / shadow / indent, white-space, word-break,
// list-style cluster, vertical-align, font-variant.
// `_applyTypographyProp(decl, id, key, resolved, value)` returns true
// if it claimed the key, false otherwise. Each `case` body is byte-
// identical to the matching arm of the pre-split `_applyProperty`
// switch. Embed order: loaded AFTER web-compat-style-decl.js.

function _applyTypographyProp(decl, id, key, resolved, value) {
    switch (key) {
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
                return true;
            }
            if (fsResolved === "larger") {
                setFontSize(id, inherited * 1.2);
                return true;
            }
            var fs = resolveCSSLength(resolved);
            if (!fs) return true;
            if (fs.unit === "em" || fs.unit === "rem") {
                setFontSize(id, fs.value * inherited);
            } else if (fs.unit === "%") {
                setFontSize(id, fs.value / 100 * inherited);
            } else {
                setFontSize(id, fs.value);
            }
            return true;
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
            return true;
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
            return true;
        case "letterSpacing": {
            // pulp Wave 2 css.2 — `normal` keyword + `em` unit.
            //   • normal → 0 (CSS spec — no extra spacing)
            //   • em     → resolved against the same default font-size as
            //              fontSize above (14px).
            var lsResolved = String(resolved).trim().toLowerCase();
            if (lsResolved === "normal") {
                setLetterSpacing(id, 0);
                return true;
            }
            var ls = resolveCSSLength(resolved);
            if (!ls) return true;
            if (ls.unit === "em" || ls.unit === "rem") {
                setLetterSpacing(id, ls.value * 14);
            } else {
                setLetterSpacing(id, ls.value);
            }
            return true;
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
                return true;
            }
            // Unitless number (no `px` / `em` / `%` suffix). parseCSSLength
            // treats bare numbers as px, so we have to detect this case
            // before falling through — a value of `1.5` should multiply,
            // not be set as 1.5 px.
            if (/^-?[\d.]+$/.test(lhResolved)) {
                setLineHeight(id, parseFloat(lhResolved) * 14);
                return true;
            }
            var lh = resolveCSSLength(resolved);
            if (!lh) return true;
            if (lh.unit === "em" || lh.unit === "rem") {
                setLineHeight(id, lh.value * 14);
            } else if (lh.unit === "%") {
                setLineHeight(id, lh.value / 100 * 14);
            } else {
                setLineHeight(id, lh.value);
            }
            return true;
        }
        case "textAlign":
            setTextAlign(id, resolved);
            return true;
        case "textTransform":
            setTextTransform(id, resolved);
            return true;
        case "textDecoration":
            setTextDecoration(id, resolved);
            return true;
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
            return true;
        case "textDecorationColor": {
            var tdc = parseCSSColor(resolved);
            if (tdc && typeof setTextDecorationColor === "function")
                setTextDecorationColor(id, tdc);
            return true;
        }
        case "textDecorationStyle":
            if (typeof setTextDecorationStyle === "function")
                setTextDecorationStyle(id, resolved);
            return true;
        case "textOverflow":
            setTextOverflow(id, resolved);
            return true;

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
            var lsTypes = {
                "none": 1, "disc": 1, "circle": 1, "square": 1, "decimal": 1,
                // Counter-style keywords (pulp #1514). Storage-only on the
                // bridge today; the shorthand parser still needs to route
                // them to setListStyleType so the View round-trips honestly.
                "decimal-leading-zero": 1,
                "lower-roman": 1, "upper-roman": 1,
                "lower-alpha": 1, "upper-alpha": 1,
                "lower-latin": 1, "upper-latin": 1,
                "lower-greek": 1, "armenian": 1, "georgian": 1,
            };
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
            return true;
        }
        case "listStyleType": {
            if (typeof setListStyleType !== "undefined") setListStyleType(id, resolved);
            return true;
        }
        case "listStyleImage": {
            if (typeof setListStyleImage !== "undefined") setListStyleImage(id, resolved);
            return true;
        }
        case "listStylePosition": {
            if (typeof setListStylePosition !== "undefined") setListStylePosition(id, resolved);
            return true;
        }

        // white-space: "nowrap", "pre", "normal"
        case "whiteSpace":
            if (typeof setWhiteSpace === "function") setWhiteSpace(id, resolved);
            return true;

        // word-break / overflow-wrap
        case "wordBreak":
        case "overflowWrap":
        case "wordWrap":
            if (typeof setWordBreak === "function") setWordBreak(id, resolved);
            return true;

        // text-shadow: "2px 2px 4px rgba(0,0,0,0.5)"
        case "textShadow": {
            var tsm = resolved.match(/(-?[\d.]+)px\s+(-?[\d.]+)px\s+([\d.]+)px\s+(.*)/);
            if (tsm && typeof setTextShadow === "function") {
                var tsc = parseCSSColor(tsm[4].trim());
                setTextShadow(id, parseFloat(tsm[1]), parseFloat(tsm[2]), parseFloat(tsm[3]), tsc || tsm[4].trim());
            }
            return true;
        }

        // font-family — pulp #1151, font v2 Slice 1.1.a
        // CSS font-family is a comma-separated fallback list, e.g.
        //   font-family: 'JetBrains Mono', ui-monospace, SFMono-Regular, monospace;
        //
        // Pre-Slice 1.1.a: the bridge split the list and passed only
        // the FIRST family to setFontFamily. The remaining fallback
        // names never reached the C++ side, so an author who wrote
        // `'IBM Plex Mono', monospace` lost the `monospace` safety net
        // and saw silent tofu when IBM Plex Mono wasn't installed.
        //
        // Post-Slice 1.1.a: pass the entire raw comma-list to
        // setFontFamily verbatim. The C++ side (skia_canvas /
        // text_shaper / sdf_atlas, now all routed through
        // FontResolver) splits the list once and walks it cascade-style
        // — registered → bundled → platform per family in order,
        // emitting a FallbackTrace for every step. The full intent of
        // the author's CSS font-family stack reaches the resolver.
        //
        // No JS-side trimming or quote-stripping is necessary anymore;
        // the resolver handles both. Passing the raw string preserves
        // exact CSS author intent for the trace records.
        case "fontFamily":
            if (typeof setFontFamily === "function") {
                setFontFamily(id, String(resolved));
            }
            return true;

        // text-indent — first-line indent. Storage-only; SkParagraph
        // setTextIndent integration is the follow-up.
        case "textIndent": {
            var ti = resolveCSSLength(resolved);
            if (ti && typeof setTextIndent === "function") setTextIndent(id, ti.value);
            return true;
        }

        // vertical-align — line-box vertical alignment. Maps the four
        // canvas::TextVerticalAlign slots (top|middle|bottom|baseline);
        // sub/super and length values fall back to baseline.
        case "verticalAlign":
            if (typeof setVerticalAlign === "function") setVerticalAlign(id, resolved);
            return true;

        // font-variant — RN-style font-feature setting. Storage-only; the
        // HarfBuzz feature wiring is deferred. Catalog: partial.
        case "fontVariant":
            if (typeof setFontVariant === "function") setFontVariant(id, resolved);
            return true;

        default:
            return false;
    }
}
