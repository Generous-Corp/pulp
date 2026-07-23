#!/usr/bin/env python3
"""Generate WidgetBridge TypeScript globals, mock registry, and docs."""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


MANIFEST = Path("core/view/src/widget_bridge_api_manifest.tsv")
AUTOCAPS = Path("core/view/include/pulp/view/reload_autocaps.hpp")
CAPABILITIES = Path("core/view/include/pulp/view/reload_capabilities.hpp")
TYPES_OUT = Path("packages/pulp-react/src/bridge-globals.generated.d.ts")
MOCK_OUT = Path("packages/pulp-react/src/bridge-mock-functions.generated.ts")
SAFE_MOCK_OUT = Path("packages/pulp-react/src/bridge-mock-safe-functions.generated.ts")
DOCS_OUT = Path("docs/reference/js-bridge.md")

GENERATED_BY = "tools/scripts/generate_widget_bridge_api.py"
FINGERPRINT_PREFIX = "Pulp-WidgetBridge-Input-Fingerprint: "
FINGERPRINT_INPUTS = [
    Path(GENERATED_BY),
    MANIFEST,
    AUTOCAPS,
    CAPABILITIES,
]
FNV1A64_OFFSET = 0xCBF29CE484222325
FNV1A64_PRIME = 0x100000001B3
FNV1A64_MASK = 0xFFFFFFFFFFFFFFFF

MOCK_ONLY_FUNCTIONS = [
    "createButton",
    "insertChild",
    "moveWidget",
]

CATEGORY_LABELS = {
    "accessibility": "Accessibility",
    "animation": "Animation",
    "canvas2d": "Canvas 2D",
    "css_style": "CSS Style",
    "dom": "DOM Compatibility",
    "events": "Events",
    "gpu": "GPU",
    "layout": "Layout",
    "metadata": "Metadata",
    "platform_services": "Platform Services",
    "runtime": "Runtime",
    "runtime_import": "Runtime Import",
    "shader": "Shader",
    "state_binding": "State Binding",
    "storage_assets": "Storage and Assets",
    "svg": "SVG",
    "theme": "Theme",
    "tokens": "Tokens",
    "typography": "Typography",
    "widget_assets": "Widget Assets",
    "widget_factory": "Widget Factory",
    "widget_schema": "Widget Schema",
    "widget_value": "Widget Value",
}

DOC_CATEGORY_NOTES = {
    "widget_schema": [
        "`setWidgetLottie` stores Lottie JSON and scrub state on a widget for tool workflows.",
        "Native Skottie rendering is available through the `LottieView` widget and `LottieAnimation` canvas path; opt in with the `PULP_LOTTIE` CMake option, which links the Skia skottie module when the bundled Skia toolchain provides the required SkJSON and skresources support.",
    ],
}

CAPABILITY_ORDER = ["exec", "clipboard", "filesystem", "storage", "ai", "runtime_import", "network"]
CALLABLE_KINDS = {"function", "promise_function"}


@dataclass(frozen=True)
class ManifestRow:
    name: str
    category: str
    kind: str
    source: str
    jsx: str = ""


JS_PREAMBLE_ROWS = [
    ManifestRow("on", "events", "function", "core/view/src/widget_bridge.cpp", "event:names"),
]


TYPE_PREAMBLE = """export {};

export type PulpBridgeCapability =
    | 'exec'
    | 'clipboard'
    | 'filesystem'
    | 'storage'
    | 'ai'
    | 'runtime_import'
    | 'network';

export type PulpBridgeJsonObject = Record<string, unknown>;
export type PulpBridgeArrayLikeNumber = number[] | Float32Array | Uint8Array | Uint8ClampedArray;
export interface PulpBridgeRect {
    x: number;
    y: number;
    width: number;
    height: number;
    top: number;
    right: number;
    bottom: number;
    left: number;
}
export interface PulpBridgeSize {
    width: number;
    height: number;
}
export interface PulpBridgeTextMetrics {
    width: number;
    ascent?: number;
    descent?: number;
    lineHeight?: number;
}
export interface PulpBridgeCanvasImageData {
    width: number;
    height: number;
    data: number[] | Uint8ClampedArray;
}
export interface PulpBridgeBindingTransform {
    db?: boolean;
    dbMin?: number;
    dbMax?: number;
    scale?: number;
    offset?: number;
    min?: number;
    max?: number;
    clamp?: boolean;
}
export type PulpBridgeHostObject = Record<string, unknown>;
export type PulpBridgeUnknownFunction = (...args: unknown[]) => unknown;
"""


