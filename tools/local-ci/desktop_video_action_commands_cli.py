"""Desktop `video` action command — apply a video recipe + record, then dispatch."""
from __future__ import annotations

import argparse
from collections.abc import Callable
import importlib.util
import json
from pathlib import Path

try:
    import reaper_video_recipe
except ModuleNotFoundError:
    _reaper_recipe_spec = importlib.util.spec_from_file_location(
        "reaper_video_recipe",
        Path(__file__).resolve().with_name("reaper_video_recipe.py"),
    )
    if _reaper_recipe_spec is None or _reaper_recipe_spec.loader is None:
        raise
    reaper_video_recipe = importlib.util.module_from_spec(_reaper_recipe_spec)
    _reaper_recipe_spec.loader.exec_module(reaper_video_recipe)



VIDEO_PROOF_RECIPES = {
    "audio-inspector-demo",
    "standalone-interaction",
    "reaper-plugin-editor",
    "inspector-workflow",
    "component-zoom",
    "design-parity",
}

RECIPE_PROOF_NOTES = {
    "standalone-interaction": [
        "Watch for the app window, the click marker, and a visible after-state or diff.",
        "This proof is useful only if the control response is readable without rerunning the app.",
    ],
    "audio-inspector-demo": [
        "Watch for the inspector or audio-inspector surface and readable probe/meter state.",
        "The storyboard should make the inspected state understandable without opening the manifest.",
    ],
    "reaper-plugin-editor": [
        "Watch for REAPER host chrome plus a real inserted plugin/editor context.",
        "A useful host proof must show more than a blank project window.",
    ],
    "inspector-workflow": [
        "Watch for the inspector pane and the selected node, probe, or meter state.",
        "The proof should demonstrate the developer workflow, not just app launch.",
    ],
    "component-zoom": [
        "Watch for full-window context before the zoomed component detail.",
        "The focus box, zoom inset, and action marker should agree on the same component.",
    ],
    "design-parity": [
        "Watch for readable source and native render panes side by side.",
        "The notes or storyboard should identify the critical region being compared.",
    ],
}


def _print_lines(lines, *, print_fn: Callable[[str], None]) -> None:
    for line in lines:
        print_fn(line)


def _set_default(args: argparse.Namespace, name: str, value) -> None:
    if getattr(args, name, None) in {None, ""}:
        setattr(args, name, value)


def _set_default_number(args: argparse.Namespace, name: str, value: float, parser_default: float) -> None:
    if float(getattr(args, name, parser_default) or parser_default) == parser_default:
        setattr(args, name, value)


def _set_auto(args: argparse.Namespace, name: str, value: str) -> None:
    if getattr(args, name, None) in {None, "", "auto"}:
        setattr(args, name, value)


def _append_recipe_proof_notes(args: argparse.Namespace, recipe: str) -> None:
    notes = list(getattr(args, "video_note", None) or [])
    for note in RECIPE_PROOF_NOTES.get(recipe, []):
        if note not in notes:
            notes.append(note)
    args.video_note = notes


