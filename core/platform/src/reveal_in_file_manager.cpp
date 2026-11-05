// Reveal-in-file-manager: common code, plus the Windows and Linux backends.
// The macOS backend lives in platform/mac/reveal_mac.mm.

#include <pulp/platform/reveal_in_file_manager.hpp>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(_WIN32)
#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <pulp/platform/child_process.hpp>
#endif

namespace pulp::platform {

const char* to_string(RevealOutcome outcome) noexcept {
    switch (outcome) {
    case RevealOutcome::revealed:
        return "revealed";
    case RevealOutcome::opened_containing_folder:
        return "opened_containing_folder";
    case RevealOutcome::not_found:
        return "not_found";
    case RevealOutcome::unsupported:
        return "unsupported";
    case RevealOutcome::failed:
        return "failed";
    }
    return "failed";
}

#if !defined(__APPLE__) || !TARGET_OS_OSX

RevealOutcome reveal_in_file_manager(const std::filesystem::path& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec))
        return RevealOutcome::not_found;

#if defined(_WIN32)
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec)
        return RevealOutcome::failed;

    // COM may already be initialised on this thread in a different mode;
    // that is usable, so only a hard failure is an error.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool owns_com = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE)
        return RevealOutcome::failed;

    RevealOutcome outcome = RevealOutcome::failed;
    PIDLIST_ABSOLUTE item = nullptr;
    if (SUCCEEDED(SHParseDisplayName(absolute.wstring().c_str(), nullptr, &item, 0, nullptr)) &&
        item) {
        if (SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0)))
            outcome = RevealOutcome::revealed;
        CoTaskMemFree(item);
    }
    if (owns_com)
        CoUninitialize();
    return outcome;
#elif defined(__linux__) && !defined(__ANDROID__)
    // xdg-open can open a folder but has no way to select a file in it.
    const std::filesystem::path folder =
        std::filesystem::is_directory(path, ec) ? path : path.parent_path();
    ProcessOptions options;
    options.timeout_ms = 5000;
    options.capture_stdout = false;
    options.capture_stderr = false;
    const ProcessResult result = ChildProcess::run("xdg-open", {folder.string()}, options);
    if (result.timed_out)
        return RevealOutcome::failed;
    // 127 / -1: the launcher is absent, which is "no file manager", not a failure.
    if (result.exit_code == 127 || result.exit_code == -1)
        return RevealOutcome::unsupported;
    return result.exit_code == 0 ? RevealOutcome::opened_containing_folder : RevealOutcome::failed;
#else
    return RevealOutcome::unsupported;
#endif
}

#endif

} // namespace pulp::platform
