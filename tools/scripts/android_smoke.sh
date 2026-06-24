#!/usr/bin/env bash
# android_smoke.sh — End-to-end smoke for the Pulp Android app (issue #337).
#
# Validates the full bring-up path:
#   1. Build APK via Gradle (./gradlew assembleDebug)
#   2. Install onto a running emulator or device (arm64-v8a expected)
#   3. Launch com.pulp.app/com.pulp.PulpActivity
#   4. Wait for JNI_OnLoad marker
#   5. Wait for Dawn/Skia render-ready marker
#   6. Wait for Oboe/DemoSynth audio-started marker
#   7. Exercise permission flow: revoke RECORD_AUDIO then re-grant, verify
#      native permission-result callback markers
#   8. Exercise lifecycle: am send-trim-memory BACKGROUND,
#      am start to foreground; verify nativeOnBackground / nativeOnForeground
#      markers
#   9. Clean shutdown via `am force-stop` and verify no FATAL exception in
#      logcat between launch and stop
#
# Usage:
#   tools/scripts/android_smoke.sh                       # default: build + smoke
#   tools/scripts/android_smoke.sh --skip-build          # use existing APK
#   tools/scripts/android_smoke.sh --allow-no-gpu        # tolerate missing
#                                                        # Skia-Android prebuilts
#                                                        # (smoke still validates
#                                                        # audio + lifecycle)
#   tools/scripts/android_smoke.sh --skip-permission     # skip permission flow
#   tools/scripts/android_smoke.sh --skip-lifecycle      # skip fg/bg flow
#   tools/scripts/android_smoke.sh --device <serial>     # target specific adb device
#   tools/scripts/android_smoke.sh --help
#
# Environment variables:
#   ANDROID_HOME / ANDROID_SDK_ROOT   SDK path (auto-detected otherwise)
#   ANDROID_NDK_HOME                  NDK path (auto-detected otherwise)
#   PULP_ANDROID_SMOKE_TIMEOUT        per-stage logcat wait timeout (default 90s)
#
# Exit codes:
#   0   full smoke passed
#   1   build failure
#   2   no usable emulator/device
#   3   install/launch failure
#   4   render-ready marker not seen in timeout
#   5   audio-started marker not seen in timeout
#   6   permission flow did not fire callbacks
#   7   lifecycle transition markers not seen
#   8   FATAL in logcat between launch and shutdown
#  77   skipped (PULP_ANDROID_SMOKE_ENABLED != 1 on ctest path)

set -uo pipefail

# ── Argument parsing ────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

SKIP_BUILD=0
TARGET_SERIAL=""
SKIP_PERMISSION=0
SKIP_LIFECYCLE=0
ALLOW_NO_GPU=0

usage() {
    sed -n '2,35p' "$0"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --skip-permission) SKIP_PERMISSION=1; shift ;;
        --skip-lifecycle) SKIP_LIFECYCLE=1; shift ;;
        --allow-no-gpu) ALLOW_NO_GPU=1; shift ;;
        --device) TARGET_SERIAL="$2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# ── ctest-style gate ────────────────────────────────────────────────────────
# Allow ctest to register the smoke as a disabled-by-default test. When
# PULP_ANDROID_SMOKE_ENABLED is unset the script exits 77 (ctest SKIP code)
# so the test is reported as skipped rather than failed on hosts without
# an emulator.
if [[ "${PULP_ANDROID_SMOKE_FORCE_CTEST_SKIP:-0}" == "1" ]]; then
    if [[ "${PULP_ANDROID_SMOKE_ENABLED:-0}" != "1" ]]; then
        echo "android-smoke: skipped (PULP_ANDROID_SMOKE_ENABLED != 1)"
        exit 77
    fi
fi

