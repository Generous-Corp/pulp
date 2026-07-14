"""HTML rendering for desktop automation publish reports.

Optimized for a reviewer skimming proofs (often on mobile Safari): each run
leads with a verdict + the visual proof above the fold, keeps the
"what to verify" notes compact, and tucks secondary metadata + the review
storyboard into a collapsible block. Self-contained, offline-friendly: all
styling is inline so the report opens correctly from ``file://``.
"""

from __future__ import annotations

import html
import json
from pathlib import Path


def _esc(value: object) -> str:
    return html.escape(str(value))


def _pill(text: str, *, kind: str = "") -> str:
    cls = "pill" + (f" pill-{kind}" if kind else "")
    return f"<span class=\"{cls}\">{_esc(text)}</span>"


def _thumb(title: str, rel_path: str) -> str:
    href = _esc(rel_path)
    return (
        "<figure class=\"thumb\">"
        + f"<a href=\"{href}\" target=\"_blank\" rel=\"noopener\">"
        + f"<img src=\"{href}\" alt=\"{_esc(title)}\" loading=\"lazy\" /></a>"
        + f"<figcaption>{_esc(title)}</figcaption>"
        + "</figure>"
    )


def _run_status(artifacts: dict) -> tuple[str, str]:
    """Verdict shown first in each run header so a reviewer can triage fast."""
    image_change = artifacts.get("image_change")
    if isinstance(image_change, dict) and "changed" in image_change:
        return ("changed", "changed") if image_change.get("changed") else ("clean", "clean")
    return ("captured", "captured")


def _run_pills(run: dict, artifacts: dict) -> list[str]:
    """Secondary metadata chips. Each chip's text stays ``key: value`` so the
    rendered HTML carries the literal substring downstream consumers grep for."""
    # Lazy import avoids a reporting_publish <-> reporting_video import cycle.
    from reporting_video import (
        _proof_action_marker_summary,
        _proof_focus_summary,
        _proof_context_items,
        _source_summary,
    )

    proof = run.get("video_proof_composition") if isinstance(run.get("video_proof_composition"), dict) else {}
    focus = _proof_focus_summary(proof)
    action = _proof_action_marker_summary(proof)
    context_items = _proof_context_items(proof)

    pills: list[str] = []
    if run.get("interaction_mode"):
        pills.append(_pill(f"interaction: {run['interaction_mode']}", kind="info"))
    if proof.get("template"):
        pills.append(_pill(f"template: {proof['template']}", kind="accent"))
    source_label = proof.get("source_label") or _source_summary(
        run.get("source") if isinstance(run.get("source"), dict) else {}
    )
    if source_label:
        pills.append(_pill(f"source: {source_label}"))
    if run.get("adapter"):
        pills.append(_pill(f"adapter: {run['adapter']}"))
    if run.get("host"):
        pills.append(_pill(f"host: {run['host']}"))
    if focus.get("label"):
        pills.append(_pill(f"focus: {focus['label']}", kind="focus"))
    if action:
        action_label = action.get("label") or action.get("kind") or "action"
        pills.append(_pill(f"action: {action_label}", kind="action"))
    if artifacts.get("image_change"):
        pills.append(_pill(f"image_change: {json.dumps(artifacts['image_change'], sort_keys=True)}"))
    for key, value in context_items[:6]:
        pills.append(_pill(f"context.{key}: {value}"))
    return pills


def _storyboard_html(artifacts: dict, *, publish_dir: Path | None) -> str:
    if publish_dir is None:
        return ""
    from reporting_video import _artifact_metadata, _proof_storyboard_from_metadata

    composed_metadata = _artifact_metadata(publish_dir, artifacts.get("video_composed_metadata"))
    storyboard = _proof_storyboard_from_metadata(composed_metadata)
    steps = storyboard.get("steps") if isinstance(storyboard, dict) else None
    if not steps:
        return ""
    items: list[str] = []
    for step in steps[:8]:
        label = _esc(step.get("label") or "")
        detail = step.get("detail")
        detail_html = f"<span class=\"step-detail\">{_esc(detail)}</span>" if detail else ""
        items.append(f"<li><span class=\"step-label\">{label}</span>{detail_html}</li>")
    title = _esc(storyboard.get("title") or "Review storyboard")
    return f"<div class=\"storyboard\"><h3>{title}</h3><ol class=\"timeline\">{''.join(items)}</ol></div>"


def _notes_html(run: dict) -> str:
    notes = run.get("video_proof_notes") if isinstance(run.get("video_proof_notes"), list) else []
    notes = [str(note).strip() for note in notes if str(note).strip()]
    if not notes:
        return ""
    return "<ul class=\"notes\">" + "".join(f"<li>{_esc(note)}</li>" for note in notes[:5]) + "</ul>"


