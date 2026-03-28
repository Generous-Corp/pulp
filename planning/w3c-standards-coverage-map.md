# Pulp UI/Layout/Rendering — W3C Standards Coverage Map

**Date:** 2026-03-27
**Scope:** Exhaustive audit against all relevant W3C and web platform standards
**Methodology:** Every classification derived from actual source code examination

---

## Summary

- 45+ W3C spec areas evaluated
- Phase 1 (Critical): ~9 days — fixes broken behavior, unblocks design tool
- Phase 2 (DX): ~17 days — full JS-driven UI expressiveness  
- Phase 3 (Advanced): ~22 days — accessibility, advanced features, completeness
- Total to world-class: ~48 days

### Critical Bugs Found
1. justify_content is dead code — declared in FlexStyle, set by JS bridge, but layout_children() never reads it
2. overflow: visible is impossible — hard-coded clip_rect breaks ComboBox/Tooltip
3. No visual focus ring — accessibility failure
4. flex-wrap absent — any grid layout is impossible

### Correct Architectural Decisions Confirmed
- Token inheritance > CSS cascade (no selectors needed)
- ValueAnimation > CSS animations (frame-accurate, audio-thread-safe)
- Always-flex > block+flex (simpler, predictable)

### Challenges Validated
- "Flexbox covers 90% of Grid" → FALSE — minimal Grid needed
- "No absolute positioning" → overlay system must be implemented
- "Custom animation > CSS animations" → CORRECT but needs WAAPI controller handles

