#pragma once

// Minimal session-bus D-Bus client over libdbus-1, loaded at runtime (no
// build-time libdbus-dev dependency, mirroring the libudev approach). Linux is
// the only real backend; on other platforms every call is an honest no-op
// (library_available() / connect_session() return false). Lives in
// core/platform — the lowest layer — so any subsystem can use it for
// session-bus IPC (xdg-desktop-portal file chooser / screenshot / notifications,
// etc.) without a build-time dependency or a layering cycle.
//
// Two consumption models live side by side here, deliberately kept separate:
//
//   * Portal CLIENT (file_chooser): a method call is sent and the reply / a
//     matching signal is awaited by *popping* messages off the incoming queue
//     (read_write + pop_message). pop_message removes a message BEFORE the
//     libdbus object-path dispatcher would ever see it.
//
//   * Object SERVER (register_object / emit_signal / dispatch): the app EXPORTS
//     objects and answers incoming method calls. This requires the libdbus
//     *dispatcher* to run so registered handlers fire — driven by dispatch(),
//     which calls dbus_connection_read_write_dispatch(). It must NOT be mixed
//     with the portal pop_message pump on the same pending traffic: pop_message
//     would steal the very method call a registered handler is meant to answer.
//
// The object-server layer is AT-SPI-agnostic generic infrastructure: it answers
// any registered object path, and is independently useful for MPRIS, desktop
// notifications, or AT-SPI accessibility export.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pulp::platform {

class DBus {
public:
    DBus();
    ~DBus();

    /// True iff libdbus-1 can be loaded right now (Linux dlopen probe). Always
    /// false off Linux. Cheap; does not connect.
    static bool library_available();

    /// Connect to the session bus. Returns false off Linux, without libdbus, or
    /// when no session bus is reachable. Idempotent (returns true if already
    /// connected).
    bool connect_session();
    bool connected() const;

    /// This connection's unique bus name (e.g. ":1.42"), assigned by the broker
    /// at connect time. Empty when not connected / off Linux. Needed so a peer
    /// (or this same process, in a loopback test) can address method calls at us.
    std::string unique_name() const;

    /// Reply value for an xdg-desktop-portal request: the response code
    /// (0 = success, 1 = user cancelled, 2 = other) plus any returned URIs.
    struct PortalResult {
        int response = 2;                  // portal "response" code
        std::vector<std::string> uris;     // results["uris"] (file:// URIs)
    };

    /// Invoke org.freedesktop.portal.FileChooser.<method> ("OpenFile" or
    /// "SaveFile") and block until the portal's Response signal arrives (or
    /// `timeout_ms` elapses). `options` are string→string entries marshalled as
    /// the portal's a{sv} with string variants; `bool_options` are marshalled as
    /// boolean variants (e.g. {"multiple", true} / {"directory", true}).
    /// Returns std::nullopt on any transport error (no portal, disconnect, …).
    std::optional<PortalResult> file_chooser(
        const std::string& method,
        const std::string& title,
        const std::map<std::string, std::string>& options,
        const std::map<std::string, bool>& bool_options,
        int timeout_ms = 120000);

    // ── Object-server layer (generic, AT-SPI-agnostic) ──────────────────────
    //
    // A thin facade over the libdbus marshalling iterator. Reader walks an
    // incoming message's arguments; Writer appends arguments to a reply or a
    // signal. Both are non-owning views over the underlying DBusMessageIter and
    // are only valid for the duration of the call that produced them.

    /// Reads basic + container values out of an incoming message's argument
    /// list. The cursor advances on each successful read.
    class Reader {
    public:
        /// Read a string ('s') or object-path ('o') at the cursor into `out` and
        /// advance. Returns false (leaving the cursor put) on type mismatch / end.
        bool read_string(std::string& out);
        bool read_int32(int& out);
        bool read_uint32(unsigned& out);
        bool read_double(double& out);
        /// Current argument's D-Bus type code, or 0 ('\0') when exhausted.
        int arg_type() const;
        /// Advance to the next argument. Returns false when there is no next.
        bool next();

    private:
        friend class DBus;
        Reader(DBus* owner, void* iter) : owner_(owner), iter_(iter) {}
        DBus* owner_ = nullptr;
        void* iter_ = nullptr;   // DBusMessageIter*
    };

    /// Appends values to an outgoing message (reply or signal). Container open/
    /// close calls must be balanced; the matching close uses the cursor handed
    /// back by the open.
    class Writer {
    public:
        bool append_string(const std::string& s);
        bool append_object_path(const std::string& p);
        bool append_int32(int v);
        bool append_uint32(unsigned v);
        bool append_double(double v);