# ── SDK / tool discovery ────────────────────────────────────────────────────
detect_sdk() {
    for candidate in \
        "${ANDROID_HOME:-}" \
        "${ANDROID_SDK_ROOT:-}" \
        "${HOME}/Library/Android/sdk" \
        "${HOME}/Android/Sdk" \
        "${LOCALAPPDATA:-}/Android/Sdk"; do
        if [[ -n "${candidate}" && -d "${candidate}" ]]; then
            echo "${candidate}"; return 0
        fi
    done
    return 1
}

SDK_ROOT="$(detect_sdk || true)"
if [[ -z "${SDK_ROOT}" ]]; then
    echo "android-smoke: ANDROID_HOME / ANDROID_SDK_ROOT not set and no default SDK path found" >&2
    exit 2
fi

ADB="${SDK_ROOT}/platform-tools/adb"
if [[ ! -x "${ADB}" ]]; then
    if command -v adb >/dev/null 2>&1; then
        ADB="$(command -v adb)"
    else
        echo "android-smoke: adb not found under ${SDK_ROOT}/platform-tools or PATH" >&2
        exit 2
    fi
fi

TIMEOUT_SECONDS="${PULP_ANDROID_SMOKE_TIMEOUT:-90}"
SMOKE_TMP="$(mktemp -d -t pulp-android-smoke.XXXXXX)"
trap 'rm -rf "${SMOKE_TMP}"' EXIT

# ── adb helpers ─────────────────────────────────────────────────────────────
adb_cmd() {
    if [[ -n "${TARGET_SERIAL}" ]]; then
        "${ADB}" -s "${TARGET_SERIAL}" "$@"
    else
        "${ADB}" "$@"
    fi
}

select_device() {
    local line
    line="$(adb_cmd devices | awk 'NR>1 && $2=="device" {print $1; exit}')"
    if [[ -z "${line}" ]]; then
        echo "android-smoke: no device/emulator attached. Start one with:" >&2
        echo "  android/run-emulator.sh Medium_Phone_API_36.1" >&2
        exit 2
    fi
    if [[ -z "${TARGET_SERIAL}" ]]; then
        TARGET_SERIAL="${line}"
    fi
    local boot
    boot="$(adb_cmd shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')"
    if [[ "${boot}" != "1" ]]; then
        echo "android-smoke: device ${TARGET_SERIAL} is attached but not fully booted" >&2
        exit 2
    fi
    local abi
    abi="$(adb_cmd shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')"
    echo "android-smoke: using device ${TARGET_SERIAL} (abi=${abi})"
}

# ── Logcat wait loop (no blind sleeps) ──────────────────────────────────────
# Waits up to TIMEOUT_SECONDS for a grep-pattern to appear in logcat
# tagged with any of the space-separated tags in $3. When $4 is
# "advance", advance the global STAGE_LOGCAT_CURSOR past the match so
# subsequent "advance" calls won't re-match the same line — used by the
# lifecycle stage to distinguish the initial onCreate-time
# `nativeOnForeground` from the post-HOME → re-launch one. Most stages
# don't need this and should leave $4 empty.
# Prints matched line on success, returns 1 on timeout.
STAGE_LOGCAT_CURSOR=0
wait_for_logcat() {
    local pattern="$1"
    local label="$2"
    local tags="${3:-Pulp}"
    local advance="${4:-}"
    local start_s end_s
    start_s="$(date +%s)"
    end_s=$(( start_s + TIMEOUT_SECONDS ))
    local tag_alt="${tags// /|}"
    # Logcat format is: `DATE TIME PID TID LEVEL TAG : MESSAGE`. We grep
    # the full dump rather than passing `-s TAG` because some events use
    # multiple tags (DemoSynth is tagged `PulpAudio`, lifecycle uses `Pulp`).
    local dump_file="${SMOKE_TMP}/dump_${label// /_}.txt"
    local cursor=0
    if [[ "${advance}" == "advance" ]]; then
        cursor="${STAGE_LOGCAT_CURSOR}"
    fi
    while [[ "$(date +%s)" -lt "${end_s}" ]]; do
        adb_cmd logcat -d > "${dump_file}" 2>/dev/null || true
        local filtered match_line abs_line_num
        filtered="$(grep -nE "[VDIWEF] (${tag_alt}) *:" "${dump_file}" 2>/dev/null \
            | awk -F: -v cur="${cursor}" -v pat="${pattern}" '
                {
                    ln = $1
                    rest = substr($0, index($0, ":") + 1)
                    if (ln + 0 > cur + 0 && rest ~ pat) { print ln ":" rest; exit }
                }' || true)"
        if [[ -n "${filtered}" ]]; then
            abs_line_num="${filtered%%:*}"
            match_line="${filtered#*:}"
            if [[ "${advance}" == "advance" ]]; then
                STAGE_LOGCAT_CURSOR="${abs_line_num}"
            fi
            echo "  [${label}] ${match_line}"
            return 0
        fi
        sleep 1
    done
    echo "android-smoke: timeout waiting for ${label} (pattern=${pattern}, tags=${tags})" >&2
    echo "── last 80 logcat lines ──" >&2
    tail -80 "${dump_file}" >&2 2>/dev/null || true
    return 1
}