def _video_html(artifacts: dict) -> str:
    video = artifacts.get("video_composed") or artifacts.get("video")
    if not video:
        return ""
    poster = artifacts.get("video_poster")
    poster_attr = f" poster=\"{_esc(poster)}\"" if poster else ""
    return (
        "<video class=\"proof-video\" controls preload=\"metadata\""
        f"{poster_attr} src=\"{_esc(video)}\"></video>"
    )


def _thumbs_html(artifacts: dict) -> str:
    figures: list[str] = []
    for title, key in (
        ("before", "before_screenshot"),
        ("after", "screenshot"),
        ("diff", "diff_screenshot"),
        ("source", "video_source_image"),
        ("diff ref", "video_diff_image"),
    ):
        rel = artifacts.get(key)
        if rel:
            figures.append(_thumb(title, str(rel)))
    if not figures:
        return ""
    return "<div class=\"strip\">" + "".join(figures) + "</div>"


def _details_html(run: dict, artifacts: dict, *, publish_dir: Path | None) -> str:
    pills = _run_pills(run, artifacts)
    storyboard = _storyboard_html(artifacts, publish_dir=publish_dir)
    meta = (
        artifacts.get("video_issue_metadata")
        or artifacts.get("video_composed_metadata")
        or artifacts.get("video_metadata")
    )
    meta_link = f"<a class=\"meta-link\" href=\"{_esc(meta)}\">video metadata</a>" if meta else ""
    body = (f"<div class=\"pills\">{''.join(pills)}</div>" if pills else "") + storyboard + meta_link
    if not body:
        return ""
    return f"<details class=\"more\"><summary>Details</summary>{body}</details>"


def desktop_publish_run_card_html(run: dict, *, publish_dir: Path | None = None, anchor: str | None = None) -> str:
    artifacts = run.get("artifacts") or {}
    status_kind, status_label = _run_status(artifacts)
    target = _esc(run.get("target") or "?")
    action = _esc(run.get("action") or "?")
    label = _esc(run.get("label") or "?")
    completed = run.get("completed_at")
    time_html = f"<time>{_esc(completed)}</time>" if completed else ""
    anchor_attr = f" id=\"{_esc(anchor)}\"" if anchor else ""
    return (
        f"<section class=\"run-card\"{anchor_attr}>"
        "<header class=\"card-head\">"
        f"<span class=\"status status-{status_kind}\">{_esc(status_label)}</span>"
        f"<h2>{target}/{action}</h2>"
        f"<span class=\"card-label\">{label}</span>"
        + time_html
        + "</header>"
        + _notes_html(run)
        + _video_html(artifacts)
        + _thumbs_html(artifacts)
        + _details_html(run, artifacts, publish_dir=publish_dir)
        + "</section>"
    )


