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

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <dlfcn.h>
#endif

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

TEST_CASE("DBus connect_system honest-fail contract",
          "[platform][dbus][system][issue-3801]") {
    const bool avail = DBus::library_available();

    DBus bus;
    REQUIRE_FALSE(bus.connected());
    const bool connected = bus.connect_system();

#if defined(__linux__)
    // On Linux connect_system can only succeed if libdbus loaded; it may still
    // fail (no system bus reachable in a bare CI container), which is fine.
    if (connected) {
        REQUIRE(avail);
        REQUIRE(bus.connected());
        REQUIRE(bus.connect_system());  // idempotent
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

TEST_CASE("DBus signal subscription honest-fails without a bus",
          "[platform][dbus][system][signal][issue-3801]") {
    DBus bus;  // never connected
    REQUIRE_FALSE(bus.connected());
    REQUIRE(bus.add_signal_handler("pulp.Test", "Pinged",
                                   [](const std::string&, const std::string&,
                                      const std::string&, DBus::Reader&) {}) == 0u);
    REQUIRE_FALSE(bus.remove_signal_handler(1u));
    REQUIRE_FALSE(bus.get_managed_objects(
        "org.example", "/", [](const std::string&, const std::string&) {}));
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
#elif defined(_WIN32)
    // Windows ships an opt-in IFileDialog backend.
    REQUIRE(installed);
    REQUIRE(FileDialog::has_backend());
    REQUIRE(FileDialog::install_native_backend());
#else
    // iOS / Android: no built-in backend.
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

// ── Object-server layer (L7a-1) ─────────────────────────────────────────────
//
// Generic, AT-SPI-agnostic D-Bus object server. The CI-verifiable proof is a
// same-process loopback: register an object that answers Echo(s)->s, then make a
// blocking method call to our OWN unique bus name + path, dispatch() to service
// the incoming call, and assert the reply round-tripped. Runs under
// `dbus-run-session -- ctest` (ephemeral private session bus, headless). Off
// Linux / without a bus, every object-server method honest-fails (false/empty).

TEST_CASE("DBus object-server honest-fails without a bus",
          "[platform][dbus][objectserver][issue-L7a1]") {
    DBus bus;  // never connected
    REQUIRE_FALSE(bus.connected());
    REQUIRE(bus.unique_name().empty());
    REQUIRE_FALSE(bus.register_object("/pulp/test", [](DBus::CallContext&) { return true; }));
    REQUIRE_FALSE(bus.unregister_object("/pulp/test"));
    REQUIRE_FALSE(bus.emit_signal("/pulp/test", "pulp.Test", "Ping",
                                  [](DBus::Writer&) {}));
    REQUIRE_FALSE(bus.dispatch(0));
    // a11y-bus switch (L7a-2): honest-fails without any bus at all.
    REQUIRE_FALSE(bus.a11y_connected());
    REQUIRE_FALSE(bus.connect_a11y_bus());
}

// L7a-2: switching a connected session DBus over to the accessibility bus.
// CI-verifiable shape: on a headless session bus (dbus-run-session) there is no
// org.a11y.Bus daemon, so connect_a11y_bus() must honest-fail (return false)
// WITHOUT tearing down the usable session connection. A real desktop with
// at-spi2-core running is the VM lane that exercises the success path.
TEST_CASE("DBus connect_a11y_bus honest-fails without an a11y daemon",
          "[platform][dbus][objectserver][a11y][issue-L7a2]") {
    if (!DBus::library_available()) {
        SKIP("libdbus not available");
    }
    DBus bus;
    if (!bus.connect_session()) {
        SKIP("no session bus (run under dbus-run-session)");
    }
    REQUIRE(bus.connected());
    REQUIRE_FALSE(bus.a11y_connected());

    // Under dbus-run-session there is no org.a11y.Bus; GetAddress fails and the
    // switch is declined. (On a desktop with at-spi2-core this would return
    // true and a11y_connected() would flip — the VM lane covers that.)
    if (bus.connect_a11y_bus()) {
        // An a11y daemon happened to be reachable (developer desktop): the
        // contract still holds — the active connection is now the a11y bus.
        REQUIRE(bus.a11y_connected());
        REQUIRE(bus.connected());
        return;
    }
    // Honest-fail path: declined, but the session connection is untouched.
    REQUIRE_FALSE(bus.a11y_connected());
}

#if defined(__linux__)
namespace {
// Minimal raw-libdbus client used ONLY by the loopback test to issue a blocking
// Echo(s)->s call at an arbitrary bus name. The production DBus facade exposes
// only the object-SERVER surface for L7a-1 (a generic client call is a later
// concern), so the test drives the client half through dlopen'd libdbus
// directly — proving a genuine cross-connection round-trip over the session bus.
struct RawClient {
    void* h = nullptr;
    void* (*bus_get_private)(int, void*) = nullptr;
    void (*conn_close)(void*) = nullptr;
    void (*conn_unref)(void*) = nullptr;
    void (*set_exit)(void*, unsigned) = nullptr;
    void (*error_init)(void*) = nullptr;
    void (*error_free)(void*) = nullptr;
    unsigned (*error_is_set)(void*) = nullptr;
    void* (*new_call)(const char*, const char*, const char*, const char*) = nullptr;
    void (*msg_unref)(void*) = nullptr;
    void (*iter_init_append)(void*, void*) = nullptr;
    unsigned (*iter_append)(void*, int, const void*) = nullptr;
    unsigned (*iter_init)(void*, void*) = nullptr;
    int (*iter_argtype)(void*) = nullptr;
    void (*iter_getbasic)(void*, void*) = nullptr;
    void* (*send_block)(void*, void*, int, void*) = nullptr;

    template <typename Fn> Fn sym(const char* n) { return reinterpret_cast<Fn>(dlsym(h, n)); }

    bool load() {
        h = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_LOCAL);
        if (!h) h = dlopen("libdbus-1.so", RTLD_LAZY | RTLD_LOCAL);
        if (!h) return false;
        bus_get_private = sym<decltype(bus_get_private)>("dbus_bus_get_private");
        conn_close = sym<decltype(conn_close)>("dbus_connection_close");
        conn_unref = sym<decltype(conn_unref)>("dbus_connection_unref");
        set_exit = sym<decltype(set_exit)>("dbus_connection_set_exit_on_disconnect");
        error_init = sym<decltype(error_init)>("dbus_error_init");
        error_free = sym<decltype(error_free)>("dbus_error_free");
        error_is_set = sym<decltype(error_is_set)>("dbus_error_is_set");
        new_call = sym<decltype(new_call)>("dbus_message_new_method_call");
        msg_unref = sym<decltype(msg_unref)>("dbus_message_unref");
        iter_init_append = sym<decltype(iter_init_append)>("dbus_message_iter_init_append");
        iter_append = sym<decltype(iter_append)>("dbus_message_iter_append_basic");
        iter_init = sym<decltype(iter_init)>("dbus_message_iter_init");
        iter_argtype = sym<decltype(iter_argtype)>("dbus_message_iter_get_arg_type");
        iter_getbasic = sym<decltype(iter_getbasic)>("dbus_message_iter_get_basic");
        send_block = sym<decltype(send_block)>("dbus_connection_send_with_reply_and_block");
        return bus_get_private && conn_close && conn_unref && set_exit && error_init &&
               error_free && error_is_set && new_call && msg_unref && iter_init_append &&
               iter_append && iter_init && iter_argtype && iter_getbasic && send_block;
    }
};
}  // namespace

TEST_CASE("DBus object-server loopback round-trips a method call",
          "[platform][dbus][objectserver][issue-L7a1][linux]") {
    if (!DBus::library_available()) {
        SKIP("libdbus not available");
    }

    // Server connection: exports /pulp/test answering Echo(s)->s and Fail()->error.
    DBus server;
    if (!server.connect_session()) {
        SKIP("no session bus (run under dbus-run-session)");
    }
    REQUIRE(server.connected());
    const std::string server_name = server.unique_name();
    REQUIRE_FALSE(server_name.empty());

    std::atomic<bool> handler_saw_call{false};
    REQUIRE(server.register_object(
        "/pulp/test",
        [&](DBus::CallContext& ctx) -> bool {
            handler_saw_call = true;
            if (ctx.member() == "Echo") {
                std::string in;
                if (!ctx.args().read_string(in)) {
                    ctx.error("org.freedesktop.DBus.Error.InvalidArgs", "expected s");
                    return true;
                }
                DBus::Writer w = ctx.reply();
                w.append_string(in + ":echoed");
                return true;
            }
            if (ctx.member() == "Fail") {
                ctx.error("pulp.Test.Error.Boom", "kaboom");
                return true;
            }
            return false;  // decline → server replies UnknownMethod
        }));

    // emit_signal builds + sends (no subscriber needed to prove marshalling).
    REQUIRE(server.emit_signal(
        "/pulp/test", "pulp.Test", "Pinged",
        [](DBus::Writer& w) { w.append_string("hello"); }));

    RawClient rc;
    REQUIRE(rc.load());

    // The blocking call (worker thread) and the server dispatcher (main thread)
    // run concurrently: send_with_reply_and_block pumps only the CLIENT
    // connection, so the SERVER connection must dispatch() to route + answer the
    // incoming call. Without the concurrent dispatch the call would time out.
    std::string echoed;
    std::atomic<bool> call_done{false};
    std::atomic<bool> call_ok{false};

    std::thread worker([&] {
        unsigned char errbuf[64];
        unsigned char itbuf[128];
        rc.error_init(errbuf);
        void* conn = rc.bus_get_private(/*DBUS_BUS_SESSION*/ 0, errbuf);
        if (!conn || rc.error_is_set(errbuf)) {
            if (conn) { rc.conn_close(conn); rc.conn_unref(conn); }
            rc.error_free(errbuf);
            call_done = true;
            return;
        }
        rc.set_exit(conn, 0u);

        void* msg = rc.new_call(server_name.c_str(), "/pulp/test",
                                "pulp.Test", "Echo");
        const char* arg = "ping";
        rc.iter_init_append(msg, itbuf);
        rc.iter_append(itbuf, /*'s'*/ 's', &arg);

        rc.error_init(errbuf);
        void* reply = rc.send_block(conn, msg, 5000, errbuf);
        rc.msg_unref(msg);
        if (reply && !rc.error_is_set(errbuf)) {
            unsigned char rit[128];
            if (rc.iter_init(reply, rit) && rc.iter_argtype(rit) == /*'s'*/ 's') {
                const char* s = nullptr;
                rc.iter_getbasic(rit, &s);
                if (s) { echoed = s; call_ok = true; }
            }
        }
        if (reply) rc.msg_unref(reply);
        rc.error_free(errbuf);
        rc.conn_close(conn);
        rc.conn_unref(conn);
        call_done = true;
    });

    // Service the incoming call on the server connection until the worker
    // observes its reply (bounded so a wedge fails the test instead of hanging).
    for (int i = 0; i < 200 && !call_done.load(); ++i) {
        server.dispatch(25);
    }
    worker.join();

    REQUIRE(call_ok.load());
    REQUIRE(echoed == "ping:echoed");
    REQUIRE(handler_saw_call.load());

    REQUIRE(server.unregister_object("/pulp/test"));
    // Re-registration after teardown works (handler-replace contract).
    REQUIRE(server.register_object("/pulp/test",
                                   [](DBus::CallContext&) { return true; }));
    REQUIRE(server.unregister_object("/pulp/test"));
}
#else  // ── object-server loopback: Linux-only runtime; stub elsewhere ────────
TEST_CASE("DBus object-server loopback round-trips a method call",
          "[platform][dbus][objectserver][issue-L7a1]") {
    SKIP("D-Bus object server is a Linux-only runtime backend");
}
#endif

#if defined(__linux__)
// Signal subscription loopback (PR1 / #3801): one DBus emits a signal, a SECOND
// DBus subscribed to the same interface+member receives it via the connection
// filter during dispatch(). Two connections on the same session bus prove the
// broker actually delivers the broadcast (a same-connection self-signal is not
// guaranteed to loop back). Runs under `dbus-run-session -- ctest`.
TEST_CASE("DBus signal subscription receives a matching broadcast",
          "[platform][dbus][system][signal][issue-3801][linux]") {
    if (!DBus::library_available()) {
        SKIP("libdbus not available");
    }

    DBus subscriber;
    DBus emitter;
    if (!subscriber.connect_session() || !emitter.connect_session()) {
        SKIP("no session bus (run under dbus-run-session)");
    }
    REQUIRE(subscriber.connected());
    REQUIRE(emitter.connected());

    std::atomic<bool> fired{false};
    std::string got_path, got_iface, got_member, got_arg;
    int got_int = 0;

    const unsigned token = subscriber.add_signal_handler(
        "pulp.Test.Signal", "Pinged",
        [&](const std::string& path, const std::string& iface,
            const std::string& member, DBus::Reader& args) {
            got_path = path;
            got_iface = iface;
            got_member = member;
            std::string s;
            if (args.read_string(s)) got_arg = s;
            int n = 0;
            if (args.read_int32(n)) got_int = n;
            fired = true;
        });
    REQUIRE(token != 0u);

    // A non-matching subscription on the same interface (different member) must
    // NOT fire for the Pinged signal.
    std::atomic<bool> wrong_member_fired{false};
    const unsigned token2 = subscriber.add_signal_handler(
        "pulp.Test.Signal", "Ponged",
        [&](const std::string&, const std::string&, const std::string&,
            DBus::Reader&) { wrong_member_fired = true; });
    REQUIRE(token2 != 0u);

    REQUIRE(emitter.emit_signal(
        "/pulp/test/signal", "pulp.Test.Signal", "Pinged",
        [](DBus::Writer& w) { w.append_string("hi"); w.append_int32(42); }));

    // Pump the subscriber until the signal is observed (bounded so a wedge fails
    // the test instead of hanging).
    for (int i = 0; i < 200 && !fired.load(); ++i) {
        subscriber.dispatch(25);
    }

    REQUIRE(fired.load());
    REQUIRE(got_path == "/pulp/test/signal");
    REQUIRE(got_iface == "pulp.Test.Signal");
    REQUIRE(got_member == "Pinged");
    REQUIRE(got_arg == "hi");
    REQUIRE(got_int == 42);
    REQUIRE_FALSE(wrong_member_fired.load());

    REQUIRE(subscriber.remove_signal_handler(token));
    REQUIRE(subscriber.remove_signal_handler(token2));
    REQUIRE_FALSE(subscriber.remove_signal_handler(token));  // already removed

    // After removal a fresh broadcast is no longer routed to the handler.
    fired = false;
    REQUIRE(emitter.emit_signal(
        "/pulp/test/signal", "pulp.Test.Signal", "Pinged",
        [](DBus::Writer& w) { w.append_string("again"); w.append_int32(7); }));
    for (int i = 0; i < 40; ++i) subscriber.dispatch(25);
    REQUIRE_FALSE(fired.load());
}

// get_managed_objects shape (PR1 / #3801): export a mock ObjectManager that
// answers GetManagedObjects()->a{oa{sa{sv}}}, then drive get_managed_objects()
// from a SECOND connection and assert every (object, interface) pair is reported
// and that property values are correctly skipped during the walk.
TEST_CASE("DBus get_managed_objects walks a mock ObjectManager reply",
          "[platform][dbus][system][objectmanager][issue-3801][linux]") {
    if (!DBus::library_available()) {
        SKIP("libdbus not available");
    }

    DBus server;
    DBus client;
    if (!server.connect_session() || !client.connect_session()) {
        SKIP("no session bus (run under dbus-run-session)");
    }
    const std::string server_name = server.unique_name();
    REQUIRE_FALSE(server_name.empty());

    // Export /pulp/om answering ObjectManager.GetManagedObjects with two objects:
    //   /pulp/om/dev1 → { "pulp.Device": { "Name": <string "alpha"> },
    //                     "pulp.Battery": {} }
    //   /pulp/om/dev2 → { "pulp.Device": {} }
    REQUIRE(server.register_object(
        "/pulp/om", [&](DBus::CallContext& ctx) -> bool {
            if (ctx.interface() != "org.freedesktop.DBus.ObjectManager" ||
                ctx.member() != "GetManagedObjects") {
                return false;
            }
            DBus::Writer w = ctx.reply();
            // a{oa{sa{sv}}}
            auto objs = w.open_array("{oa{sa{sv}}}");
            DBus::Writer ow = w.sub(objs);

            auto add_object =
                [&](const char* path,
                    const std::vector<std::pair<std::string, bool>>& ifaces) {
                    auto oe = ow.open_dict_entry();
                    DBus::Writer oew = ow.sub(oe);
                    oew.append_object_path(path);
                    auto ifarr = oew.open_array("{sa{sv}}");
                    DBus::Writer iw = oew.sub(ifarr);
                    for (const auto& [iface, with_prop] : ifaces) {
                        auto ie = iw.open_dict_entry();
                        DBus::Writer iew = iw.sub(ie);
                        iew.append_string(iface);
                        auto props = iew.open_array("{sv}");
                        DBus::Writer pw = iew.sub(props);
                        if (with_prop) {
                            auto pe = pw.open_dict_entry();
                            DBus::Writer pew = pw.sub(pe);
                            pew.append_string("Name");
                            auto var = pew.open_variant("s");
                            pew.sub(var).append_string("alpha");
                            pew.close_container(var);
                            pw.close_container(pe);
                        }
                        iew.close_container(props);
                        iw.close_container(ie);
                    }
                    oew.close_container(ifarr);
                    ow.close_container(oe);
                };

            add_object("/pulp/om/dev1",
                       {{"pulp.Device", true}, {"pulp.Battery", false}});
            add_object("/pulp/om/dev2", {{"pulp.Device", false}});

            w.close_container(objs);
            return true;
        }));

    std::vector<std::pair<std::string, std::string>> pairs;
    std::atomic<bool> call_done{false};
    std::atomic<bool> call_ok{false};
    std::thread worker([&] {
        call_ok = client.get_managed_objects(
            server_name, "/pulp/om",
            [&](const std::string& obj, const std::string& iface) {
                pairs.emplace_back(obj, iface);
            });
        call_done = true;
    });
    for (int i = 0; i < 200 && !call_done.load(); ++i) server.dispatch(25);
    worker.join();

    REQUIRE(call_ok.load());
    // Three (object, interface) pairs, properties skipped without disrupting the walk.
    REQUIRE(pairs.size() == 3u);
    auto has = [&](const std::string& o, const std::string& i) {
        return std::find(pairs.begin(), pairs.end(),
                         std::make_pair(o, i)) != pairs.end();
    };
    REQUIRE(has("/pulp/om/dev1", "pulp.Device"));
    REQUIRE(has("/pulp/om/dev1", "pulp.Battery"));
    REQUIRE(has("/pulp/om/dev2", "pulp.Device"));

    REQUIRE(server.unregister_object("/pulp/om"));
}

TEST_CASE("Linux portal backend honest-fails when no portal is running",
          "[platform][dbus][file-dialog][issue-301][linux]") {
    // Gated on PULP_TEST_LINUX_PORTAL_ABSENT so we never raise a real (blocking)
    // dialog on a portal-equipped developer desktop. The tartci VM sets it.
    if (std::getenv("PULP_TEST_LINUX_PORTAL_ABSENT") == nullptr) {
        SKIP("set PULP_TEST_LINUX_PORTAL_ABSENT on a host without xdg-desktop-portal");
    }
    if (!DBus::library_available()) {
        SKIP("libdbus not available");
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