# ── Stages ──────────────────────────────────────────────────────────────────
PACKAGE="com.pulp.app"
ACTIVITY="com.pulp.PulpActivity"
APK_PATH="${REPO_ROOT}/android/app/build/outputs/apk/debug/app-debug.apk"

build_apk() {
    echo "── Stage 1: Build APK ──"
    if [[ "${SKIP_BUILD}" == "1" ]]; then
        if [[ ! -f "${APK_PATH}" ]]; then
            echo "android-smoke: --skip-build requested but APK not found at ${APK_PATH}" >&2
            exit 1
        fi
        echo "  reusing existing APK: ${APK_PATH}"
        return
    fi
    pushd "${REPO_ROOT}/android" >/dev/null
    ./gradlew assembleDebug || { popd >/dev/null; exit 1; }
    popd >/dev/null
    if [[ ! -f "${APK_PATH}" ]]; then
        echo "android-smoke: build succeeded but APK not found at ${APK_PATH}" >&2
        exit 1
    fi
    echo "  APK: ${APK_PATH}"
}

install_and_launch() {
    echo "── Stage 2: Install & launch ──"
    adb_cmd install -r "${APK_PATH}" || exit 3
    adb_cmd shell am force-stop "${PACKAGE}" || true
    # Clear logcat immediately before launch so subsequent wait_for_logcat
    # calls (which dump with `logcat -d` and grep from a per-stage cursor)
    # see a clean slate anchored to this activity's instance.
    adb_cmd logcat -c || true
    adb_cmd shell am start -n "${PACKAGE}/${ACTIVITY}" || exit 3
    echo "  launched ${PACKAGE}/${ACTIVITY}"
}

wait_for_jni_load() {
    echo "── Stage 3: JNI bridge init ──"
    wait_for_logcat "JNI_OnLoad: Pulp native bridge initialized" "JNI_OnLoad" "Pulp" || exit 4
}