SIGNATURE_OVERRIDES: dict[str, str] = {
    "__pulpRuntimeImport__": "(specifier: string, options?: unknown) => unknown",
    "__pulpRuntimeSettle__": "(rounds?: number) => void",
    "__requestFrame__": "(callbackName: string) => number",
    "__cancelFrame__": "(requestId: number) => void",
    "__flushFrames__": "() => void",
    "__motionPublishValue__": "(key: string, value: number) => void",
    "__motionSetProvenance__": "(key: string, source: string) => void",
    "__motionClearProvenance__": "(key: string) => void",
    "__scheduleTimer__": "(callbackName: string, delayMs: number, repeat?: boolean) => number",
    "__cancelTimer__": "(timerId: number) => void",
    "__flushTimers__": "() => void",
    "__performanceNow__": "() => number",
    "animate": "(id: string, property: string, targetValue: number | string, durationMs: number, easingName?: string) => void",
    "applyTokenDiff": "(json: string) => void",
    "beginPath": "() => void",
    "bindMeter": "(widgetId: string, source: string, transform?: PulpBridgeBindingTransform) => boolean",
    "bindWidgetToParam": "(widgetId: string, paramName: string, transform?: PulpBridgeBindingTransform) => boolean",
    "canvasArc": "(canvasId: string, x: number, y: number, radius: number, startAngle: number, endAngle: number, anticlockwise?: boolean) => void",
    "canvasBeginPath": "(canvasId: string) => void",
    "canvasClear": "(canvasId: string) => void",
    "canvasClearGradient": "(canvasId: string) => void",
    "canvasClearRect": "(canvasId: string, x: number, y: number, width: number, height: number) => void",
    "canvasClearStrokeGradient": "(canvasId: string) => void",
    "canvasClip": "(canvasId: string) => void",
    "canvasClipRect": "(canvasId: string, x: number, y: number, width: number, height: number) => void",
    "canvasClosePath": "(canvasId: string) => void",
    "canvasCubicTo": "(canvasId: string, cp1x: number, cp1y: number, cp2x: number, cp2y: number, x: number, y: number) => void",
    "canvasDrawImage": "(canvasId: string, source: string, dx: number, dy: number, dw?: number, dh?: number, sx?: number, sy?: number, sw?: number, sh?: number) => void",
    "canvasFillCircle": "(canvasId: string, x: number, y: number, radius: number, color?: string) => void",
    "canvasFillPath": "(canvasId: string) => void",
    "canvasFillRect": "(canvasId: string, x: number, y: number, width: number, height: number, color?: string) => void",
    "canvasFillRoundedRect": "(canvasId: string, x: number, y: number, width: number, height: number, radius: number, color?: string) => void",
    "canvasFillText": "(canvasId: string, text: string, x: number, y: number, fontSize?: number, color?: string) => void",
    "canvasGetImageData": "(canvasId: string, x: number, y: number, width: number, height: number) => PulpBridgeCanvasImageData",
    "canvasGlobalCompositeOperation": "(canvasId: string, operation: string) => void",
    "canvasLineTo": "(canvasId: string, x: number, y: number) => void",
    "canvasMeasureText": "(canvasId: string, text: string) => PulpBridgeTextMetrics",
    "canvasMoveTo": "(canvasId: string, x: number, y: number) => void",
    "canvasPathArc": "(canvasId: string, x: number, y: number, radius: number, startAngle: number, endAngle: number, anticlockwise?: boolean) => void",
    "canvasPathArcTo": "(canvasId: string, x1: number, y1: number, x2: number, y2: number, radius: number) => void",
    "canvasPathEllipse": "(canvasId: string, x: number, y: number, radiusX: number, radiusY: number, rotation: number, startAngle: number, endAngle: number, anticlockwise?: boolean) => void",
    "canvasPathRoundRect": "(canvasId: string, x: number, y: number, width: number, height: number, radius: number) => void",
    "canvasPutImageData": "(canvasId: string, imageData: PulpBridgeCanvasImageData, x: number, y: number) => void",
    "canvasQuadTo": "(canvasId: string, cpx: number, cpy: number, x: number, y: number) => void",
    "canvasRect": "(canvasId: string, x: number, y: number, width: number, height: number, color?: string) => void",
    "canvasRestore": "(canvasId: string) => void",
    "canvasRotate": "(canvasId: string, radians: number) => void",
    "canvasSave": "(canvasId: string) => void",
    "canvasScale": "(canvasId: string, x: number, y: number) => void",
    "canvasSetBlendMode": "(canvasId: string, blendMode: string) => void",
    "canvasSetConicGradient": "(canvasId: string, x: number, y: number, angle: number, stopsJson: string) => void",
    "canvasSetDirection": "(canvasId: string, direction: 'ltr' | 'rtl' | 'inherit' | string) => void",
    "canvasSetFillColor": "(canvasId: string, color: string) => void",
    "canvasSetFillPattern": "(canvasId: string, source: string, repetition?: string) => void",
    "canvasSetFilter": "(canvasId: string, filter: string) => void",
    "canvasSetFont": "(canvasId: string, fontFamily: string, fontSize: number) => void",
    "canvasSetFontFull": "(canvasId: string, fontFamily: string, fontSize: number, fontWeight?: number, fontStyle?: string) => void",
    "canvasSetGlobalAlpha": "(canvasId: string, alpha: number) => void",
    "canvasSetImageSmoothing": "(canvasId: string, enabled: boolean, quality?: string) => void",
    "canvasSetLineCap": "(canvasId: string, lineCap: string) => void",
    "canvasSetLineDash": "(canvasId: string, segments: number[]) => void",
    "canvasSetLineJoin": "(canvasId: string, lineJoin: string) => void",
    "canvasSetLineWidth": "(canvasId: string, width: number) => void",
    "canvasSetLinearGradient": "(canvasId: string, x0: number, y0: number, x1: number, y1: number, stopsJson: string) => void",
    "canvasSetMiterLimit": "(canvasId: string, limit: number) => void",
    "canvasSetRadialGradient": "(canvasId: string, x: number, y: number, radius: number, stopsJson: string) => void",
    "canvasSetShadowBlur": "(canvasId: string, blur: number) => void",
    "canvasSetShadowColor": "(canvasId: string, color: string) => void",
    "canvasSetShadowOffsetX": "(canvasId: string, x: number) => void",
    "canvasSetShadowOffsetY": "(canvasId: string, y: number) => void",
    "canvasSetStrokeColor": "(canvasId: string, color: string) => void",
    "canvasSetStrokeConicGradient": "(canvasId: string, x: number, y: number, angle: number, stopsJson: string) => void",
    "canvasSetStrokeLinearGradient": "(canvasId: string, x0: number, y0: number, x1: number, y1: number, stopsJson: string) => void",
    "canvasSetStrokePattern": "(canvasId: string, source: string, repetition?: string) => void",
    "canvasSetStrokeRadialGradient": "(canvasId: string, x: number, y: number, radius: number, stopsJson: string) => void",
    "canvasSetTextAlign": "(canvasId: string, align: string) => void",
    "canvasSetTextBaseline": "(canvasId: string, baseline: string) => void",
    "canvasSetTransform": "(canvasId: string, a: number, b: number, c: number, d: number, e: number, f: number) => void",
    "canvasStrokeCircle": "(canvasId: string, x: number, y: number, radius: number, color?: string, lineWidth?: number) => void",
    "canvasStrokeLine": "(canvasId: string, x1: number, y1: number, x2: number, y2: number, color?: string, lineWidth?: number) => void",
    "canvasStrokePath": "(canvasId: string) => void",
    "canvasStrokeRect": "(canvasId: string, x: number, y: number, width: number, height: number, color?: string, lineWidth?: number) => void",
    "canvasStrokeRoundedRect": "(canvasId: string, x: number, y: number, width: number, height: number, radius: number, color?: string, lineWidth?: number) => void",
    "canvasStrokeText": "(canvasId: string, text: string, x: number, y: number, fontSize?: number, color?: string) => void",
    "canvasTranslate": "(canvasId: string, x: number, y: number) => void",
    "chooseFolder": "(title?: string) => string",
    "claimOverlay": "(id: string) => void",
    "clearBoxShadow": "(id: string) => void",
    "clearTransform": "(id: string) => void",
    "clearWidgetSchema": "(id: string) => void",
    "compileShader": "(skslCode: string) => PulpBridgeJsonObject",
    "defineKeyframes": "(name: string, stopsJson: string) => void",
    "drawPath": "(commands: string) => void",
    "enableInspectClick": "() => void",
    "exec": "(command: string) => string",
    "execAsync": "(command: string, callbackId: string) => void",
    "exportDesignTokens": "() => string",
    "getAICli": "() => string",
    "getComputedValue": "(id: string, property: string) => string",
    "getGPUInfo": "() => PulpBridgeJsonObject",
    "getLayoutAncestorRects": "(id: string) => PulpBridgeRect[]",
    "getLayoutRect": "(id: string) => PulpBridgeRect",
    "getMotionToken": "(name: string) => number | undefined",
    "getParam": "(name: string) => number",
    "getRootSize": "() => PulpBridgeSize",
    "getStringToken": "(name: string) => string | undefined",
    "getText": "(id: string) => string",
    "getThemeJson": "() => string",
    "getValue": "(id: string) => number",
    "importDesignTokens": "(json: string) => void",
    "layout": "() => void",
    "loadFont": "(path: string) => boolean",
    "loadStylePreset": "(name: string) => unknown",
    "measureText": "(text: string, fontSize?: number) => PulpBridgeTextMetrics",
    "nativeReleasePointerCapture": "(id: string, pointerId: number) => void",
    "nativeSetPointerCapture": "(id: string, pointerId: number) => void",
    "navigatorGPU": "PulpBridgeHostObject",
    "on": "(id: string, eventName: string, fn: (...args: unknown[]) => void) => void",
    "readClipboard": "() => string",
    "registerContextMenu": "(id: string, callbackName: string) => void",
    "registerDrop": "(id: string) => void",
    "registerFont": "(family: string, path: string) => boolean",
    "registerGesture": "(id: string) => void",
    "registerHover": "(id: string) => void",
    "registerPointer": "(id: string) => void",
    "registerShortcut": "(keyCode: number, modMask: number, callbackName: string) => void",
    "registerWheel": "(id: string) => void",
    "releaseOverlay": "(id: string) => void",
    "removeWidget": "(id: string) => void",
    "saveStylePreset": "(name: string, payload: unknown) => void",
    "seekWidgetLottie": "(id: string, time01: number) => void",
    "setAICli": "(command: string) => void",
    "setAnimation": "(id: string, name: string, duration: number, iterations?: number, direction?: string) => void",
    "setBorder": "(id: string, color: string, width?: number, radius?: number) => void",
    "setBorderBottomColor": "(id: string, color: string) => void",
    "setBorderLeftColor": "(id: string, color: string) => void",
    "setBorderRightColor": "(id: string, color: string) => void",
    "setBorderSide": "(id: string, side: 'top' | 'right' | 'bottom' | 'left', width: number, color: string) => void",
    "setBorderTopColor": "(id: string, color: string) => void",
    "setBoxShadow": "(id: string, offsetX: number, offsetY: number, blur: number, spread: number, color: string, inset?: boolean) => void",
    "setCornerRadius": "(id: string, corner: 'All' | 'TopLeft' | 'TopRight' | 'BottomLeft' | 'BottomRight' | string, radius: number) => void",
    "setFaderSkin": "(id: string, trackColor?: string, fillColor?: string, thumbColor?: string, thumbBorderColor?: string, thumbWidth?: number, thumbHeight?: number, cornerRadius?: number) => void",
    "setFaderTrackBorder": "(id: string, color: string) => void",
    "setFaderTrackWidth": "(id: string, width: number) => void",
    "setFlex": "(id: string, key: string, value: number | string) => void",
    "setGrid": "(id: string, key: string, value: number | string) => void",
    "setImageSource": "(id: string, path: string) => void",
    "setItems": "(id: string, items: string[] | string) => void",
    "setKnobSpriteCore": "(id: string, coreX: number, coreY: number, coreWidth: number, coreHeight: number) => void",
    "setKnobSpriteStrip": "(id: string, imagePath: string, frameCount?: number, orientation?: 'vertical' | 'horizontal' | string) => void",
    "setListItems": "(id: string, items: string[] | string) => void",
    "setMeterBarRatio": "(id: string, ratio: number) => void",
    "setMeterColors": "(id: string, backgroundColor: string, stopsCsv: string) => void",
    "setMeterLevel": "(id: string, peak: number, rms?: number) => void",
    "setMotionToken": "(name: string, value: number) => void",
    "setParam": "(name: string, value: number) => void",
    "setPanelStyle": "(id: string, backgroundToken: string, borderToken?: string, radius?: number, width?: number) => void",
    "setPosition": "(id: string, position: string) => void",
    "setSubpixelLayout": "(id: string, enabled: boolean) => void",
    "setTop": "(id: string, value: number | string) => void",
    "setRight": "(id: string, value: number | string) => void",
    "setBottom": "(id: string, value: number | string) => void",
    "setLeft": "(id: string, value: number | string) => void",
    "setScrollContentSize": "(id: string, width: number, height: number) => void",
    "setSelected": "(id: string, index: number) => void",
    "setStringToken": "(name: string, value: string) => void",
    "setSvgLine": "(id: string, x1: number, y1: number, x2: number, y2: number) => void",
    "setSvgPath": "(id: string, pathData: string) => void",
    "setSvgRect": "(id: string, x: number, y: number, width: number, height: number) => void",
    "setSvgStrokeWidth": "(id: string, width: number) => void",
    "setSvgViewBox": "(id: string, width: number, height: number) => void",
    "setTextRuns": "(id: string, runsJson: string) => void",
    "setTheme": "(name: 'dark' | 'light' | 'pro_audio' | string) => void",
    "setTransform": "(id: string, a: number, b: number, c: number, d: number, e: number, f: number) => void",
    "setTransformOrigin": "(id: string, x: number, y?: number) => void",
    "setTranslate": "(id: string, x: number, y: number) => void",
    "setTransition": "(id: string, css: string) => void",
    "setTransitionDelay": "(id: string, seconds: number) => void",
    "setTransitionDuration": "(id: string, seconds: number) => void",
    "setTransitionProperty": "(id: string, properties: string) => void",
    "setTransitionTimingFunction": "(id: string, easing: string) => void",
    "setWidgetLottie": "(id: string, lottieJson: string) => void",
    "setWidgetSchema": "(id: string, schemaJson: string) => void",
    "setWidgetShader": "(id: string, skslCode: string) => PulpBridgeJsonObject",
    "clearWidgetShader": "(id: string) => PulpBridgeJsonObject",
    "setScale": "(id: string, scale: number) => void",
    "setSkew": "(id: string, xDegrees: number, yDegrees: number) => void",
    "setSpectrumData": "(id: string, samples: PulpBridgeArrayLikeNumber) => void",
    "setWaveformData": "(id: string, samples: PulpBridgeArrayLikeNumber) => void",
    "setXY": "(id: string, x: number, y: number) => void",
    "showContextMenu": "(itemsJson: string, x: number, y: number) => number",
    "showOpenDialog": "(title?: string, filterDescription?: string, extensions?: string) => string",
    "showSaveDialog": "(title?: string, filterDescription?: string, extensions?: string) => string",
    "storageGetItem": "(key: string) => string | null",
    "storageRemoveItem": "(key: string) => void",
    "storageSetItem": "(key: string, value: string) => void",
    "unbindWidget": "(widgetId: string) => number",
    "writeClipboard": "(text: string) => void",
}