        /// A nested container being built. `iter` is the sub-iterator; pass it to
        /// the matching close_container().
        struct Container { void* iter = nullptr; bool open = false; };

        /// Open an array. `element_signature` is the D-Bus signature of the
        /// element type (e.g. "s", "(so)", "{sv}"). The returned Container's
        /// own writer methods append into the array.
        Container open_array(const std::string& element_signature);
        /// Open a struct ("(...)" — no signature string for libdbus structs).
        Container open_struct();
        /// Open a variant holding a single value of `value_signature` (e.g. "s").
        Container open_variant(const std::string& value_signature);
        /// Open a dict-entry (only valid inside an a{..} array).
        Container open_dict_entry();
        bool close_container(Container& c);

        /// Append into an already-open container instead of the top level.
        Writer sub(Container& c);

    private:
        friend class DBus;
        Writer(DBus* owner, void* iter) : owner_(owner), iter_(iter) {}
        DBus* owner_ = nullptr;
        void* iter_ = nullptr;   // DBusMessageIter*
    };

    /// Context for one incoming method call against a registered object. The
    /// handler reads arguments via args(), and answers with EXACTLY ONE of
    /// reply() (success) or error() (failure). A handler that returns true
    /// without calling either yields an empty success reply; returning false
    /// makes the server emit org.freedesktop.DBus.Error.UnknownMethod.
    class CallContext {
    public:
        const std::string& path() const { return path_; }
        const std::string& interface() const { return interface_; }
        const std::string& member() const { return member_; }
        const std::string& sender() const { return sender_; }

        /// Argument reader over the incoming call.
        Reader& args() { return reader_; }

        /// Begin a success reply. Returns a Writer to append return values into.
        /// Calling reply() marks the call answered; the server sends it after the
        /// handler returns. May only be called once per call.
        Writer reply();
        /// Answer with a D-Bus error (e.g. "org.freedesktop.DBus.Error.Failed").
        /// Marks the call answered. Mutually exclusive with reply().
        void error(const std::string& name, const std::string& message);

    private:
        friend class DBus;
        CallContext(DBus* owner, void* msg, void* args_iter)
            : owner_(owner), msg_(msg), reader_(owner, args_iter) {}
        DBus* owner_ = nullptr;
        void* msg_ = nullptr;          // incoming DBusMessage*
        void* reply_msg_ = nullptr;    // DBusMessage* built by reply()/error()
        void* reply_iter_ = nullptr;   // heap append-iterator for reply(), freed
                                       // by the server after the message is sent
        bool answered_ = false;
        std::string path_, interface_, member_, sender_;
        Reader reader_;
    };

    /// Handler for incoming method calls on a registered object path. Return
    /// true if the call was recognised + answered (via ctx.reply()/error()),
    /// false to decline — the server then replies UnknownMethod. A call is
    /// NEVER dropped silently.
    using IncomingHandler = std::function<bool(CallContext& ctx)>;

    /// Export an object at `path`; `handler` answers method calls routed to it.
    /// Re-registering a path replaces its handler. Returns false off Linux /
    /// without a connection / if libdbus rejects the path.
    bool register_object(const std::string& path, IncomingHandler handler);
    /// Remove a previously registered object. Returns false if not registered.
    bool unregister_object(const std::string& path);

    /// Emit a signal from `path`/`interface`/`member`. `build_args` appends the
    /// signal body via the Writer. Returns false off Linux / without a
    /// connection / on marshalling or send failure.
    bool emit_signal(const std::string& path, const std::string& interface,
                     const std::string& member,
                     const std::function<void(Writer&)>& build_args);

    /// Run the libdbus dispatcher once so registered object handlers fire and
    /// outgoing traffic flushes. Blocks up to `timeout_ms` waiting for I/O
    /// (0 = non-blocking poll). Returns false off Linux / without a connection.
    /// This is the object-server consumption model; do NOT interleave it with
    /// the portal file_chooser pop_message pump on the same pending traffic.
    bool dispatch(int timeout_ms = 0);

    DBus(const DBus&) = delete;
    DBus& operator=(const DBus&) = delete;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

/// Convert a file:// URI to a local filesystem path (percent-decoded). Returns
/// the input unchanged if it has no file:// scheme. Pure + testable everywhere.
std::string file_uri_to_path(const std::string& uri);

}  // namespace pulp::platform