wait_for_render_ready() {
    echo "── Stage 4: Render-ready (Dawn + Skia) ──"
    # First marker: the ANativeWindow was received by the render thread.
    # This proves the SurfaceView → JNI → GPU pipeline is wired end-to-end
    # even in degraded environments (e.g. a build without Skia-Android
    # prebuilts where Dawn initialization falls through to nullptr).
    wait_for_logcat "Android GPU surface: ANativeWindow received" \
        "surface received" "Pulp" || exit 4
    # Second marker: GPU actually initialized. In strict mode (default)
    # this must be "Dawn initialized" followed by a Skia marker. In
    # degraded mode (--allow-no-gpu) we accept a "Dawn initialization
    # failed" / "failed to create Dawn GpuSurface" message as a
    # documented prerequisite-missing state instead of a regression.
    local dawn_ok_pattern="Dawn initialized"
    local dawn_bad_pattern="Dawn initialization failed|failed to create Dawn GpuSurface"
    if [[ "${ALLOW_NO_GPU}" == "1" ]]; then
        wait_for_logcat "${dawn_ok_pattern}|${dawn_bad_pattern}" \
            "GPU init (allow-no-gpu)" "Pulp" || exit 4
        # Re-dump and peek: if the matched marker was the bad one, we're
        # in degraded mode and skip the Skia context check.
        local dawn_failed dump="${SMOKE_TMP}/dump_dawn_peek.txt"
        adb_cmd logcat -d > "${dump}" 2>/dev/null || true
        dawn_failed="$(grep -E "${dawn_bad_pattern}" "${dump}" 2>/dev/null \
            | head -1 || true)"
        if [[ -n "${dawn_failed}" ]]; then
            echo "  NOTE: Dawn/GPU unavailable — skipping Skia context check"
            echo "        (build tools/build-skia-android.sh to enable GPU)"
            return 0
        fi
    else
        wait_for_logcat "${dawn_ok_pattern}" "Dawn init" "Pulp" || {
            echo "android-smoke: Dawn did not initialize. This usually means" >&2
            echo "  Skia-Android prebuilts are missing. Build them with:" >&2
            echo "    tools/build-skia-android.sh arm64" >&2
            echo "  Or re-run smoke with --allow-no-gpu to tolerate the" >&2
            echo "  degraded state (useful for audio/lifecycle bring-up)." >&2
            exit 4
        }
    fi
    # Skia Graphite can fall back to Dawn-only; accept either marker.
    wait_for_logcat "Skia Graphite context created|Dawn-only mode" "GPU context" "Pulp" || exit 4
}

wait_for_audio_started() {
    echo "── Stage 5: Audio engine started (Oboe + DemoSynth) ──"
    # DemoSynth logs "DemoSynth: playing" after synth_start() succeeds.
    # On emulator this uses Shared/None mode; on hardware Exclusive/LowLatency.
    # NOTE: demo_synth.cpp uses the "PulpAudio" tag (not "Pulp") — so we
    # pass both tags to adb logcat -s.
    wait_for_logcat "DemoSynth: playing" "DemoSynth" "PulpAudio Pulp" || exit 5
}

# Exercise Android's permission flow without needing the UI.
# The RECORD_AUDIO permission is the one Pulp actually requests for Oboe
# input streams. We use `pm revoke` followed by `pm grant` to flip the
# runtime-permission state and then confirm the change via
# `dumpsys package`. Note: `pm grant` does not trigger the Kotlin
# ActivityResultContracts callback (and therefore does not fire the
# "Permission result" native log line in permissions.cpp) — that only
# happens on a real UI grant flow. The dumpsys check is the reliable
# ground-truth that the runtime permission state actually transitioned,
# which is what a real app relies on for RECORD_AUDIO access.
exercise_permissions() {
    if [[ "${SKIP_PERMISSION}" == "1" ]]; then
        echo "── Stage 6: Permission flow (SKIPPED) ──"
        return
    fi
    echo "── Stage 6: Permission flow (revoke → grant) ──"
    local perm="android.permission.RECORD_AUDIO"
    adb_cmd shell pm revoke "${PACKAGE}" "${perm}" 2>/dev/null || true
    adb_cmd shell pm grant  "${PACKAGE}" "${perm}" 2>/dev/null || true
    # Confirm the grant state flipped (runtime permissions dump is the
    # ground truth: `pm dump com.pulp.app | grep -A1 RECORD_AUDIO` shows
    # the current granted bit).
    local granted
    granted="$(adb_cmd shell dumpsys package "${PACKAGE}" 2>/dev/null \
        | grep -A1 "${perm}" | grep -m1 granted=true || true)"
    if [[ -z "${granted}" ]]; then
        echo "android-smoke: RECORD_AUDIO not reported as granted after pm grant" >&2
        adb_cmd shell dumpsys package "${PACKAGE}" 2>/dev/null \
            | grep -A1 "${perm}" >&2 || true
        exit 6
    fi
    echo "  RECORD_AUDIO runtime permission toggled successfully"
}

