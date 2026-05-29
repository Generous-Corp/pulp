#!/usr/bin/env python3
"""Build a route-manifest seed from a static HTML/CSS fixture."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
from collections import Counter
from html.parser import HTMLParser
from typing import Any
from frontend_ir_common import write_json


CSS_RULE_RE = re.compile(r"([^{}]+)\{([^{}]*)\}", re.DOTALL)
DECL_RE = re.compile(r"([A-Za-z_-][A-Za-z0-9_-]*)\s*:\s*([^;]+)")
SKIP_TAGS = {"html", "head", "meta", "title", "link", "script", "style"}
TEXT_TAGS = {"a", "em", "label", "p", "small", "span", "strong", "h1", "h2", "h3", "h4", "h5", "h6"}
LAYOUT_TAGS = {"article", "aside", "body", "div", "footer", "header", "main", "nav", "section"}
VECTOR_TAGS = {"svg", "path", "circle", "line", "polyline", "polygon", "rect"}


def read_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def read_bytes(path: pathlib.Path) -> bytes:
    return path.read_bytes()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def repo_relative(path: pathlib.Path, repo_root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def declarations(style_text: str) -> list[tuple[str, str]]:
    return [(match.group(1).strip(), match.group(2).strip()) for match in DECL_RE.finditer(style_text)]


def primitive_for_tag(tag: str, attrs: dict[str, str]) -> str:
    if tag == "button":
        return "button"
    if tag == "input":
        input_type = attrs.get("type", "text")
        if input_type == "range":
            return "fader"
        if input_type in {"checkbox", "radio"}:
            return "toggle"
        return "text_editor"
    if tag == "textarea":
        return "text_editor"
    if tag == "select":
        return "select"
    if tag == "img":
        return "image"
    if tag == "meter":
        return "meter"
    if tag in VECTOR_TAGS:
        return "vector"
    if tag in TEXT_TAGS:
        return "text"
    if tag in LAYOUT_TAGS:
        return "layout"
    return "element"


def is_local_reference(value: str) -> bool:
    return bool(value) and not re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", value) and not value.startswith("#")


def local_reference_path(reference: str, base_dir: pathlib.Path) -> pathlib.Path | None:
    if not is_local_reference(reference):
        return None
    candidate = (base_dir / reference).resolve()
    return candidate if candidate.exists() else None


class StaticHtmlAnalyzer(HTMLParser):
    def __init__(self, source_path: str) -> None:
        super().__init__(convert_charrefs=True)
        self.source_path = source_path
        self.rows: list[dict[str, Any]] = []
        self.tag_counts: Counter[str] = Counter()
        self.class_names: Counter[str] = Counter()
        self.style_keys: Counter[str] = Counter()
        self.inline_style_attributes = 0
        self.inline_style_values = 0
        self.svg_vector_nodes = 0
        self.stylesheet_refs: list[str] = []
        self.asset_refs: list[str] = []
        self._tag_ordinals: Counter[str] = Counter()
        self._in_style = False
        self._style_text: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attrs_dict = {key: value or "" for key, value in attrs}
        if tag == "style":
            self._in_style = True
            return
        if tag == "link":
            rel = {item.strip().lower() for item in attrs_dict.get("rel", "").split()}
            href = attrs_dict.get("href", "")
            if "stylesheet" in rel and href:
                self.stylesheet_refs.append(href)
            return
        if tag in SKIP_TAGS:
            return

        line, _ = self.getpos()
        ordinal = self._tag_ordinals[tag]
        self._tag_ordinals[tag] += 1
        self.tag_counts[tag] += 1

        classes = [name for name in attrs_dict.get("class", "").split() if name]
        for class_name in classes:
            self.class_names[class_name] += 1

        inline_decls = declarations(attrs_dict.get("style", ""))
        if inline_decls:
            self.inline_style_attributes += 1
            self.inline_style_values += len(inline_decls)
            for prop, _ in inline_decls:
                self.style_keys[prop] += 1

        if tag in VECTOR_TAGS:
            self.svg_vector_nodes += 1
        if tag == "img" and attrs_dict.get("src"):
            self.asset_refs.append(attrs_dict["src"])

        row: dict[str, Any] = {
            "id": f"html.{len(self.rows)}.{tag}",
            "stable_source_path": f"{self.source_path}:{line}:{tag}[{ordinal}]",
            "source_component_family": tag,
            "source_component_name": tag,
            "source_line": line,
            "route_type": "native_html",
            "required_native_primitive": primitive_for_tag(tag, attrs_dict),
            "confidence": 0.72,
        }
        if classes:
            row["class_names"] = classes
        if inline_decls:
            row["style_properties"] = [prop for prop, _ in inline_decls]
        self.rows.append(row)

    def handle_endtag(self, tag: str) -> None:
        if tag == "style":
            self._in_style = False

    def handle_data(self, data: str) -> None:
        if self._in_style:
            self._style_text.append(data)

    @property
    def style_text(self) -> str:
        return "\n".join(self._style_text)


def css_summary(style_texts: list[str]) -> tuple[int, int, Counter[str]]:
    css_rules = 0
    css_values = 0
    keys: Counter[str] = Counter()
    for style_text in style_texts:
        for rule in CSS_RULE_RE.finditer(style_text):
            css_rules += 1
            for prop, _ in declarations(rule.group(2)):
                css_values += 1
                keys[prop] += 1
    return css_rules, css_values, keys


def artifact_for_path(path: pathlib.Path, repo_root: pathlib.Path) -> dict[str, str]:
    data = read_bytes(path)
    return {
        "path": repo_relative(path, repo_root),
        "sha256": sha256_bytes(data),
    }


def build_route_manifest(source_path: pathlib.Path, repo_root: pathlib.Path, fixture: str) -> dict[str, Any]:
    source_text = read_text(source_path)
    source_rel = repo_relative(source_path, repo_root)
    analyzer = StaticHtmlAnalyzer(source_rel)
    analyzer.feed(source_text)

    stylesheet_paths = [
        path for ref in analyzer.stylesheet_refs
        if (path := local_reference_path(ref, source_path.parent)) is not None
    ]
    asset_paths = [
        path for ref in analyzer.asset_refs
        if (path := local_reference_path(ref, source_path.parent)) is not None
    ]
    stylesheet_texts = [analyzer.style_text] + [read_text(path) for path in stylesheet_paths]
    css_rules, css_values, css_keys = css_summary(stylesheet_texts)
    style_keys = sorted(set(analyzer.style_keys) | set(css_keys))

    source_summary = {
        "schema": "pulp-source-audit-summary-v1",
        "lines": len(source_text.splitlines()),
        "bytes": len(source_text.encode("utf-8")),
        "input": {
            "bytes": len(source_text.encode("utf-8")),
        },
        "summary": {
            "html_elements": len(analyzer.rows),
            "class_attributes": sum(1 for row in analyzer.rows if row.get("class_names")),
            "class_names": sum(analyzer.class_names.values()),
            "css_rules": css_rules,
            "css_values": css_values + analyzer.inline_style_values,
            "css_values_valid": css_values + analyzer.inline_style_values,
            "css_values_invalid": 0,
            "style_attributes": analyzer.inline_style_attributes,
            "inline_style_attributes": analyzer.inline_style_attributes,
            "inline_style_values": analyzer.inline_style_values,
            "svg_vector_nodes": analyzer.svg_vector_nodes,
            "stylesheet_links": len(analyzer.stylesheet_refs),
            "local_stylesheet_resources": len(stylesheet_paths),
            "image_assets": len(analyzer.asset_refs),
            "local_image_resources": len(asset_paths),
            "component_counts": dict(sorted(analyzer.tag_counts.items())),
        },
        "styleKeys": style_keys,
    }

    inputs: dict[str, Any] = {
        "sourceHtml": artifact_for_path(source_path, repo_root),
        "sourceAuditSummary": source_summary,
    }
    if stylesheet_paths:
        inputs["styleSheets"] = [artifact_for_path(path, repo_root) for path in stylesheet_paths]
    if asset_paths:
        inputs["assets"] = [artifact_for_path(path, repo_root) for path in asset_paths]

    return {
        "schema": "pulp-static-html-route-manifest-v1",
        "fixture": fixture,
        "inputs": inputs,
        "scope": {
            "native_html_probe": True,
            "renderer_proof": "not_started",
        },
        "component_family_coverage": {
            "source_component_names_total": len(analyzer.tag_counts),
            "source_component_names_classified": len(analyzer.tag_counts),
            "expanded_route_rows": len(analyzer.rows),
        },
        "route_metrics": {
            "nodes_total": len(analyzer.rows),
            "native_html_candidate_node_routes": len(analyzer.rows),
            "js_engine_initialized": False,
            "requires_runtime_js": False,
            "linked_stylesheets_total": len(analyzer.stylesheet_refs),
            "linked_stylesheets_resolved": len(stylesheet_paths),
        },
        "source_contract_overlay": {
            "source": {
                "source_of_truth": "archived_fixture",
            },
            "route_rows": analyzer.rows,
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--html", required=True, type=pathlib.Path)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args(argv)

    write_json(args.output, build_route_manifest(args.html, args.repo_root, args.fixture))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
