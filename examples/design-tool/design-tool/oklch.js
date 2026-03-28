// OKLCH Color Engine for Pulp Style Designer
// Ported from ai-style-designer (MIT). Pure math, no dependencies.
// Provides: sRGB ↔ OKLab ↔ OKLCH conversion, gamut mapping,
// shade generation, color harmony, contrast checking.

var OklchEngine = {
    srgbToLinear: function(c) {
        var s = c / 255;
        return s <= 0.04045 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4);
    },

    linearToSrgb: function(c) {
        var s = c <= 0.0031308 ? 12.92 * c : 1.055 * Math.pow(c, 1 / 2.4) - 0.055;
        return Math.round(Math.max(0, Math.min(1, s)) * 255);
    },

    srgbToOklab: function(r, g, b) {
        var lr = this.srgbToLinear(r);
        var lg = this.srgbToLinear(g);
        var lb = this.srgbToLinear(b);

        var l = 0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb;
        var m = 0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb;
        var s = 0.0883024619 * lr + 0.2220049381 * lg + 0.6696926000 * lb;

        var l_ = Math.cbrt(l);
        var m_ = Math.cbrt(m);
        var s_ = Math.cbrt(s);

        return {
            L: 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
            a: 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
            b: 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_
        };
    },

    oklabToSrgb: function(L, a, b) {
        var l_ = L + 0.3963377774 * a + 0.2158037573 * b;
        var m_ = L - 0.1055613458 * a - 0.0638541728 * b;
        var s_ = L - 0.0894841775 * a - 1.2914855480 * b;

        var l = l_ * l_ * l_;
        var m = m_ * m_ * m_;
        var s = s_ * s_ * s_;

        return {
            r: this.linearToSrgb(+4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s),
            g: this.linearToSrgb(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
            b: this.linearToSrgb(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s)
        };
    },

    oklabToOklch: function(L, a, b) {
        var C = Math.sqrt(a * a + b * b);
        var H = Math.atan2(b, a) * 180 / Math.PI;
        if (H < 0) H += 360;
        return { L: L, C: C, H: H };
    },

    oklchToOklab: function(L, C, H) {
        var hRad = H * Math.PI / 180;
        return { L: L, a: C * Math.cos(hRad), b: C * Math.sin(hRad) };
    },

    srgbToOklch: function(r, g, b) {
        var lab = this.srgbToOklab(r, g, b);
        return this.oklabToOklch(lab.L, lab.a, lab.b);
    },

    oklchToSrgb: function(L, C, H) {
        var lab = this.oklchToOklab(L, C, H);
        return this.oklabToSrgb(lab.L, lab.a, lab.b);
    },

    hexToOklch: function(hex) {
        var h = hex.replace('#', '');
        var r = parseInt(h.slice(0, 2), 16);
        var g = parseInt(h.slice(2, 4), 16);
        var b = parseInt(h.slice(4, 6), 16);
        return this.srgbToOklch(r, g, b);
    },

    oklchToHex: function(L, C, H) {
        var rgb = this.oklchToSrgb(L, C, H);
        var toHex = function(v) {
            var s = Math.max(0, Math.min(255, v)).toString(16);
            return s.length < 2 ? '0' + s : s;
        };
        return '#' + toHex(rgb.r) + toHex(rgb.g) + toHex(rgb.b);
    },

    isInGamut: function(L, C, H) {
        var lab = this.oklchToOklab(L, C, H);
        var l_ = lab.L + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
        var m_ = lab.L - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
        var s_ = lab.L - 0.0894841775 * lab.a - 1.2914855480 * lab.b;
        var l = l_ * l_ * l_;
        var m = m_ * m_ * m_;
        var s = s_ * s_ * s_;
        var lr = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
        var lg = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
        var lb = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
        var eps = -0.002;
        return lr >= eps && lr <= 1.002 && lg >= eps && lg <= 1.002 && lb >= eps && lb <= 1.002;
    },

    gamutMap: function(L, C, H) {
        if (this.isInGamut(L, C, H)) return { L: L, C: C, H: H };
        var lo = 0, hi = C;
        for (var i = 0; i < 20; i++) {
            var mid = (lo + hi) / 2;
            if (this.isInGamut(L, mid, H)) lo = mid;
            else hi = mid;
        }
        return { L: L, C: lo, H: H };
    },

    relativeLuminance: function(hex) {
        var h = hex.replace('#', '');
        var r = this.srgbToLinear(parseInt(h.slice(0, 2), 16));
        var g = this.srgbToLinear(parseInt(h.slice(2, 4), 16));
        var b = this.srgbToLinear(parseInt(h.slice(4, 6), 16));
        return 0.2126 * r + 0.7152 * g + 0.0722 * b;
    },

    contrastRatio: function(hex1, hex2) {
        var l1 = this.relativeLuminance(hex1);
        var l2 = this.relativeLuminance(hex2);
        var lighter = Math.max(l1, l2);
        var darker = Math.min(l1, l2);
        return (lighter + 0.05) / (darker + 0.05);
    },

    contrastLevel: function(ratio) {
        if (ratio >= 7) return 'AAA';
        if (ratio >= 4.5) return 'AA';
        if (ratio >= 3) return ratio.toFixed(1) + ':1';
        return 'fail';
    }
};

