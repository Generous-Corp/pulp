// ═══════════════════════════════════════════════════════════════════════════════
// CSSStyleDeclaration — paint domain handler (P5-5 split of _applyProperty)
// ═══════════════════════════════════════════════════════════════════════════════
//
// Handles the paint-surface CSS properties: colors, borders, opacity,
// box-shadow, filters, backgrounds, outline, visibility, clip-path /
// mask cluster, mix-blend-mode, isolation, appearance, object-fit.
// `_applyPaintProp(decl, id, key, resolved, value)` returns true if it
// claimed the key, false otherwise. Each `case` body is byte-identical
// to the matching arm of the pre-split `_applyProperty` switch.
//
// `decl` carries the CSSStyleDeclaration `this`; the scrollBehavior /
// isolation cases read `decl.__pulpId__`. Embed order: loaded AFTER
// web-compat-style-decl.js (the dispatcher).

function _applyPaintProp(decl, id, key, resolved, value) {
    switch (key) {
        // Colors
        case "backgroundColor": {
            var bgColor = parseCSSColor(resolved);
            if (bgColor) setBackground(id, bgColor);
            return true;
        }
        case "color": {
            var txtColor = parseCSSColor(resolved);
            if (txtColor) setTextColor(id, txtColor);
            return true;
        }

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
            var br = resolveCSSLength(resolved);
            if (br) setBorderRadius(id, br.value);
            return true;
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
            return true;
        }
        case "borderColor": {
            var bcc = parseCSSColor(resolved);
            if (bcc) setBorderColor(id, bcc);
            return true;
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
            if (bwResolved === "thin")   { setBorderWidth(id, 1); return true; }
            if (bwResolved === "medium") { setBorderWidth(id, 2); return true; }
            if (bwResolved === "thick")  { setBorderWidth(id, 4); return true; }
            var bw = resolveCSSLength(resolved);
            if (bw) setBorderWidth(id, bw.value);
            return true;
        }
        // pulp #1434 Triage #10 — borderStyle keyword passes verbatim to
        // setBorderStyle. The bridge maps to View::BorderStyle. Skia
        // installs SkDashPathEffect for dashed/dotted; other named
        // styles currently degrade to solid (paint-side gap).
        case "borderStyle": {
            if (typeof setBorderStyle !== "undefined") {
                setBorderStyle(id, resolved);
            }
            return true;
        }

        // ── Per-side borders ────────────────────────────────────────────
        case "borderTop": case "borderRight": case "borderBottom": case "borderLeft": {
            var side = key.replace("border", "").toLowerCase();
            var bsp = resolved.match(/([\d.]+)px\s+\w+\s+(.+)/);
            if (bsp) {
                var bsc = parseCSSColor(bsp[2].trim());
                if (typeof setBorderSide === "function")
                    setBorderSide(id, side, parseFloat(bsp[1]), bsc || bsp[2].trim());
            }
            return true;
        }
        // pulp #1027 (audit PR #1166 finding #4) — per-side flat props
        // (RN parity). Route to setBorderTop/Right/Bottom/Left{Color,Width}
        // which preserve the OTHER attribute on the View (see
        // applyBorderSide in widget_bridge.cpp). Calling setBorderSide
        // with a placeholder 0/"" for the unset slot would clobber it.
        case "borderTopWidth": {
            var bwT = resolveCSSLength(resolved);
            if (bwT && typeof setBorderTopWidth === "function") setBorderTopWidth(id, bwT.value);
            return true;
        }
        case "borderRightWidth": {
            var bwR = resolveCSSLength(resolved);
            if (bwR && typeof setBorderRightWidth === "function") setBorderRightWidth(id, bwR.value);
            return true;
        }
        case "borderBottomWidth": {
            var bwB = resolveCSSLength(resolved);
            if (bwB && typeof setBorderBottomWidth === "function") setBorderBottomWidth(id, bwB.value);
            return true;
        }
        case "borderLeftWidth": {
            var bwL = resolveCSSLength(resolved);
            if (bwL && typeof setBorderLeftWidth === "function") setBorderLeftWidth(id, bwL.value);
            return true;
        }
        case "borderTopColor": {
            var bcT = parseCSSColor(resolved);
            if (bcT && typeof setBorderTopColor === "function") setBorderTopColor(id, bcT);
            return true;
        }
        case "borderRightColor": {
            var bcR = parseCSSColor(resolved);
            if (bcR && typeof setBorderRightColor === "function") setBorderRightColor(id, bcR);
            return true;
        }
        case "borderBottomColor": {
            var bcB = parseCSSColor(resolved);
            if (bcB && typeof setBorderBottomColor === "function") setBorderBottomColor(id, bcB);
            return true;
        }
        case "borderLeftColor": {
            var bcL = parseCSSColor(resolved);
            if (bcL && typeof setBorderLeftColor === "function") setBorderLeftColor(id, bcL);
            return true;
        }

        // Per-corner border-radius
        case "borderTopLeftRadius": case "borderTopRightRadius":
        case "borderBottomLeftRadius": case "borderBottomRightRadius": {
            var corner = key.replace("border", "").replace("Radius", "");
            var cr = resolveCSSLength(resolved);
            if (cr && typeof setCornerRadius === "function")
                setCornerRadius(id, corner, cr.value);
            return true;
        }

        // Opacity
        case "opacity":
            setOpacity(id, parseFloat(resolved) || 0);
            return true;

        // Box shadow: "2px 4px 8px rgba(0,0,0,0.3)" or "inset 2px 4px 8px ..." (issue-925)
        case "boxShadow": {
            if (resolved === "none" || resolved === "" || resolved == null) {
                if (typeof clearBoxShadow === "function") clearBoxShadow(id);
                return true;
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
            return true;
        }

        // Filter
        case "filter":
            setFilter(id, resolved);
            return true;

        // pulp #1434 (batch 3) — backdrop-filter route. The bridge
        // setter is numeric (`setBackdropFilter(id, blur_px)`), so we
        // parse a `blur(Npx)` substring out of the CSS value. This
        // matches what `setFilter` already does on the bridge side
        // (see widget_bridge.cpp::setFilter — same blur-only surface).
        // Any other filter function is intentionally ignored here;
        // matching the `unsupportedValues: ["other filter functions"]`
        // entry in compat.json. `none` / empty / 0 clears the slot.
        case "backdropFilter": {
            if (typeof setBackdropFilter !== "function") return true;
            var bdf = String(resolved).trim().toLowerCase();
            if (bdf === "" || bdf === "none") {
                setBackdropFilter(id, 0);
                return true;
            }
            // Match `blur(Npx)` or `blur(N)` (treat unitless as px).
            var bdm = bdf.match(/blur\(\s*([\d.]+)\s*(px)?\s*\)/);
            if (bdm) {
                setBackdropFilter(id, parseFloat(bdm[1]) || 0);
            }
            return true;
        }

        // pulp #1515 — CSS `clip-path` cluster. The bridge only honors
        // the `path("...")` form (Skia parses via SkPath::FromSVGString
        // and installs the clip on paint). URL refs (`url(#id)`) and
        // named shape forms (`circle()`, `inset()`, `polygon()`,
        // `ellipse()`) are deferred — for those we set an empty slot
        // so the partial coverage is honest. `none` / empty clears.
        case "clipPath": {
            if (typeof setClipPath !== "function") return true;
            var cpv = String(resolved).trim();
            if (cpv === "" || cpv === "none") {
                setClipPath(id, "");
                return true;
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
            return true;
        }

        // pulp #1515 — CSS `mask-image`. Storage-only today; the
        // paint pipeline does not yet composite a shader mask onto a
        // saveLayer. Forwarding the value through to the bridge keeps
        // the slot round-trippable so harness tests can assert the
        // shim accepts the value, and so a future paint slice can
        // honor it without a JS-side change.
        case "maskImage": {
            if (typeof setMaskImage !== "function") return true;
            var miv = String(resolved).trim();
            if (miv === "none") miv = "";
            setMaskImage(id, miv);
            return true;
        }

        // pulp #1515 followup — `mask-size` pairs with mask-image.
        // Storage-only today; consumed by the future paint slice that
        // wires the mask shader onto the saveLayer.
        case "maskSize": {
            if (typeof setMaskSize !== "function") return true;
            setMaskSize(id, String(resolved).trim());
            return true;
        }

        // CSS `appearance`. Pulp paints all widgets custom (no native
        // form-widget rendering), so this is observably storage-only.
        // Authors who set `appearance: none` get the same paint behavior
        // they always had; the value round-trips through the View slot
        // for any tooling that inspects computed style.
        case "appearance":
        case "WebkitAppearance":
        case "MozAppearance": {
            if (typeof setAppearance !== "function") return true;
            setAppearance(id, String(resolved).trim());
            return true;
        }

        // CSS `object-fit` — controls fitting of <img> intrinsic
        // size into its layout box. Storage-only today; ImageView
        // paint-time consumption needs natural-size access (follow-up).
        case "objectFit": {
            if (typeof setObjectFit !== "function") return true;
            setObjectFit(id, String(resolved).trim());
            return true;
        }

        // CSS `object-position` — alignment of object-fit residual
        // space. Pairs with object-fit. Storage-only today.
        case "objectPosition": {
            if (typeof setObjectPosition !== "function") return true;
            setObjectPosition(id, String(resolved).trim());
            return true;
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
            return true;
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
            return true;
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
            return true;
        case "backgroundClip":
            if (typeof setBackgroundClip === "function") {
                setBackgroundClip(id, resolved);
            }
            return true;
        case "backgroundOrigin":
            if (typeof setBackgroundOrigin === "function") {
                setBackgroundOrigin(id, resolved);
            }
            return true;

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
            return true;
        }
        case "outlineWidth": {
            var ow = resolveCSSLength(resolved);
            if (ow) {
                if (typeof setOutlineWidth === "function") setOutlineWidth(id, ow.value);
                else if (typeof setOutline === "function") setOutline(id, ow.value, "");
            }
            return true;
        }
        case "outlineColor": {
            var occ = parseCSSColor(resolved);
            if (occ) {
                if (typeof setOutlineColor === "function") setOutlineColor(id, occ);
                else if (typeof setOutline === "function") setOutline(id, 0, occ);
            }
            return true;
        }
        // pulp #1519 — outline-offset / outline-style now have dedicated
        // bridge setters. Outline doesn't take Yoga layout space, so the
        // CSS path mirrors borderStyle keyword set verbatim.
        case "outlineOffset": {
            var oo = resolveCSSLength(resolved);
            if (oo && typeof setOutlineOffset === "function") setOutlineOffset(id, oo.value);
            return true;
        }
        case "outlineStyle": {
            if (typeof setOutlineStyle === "function") setOutlineStyle(id, resolved);
            return true;
        }

        // visibility: "hidden" vs display:none — hidden preserves layout space
        case "visibility":
            if (typeof setVisibility === "function") setVisibility(id, resolved);
            else if (resolved === "hidden") setOpacity(id, 0);
            else setOpacity(id, 1);
            return true;

        // background-size: "cover", "contain", "100px 200px"
        case "backgroundSize":
            if (typeof setBackgroundSize === "function") setBackgroundSize(id, resolved);
            return true;

        // background-position: "center", "top left", "50% 50%"
        case "backgroundPosition":
            if (typeof setBackgroundPosition === "function") setBackgroundPosition(id, resolved);
            return true;

        // background-repeat
        case "backgroundRepeat":
            if (typeof setBackgroundRepeat === "function") setBackgroundRepeat(id, resolved);
            return true;

        // mix-blend-mode — already wired via setMixBlendMode bridge fn
        // (#1549). Mirroring the RN surface so CSS authors get the same
        // 16-keyword set.
        case "mixBlendMode":
            if (typeof setMixBlendMode === "function") setMixBlendMode(id, resolved);
            return true;

        // pulp #1737 RN-OOS-fixup (final round) — CSS isolation honest
        // CSS-subset flip. Pulp's per-View save_layer_with_blend model
        // is structurally isolated by default (each blended View has
        // its own composition layer; z-index is paint-order scoped to
        // siblings within a parent), so the keyword round-trips to a
        // View slot for el.style reads while the paint pipeline's
        // existing per-View layering already provides the isolation
        // contract. See compat.json css/isolation notes.
        case "isolation":
            if (typeof __pulpBridge__?.setIsolation === "function") {
                __pulpBridge__.setIsolation(decl.__pulpId__, String(value || "auto"));
            }
            return true;

        default:
            return false;
    }
}