def _apply_desktop_video_recipe(args: argparse.Namespace) -> None:
    recipe = getattr(args, "recipe", None)
    if not recipe:
        return
    if recipe not in VIDEO_PROOF_RECIPES:
        raise ValueError(f"unknown desktop video recipe `{recipe}`.")

    if recipe == "standalone-interaction":
        _set_default(args, "label", "standalone-interaction-proof")
        _set_default(args, "video_title", "Standalone UI interaction")
        _set_default(args, "video_template", "standalone")
        _set_default_number(args, "video_duration", 6.0, 8.0)
        _set_default_number(args, "video_fps", 8.0, 30.0)
        _set_auto(args, "video_recorder", "frame-sequence")
        if any([args.click, args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label]):
            args.capture_before = True
        _append_recipe_proof_notes(args, recipe)
        return

    if recipe == "audio-inspector-demo":
        args.action = "smoke"
        _set_default(args, "label", "audio-inspector-demo-proof")
        _set_default(args, "video_title", "Standalone Audio Inspector Demo")
        _set_default(args, "video_template", "inspector-workflow")
        _append_recipe_proof_notes(args, recipe)
        return

    if recipe == "reaper-plugin-editor":
        plugin = getattr(args, "plugin", None)
        plugin_format = getattr(args, "plugin_format", None)
        if not plugin or not plugin_format:
            raise ValueError("recipe `reaper-plugin-editor` requires --plugin and --plugin-format.")
        if any([args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label]):
            raise ValueError("recipe `reaper-plugin-editor` records a host window; use --click X,Y instead of ViewInspector selectors.")
        _set_default(args, "host_app", "REAPER")
        if not getattr(args, "launch_command", None):
            if plugin_format == "clap":
                ok, detail = reaper_video_recipe.installed_clap_bundle_status(plugin)
                if not ok:
                    raise ValueError(
                        f"recipe `reaper-plugin-editor` requires an installed {plugin} CLAP bundle: {detail} "
                        f"Build `PulpSynth_CLAP`/the requested CLAP target and install or symlink it under ~/Library/Audio/Plug-Ins/CLAP."
                    )
                ok, detail = reaper_video_recipe.reaper_clap_cache_status(plugin)
                if not ok:
                    raise ValueError(
                        f"recipe `reaper-plugin-editor` requires REAPER to have a valid {plugin} CLAP cache entry: {detail} "
                        "Open REAPER's Preferences > Plug-ins > CLAP and rescan, or remove the stale REAPER CLAP cache entry and relaunch REAPER."
                    )
            recipe_files = reaper_video_recipe.write_reaper_plugin_editor_recipe(
                plugin=plugin,
                plugin_format=plugin_format,
            )
            args.launch_command = recipe_files["command"]
            args.capture_bundle_id = "com.cockos.reaper"
            args.reaper_recipe_files = recipe_files
        _set_default(args, "label", f"reaper-{plugin_format}-{plugin}-proof")
        _set_default(args, "video_title", f"{plugin} {plugin_format.upper()} editor in {args.host_app}")
        _set_default(args, "video_template", "plugin-host")
        if getattr(args, "reaper_recipe_files", None):
            if getattr(args, "video_note", None) is None:
                args.video_note = []
            args.video_note.append(f"REAPER launched from a generated wrapper and opened {plugin} as {plugin_format.upper()}.")
        if not getattr(args, "click", None):
            args.action = "smoke"
        args.capture_before = True
        _append_recipe_proof_notes(args, recipe)
        return

    if recipe == "inspector-workflow":
        args.action = "smoke"
        args.capture_ui_snapshot = False
        _set_default(args, "label", "inspector-workflow-proof")
        _set_default(args, "video_title", "Inspector workflow proof")
        _set_default(args, "video_template", "inspector-workflow")
        _append_recipe_proof_notes(args, recipe)
        return

    if recipe == "component-zoom":
        component_id = getattr(args, "component_id", None)
        if component_id and not getattr(args, "click_view_id", None):
            args.click_view_id = component_id
        if not any([args.click, args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label]):
            raise ValueError("recipe `component-zoom` requires --component-id or a click selector.")
        args.capture_ui_snapshot = True
        args.capture_before = True
        _set_default(args, "video_template", "component-zoom")
        _set_default(args, "label", f"component-{args.click_view_id or component_id or 'zoom'}-proof")
        _set_default(args, "video_title", "Component validation")
        _set_default_number(args, "video_duration", 6.0, 8.0)
        _set_default_number(args, "video_fps", 8.0, 30.0)
        _set_auto(args, "video_recorder", "frame-sequence")
        _append_recipe_proof_notes(args, recipe)
        return

    if recipe == "design-parity":
        if not getattr(args, "source_image", None):
            raise ValueError("recipe `design-parity` requires --source-image.")
        args.action = "inspect"
        args.capture_ui_snapshot = True
        _set_default(args, "video_template", "design-parity")
        _set_default(args, "source_label", "Source reference")
        _set_default(args, "label", "design-parity-proof")
        _set_default(args, "video_title", "Design import parity")
        _append_recipe_proof_notes(args, recipe)


