"""Form-control support boundary for the differential lab.

Chromium paints a form control from CSS or from its native theme, and computed
`appearance` alone does not say which. With `appearance: none` the control is
an ordinary box. With any other appearance the theme paints it UNLESS the author
styled the box: a button or progress bar with an author background, border, or
border radius, or a text field or textarea with any of those or a box-shadow,
is painted from CSS while its computed appearance still reads `auto`. A select
in that state keeps a theme-drawn arrow; checkboxes, radios, ranges and other
picker controls keep the theme whatever the author sets. Because the rule keys
on who declared a property, the capture records the verdict from Chromium's
matched-rule origins (`formControlTheme` in the DOM snapshot), and this module
reads it rather than re-deriving it from computed values.

Where the theme really paints, those pixels belong to the browser's own theme
code, so Pulp's renderer cannot reproduce them from the capture without copying
that appearance. This module states that boundary per control and attributes
the visual residual to it. It never removes a fixture from a denominator or
alters a score; it only says which part of a score is the theme.
"""

from __future__ import annotations

from typing import Any

FORM_CONTROL_TAGS = frozenset({
    "input", "button", "select", "textarea", "meter", "progress"})

# Input types whose `appearance: none` rendering still contains parts drawn in a
# user-agent shadow tree the snapshot does not serialize: a range's thumb, a
# colour swatch, a file chooser's button, the fields of a date or time picker.
SHADOW_PART_INPUT_TYPES = frozenset({
    "range", "color", "file", "date", "datetime-local", "month", "time",
    "week"})

UA_DEFAULT_FORM_CHROME = "ua-default-form-chrome"
UA_SHADOW_FORM_PART = "ua-shadow-form-part"
AUTHOR_STYLED = "author-styled"
APPEARANCE_UNRECORDED = "appearance-unrecorded"
THEME_UNRESOLVED = "theme-unresolved"

BOUNDARY_STATUSES = frozenset({UA_DEFAULT_FORM_CHROME, UA_SHADOW_FORM_PART})


def control_status(tag: str, input_type: str, styles: dict[str, str],
                   theme: str | None = None,
                   theme_recorded: bool = False) -> str:
    """Classify one laid-out form control.

    `theme` is the capture's verdict for this control (`none`, `native`,
    `author`, `author-arrow`, or None when it could not be resolved), and
    `theme_recorded` whether the capture carried verdicts at all.
    """
    appearance = styles.get("appearance")
    if appearance is None:
        return APPEARANCE_UNRECORDED
    if appearance.strip().lower() == "none":
        if tag == "input" and input_type in SHADOW_PART_INPUT_TYPES:
            return UA_SHADOW_FORM_PART
        return AUTHOR_STYLED
    # Computed appearance is not `none`; only the verdict says whether the
    # theme actually painted. Without one the answer is unknown, not a
    # boundary.
    if not theme_recorded or theme is None:
        return THEME_UNRESOLVED
    if theme == "native":
        return UA_DEFAULT_FORM_CHROME
    if theme == "author-arrow":
        return UA_SHADOW_FORM_PART
    if tag == "input" and input_type in SHADOW_PART_INPUT_TYPES:
        return UA_SHADOW_FORM_PART
    return AUTHOR_STYLED


def theme_verdicts(snapshot: dict[str, Any] | None) -> dict[int, str | None] | None:
    """Node index -> the capture's theme verdict, or None for a capture that
    predates the verdict."""
    report = (snapshot or {}).get("formControlTheme")
    if not isinstance(report, dict):
        return None
    verdicts: dict[int, str | None] = {}
    for control in report.get("controls", []):
        node = control.get("node_index")
        if isinstance(node, int):
            verdicts[node] = control.get("theme")
    return verdicts


