---
name: Pulp — Ink & Signal
description: Dark-first design system for Pulp audio/creative software. Cool graphite neutrals, luminous screenprint inks, geometric type, springy motion. Primary is signal teal; coral is reserved for peaking & danger.
defaultMode: dark
modes: [dark, light]
colors:
  ink:
    signal: "#16DAC2"
    signal-deep: "#10B6A3"
    signal-bright: "#46F0DB"
    violet: "#8B6CF5"
    violet-deep: "#6F4DE8"
    indigo: "#5E78FF"
    indigo-deep: "#4760E6"
    amber: "#F6B847"
    amber-deep: "#E09E2A"
    leaf: "#3FCF77"
    pink: "#FF7AA8"
    coral: "#FF5C4D"
    coral-deep: "#F23B33"
    on-ink: "#052320"
  stone:
    50: "#F5F7F8"
    100: "#EBEEF1"
    150: "#E1E5E9"
    200: "#D0D6DD"
    300: "#B5BDC7"
    400: "#9099A5"
    500: "#6C7682"
    600: "#505964"
    700: "#39414A"
    800: "#272C33"
    850: "#1E232A"
    900: "#161A20"
    950: "#0F1217"
    990: "#0A0C10"
  dark:
    surface:
      app: "#161A21"
      sunken: "#0E1116"
      panel: "#1E2530"
      raised: "#28303C"
      overlay: "#2F3743"
      inset: "#0C0F14"
      hover: "rgba(225,235,250,0.06)"
      active: "rgba(225,235,250,0.10)"
    line:
      soft: "rgba(220,232,250,0.07)"
      base: "rgba(220,232,250,0.12)"
      strong: "rgba(220,232,250,0.22)"
    text:
      strong: "#F3F6F9"
      base: "#D6DCE4"
      muted: "#939CA9"
      faint: "#646D7A"
      inverse: "#161A21"
    accent:
      base: "#16DAC2"
      press: "#10B6A3"
      text: "#052320"
      soft: "rgba(22,218,194,0.15)"
      soft-2: "rgba(22,218,194,0.26)"
      line: "rgba(22,218,194,0.45)"
      ring: "rgba(22,218,194,0.5)"
    control:
      base: "#2A323D"
      hover: "#323B47"
      press: "#222932"
      line: "rgba(220,230,245,0.14)"
    knob:
      hi: "#424B58"
      base: "#2C333E"
      lo: "#181C23"
      rim: "rgba(0,0,0,0.55)"
    status:
      info: "#5E78FF"
      info-soft: "rgba(94,120,255,0.16)"
      success: "#3FCF77"
      success-soft: "rgba(63,207,119,0.16)"
      warning: "#F6B847"
      warning-soft: "rgba(246,184,71,0.18)"
      danger: "#FF5C4D"
      danger-soft: "rgba(255,92,77,0.16)"
      text: "#052320"
    shadow:
      color: "rgba(0,0,0,0.5)"
      color-2: "rgba(0,0,0,0.34)"
      highlight-top: "rgba(255,255,255,0.09)"
  light:
    surface:
      app: "#EDEFF2"
      sunken: "#E0E4E9"
      panel: "#FAFBFC"
      raised: "#FFFFFF"
      overlay: "#FFFFFF"
      inset: "#E6E9EE"
      hover: "rgba(16,20,28,0.04)"
      active: "rgba(16,20,28,0.07)"
    line:
      soft: "rgba(20,28,40,0.06)"
      base: "rgba(20,28,40,0.12)"
      strong: "rgba(20,28,40,0.22)"
    text:
      strong: "#14171C"
      base: "#2C313A"
      muted: "#5A636F"
      faint: "#939BA8"
      inverse: "#FAFBFC"
    accent:
      base: "#10B6A3"
      press: "#0C9A8A"
      text: "#042420"
      soft: "rgba(16,182,163,0.15)"
      soft-2: "rgba(16,182,163,0.26)"
      line: "rgba(16,182,163,0.5)"
      ring: "rgba(16,182,163,0.45)"
    control:
      base: "#FFFFFF"
      hover: "#F6F8FA"
      press: "#ECEFF2"
      line: "rgba(20,28,40,0.14)"
    knob:
      hi: "#FFFFFF"
      base: "#EDF0F4"
      lo: "#D6DCE4"
      rim: "rgba(20,28,40,0.16)"
    status:
      info: "#4760E6"
      info-soft: "rgba(71,96,230,0.14)"
      success: "#2BA85E"
      success-soft: "rgba(43,168,94,0.14)"
      warning: "#E09E2A"
      warning-soft: "rgba(224,158,42,0.2)"
      danger: "#F23B33"
      danger-soft: "rgba(242,59,51,0.14)"
      text: "#FFFFFF"
    shadow:
      color: "rgba(20,28,45,0.16)"
      color-2: "rgba(20,28,45,0.10)"
      highlight-top: "rgba(255,255,255,0.9)"
  data:
    1: "#16DAC2"
    2: "#8B6CF5"
    3: "#F6B847"
    4: "#5E78FF"
    5: "#FF7AA8"
    6: "#3FCF77"
  signal:
    low: "#16DAC2"
    mid: "#F6B847"
    high: "#FF5C4D"
    wave: "#16DAC2"
