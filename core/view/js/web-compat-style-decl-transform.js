// ═══════════════════════════════════════════════════════════════════════════════
// CSSStyleDeclaration — transform domain handler (P5-5 split of _applyProperty)
// ═══════════════════════════════════════════════════════════════════════════════
//
// Handles the transform / transition / animation CSS properties.
// `_applyTransformProp(decl, id, key, resolved, value)` returns true if
// it claimed the key, false otherwise. Each `case` body is byte-
// identical to the matching arm of the pre-split `_applyProperty`
// switch. Embed order: loaded AFTER web-compat-style-decl.js.

function _applyTransformProp(decl, id, key, resolved, value) {
    switch (key) {
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
            return true;
        }
        case "transformOrigin": {
            // "center", "left top", "50% 50%", "10px 20px"
            var ox = 0.5, oy = 0.5;
            var op = resolved.split(/\s+/);
            function _parseOrigin(v) {
                if (v === "center") return 0.5;
                if (v === "left" || v === "top") return 0;
                if (v === "right" || v === "bottom") return 1;
                var l = resolveCSSLength(v);
                if (l && l.unit === "%") return l.value / 100;
                return 0.5;
            }
            ox = _parseOrigin(op[0] || "center");
            oy = _parseOrigin(op[1] || op[0] || "center");
            setTransformOrigin(id, ox, oy);
            return true;
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
            return true;
        }
        case "transitionDuration": {
            var td = parseFloat(resolved);
            if (resolved.indexOf("ms") >= 0) td /= 1000;
            setTransitionDuration(id, td);
            return true;
        }
        case "transitionProperty": {
            if (typeof setTransitionProperty !== "undefined") {
                setTransitionProperty(id, resolved);
            }
            return true;
        }
        case "transitionTimingFunction": {
            if (typeof setTransitionTimingFunction !== "undefined") {
                setTransitionTimingFunction(id, resolved);
            }
            return true;
        }
        case "transitionDelay": {
            var dly = parseFloat(resolved);
            if (resolved.indexOf("ms") >= 0) dly /= 1000;
            if (typeof setTransitionDelay !== "undefined") {
                setTransitionDelay(id, dly);
            }
            return true;
        }

        // ── Animation properties ────────────────────────────────────────
        case "animationName":
            if (typeof setAnimation === "function") setAnimation(id, "name", resolved);
            return true;
        case "animationDuration": {
            var ad = parseFloat(resolved);
            if (resolved.indexOf("ms") >= 0) ad /= 1000;
            if (typeof setAnimation === "function") setAnimation(id, "duration", ad);
            return true;
        }
        case "animationTimingFunction":
            if (typeof setAnimation === "function") setAnimation(id, "easing", resolved);
            return true;
        case "animationDelay": {
            var adl = parseFloat(resolved);
            if (resolved.indexOf("ms") >= 0) adl /= 1000;
            if (typeof setAnimation === "function") setAnimation(id, "delay", adl);
            return true;
        }
        case "animationIterationCount":
            if (typeof setAnimation === "function")
                setAnimation(id, "iterations", resolved === "infinite" ? -1 : parseFloat(resolved) || 1);
            return true;
        case "animationDirection":
            if (typeof setAnimation === "function") setAnimation(id, "direction", resolved);
            return true;
        case "animationFillMode":
            if (typeof setAnimation === "function") setAnimation(id, "fill", resolved);
            return true;
        // pulp #1434 A4 Bundle 2 — animation-play-state. Forwards the
        // CSS keyword (`running` | `paused`) through the existing
        // setAnimation control-token ABI so the bridge can route it to
        // the staged_animation slot. The full pause/resume of the
        // active_animations playback driver is the follow-up; storing
        // the keyword today is enough for the catalog to claim partial
        // and for round-trip validation.
        case "animationPlayState":
            if (typeof setAnimation === "function") setAnimation(id, "play_state", resolved);
            return true;
        case "animation": {
            // Shorthand: "name duration easing delay iterations direction fill"
            var atr = parseTransition(resolved); // reuse transition parser for timing
            if (typeof setAnimation === "function") {
                setAnimation(id, "name", atr.property);
                setAnimation(id, "duration", atr.duration);
                setAnimation(id, "easing", atr.easing);
                setAnimation(id, "delay", atr.delay);
            }
            return true;
        }

        default:
            return false;
    }
}
