#include "inspector_container_report.hpp"

#include "cli_common.hpp"
#include "cli_fs_util.hpp"

namespace pulp::cli::inspector_shipping {
namespace {

class ScopedInspectionRoot {
public:
    ScopedInspectionRoot() : path(fsutil::temporary_archive_root()) {}
    ~ScopedInspectionRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    ScopedInspectionRoot(const ScopedInspectionRoot&) = delete;
    ScopedInspectionRoot& operator=(const ScopedInspectionRoot&) = delete;

    fs::path path;
};

} // namespace

bool load_container_artifact_report(const fs::path& input, Report& report,
                                    std::string& error) {
#if !defined(__APPLE__)
    (void)input;
    (void)report;
    error = "container inspector evidence scans are macOS-only";
    return false;
#else
    ScopedInspectionRoot temporary;
    std::error_code create_error;
    fs::create_directories(temporary.path, create_error);
    if (create_error) {
        error = "could not create temporary inspector evidence directory";
        return false;
    }

    if (input.extension() == ".pkg") {
        const auto expanded = temporary.path / "expanded";
        const auto command = "pkgutil --expand-full " + shell_quote(input) + " " +
            shell_quote(expanded) + " >/dev/null 2>&1";
        if (run(command) != 0) {
            error = "could not expand package for inspector evidence scan: " +
                input.string();
            return false;
        }
        report = load_artifact_report(expanded);
    } else {
        const auto mount = temporary.path / "mount";
        fs::create_directories(mount, create_error);
        if (create_error) {
            error = "could not create temporary disk image mount point";
            return false;
        }
        const auto attach = "hdiutil attach -readonly -nobrowse -mountpoint " +
            shell_quote(mount) + " " + shell_quote(input) + " >/dev/null 2>&1";
        if (run(attach) != 0) {
            error = "could not mount disk image for inspector evidence scan: " +
                input.string();
            return false;
        }
        report = load_artifact_report(mount);
        const auto detach = "hdiutil detach " + shell_quote(mount) +
            " >/dev/null 2>&1";
        if (run(detach) != 0) {
            error = "could not detach disk image after inspector evidence scan: " +
                input.string();
            return false;
        }
    }
    if (!report.complete) {
        error = report.error.empty()
            ? "inspector capability evidence is incomplete"
            : report.error;
        return false;
    }
    return true;
#endif
}

} // namespace pulp::cli::inspector_shipping