typography:
  fontFamily:
    display: "'Jost', 'Futura', 'Century Gothic', system-ui, sans-serif"
    sans: "'Jost', system-ui, -apple-system, 'Segoe UI', sans-serif"
    mono: "'JetBrains Mono', ui-monospace, 'SF Mono', Menlo, monospace"
  fontSize:
    2xs: "0.6875rem"
    xs: "0.75rem"
    sm: "0.8125rem"
    base: "0.9375rem"
    md: "1rem"
    lg: "1.125rem"
    xl: "1.375rem"
    2xl: "1.75rem"
    3xl: "2.25rem"
    4xl: "3rem"
    5xl: "4rem"
    6xl: "5.25rem"
  fontWeight:
    regular: 400
    medium: 500
    semibold: 600
    bold: 700
    display: 800
  lineHeight:
    tight: 1.05
    snug: 1.2
    normal: 1.45
    relaxed: 1.6
  letterSpacing:
    display: "-0.02em"
    tight: "-0.01em"
    normal: "0em"
    label: "0.12em"
    mono: "0.01em"
spacing:
  px: "1px"
  0: "0"
  0-5: "2px"
  1: "4px"
  1-5: "6px"
  2: "8px"
  2-5: "10px"
  3: "12px"
  4: "16px"
  5: "20px"
  6: "24px"
  7: "28px"
  8: "32px"
  10: "40px"
  12: "48px"
  14: "56px"
  16: "64px"
  20: "80px"
  24: "96px"
  32: "128px"
rounded:
  xs: "4px"
  sm: "6px"
  md: "10px"
  lg: "14px"
  xl: "20px"
  2xl: "28px"
  pill: "999px"
shadows:
  xs: "0 1px 2px rgba(0,0,0,0.34)"
  sm: "0 2px 6px rgba(0,0,0,0.34), 0 1px 2px rgba(0,0,0,0.34)"
  md: "0 6px 18px rgba(0,0,0,0.5), 0 2px 5px rgba(0,0,0,0.34)"
  lg: "0 16px 40px rgba(0,0,0,0.5), 0 4px 12px rgba(0,0,0,0.34)"
  xl: "0 30px 70px rgba(0,0,0,0.5), 0 10px 24px rgba(0,0,0,0.5)"
  elev-control: "inset 0 1px 0 rgba(255,255,255,0.09), 0 1px 2px rgba(0,0,0,0.34)"
  elev-inset: "inset 0 1px 3px rgba(0,0,0,0.5), inset 0 0 0 1px rgba(220,232,250,0.07)"
  focus-ring: "0 0 0 3px rgba(22,218,194,0.5)"
  focus-ring-offset: "0 0 0 2px #161A21, 0 0 0 4px rgba(22,218,194,0.5)"
  glow-sm: "0 0 10px rgba(22,218,194,0.45)"
  glow-md: "0 0 20px rgba(22,218,194,0.55)"
  glow-lg: "0 0 32px rgba(22,218,194,0.60)"
components:
  control:
    height:
      sm: "28px"
      md: "36px"
      lg: "44px"
  border:
    width:
      hair: "1px"
      soft: "1px"
      strong: "1px"
    style:
      hair: "1px solid rgba(220,232,250,0.12)"
      soft: "1px solid rgba(220,232,250,0.07)"
      strong: "1px solid rgba(220,232,250,0.22)"
  radius:
    button: "10px"
    input: "10px"
    chip: "10px"
    row: "14px"
    card: "20px"
    panel: "20px"
    dialog: "20px"
    shell: "28px"
    knob: "999px"
    toggle: "999px"
  motion:
    easing:
      spring: "cubic-bezier(.34, 1.56, .64, 1)"
      out: "cubic-bezier(.22, 1, .36, 1)"
      in-out: "cubic-bezier(.65, 0, .35, 1)"
      linear: "linear"
    duration:
      1: "80ms"
      2: "140ms"
      3: "220ms"
      4: "320ms"
      5: "480ms"
---

# Pulp — Ink & Signal

Design-system spec (tokens only). Dark-first: `:root` is the dark studio theme;
`[data-theme="light"]` is the paper twin. Components consume semantic tokens only,
never the raw ramps. Coral/red is reserved for peaking and danger — it never
carries a neutral action.