ID_STRING_SETTERS = {
    "setAccentColor",
    "setAccessibilityLabel",
    "setAccessibilityRole",
    "setAnchor",
    "setAppearance",
    "setBackground",
    "setBackgroundAttachment",
    "setBackgroundClip",
    "setBackgroundGradient",
    "setBackgroundOrigin",
    "setBackgroundPosition",
    "setBackgroundRepeat",
    "setBackgroundSize",
    "setBackfaceVisibility",
    "setBorderColor",
    "setBorderStyle",
    "setBottom",
    "setBoxSizing",
    "setClipPath",
    "setColorToken",
    "setCursor",
    "setDirection",
    "setFilter",
    "setFontFamily",
    "setFontStyle",
    "setFontVariant",
    "setLabel",
    "setLeft",
    "setListStyleImage",
    "setListStylePosition",
    "setListStyleType",
    "setMask",
    "setMaskImage",
    "setMaskSize",
    "setObjectFit",
    "setObjectPosition",
    "setOrientation",
    "setOutlineColor",
    "setOutlineStyle",
    "setOverflow",
    "setOverscrollBehavior",
    "setPlaceholder",
    "setPointerEvents",
    "setRight",
    "setScrollBehavior",
    "setSource",
    "setSvgFill",
    "setSvgFillGradient",
    "setSvgFillRule",
    "setSvgStroke",
    "setText",
    "setTextAlign",
    "setTextColor",
    "setTextDecoration",
    "setTextDecorationColor",
    "setTextDecorationStyle",
    "setTextOverflow",
    "setTextTransform",
    "setTop",
    "setUserSelect",
    "setVerticalAlign",
    "setVisibility",
    "setWhiteSpace",
    "setWidgetStyle",
    "setWordBreak",
}

