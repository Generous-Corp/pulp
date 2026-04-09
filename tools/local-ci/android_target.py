"""
Android build target for local CI.

Usage:
    python3 tools/local-ci/local_ci.py run --target android <branch>

Requires:
    - ANDROID_HOME environment variable or ~/Library/Android/sdk
    - NDK 30.x installed
    - Java 17+
"""

import os
import subprocess
import sys
from pathlib import Path


def find_android_sdk() -> str:
    """Find Android SDK path."""
    sdk = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if sdk and os.path.isdir(sdk):
        return sdk

    # Default locations
    home = Path.home()
    candidates = [
        home / "Library" / "Android" / "sdk",        # macOS
        home / "Android" / "Sdk",                      # Linux
        Path("C:/Users") / os.getlogin() / "AppData" / "Local" / "Android" / "Sdk",  # Windows
    ]
    for path in candidates:
        if path.is_dir():
            return str(path)

    return ""


def find_ndk(sdk_path: str) -> str:
    """Find the latest NDK installation."""
    ndk_root = Path(sdk_path) / "ndk"
    if not ndk_root.is_dir():
        return ""

    ndks = sorted(ndk_root.iterdir(), reverse=True)
    for ndk in ndks:
        toolchain = ndk / "build" / "cmake" / "android.toolchain.cmake"
        if toolchain.exists():
            return str(ndk)

    return ""


def check_prerequisites() -> list:
    """Check Android build prerequisites. Returns list of issues."""
    issues = []

    sdk = find_android_sdk()
    if not sdk:
        issues.append("Android SDK not found. Set ANDROID_HOME.")
        return issues

    ndk = find_ndk(sdk)
    if not ndk:
        issues.append(f"No NDK found in {sdk}/ndk/. Install via: sdkmanager 'ndk;30.0.14904198'")

    # Check Java
    try:
        result = subprocess.run(["java", "-version"], capture_output=True, text=True)
        version_line = result.stderr.split("\n")[0] if result.stderr else ""
        if "17" not in version_line and "21" not in version_line:
            issues.append(f"Java 17+ required. Found: {version_line}")
    except FileNotFoundError:
        issues.append("Java not found in PATH")

    return issues


def build_android(project_root: Path, verbose: bool = False) -> int:
    """Build Android APK via Gradle."""
    android_dir = project_root / "android"
    gradlew = android_dir / "gradlew"

    if not gradlew.exists():
        print("Error: android/gradlew not found", file=sys.stderr)
        return 1

    sdk = find_android_sdk()
    if not sdk:
        print("Error: Android SDK not found", file=sys.stderr)
        return 1

    # Write local.properties
    props = android_dir / "local.properties"
    props.write_text(f"sdk.dir={sdk}\n")

    env = os.environ.copy()
    env["ANDROID_HOME"] = sdk

    cmd = [str(gradlew), "assembleDebug"]
    if not verbose:
        cmd.append("-q")

    print(f"Building Android APK from {android_dir}...")
    result = subprocess.run(cmd, cwd=android_dir, env=env)

    if result.returncode == 0:
        apk = android_dir / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk"
        if apk.exists():
            size_mb = apk.stat().st_size / (1024 * 1024)
            print(f"✅ Android APK built: {apk} ({size_mb:.1f} MB)")
        else:
            print("✅ Build succeeded but APK path not found at expected location")
    else:
        print("❌ Android build failed")

    return result.returncode


ANDROID_TARGET = {
    "name": "android",
    "build_fn": build_android,
    "check_fn": check_prerequisites,
    "requires": ["ANDROID_HOME"],
    "abi": ["arm64-v8a", "x86_64"],
}