## Colors

### Dark Mode

| name | value |
| --- | --- |
| ink.signal | #16DAC2 |
| ink.signal-deep | #10B6A3 |
| ink.signal-bright | #46F0DB |
| ink.violet | #8B6CF5 |
| ink.violet-deep | #6F4DE8 |
| ink.indigo | #5E78FF |
| ink.indigo-deep | #4760E6 |
| ink.amber | #F6B847 |
| ink.amber-deep | #E09E2A |
| ink.leaf | #3FCF77 |
| ink.pink | #FF7AA8 |
| ink.coral | #FF5C4D |
| ink.coral-deep | #F23B33 |
| on-ink | #052320 |
| accent | #16DAC2 |
| accent-press | #10B6A3 |
| accent-text | #052320 |
| accent-soft | rgba(22,218,194,0.15) |
| accent-soft-2 | rgba(22,218,194,0.26) |
| accent-line | rgba(22,218,194,0.45) |
| accent-ring | rgba(22,218,194,0.5) |
| surface-app | #161A21 |
| surface-sunken | #0E1116 |
| surface-panel | #1E2530 |
| surface-raised | #28303C |
| surface-overlay | #2F3743 |
| surface-inset | #0C0F14 |
| surface-hover | rgba(225,235,250,0.06) |
| surface-active | rgba(225,235,250,0.10) |
| line-soft | rgba(220,232,250,0.07) |
| line | rgba(220,232,250,0.12) |
| line-strong | rgba(220,232,250,0.22) |
| text-strong | #F3F6F9 |
| text | #D6DCE4 |
| text-muted | #939CA9 |
| text-faint | #646D7A |
| text-inverse | #161A21 |
| control | #2A323D |
| control-hover | #323B47 |
| control-press | #222932 |
| control-line | rgba(220,230,245,0.14) |
| knob-hi | #424B58 |
| knob-base | #2C333E |
| knob-lo | #181C23 |
| knob-rim | rgba(0,0,0,0.55) |
| info | #5E78FF |
| info-soft | rgba(94,120,255,0.16) |
| success | #3FCF77 |
| success-soft | rgba(63,207,119,0.16) |
| warning | #F6B847 |
| warning-soft | rgba(246,184,71,0.18) |
| danger | #FF5C4D |
| danger-soft | rgba(255,92,77,0.16) |
| shadow-color | rgba(0,0,0,0.5) |
| shadow-color-2 | rgba(0,0,0,0.34) |
| highlight-top | rgba(255,255,255,0.09) |
| data-1 | #16DAC2 |
| data-2 | #8B6CF5 |
| data-3 | #F6B847 |
| data-4 | #5E78FF |
| data-5 | #FF7AA8 |
| data-6 | #3FCF77 |
| signal-low | #16DAC2 |
| signal-mid | #F6B847 |
| signal-high | #FF5C4D |
| signal-wave | #16DAC2 |
| stone-50 | #F5F7F8 |
| stone-100 | #EBEEF1 |
| stone-150 | #E1E5E9 |
| stone-200 | #D0D6DD |
| stone-300 | #B5BDC7 |
| stone-400 | #9099A5 |
| stone-500 | #6C7682 |
| stone-600 | #505964 |
| stone-700 | #39414A |
| stone-800 | #272C33 |
| stone-850 | #1E232A |
| stone-900 | #161A20 |
| stone-950 | #0F1217 |
| stone-990 | #0A0C10 |

### Light Mode

| name | value |
| --- | --- |
| accent | #10B6A3 |
| accent-press | #0C9A8A |
| accent-text | #042420 |
| accent-soft | rgba(16,182,163,0.15) |
| accent-soft-2 | rgba(16,182,163,0.26) |
| accent-line | rgba(16,182,163,0.5) |
| accent-ring | rgba(16,182,163,0.45) |
| surface-app | #EDEFF2 |
| surface-sunken | #E0E4E9 |
| surface-panel | #FAFBFC |
| surface-raised | #FFFFFF |
| surface-overlay | #FFFFFF |
| surface-inset | #E6E9EE |
| surface-hover | rgba(16,20,28,0.04) |
| surface-active | rgba(16,20,28,0.07) |
| line-soft | rgba(20,28,40,0.06) |
| line | rgba(20,28,40,0.12) |
| line-strong | rgba(20,28,40,0.22) |
| text-strong | #14171C |
| text | #2C313A |
| text-muted | #5A636F |
| text-faint | #939BA8 |
| text-inverse | #FAFBFC |
| control | #FFFFFF |
| control-hover | #F6F8FA |
| control-press | #ECEFF2 |
| control-line | rgba(20,28,40,0.14) |
| knob-hi | #FFFFFF |
| knob-base | #EDF0F4 |
| knob-lo | #D6DCE4 |
| knob-rim | rgba(20,28,40,0.16) |
| info | #4760E6 |
| info-soft | rgba(71,96,230,0.14) |
| success | #2BA85E |
| success-soft | rgba(43,168,94,0.14) |
| warning | #E09E2A |
| warning-soft | rgba(224,158,42,0.2) |
| danger | #F23B33 |
| danger-soft | rgba(242,59,51,0.14) |
| shadow-color | rgba(20,28,45,0.16) |
| shadow-color-2 | rgba(20,28,45,0.10) |
| highlight-top | rgba(255,255,255,0.9) |