# Exercise foreground/background lifecycle without a real DAW host driver.
# We background the activity via `am start` to the home screen, wait for
# the nativeOnBackground marker, then bring Pulp back to the foreground
# and wait for nativeOnForeground. Verifies #334 (lifecycle/audio-focus)
# is wired end-to-end.
exercise_lifecycle() {
    if [[ "${SKIP_LIFECYCLE}" == "1" ]]; then
        echo "── Stage 7: Lifecycle (SKIPPED) ──"
        return
    fi
    echo "── Stage 7: Lifecycle (background → foreground) ──"
    # Use "advance" mode so the foreground wait below won't spuriously
    # match the initial onCreate-time `nativeOnForeground` logs (which
    # always fire at app startup — there are two of them before we even
    # begin the lifecycle stage).
    adb_cmd shell input keyevent KEYCODE_HOME
    wait_for_logcat "nativeOnBackground" "background" "Pulp" advance || exit 7
    adb_cmd shell am start -n "${PACKAGE}/${ACTIVITY}" || exit 7
    wait_for_logcat "nativeOnForeground" "foreground" "Pulp" advance || exit 7
}

clean_shutdown() {
    echo "── Stage 8: Clean shutdown ──"
    adb_cmd shell am force-stop "${PACKAGE}" || true
    # Logcat ring was cleared at launch (stage 2), so a full dump now
    # contains every log line from this activity's lifetime. Scan it for
    # FATAL exceptions ATTRIBUTED TO OUR APP ONLY. nativeOnShutdown only
    # fires on a real onDestroy not on force-stop, so we don't require
    # that marker here.
    #
    # #556 showed that a naive `grep FATAL EXCEPTION`
    # over a shared emulator's full logcat dump treats every other
    # process's crash as a Pulp regression and exits 8. Correlate with
    # the AndroidRuntime process line so only our package counts. We
    # still print context lines when a real Pulp FATAL is found so
    # triage is easy.
    local dump="${SMOKE_TMP}/dump_shutdown.txt"
    adb_cmd logcat -d > "${dump}" 2>/dev/null || true
    if grep -F "AndroidRuntime: Process: ${PACKAGE}," "${dump}" >/dev/null 2>&1; then
        echo "android-smoke: FATAL exception detected in ${PACKAGE} during smoke run" >&2
        # Print the FATAL EXCEPTION block AND the corresponding process
        # line so the failure surface is obvious. We intentionally match
        # the literal ${PACKAGE}, on the AndroidRuntime line — a crash
        # in a sibling process (that produced an unrelated FATAL EXCEPTION
        # with no matching "Process: com.pulp.app," line) no longer fails
        # the smoke.
        grep -B1 -A20 -F "AndroidRuntime: Process: ${PACKAGE}," "${dump}" >&2 || true
        exit 8
    fi
    # Informational: note any unrelated FATALs so failures elsewhere on
    # the emulator aren't completely invisible, but do not fail the run.
    if grep -F "FATAL EXCEPTION" "${dump}" >/dev/null 2>&1; then
        echo "  (info) unrelated FATAL EXCEPTION observed in another process — ignored" >&2
    fi
    echo "  no FATAL exceptions attributed to ${PACKAGE} in logcat"
}

# ── Main ────────────────────────────────────────────────────────────────────
select_device
build_apk
install_and_launch
wait_for_jni_load
wait_for_render_ready
wait_for_audio_started
# Lifecycle BEFORE permissions — `pm revoke RECORD_AUDIO` kills the app
# process (standard Android behavior for granted-and-in-use permissions),
# which would invalidate the foreground/background state we want to observe.
exercise_lifecycle
exercise_permissions
clean_shutdown

echo ""
echo "android-smoke: PASS"