ID_NUMBER_SETTERS = {
    "setBackdropFilter",
    "setBorderBottomLeftRadius",
    "setBorderBottomRightRadius",
    "setBorderBottomWidth",
    "setBorderRadius",
    "setBorderRightWidth",
    "setBorderTopLeftRadius",
    "setBorderTopRightRadius",
    "setBorderTopWidth",
    "setBorderWidth",
    "setCornerRadius",
    "setDimensionToken",
    "setElevation",
    "setFontSize",
    "setFontWeight",
    "setLeft",
    "setLetterSpacing",
    "setLineClamp",
    "setLineHeight",
    "setListRowHeight",
    "setListSelected",
    "setMax",
    "setMin",
    "setOpacity",
    "setOutlineOffset",
    "setOutlineWidth",
    "setProgress",
    "setRotation",
    "setScale",
    "setShadowOpacity",
    "setShadowRadius",
    "setStep",
    "setTextIndent",
    "setTextShadowRadius",
    "setValue",
    "setZIndex",
}

ID_BOOLEAN_SETTERS = {
    "setDebugPaint",
    "setEnabled",
    "setIncludeFontPadding",
    "setMultiLine",
    "setVisible",
}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def fnv1a64_update(value: int, data: bytes) -> int:
    for byte in data:
        value ^= byte
        value = (value * FNV1A64_PRIME) & FNV1A64_MASK
    return value