## Typography

- font-display: 'Jost', 'Futura', 'Century Gothic', system-ui, sans-serif
- font-sans: 'Jost', system-ui, -apple-system, 'Segoe UI', sans-serif
- font-mono: 'JetBrains Mono', ui-monospace, 'SF Mono', Menlo, monospace
- text-2xs: 0.6875rem
- text-xs: 0.75rem
- text-sm: 0.8125rem
- text-base: 0.9375rem
- text-md: 1rem
- text-lg: 1.125rem
- text-xl: 1.375rem
- text-2xl: 1.75rem
- text-3xl: 2.25rem
- text-4xl: 3rem
- text-5xl: 4rem
- text-6xl: 5.25rem
- weight-regular: 400
- weight-medium: 500
- weight-semibold: 600
- weight-bold: 700
- weight-display: 800
- leading-tight: 1.05
- leading-snug: 1.2
- leading-normal: 1.45
- leading-relaxed: 1.6
- tracking-display: -0.02em
- tracking-tight: -0.01em
- tracking-normal: 0em
- tracking-label: 0.12em
- tracking-mono: 0.01em

## Spacing

| name | value |
| --- | --- |
| space-px | 1px |
| space-0 | 0 |
| space-0-5 | 2px |
| space-1 | 4px |
| space-1-5 | 6px |
| space-2 | 8px |
| space-2-5 | 10px |
| space-3 | 12px |
| space-4 | 16px |
| space-5 | 20px |
| space-6 | 24px |
| space-7 | 28px |
| space-8 | 32px |
| space-10 | 40px |
| space-12 | 48px |
| space-14 | 56px |
| space-16 | 64px |
| space-20 | 80px |
| space-24 | 96px |
| space-32 | 128px |
| control-h-sm | 28px |
| control-h-md | 36px |
| control-h-lg | 44px |

## Border Radius

| name | value |
| --- | --- |
| radius-xs | 4px |
| radius-sm | 6px |
| radius-md | 10px |
| radius-lg | 14px |
| radius-xl | 20px |
| radius-2xl | 28px |
| radius-pill | 999px |

## Shadows

- shadow-xs: 0 1px 2px rgba(0,0,0,0.34)
- shadow-sm: 0 2px 6px rgba(0,0,0,0.34), 0 1px 2px rgba(0,0,0,0.34)
- shadow-md: 0 6px 18px rgba(0,0,0,0.5), 0 2px 5px rgba(0,0,0,0.34)
- shadow-lg: 0 16px 40px rgba(0,0,0,0.5), 0 4px 12px rgba(0,0,0,0.34)
- shadow-xl: 0 30px 70px rgba(0,0,0,0.5), 0 10px 24px rgba(0,0,0,0.5)
- elev-control: inset 0 1px 0 rgba(255,255,255,0.09), 0 1px 2px rgba(0,0,0,0.34)
- elev-inset: inset 0 1px 3px rgba(0,0,0,0.5), inset 0 0 0 1px rgba(220,232,250,0.07)
- focus-ring: 0 0 0 3px rgba(22,218,194,0.5)
- focus-ring-offset: 0 0 0 2px #161A21, 0 0 0 4px rgba(22,218,194,0.5)
- glow-sm: 0 0 10px rgba(22,218,194,0.45)
- glow-md: 0 0 20px rgba(22,218,194,0.55)
- glow-lg: 0 0 32px rgba(22,218,194,0.60)

## Borders

- border-hair: 1px solid rgba(220,232,250,0.12)
- border-soft: 1px solid rgba(220,232,250,0.07)
- border-strong: 1px solid rgba(220,232,250,0.22)

## Motion

- ease-spring: cubic-bezier(.34, 1.56, .64, 1)
- ease-out: cubic-bezier(.22, 1, .36, 1)
- ease-in-out: cubic-bezier(.65, 0, .35, 1)
- ease-linear: linear
- dur-1: 80ms
- dur-2: 140ms
- dur-3: 220ms
- dur-4: 320ms
- dur-5: 480ms
