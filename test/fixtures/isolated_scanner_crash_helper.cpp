// SPDX-License-Identifier: MIT
//
// Tiny helper binary for `pulp-test-isolated-scanner`. Used in place of
// `pulp-scan-worker` to exercise the parent's classification of an
// abnormal worker exit. Triggered ONLY by the test (no install
// rule). Three modes via argv[1]:
//
//   crash    — deref a null pointer (SIGSEGV / Windows AV exception)
//   timeout  — sleep for 60s so the parent's 1s timeout fires
//   garbage  — exit 0 with non-JSON stdout (worker contract violation)
//
// Anything else exits 0 silently (mimics the NotPlugin path the real
// worker takes when it can't extract a descriptor).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>

// On Windows an unhandled access violation is normally intercepted by
// Windows Error Reporting / a registered post-mortem debugger, which can
// hold the faulting process alive long enough that the parent scanner's
// timeout fires first — the worker is then misclassified as Timeout instead
// of Crash (the failure this helper exists to exercise). Suppress that
// machinery and install a top-level filter that terminates immediately with
// the exception code, so the AV surfaces as a fast, non-zero "crash" exit.
static LONG WINAPI terminate_on_unhandled(EXCEPTION_POINTERS* info) {
    TerminateProcess(GetCurrentProcess(), info->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;  // unreachable
}

static void arm_fast_fail_crash() {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    SetUnhandledExceptionFilter(terminate_on_unhandled);
}
#endif

int main(int argc, char** argv) {
    // Allow either `helper <mode>` (test for worker contract) or
    // `helper <bundle> <mode>` (matches real-worker signature where
    // argv[1] is the bundle path). The test uses the latter form.
    std::string mode;
    if (argc >= 3) {
        mode = argv[2];
    } else if (argc == 2) {
        mode = argv[1];
    }

    if (mode == "crash") {
#ifdef _WIN32
        arm_fast_fail_crash();
#endif
        // Volatile to defeat the optimizer collapsing this away.
        volatile int* p = nullptr;
        *p = 42;  // boom
        return 99;  // unreachable
    }
    if (mode == "timeout") {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }
    if (mode == "garbage") {
        std::printf("this is not json\n");
        return 0;
    }
    if (mode == "unsupported") {
        // Mimic the real worker's "unsupported bundle extension" exit.
        std::fprintf(stderr, "test helper: unsupported bundle extension\n");
        return 3;
    }
    // Default — silent clean exit.
    return 0;
}
