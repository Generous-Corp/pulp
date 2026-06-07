// Reusable session-bus D-Bus client + the Linux xdg-desktop-portal file-dialog
// backend it powers (#301 / L6).
//
// What runs everywhere:
//   - file_uri_to_path(): pure URI→path decoding.
//   - DBus availability/connect honest-fail contract (off Linux: always false).
//   - FileDialog::install_native_backend() honest-fail + idempotency.
//
// The live dialog success path needs a portal service AND user interaction, so
// it is NOT unit-testable. We DO verify the honest-fail-without-a-portal path,
// but only when PULP_TEST_LINUX_PORTAL_ABSENT is set (the tartci VM has libdbus
// + a session bus but no xdg-desktop-portal) — otherwise installing the portal
// backend and calling a dialog method on a portal-equipped desktop would raise
// a real blocking picker.

#include <catch2/catch_test_macros.hpp>

#include <pulp/platform/dbus.hpp>
#include <pulp/platform/file_dialog.hpp>

#include <cstdlib>
#include <string>

using pulp::platform::DBus;
using pulp::platform::FileDialog;
using pulp::platform::FileFilter;
using pulp::platform::file_uri_to_path;

TEST_CASE("file_uri_to_path decodes file:// URIs", "[platform][dbus][file-dialog][issue-301]") {
    // file:///abs/path → /abs/path
    REQUIRE(file_uri_to_path("file:///home/user/song.wav") == "/home/user/song.wav");
    // file://host/path keeps the leading slash of the path component
    REQUIRE(file_uri_to_path("file://localhost/tmp/a.flac") == "/tmp/a.flac");
    // Percent-decoding (space, parens, unicode bytes).
    REQUIRE(file_uri_to_path("file:///tmp/My%20Song%20(1).wav") == "/tmp/My Song (1).wav");
    REQUIRE(file_uri_to_path("file:///tmp/%E2%99%AA.wav") ==
            std::string("/tmp/\xE2\x99\xAA.wav"));
    // A literal percent that isn't a valid escape is passed through untouched.
    REQUIRE(file_uri_to_path("file:///tmp/100%done.wav") == "/tmp/100%done.wav");
    // Non-file:// input is returned unchanged.
    REQUIRE(file_uri_to_path("/already/a/path") == "/already/a/path");
    REQUIRE(file_uri_to_path("https://example.com/x") == "https://example.com/x");
    REQUIRE(file_uri_to_path("") == "");
}

TEST_CASE("DBus availability + connect honest-fail contract", "[platform][dbus][issue-301]") {
    const bool avail = DBus::library_available();

    DBus bus;
    REQUIRE_FALSE(bus.connected());
    const bool connected = bus.connect_session();

#if defined(__linux__)
    // On Linux connect can only succeed if libdbus loaded; it may still fail
    // (no session bus in a bare CI container), which is fine.
    if (connected) {
        REQUIRE(avail);
        REQUIRE(bus.connected());
        // Idempotent.
        REQUIRE(bus.connect_session());
    } else {
        REQUIRE_FALSE(bus.connected());
    }
#else
    // Off Linux every call is an honest no-op.
    REQUIRE_FALSE(avail);
    REQUIRE_FALSE(connected);
    REQUIRE_FALSE(bus.connected());
#endif
}

TEST_CASE("FileDialog::install_native_backend honest-fail + idempotency",
          "[platform][dbus][file-dialog][issue-301]") {
    FileDialog::clear_backend();

    const bool installed = FileDialog::install_native_backend();

#if defined(__linux__)
    // Installs iff libdbus is loadable. has_backend() then matches.
    REQUIRE(installed == DBus::library_available());
    REQUIRE(FileDialog::has_backend() == installed);
    // Idempotent — a second call leaves the backend in place.
    REQUIRE(FileDialog::install_native_backend() == installed);
#elif defined(__APPLE__)
    // macOS has a compiled-in native impl; install just reports has_backend().
    REQUIRE(installed == FileDialog::has_backend());
#else
    // iOS / Windows / Android: no built-in backend.
    REQUIRE_FALSE(installed);
    REQUIRE_FALSE(FileDialog::has_backend());
#endif

    FileDialog::clear_backend();
}

#if !defined(__APPLE__)
// Apple routes the dialog methods to the native impl (file_dialog_mac.mm), not
// the host backend, so this routing assertion — and its open_file() call, which
// would raise a real NSOpenPanel — only makes sense off Apple.
TEST_CASE("FileDialog::install_native_backend preserves a host-set backend",
          "[platform][dbus][file-dialog][issue-301]") {
    FileDialog::clear_backend();

    FileDialog::Backend host;
    bool host_called = false;
    host.open_file = [&](const std::string&, const std::vector<FileFilter>&,
                         const std::string&) {
        host_called = true;
        return std::optional<std::string>("/host/picked.wav");
    };
    FileDialog::set_backend(host);

    // install_native_backend() must not clobber a host-registered backend.
    FileDialog::install_native_backend();
    auto picked = FileDialog::open_file("t", {}, "");
    REQUIRE(host_called);
    REQUIRE(picked.has_value());
    REQUIRE(*picked == "/host/picked.wav");

    FileDialog::clear_backend();
}
#endif // !defined(__APPLE__)

#if defined(__linux__)
TEST_CASE("Linux portal backend honest-fails when no portal is running",
          "[platform][dbus][file-dialog][issue-301][linux]") {
    // Gated on PULP_TEST_LINUX_PORTAL_ABSENT so we never raise a real (blocking)
    // dialog on a portal-equipped developer desktop. The tartci VM sets it.
    if (std::getenv("PULP_TEST_LINUX_PORTAL_ABSENT") == nullptr) {
        SUCCEED("skipped: set PULP_TEST_LINUX_PORTAL_ABSENT on a host without xdg-desktop-portal");
        return;
    }
    if (!DBus::library_available()) {
        SUCCEED("skipped: libdbus not available");
        return;
    }

    FileDialog::clear_backend();
    REQUIRE(FileDialog::install_native_backend());
    REQUIRE(FileDialog::has_backend());

    // No portal service → every method honest-fails (no hang, no crash).
    REQUIRE_FALSE(FileDialog::open_file("Open", {}, "").has_value());
    REQUIRE(FileDialog::open_files("Open", {}, "").empty());
    REQUIRE_FALSE(FileDialog::save_file("Save", {}, "", "preset.pulp").has_value());
    REQUIRE_FALSE(FileDialog::choose_folder("Folder", "").has_value());

    FileDialog::clear_backend();
}
#endif