// ── Color Harmony ────────────────────────────────────────────────

var ColorHarmony = {
    monochromatic: function(accentH) { return { H: accentH, C: 0.025 }; },
    analogous: function(accentH) { return { H: (accentH + 30) % 360, C: 0.025 }; },
    complementary: function(accentH) { return { H: (accentH + 180) % 360, C: 0.025 }; },
    splitComplementary: function(accentH) { return { H: (accentH + 150) % 360, C: 0.025 }; },
    none: function() { return { H: 0, C: 0 }; },

    deriveNeutral: function(accentH, mode) {
        var fn = this[mode || 'monochromatic'];
        if (!fn) return { L: 0.35, C: 0, H: 0 };
        var result = fn(accentH);
        return { L: 0.35, C: result.C, H: result.H };
    }
};

// ── Shade Ramp Generator ─────────────────────────────────────────

var ShadeGenerator = {
    LIGHTNESS_TARGETS: {
        50: 0.97, 100: 0.95, 200: 0.90, 300: 0.85,
        400: 0.75, 500: 0.60, 600: 0.50, 700: 0.40,
        800: 0.30, 900: 0.20, 950: 0.13
    },
    STEPS: [50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 950],

    generateRamp: function(baseL, baseC, baseH) {
        var shades = {};
        for (var i = 0; i < this.STEPS.length; i++) {
            var step = this.STEPS[i];
            var targetL = this.LIGHTNESS_TARGETS[step];
            var chromaScale = Math.max(0, 1 - Math.abs(targetL - baseL) * 0.5);
            var C = baseC * chromaScale;
            var mapped = OklchEngine.gamutMap(targetL, C, baseH);
            shades[step] = {
                L: mapped.L, C: mapped.C, H: mapped.H,
                hex: OklchEngine.oklchToHex(mapped.L, mapped.C, mapped.H)
            };
        }
        return shades;
    }
};

// ── Palette System ───────────────────────────────────────────────
// Creates a complete color system from a single accent color

var PaletteSystem = {
    create: function(accentHex, harmonyMode) {
        var accent = OklchEngine.hexToOklch(accentHex);
        var neutral = ColorHarmony.deriveNeutral(accent.H, harmonyMode || 'monochromatic');

        return {
            accent: ShadeGenerator.generateRamp(accent.L, accent.C, accent.H),
            neutral: ShadeGenerator.generateRamp(neutral.L, neutral.C, neutral.H),
            success: ShadeGenerator.generateRamp(0.55, 0.15, 145),
            warning: ShadeGenerator.generateRamp(0.70, 0.15, 85),
            error: ShadeGenerator.generateRamp(0.55, 0.20, 25)
        };
    },

    // Generate a Pulp theme from a palette system (dark mode)
    toThemeDiff: function(palette) {
        return JSON.stringify({
            colors: {
                "bg.primary": palette.neutral[950].hex,
                "bg.secondary": palette.neutral[900].hex,
                "bg.surface": palette.neutral[800].hex,
                "bg.elevated": palette.neutral[700].hex,
                "text.primary": palette.neutral[50].hex,
                "text.secondary": palette.neutral[300].hex,
                "text.disabled": palette.neutral[600].hex,
                "accent.primary": palette.accent[500].hex,
                "accent.secondary": palette.accent[300].hex,
                "accent.success": palette.success[500].hex,
                "accent.warning": palette.warning[500].hex,
                "accent.error": palette.error[500].hex,
                "control.track": palette.neutral[700].hex,
                "control.fill": palette.accent[500].hex,
                "control.thumb": palette.neutral[100].hex,
                "control.border": palette.neutral[600].hex
            }
        });
    }
};
