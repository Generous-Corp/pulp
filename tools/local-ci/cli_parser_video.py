"""Video-proof argument group for the desktop smoke/click subcommands."""

from __future__ import annotations

import argparse


VIDEO_PROOF_TEMPLATE_CHOICES = [
    "validation-proof",
    "design-parity",
    "component-zoom",
    "plugin-host",
    "inspector-workflow",
    "standalone",
    "mobile-simulator",
    "mobile-emulator",
]


def add_desktop_video_args(command_parser: argparse.ArgumentParser) -> None:
    command_parser.add_argument(
        "--run-in-terminal",
        action="store_true",
        help="On macOS, re-run this local-ci command inside Terminal.app so Screen Recording permission follows Terminal's TCC grant.",
    )
    command_parser.add_argument(
        "--record-video",
        action="store_true",
        help="Record a short window-region MP4 proof while the desktop action runs.",
    )
    command_parser.add_argument(
        "--duration",
        "--video-duration",
        dest="video_duration",
        type=float,
        default=8.0,
        help="Seconds of video to record when --record-video is set (default: 8)",
    )
    command_parser.add_argument(
        "--video-fps",
        type=float,
        default=30.0,
        help="Frames per second for --record-video captures (default: 30)",
    )
    command_parser.add_argument(
        "--video-capture-target",
        choices=["app", "terminal"],
        default="app",
        help="Capture the launched app window (default) or a Terminal.app window running the command.",
    )
    command_parser.add_argument(
        "--video-recorder",
        choices=["auto", "avfoundation", "frame-sequence"],
        default="auto",
        help="macOS recorder backend for app-window video proofs (default: auto).",
    )
    command_parser.add_argument(
        "--video-focus",
        choices=["auto", "off"],
        default="auto",
        help=(
            "Composed-proof framing for an interaction: 'auto' (default) zooms the "
            "embedded recording to the clicked control so the change is clearly "
            "visible; 'off' keeps the full-window framing. Overlays (tap marker / "
            "zoom-center) are placed correctly in either mode."
        ),
    )
    command_parser.add_argument(
        "--capture-bundle-id",
        help="After launching --command on macOS, capture this bundle id's window instead of the command process window.",
    )
    command_parser.add_argument(
        "--video-attachment-budget-mb",
        type=float,
        default=100.0,
        help="Attachment budget in decimal MB for issue-ready video metadata (default: 100)",
    )
    command_parser.add_argument(
        "--small-video",
        action="store_true",
        help="Also create a small fallback MP4 for a 10 MB-style attachment budget.",
    )
    command_parser.add_argument(
        "--small-video-budget-mb",
        type=float,
        default=10.0,
        help="Attachment budget in decimal MB for --small-video fallback (default: 10)",
    )
    command_parser.add_argument(
        "--compose-video-proof",
        action="store_true",
        help="Use Remotion to render an annotated proof video from the raw recording.",
    )
    command_parser.add_argument(
        "--video-template",
        choices=VIDEO_PROOF_TEMPLATE_CHOICES,
        help="Remotion proof template for direct --record-video composition.",
    )
    command_parser.add_argument("--source-image", help="Optional source/reference image for design-parity proof composition")
    command_parser.add_argument("--source-label", help="Label for --source-image (default: Source reference)")
    command_parser.add_argument("--video-title", help="Override the composed proof video title")
    command_parser.add_argument(
        "--video-note",
        action="append",
        default=[],
        help="Short note to show in the composed proof video; may be repeated.",
    )
    command_parser.add_argument(
        "--video-audio",
        choices=["none", "plugin", "system"],
        default="none",
        help="Audio source for the proof video. 'plugin' muxes an explicit --video-audio-file WAV; 'system' records an explicit AVFoundation audio device.",
    )
    command_parser.add_argument(
        "--video-audio-file",
        help="WAV file to mux into the proof video when --video-audio plugin is selected.",
    )
    command_parser.add_argument(
        "--video-audio-device",
        help="AVFoundation audio device index or name for --video-audio system. Can also be set with PULP_VIDEO_AUDIO_DEVICE.",
    )
