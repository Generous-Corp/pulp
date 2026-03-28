# Phase 3: Platform Maturity Parity

> See full document in task notification output. Key findings below.

## Critical Path to v1
1. **Windows and Linux platform layers** (windowing + input via SDL3 or native)
2. **Accessibility implementation** (at minimum macOS VoiceOver)
3. **IME composition** (marked text support)
4. **Cursor management** (connect existing enum to platform APIs)
5. **Visual regression CI** (connect headless rendering to automated comparison)

## Cross-Platform Readiness

| Area | macOS | Windows | Linux | iOS |
|------|-------|---------|-------|-----|
| Window creation | ✅ | 🔲 | 🔲 | ✅ |
| GPU rendering | ✅ | ⚠️ (SDL3) | ⚠️ (SDL3) | ✅ |
| Input (mouse/keyboard) | ✅ | 🔲 | 🔲 | ⚠️ |
| IME | ⚠️ | 🔲 | 🔲 | 🔲 |
| Clipboard | ✅ | needs-verify | needs-verify | N/A |
| Accessibility | 🔲 | 🔲 | 🔲 | 🔲 |
| Plugin hosting | ✅ | ⚠️ | ⚠️ | ✅ |
| Signing/packaging | ✅ | ⚠️ | ⚠️ | N/A |

## Risk Assessment
- **Critical:** Windows platform layer, Accessibility
- **High:** IME, Linux platform layer, Cursor management
- **Medium:** Layout performance, Visual regression CI, Tab focus, Context menus
- **Low:** RTL text, System theme detection, Remote debugging
