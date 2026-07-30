"""Source-stack recognition and conservative dynamic-feature blockers."""

from __future__ import annotations

import re
from typing import Any


def source_recognition(source: str) -> dict[str, Any]:
    lowered = source.lower()
    imports = re.findall(
        r"""(?:from\s+|require\s*\(\s*)['"]([^'"]+)['"]""", source)
    features = {
        "react": bool(re.search(r"\b(?:react|jsx|tsx|useState|useEffect)\b", source)),
        "vue": bool(re.search(r"\b(?:createApp|defineComponent|<template)\b", source)),
        "svelte": bool(re.search(r"\b(?:onMount|svelte:)\b", source)),
        "tailwind": bool(re.search(
            r"""class(?:Name)?\s*=\s*["'][^"']*\b(?:flex|grid|gap-|p-|m-|bg-|text-)""",
            source)),
        "shadcn": any("components/ui/" in item for item in imports),
        "radix": any("@radix-ui/" in item for item in imports),
        "react_aria": any("react-aria" in item for item in imports),
        "framer_motion": any("framer-motion" in item for item in imports),
        "lucide": any("lucide" in item for item in imports),
        "heroicons": any("heroicons" in item for item in imports),
        "canvas": "<canvas" in lowered,
        "webgl": bool(re.search(r"\b(?:webgl2?|three(?:\.js)?)\b", lowered)),
        "svg": "<svg" in lowered,
        "custom_fonts": bool(re.search(
            r"@font-face|fonts\.(?:googleapis|gstatic)\.com", lowered)),
        "css_variables": bool(re.search(r"--[\w-]+\s*:|var\s*\(", source)),
        "media_queries": "@media" in lowered,
        "pseudo_elements": bool(re.search(r"::(?:before|after)\b", lowered)),
        "portals": bool(re.search(r"\b(?:createPortal|Portal)\b", source)),
        "shadow_dom": bool(re.search(r"\battachShadow\b", source)),
        "async_initialization": bool(re.search(
            r"\b(?:fetch\s*\(|setTimeout\s*\(|Promise\b|async\s+)", source)),
        "conditional_rendering": bool(re.search(
            r"\?\s*<|&&\s*<|v-if=|\{#if\b", source)),
        "animation": bool(re.search(
            r"@keyframes|\banimation\s*:|requestAnimationFrame|framer-motion",
            lowered)),
        "javascript": bool(re.search(
            r"<script(?:\s[^>]*)?>\s*(?!</script>)", source, re.IGNORECASE)),
        "absolute_positioning": bool(re.search(
            r"position\s*:\s*(?:absolute|fixed)|\b(?:absolute|fixed)\b", lowered)),
        "flex_layout": bool(re.search(r"display\s*:\s*flex|\bflex\b", lowered)),
        "grid_layout": bool(re.search(r"display\s*:\s*grid|\bgrid\b", lowered)),
        "images": bool(re.search(r"<img\b|background-image\s*:|url\s*\(", lowered)),
    }
    if features["react"]:
        framework = "react"
    elif features["vue"]:
        framework = "vue"
    elif features["svelte"]:
        framework = "svelte"
    elif "<html" in lowered or "<!doctype" in lowered:
        framework = "html"
    else:
        framework = "unknown"
    return {
        "framework": framework,
        "imports": sorted(set(imports)),
        "features": features,
    }


def unsupported_features(recognition: dict[str, Any]) -> list[str]:
    features = recognition["features"]
    names = [
        "canvas", "webgl", "portals", "shadow_dom", "async_initialization",
        "conditional_rendering", "animation",
    ]
    result = [name for name in names if features.get(name)]
    if features.get("javascript"):
        result.append("javascript_evaluation")
    if recognition["framework"] in {"react", "vue", "svelte"}:
        result.append("javascript_evaluation")
    return sorted(result)