_STYLE = (
    "*{box-sizing:border-box}"
    "html{-webkit-text-size-adjust:100%}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;"
    "padding:20px 16px 48px;color:#e6eaf2;line-height:1.45;overflow-x:hidden;background:#0b0f17}"
    "img,video{max-width:100%}"
    "a{color:#6ea8fe}"
    ".wrap{max-width:920px;margin:0 auto}"
    ".page{margin:0 0 16px}"
    ".page h1{margin:0;font-size:20px;font-weight:650;color:#f3f6fc;letter-spacing:-.01em}"
    ".sub{margin:5px 0 0;color:#8a94a6;font-size:13px}"
    ".sub b{color:#cdd5e3;font-weight:600}"
    ".sub .c-changed{color:#f5b25a}.sub .c-clean{color:#5fd0a0}"
    ".nav{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px}"
    ".nav a{font-size:11px;text-decoration:none;color:#aeb9cc;padding:3px 9px;border:1px solid #243049;"
    "border-radius:999px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}"
    ".nav a:hover{border-color:#3a4a6b;color:#dbe3f0}"
    ".run-card{border:1px solid #1e2738;border-radius:10px;padding:14px 16px;margin:0 0 14px;background:#121826;"
    "scroll-margin-top:14px}"
    ".card-head{display:flex;flex-wrap:wrap;align-items:baseline;gap:6px 10px}"
    ".status{font-size:10px;font-weight:700;text-transform:uppercase;letter-spacing:.06em;padding:3px 8px;"
    "border-radius:999px;align-self:center}"
    ".status-changed{background:#3a2a0c;color:#f5b25a;border:1px solid #6b4d18}"
    ".status-clean{background:#0d2a1d;color:#5fd0a0;border:1px solid #1c5a40}"
    ".status-captured{background:#12233a;color:#7aa0c4;border:1px solid #294568}"
    ".card-head h2{margin:0;font-size:15px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;color:#f3f6fc}"
    ".card-label{color:#aeb9cc;font-size:13.5px;flex:1 1 auto;min-width:0}"
    ".card-head time{color:#6b7686;font-size:11px;font-variant-numeric:tabular-nums}"
    ".notes{margin:9px 0 0;padding-left:18px;color:#c7d0de;font-size:13px}.notes li{margin:2px 0}"
    ".proof-video{display:block;width:100%;max-height:260px;object-fit:contain;margin-top:12px;"
    "border-radius:8px;border:1px solid #1e2738;background:#000}"
    ".strip{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:8px;margin-top:10px}"
    ".thumb{margin:0}.thumb img{display:block;width:100%;height:108px;object-fit:cover;border-radius:8px;"
    "border:1px solid #1e2738;background:#000;transition:border-color .15s ease}"
    ".thumb a:hover img{border-color:#3a4a6b}"
    ".thumb figcaption{margin-top:4px;color:#8a94a6;font-size:11px;text-transform:capitalize}"
    ".more{margin-top:10px;border-top:1px solid #1b2434;padding-top:8px}"
    ".more>summary{cursor:pointer;font-size:12px;color:#8a94a6;list-style:revert}"
    ".more>summary:hover{color:#6ea8fe}"
    ".pills{display:flex;flex-wrap:wrap;gap:7px;margin-top:10px}"
    ".pill{display:inline-flex;align-items:center;padding:3px 9px;border-radius:999px;font-size:11px;max-width:100%;"
    "overflow-wrap:anywhere;white-space:normal;border:1px solid #25324a;background:#161f30;color:#b7c2d3;"
    "font-family:ui-monospace,SFMono-Regular,Menlo,monospace}"
    ".pill-info{border-color:#1e3a5f;background:#0f2740;color:#7dd3fc}"
    ".pill-accent{border-color:#3f3a8a;background:#1c1b3a;color:#c7d2fe}"
    ".pill-focus{border-color:#6b4d18;background:#2c2208;color:#fcd97a}"
    ".pill-action{border-color:#6b2444;background:#2c0d1c;color:#f7b8d2}"
    ".storyboard h3{margin:12px 0 6px;font-size:11px;text-transform:uppercase;letter-spacing:.07em;color:#8a94a6}"
    ".timeline{list-style:none;margin:0;padding:0;counter-reset:step}"
    ".timeline li{position:relative;padding:0 0 10px 28px;counter-increment:step}"
    ".timeline li::before{content:counter(step);position:absolute;left:0;top:0;width:19px;height:19px;display:flex;"
    "align-items:center;justify-content:center;border-radius:50%;font-size:10px;font-weight:600;color:#0b0f17;background:#6ea8fe}"
    ".timeline li:not(:last-child)::after{content:'';position:absolute;left:9px;top:21px;bottom:0;width:2px;background:#1e2738}"
    ".step-label{font-weight:600;color:#e6eaf2;font-size:13px}.step-detail{display:block;color:#9aa6b8;font-size:12.5px}"
    ".meta-link{display:inline-block;margin-top:10px;font-size:12px}"
    "@media (max-width:640px){"
    "body{padding:14px 12px 36px}.page h1{font-size:18px}"
    ".run-card{padding:12px 13px}"
    ".proof-video{max-height:210px}"
    ".strip{grid-template-columns:repeat(auto-fill,minmax(96px,1fr));gap:6px}.thumb img{height:84px}"
    "}"
)


def desktop_publish_index_html(index_payload: dict, *, publish_dir: Path | None = None) -> str:
    published_runs = index_payload.get("runs") or []
    changed = sum(1 for run in published_runs if _run_status((run.get("artifacts") or {}))[0] == "changed")
    clean = sum(1 for run in published_runs if _run_status((run.get("artifacts") or {}))[0] == "clean")
    cards: list[str] = []
    nav: list[str] = []
    for i, run in enumerate(published_runs, start=1):
        anchor = f"run-{i}"
        cards.append(desktop_publish_run_card_html(run, publish_dir=publish_dir, anchor=anchor))
        nav.append(f"<a href=\"#{anchor}\">{_esc(run.get('target') or '?')}/{_esc(run.get('action') or '?')}</a>")
    sub = [
        f"Generated {_esc(index_payload['generated_at'])}",
        f"&middot; runs: {len(published_runs)}",
    ]
    if changed:
        sub.append(f"&middot; <b class=\"c-changed\">{changed} changed</b>")
    if clean:
        sub.append(f"&middot; <b class=\"c-clean\">{clean} clean</b>")
    nav_html = f"<nav class=\"nav\">{''.join(nav)}</nav>" if nav else ""
    return (
        "\n".join(
            [
                "<!doctype html>",
                "<html lang=\"en\"><head><meta charset=\"utf-8\">",
                "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">",
                "<title>Pulp Desktop Automation Report</title>",
                f"<style>{_STYLE}</style>",
                "</head><body><div class=\"wrap\">",
                "<header class=\"page\">",
                f"<h1>{_esc(index_payload['label'])}</h1>",
                f"<p class=\"sub\">{' '.join(sub)}</p>",
                nav_html,
                "</header>",
                *cards,
                "</div></body></html>",
            ]
        )
        + "\n"
    )