def _video_context(args: argparse.Namespace) -> dict:
    context: dict[str, str] = {}
    for attr, key in [
        ("recipe", "recipe"),
        ("host_app", "host"),
        ("plugin", "plugin"),
        ("plugin_format", "format"),
        ("component_id", "component"),
    ]:
        value = getattr(args, attr, None)
        if value:
            context[key] = str(value)
    if getattr(args, "capture_bundle_id", None):
        context["capture_bundle_id"] = str(args.capture_bundle_id)
    if getattr(args, "bundle_id", None):
        context["bundle_id"] = str(args.bundle_id)
    if getattr(args, "launch_command", None) and not context.get("bundle_id"):
        context["launch"] = "command"
    if getattr(args, "reaper_recipe_files", None):
        context["reaper_recipe"] = "generated"
    return context



def _video_kwargs(args: argparse.Namespace) -> dict:
    audio_source = getattr(args, "video_audio", "none")
    audio_file = getattr(args, "video_audio_file", None)
    if audio_source == "plugin" and not audio_file:
        raise ValueError("--video-audio plugin requires --video-audio-file <wav>.")
    if audio_source != "plugin" and audio_file:
        raise ValueError("--video-audio-file is only valid with --video-audio plugin.")
    resolved_audio_file = str(Path(audio_file).expanduser().resolve()) if audio_file else None
    return {
        "record_video": bool(getattr(args, "record_video", False)),
        "video_duration_secs": float(getattr(args, "video_duration", 8.0)),
        "video_fps": float(getattr(args, "video_fps", 30.0)),
        "video_audio_source": audio_source,
        "video_audio_file": resolved_audio_file,
        "video_audio_device": getattr(args, "video_audio_device", None),
        "video_recorder": getattr(args, "video_recorder", "auto"),
        "video_focus": getattr(args, "video_focus", "auto"),
        "video_capture_target": getattr(args, "video_capture_target", "app"),
        "capture_bundle_id": getattr(args, "capture_bundle_id", None),
        "video_attachment_budget_bytes": int(float(getattr(args, "video_attachment_budget_mb", 100.0)) * 1_000_000),
        "small_video": bool(getattr(args, "small_video", False)),
        "small_video_budget_bytes": int(float(getattr(args, "small_video_budget_mb", 10.0)) * 1_000_000),
        "compose_video_proof": bool(getattr(args, "compose_video_proof", False)),
        "video_template": getattr(args, "video_template", None),
        "video_source_image": getattr(args, "source_image", None),
        "video_source_label": getattr(args, "source_label", None),
        "video_title": getattr(args, "video_title", None),
        "video_notes": getattr(args, "video_note", None) or [],
        "video_context": _video_context(args),
    }


def cmd_desktop_video(
    args: argparse.Namespace,
    *,
    cmd_desktop_smoke_fn: Callable[[argparse.Namespace], int],
    cmd_desktop_click_fn: Callable[[argparse.Namespace], int],
    cmd_desktop_inspect_fn: Callable[[argparse.Namespace], int],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        _apply_desktop_video_recipe(args)
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    try:
        _video_kwargs(args)
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    args.record_video = True
    if not getattr(args, "compose_video_proof", False):
        args.compose_video_proof = True

    action = getattr(args, "action", "click")
    if action == "smoke":
        return cmd_desktop_smoke_fn(args)
    if action == "click":
        return cmd_desktop_click_fn(args)
    if action == "inspect":
        return cmd_desktop_inspect_fn(args)
    print_fn(f"Error: unsupported desktop video action `{action}`.")
    return 1