def normalized_text_bytes(data: bytes) -> bytes:
    return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def widget_bridge_input_fingerprint(root: Path) -> str:
    value = FNV1A64_OFFSET
    for rel in FINGERPRINT_INPUTS:
        value = fnv1a64_update(value, rel.as_posix().encode("utf-8"))
        value = fnv1a64_update(value, b"\0")
        value = fnv1a64_update(value, normalized_text_bytes((root / rel).read_bytes()))
        value = fnv1a64_update(value, b"\0")
    return f"fnv1a64:{value:016x}"


def read_manifest(root: Path) -> list[ManifestRow]:
    rows: list[ManifestRow] = []
    seen: set[str] = set()
    for line_no, raw in enumerate((root / MANIFEST).read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if fields[0] == "name":
            if fields[:5] != ["name", "category", "kind", "source", "jsx"]:
                raise ValueError(f"{MANIFEST}:{line_no}: unexpected header")
            continue
        if len(fields) not in (4, 5):
            raise ValueError(f"{MANIFEST}:{line_no}: expected 4 or 5 tab-separated fields")
        name, category, kind, source = fields[:4]
        jsx = fields[4] if len(fields) == 5 else ""
        if name in seen:
            raise ValueError(f"{MANIFEST}:{line_no}: duplicate bridge function {name}")
        seen.add(name)
        rows.append(ManifestRow(name=name, category=category, kind=kind, source=source, jsx=jsx))
    return rows


def api_rows(root: Path) -> list[ManifestRow]:
    return read_manifest(root) + JS_PREAMBLE_ROWS


def read_capability_wire_names(root: Path) -> dict[str, str]:
    text = (root / CAPABILITIES).read_text()
    matches = re.findall(r"case\s+ReloadCapability::(\w+):\s+return\s+\"([^\"]+)\"", text)
    if not matches:
        raise ValueError(f"{CAPABILITIES}: no capability_name() mappings found")
    return {enum: wire for enum, wire in matches}


def read_autocaps(root: Path) -> dict[str, str]:
    enum_to_wire = read_capability_wire_names(root)
    text = (root / AUTOCAPS).read_text()
    matches = re.findall(r'\{\s*"([^"]+)"\s*,\s*ReloadCapability::(\w+)\s*\}', text)
    if not matches:
        raise ValueError(f"{AUTOCAPS}: no autocaps_function_table() mappings found")
    out: dict[str, str] = {}
    for fn, enum in matches:
        if enum not in enum_to_wire:
            raise ValueError(f"{AUTOCAPS}: unknown ReloadCapability::{enum}")
        out[fn] = enum_to_wire[enum]
    return out


def pascal_name(value: str) -> str:
    return "".join(part.capitalize() for part in value.split("_"))


def signature_for(row: ManifestRow) -> str:
    if row.name in SIGNATURE_OVERRIDES:
        return SIGNATURE_OVERRIDES[row.name]
    if row.kind == "host_object":
        return "PulpBridgeHostObject"
    if row.kind == "promise_function":
        return "(...args: unknown[]) => Promise<unknown>"
    if row.category == "widget_factory" or row.name in {"createCanvas", "createGrid", "createSvgPath", "createSvgRect", "createSvgLine"}:
        if row.name == "createFader":
            return "(id: string, orientation: 'vertical' | 'horizontal', parentId: string) => string"
        if row.name == "createMeter":
            return "(id: string, orientation: 'vertical' | 'horizontal', parentId: string) => string"
        if row.name == "createIcon":
            return "(id: string, type: 'image_upload' | 'send' | 'search' | 'close' | string, parentId: string) => string"
        if row.name == "createLabel":
            return "(id: string, text: string, parentId: string) => string"
        if row.name == "createBadge":
            return "(id: string, text: string, tone: string, parentId: string) => string"
        return "(id: string, parentId: string) => string"
    if row.name in ID_BOOLEAN_SETTERS:
        return "(id: string, value: boolean) => void"
    if row.name in ID_NUMBER_SETTERS:
        return "(id: string, value: number) => void"
    if row.name in ID_STRING_SETTERS:
        return "(id: string, value: string) => void"
    if row.name.startswith("get"):
        return "(...args: unknown[]) => unknown"
    if row.name.startswith("set"):
        return "(id: string, ...args: unknown[]) => void"
    if row.name.startswith("register"):
        return "(id: string, ...args: unknown[]) => void"
    if row.name.startswith("__gpu") or row.name.startswith("__describe") or row.name.startswith("__decode") or row.name.startswith("__draco"):
        return "(...args: unknown[]) => unknown"
    return "(...args: unknown[]) => unknown"


def interface_name_for_capability(capability: str | None) -> str:
    if capability is None:
        return "PulpBridgeAlwaysGlobals"
    return f"PulpBridge{pascal_name(capability)}Globals"


def emit_interface(name: str, rows: list[ManifestRow]) -> list[str]:
    lines = [f"export interface {name} {{"]
    if not rows:
        lines.append("}")
        return lines
    width = max(len(row.name) for row in rows)
    for row in rows:
        pad = " " * (width - len(row.name))
        lines.append(f"    {row.name}:{pad} {signature_for(row)};")
    lines.append("}")
    return lines


def emit_types(rows: list[ManifestRow], autocaps: dict[str, str], fingerprint: str) -> str:
    manifest_names = {row.name for row in rows}
    extra_autocaps = sorted(set(autocaps) - manifest_names)
    if extra_autocaps:
        raise ValueError(f"{AUTOCAPS}: functions missing from manifest: {', '.join(extra_autocaps)}")

    by_capability: dict[str | None, list[ManifestRow]] = defaultdict(list)
    for row in rows:
        by_capability[autocaps.get(row.name)].append(row)

    capabilities = [cap for cap in CAPABILITY_ORDER if by_capability.get(cap)]

    lines = [
        f"// Generated by {GENERATED_BY}.",
        f"// {FINGERPRINT_PREFIX}{fingerprint}",
        "// Source: core/view/src/widget_bridge_api_manifest.tsv + JS preamble globals + reload_autocaps.hpp.",
        "",
        TYPE_PREAMBLE.rstrip(),
        "",
    ]
    lines.extend(emit_interface("PulpBridgeAlwaysGlobals", by_capability.get(None, [])))
    lines.append("")
    for cap in capabilities:
        lines.extend(emit_interface(interface_name_for_capability(cap), by_capability[cap]))
        lines.append("")

    lines.append("export type PulpBridgeGlobals<C extends PulpBridgeCapability = never> =")
    lines.append("    PulpBridgeAlwaysGlobals")
    for cap in capabilities:
        lines.append(f"    & ('{cap}' extends C ? {interface_name_for_capability(cap)} : {{}})")
    lines[-1] += ";"
    lines.append("")
    lines.append("declare global {")
    for row in rows:
        cap = autocaps.get(row.name)
        iface = interface_name_for_capability(cap)
        type_expr = f"{iface}['{row.name}']"
        if cap is None:
            lines.append(f"    var {row.name}: {type_expr};")
        else:
            lines.append(f"    var {row.name}: {type_expr} | undefined;")
    lines.append("}")
    return "\n".join(lines) + "\n"


def emit_mock(names: list[str], fingerprint: str, const_name: str, type_name: str, source_note: str) -> str:
    lines = [
        f"// Generated by {GENERATED_BY}.",
        f"// {FINGERPRINT_PREFIX}{fingerprint}",
        f"// Source: {source_note}.",
        "",
        f"export const {const_name} = [",
    ]
    for name in names:
        lines.append(f"    '{name}',")
    lines.extend([
        "] as const;",
        "",
        f"export type {type_name} = typeof {const_name}[number];",
        "",
    ])
    return "\n".join(lines)


def full_mock_names(rows: list[ManifestRow]) -> list[str]:
    return sorted({row.name for row in rows if row.kind in CALLABLE_KINDS} | set(MOCK_ONLY_FUNCTIONS))


def safe_mock_names(rows: list[ManifestRow], autocaps: dict[str, str]) -> list[str]:
    return sorted({row.name for row in rows if row.kind in CALLABLE_KINDS and row.name not in autocaps} | set(MOCK_ONLY_FUNCTIONS))


def md_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace("|", "\\|")


def emit_docs(rows: list[ManifestRow], autocaps: dict[str, str], fingerprint: str) -> str:
    by_category: dict[str, list[ManifestRow]] = defaultdict(list)
    for row in rows:
        by_category[row.category].append(row)

    lines = [
        "# JS Bridge API Reference",
        "",
        f"<!-- {FINGERPRINT_PREFIX}{fingerprint} -->",
        "",
        f"Generated by `{GENERATED_BY} --write` from `core/view/src/widget_bridge_api_manifest.tsv`, JS preamble globals, and the capability map in `core/view/include/pulp/view/reload_autocaps.hpp`.",
        "",
        "**Status:** experimental",
        "",
        "The bridge installs these names as globals on the active JS engine. Pure UI groups are always registered. Effectful groups are capability-scoped during hot reload, so an ungranted group's symbols are absent at runtime rather than installed and guarded. Generated TypeScript marks those gated globals as `| undefined` and exposes per-capability groups through `PulpBridgeGlobals<C>`.",
        "",
        "Run `python3 tools/scripts/generate_widget_bridge_api.py --check` for an exact content check. `pulp doctor --only WidgetBridge` performs a non-executing input-fingerprint freshness check.",
        "",
        "## Capability Model",
        "",
        "| Capability | Bridge globals |",
        "|---|---:|",
    ]
    for cap in CAPABILITY_ORDER:
        count = sum(1 for row in rows if autocaps.get(row.name) == cap)
        if count:
            lines.append(f"| `{cap}` | {count} |")
    always = sum(1 for row in rows if row.name not in autocaps)
    lines.append(f"| always registered | {always} |")
    lines.extend([
        "",
        "## API Surface",
        "",
    ])

    for category in sorted(by_category, key=lambda c: CATEGORY_LABELS.get(c, c)):
        category_rows = by_category[category]
        lines.append(f"### {CATEGORY_LABELS.get(category, category)}")
        lines.append("")
        lines.append("| Name | Kind | Signature | Capability | JSX | Source |")
        lines.append("|---|---|---|---|---|---|")
        for row in category_rows:
            cap = autocaps.get(row.name)
            cap_text = "always" if cap is None else f"requires `{cap}`"
            jsx = row.jsx if row.jsx else "-"
            lines.append(
                f"| `{row.name}` | `{row.kind}` | `{md_escape(signature_for(row))}` | {cap_text} | `{md_escape(jsx)}` | `{row.source}` |"
            )
        lines.append("")
        if notes := DOC_CATEGORY_NOTES.get(category):
            lines.extend(notes)
            lines.append("")
    return "\n".join(lines)


def build_outputs(root: Path) -> dict[Path, str]:
    rows = api_rows(root)
    autocaps = read_autocaps(root)
    fingerprint = widget_bridge_input_fingerprint(root)
    return {
        TYPES_OUT: emit_types(rows, autocaps, fingerprint),
        MOCK_OUT: emit_mock(
            full_mock_names(rows),
            fingerprint,
            "bridgeMockFunctionNames",
            "BridgeMockFunctionName",
            "core/view/src/widget_bridge_api_manifest.tsv plus JS shim globals used by @pulp/react tests",
        ),
        SAFE_MOCK_OUT: emit_mock(
            safe_mock_names(rows, autocaps),
            fingerprint,
            "bridgeSafeMockFunctionNames",
            "BridgeSafeMockFunctionName",
            "always-available WidgetBridge globals from the manifest plus JS shim globals used by @pulp/react tests",
        ),
        DOCS_OUT: emit_docs(rows, autocaps, fingerprint),
    }


def check_outputs(root: Path, outputs: dict[Path, str]) -> int:
    failed = False
    for rel, expected in outputs.items():
        path = root / rel
        actual = path.read_text() if path.exists() else ""
        if actual == expected:
            continue
        failed = True
        fromfile = str(rel)
        tofile = f"{rel} (generated)"
        sys.stdout.writelines(difflib.unified_diff(
            actual.splitlines(keepends=True),
            expected.splitlines(keepends=True),
            fromfile=fromfile,
            tofile=tofile,
        ))
    return 1 if failed else 0


def write_outputs(root: Path, outputs: dict[Path, str]) -> None:
    for rel, content in outputs.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write", action="store_true", help="rewrite generated artifacts")
    group.add_argument("--check", action="store_true", help="check generated artifacts for drift")
    args = parser.parse_args(argv)

    root = repo_root()
    outputs = build_outputs(root)
    if args.write:
        write_outputs(root, outputs)
        return 0
    return check_outputs(root, outputs)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
