# Review Depth

Every non-trivial PR gets the pre-PR closeout described in `CLAUDE.md`: an
inline architecture review and an adversarial review. How much more review a
change gets depends on how much can go wrong.

| Change | Review |
|--------|--------|
| Every PR | Inline architecture review plus adversarial review |
| A complex or fragile PR (ABI or layout, lifetime or threading, security, release or signing, gate or guard changes, large refactors) | Add a fresh adversarial review by a separate reviewer |
| A finished phase or stack | One adversarial review of the whole stack before it lands |
| A major milestone, such as completing a migration | The thermo-nuclear review |

## Calibration

Adjust the depth by measured yield. If a tier keeps finding nothing, relax
it. If defects keep escaping past it, deepen it.

For the design rules a reviewer checks new public headers against, see
[API and Header Design](api-design.md).