def form_controls(browser_flat: list[Any],
                  snapshot: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    """Every laid-out, visible form control with its support status.

    `browser_flat` is the lab's `BrowserNode` list, so hidden and zero-area
    controls are already gone. Bounds are page-space CSS pixels. `snapshot` is
    the DOM snapshot carrying the capture's theme verdicts.
    """
    verdicts = theme_verdicts(snapshot)
    result = []
    for node in browser_flat:
        if node.tag not in FORM_CONTROL_TAGS or node.bounds is None:
            continue
        input_type = (node.attributes.get("type") or "").strip().lower()
        if node.tag == "input" and not input_type:
            input_type = "text"
        if node.tag == "input" and input_type == "hidden":
            continue
        theme = verdicts.get(node.index) if verdicts is not None else None
        result.append({
            "browser_node": node.index,
            "tag": node.tag,
            "type": input_type if node.tag == "input" else None,
            "appearance": node.styles.get("appearance"),
            "theme": theme,
            "status": control_status(node.tag, input_type, node.styles,
                                     theme, verdicts is not None),
            "bounds": list(node.bounds),
        })
    return result


def attribute_residual(
    controls: list[dict[str, Any]],
    reference: Any,
    candidate: Any,
    origin: tuple[float, float],
    scale: float,
) -> dict[str, Any]:
    """Share of the foreground error that lands inside each control's box.

    `reference` and `candidate` are the same-size RGBA images the visual gate
    scored, `origin` the page-space CSS position of their top-left pixel, and
    `scale` the device pixel ratio. Foreground and error use the visual gate's
    own definitions, so the shares are fractions of the error that produced
    the score rather than of a different measurement.
    """
    from collections import Counter

    width, height = reference.size
    ref = list(reference.get_flattened_data())
    cand = list(candidate.get_flattened_data())
    ref_bg = Counter(ref).most_common(1)[0][0]
    cand_bg = Counter(cand).most_common(1)[0][0]

    def box_px(bounds: list[float]) -> tuple[int, int, int, int]:
        # One CSS pixel of margin keeps a control's anti-aliased edge with it.
        left = int((bounds[0] - origin[0] - 1.0) * scale)
        top = int((bounds[1] - origin[1] - 1.0) * scale)
        right = int(round((bounds[0] + bounds[2] - origin[0] + 1.0) * scale))
        bottom = int(round((bounds[1] + bounds[3] - origin[1] + 1.0) * scale))
        return (max(0, left), max(0, top), min(width, right), min(height, bottom))

    owner: dict[int, int] = {}
    for position, control in enumerate(controls):
        left, top, right, bottom = box_px(control["bounds"])
        for y in range(top, bottom):
            row = y * width
            for x in range(left, right):
                owner.setdefault(row + x, position)

    total = 0.0
    per_control = [0.0] * len(controls)
    for offset, (ref_px, cand_px) in enumerate(zip(ref, cand)):
        ref_fg = sum(abs(a - b) for a, b in zip(ref_px[:3], ref_bg[:3])) > 18
        cand_fg = sum(abs(a - b) for a, b in zip(cand_px[:3], cand_bg[:3])) > 18
        if not (ref_fg or cand_fg):
            continue
        delta = sum(abs(a - b) for a, b in zip(ref_px[:3], cand_px[:3]))
        total += delta
        position = owner.get(offset)
        if position is not None:
            per_control[position] += delta
    rows = []
    for control, error in zip(controls, per_control):
        rows.append({
            **control,
            "foreground_error_share": round(error / total, 6) if total else 0.0,
        })
    boundary_error = sum(
        error for control, error in zip(controls, per_control)
        if control["status"] in BOUNDARY_STATUSES)
    return {
        "controls": rows,
        "foreground_error_total": round(total, 3),
        "boundary_foreground_error_share": (
            round(boundary_error / total, 6) if total else 0.0),
    }


def support_boundaries(attribution: dict[str, Any]) -> list[dict[str, Any]]:
    """The boundaries this fixture crosses, each with its residual share."""
    boundaries = []
    for status in (UA_DEFAULT_FORM_CHROME, UA_SHADOW_FORM_PART):
        rows = [row for row in attribution["controls"] if row["status"] == status]
        if not rows:
            continue
        boundaries.append({
            "boundary": status,
            "control_count": len(rows),
            "controls": sorted(
                f"{row['tag']}[type={row['type']}]" if row["type"] else row["tag"]
                for row in rows),
            "foreground_error_share": round(
                sum(row["foreground_error_share"] for row in rows), 6),
        })
    return boundaries
