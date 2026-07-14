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
#include <memory>
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

    /// Connect to the SYSTEM bus. System services (BlueZ / org.bluez, UPower,
    /// NetworkManager, …) live on the system bus, not the per-login session bus,
    /// so a client that needs to talk to them connects here instead of
    /// connect_session(). Returns false off Linux, without libdbus, or when no
    /// system bus is reachable. Idempotent (returns true if already connected).
    /// A given DBus connects to exactly one bus — call this XOR connect_session().
    bool connect_system();

    bool connected() const;

    /// Switch this DBus over to the desktop accessibility ("a11y") bus.
    ///
    /// The a11y bus is a SECOND D-Bus daemon, separate from the session bus:
    /// its address is discovered by calling org.a11y.Bus.GetAddress on the
    /// session bus, then a fresh private connection is opened to that address.
    /// AT-SPI (Orca and friends) lives entirely on this bus — the registry, the
    /// app's exported accessible objects, and the event traffic all flow over
    /// it, never the session bus.
    ///
    /// After this returns true, the connection backing register_object /
    /// unregister_object / emit_signal / dispatch / unique_name is the a11y
    /// connection (the session connection used to discover the address is
    /// closed). connect_session() must have succeeded first.
    ///
    /// Returns false off Linux, without libdbus, when there is no session bus,
    /// when org.a11y.Bus is absent (a headless host with no a11y daemon), or
    /// when the returned address cannot be opened — i.e. honest-fail, so a
    /// non-accessibility desktop simply yields a no-op provider.
    bool connect_a11y_bus();

    /// True iff this DBus is currently backed by the a11y bus connection (i.e.
    /// connect_a11y_bus() succeeded). False on the plain session connection.
    bool a11y_connected() const;

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
        /// Default-constructed inert Reader — every read returns false. Useful
        /// as the out-parameter passed to recurse().
        Reader() = default;

        /// Read a string ('s') or object-path ('o') at the cursor into `out` and
        /// advance. Returns false (leaving the cursor put) on type mismatch / end.
        bool read_string(std::string& out);
        bool read_int32(int& out);
        bool read_uint32(unsigned& out);
        bool read_double(double& out);
        /// Read a byte ('y') at the cursor into `out` and advance. Needed to
        /// walk an 'ay' byte array such as BlueZ GattCharacteristic1.Value.
        bool read_byte(unsigned char& out);
        /// Current argument's D-Bus type code, or 0 ('\0') when exhausted.
        int arg_type() const;
        /// Advance to the next argument. Returns false when there is no next.
        bool next();

        /// Descend into the container (struct / array / variant / dict-entry)
        /// at the cursor, returning a sub-Reader over its elements. The cursor
        /// is NOT advanced — call next() afterwards to step past the container.
        /// `out_sub` receives the sub-Reader; returns false if the cursor is not
        /// on a container (or at end). The sub-Reader is valid only while this
        /// Reader and the underlying message live.
        bool recurse(Reader& out_sub);

    private:
        friend class DBus;
        Reader(DBus* owner, void* iter) : owner_(owner), iter_(iter) {}
        // A recurse() sub-iterator owns its DBusMessageIter buffer; the parent
        // hands ownership to the sub-Reader so it outlives the call returning it.
        DBus* owner_ = nullptr;
        void* iter_ = nullptr;   // DBusMessageIter* (non-owning view)
        std::shared_ptr<void> owned_iter_;  // set only for recurse() sub-readers
    };

    /// Appends values to an outgoing message (reply or signal). Container open/
    /// close calls must be balanced; the matching close uses the cursor handed
    /// back by the open.
    class Writer {
    public:
        bool append_string(const std::string& s);
        bool append_object_path(const std::string& p);
        bool append_bool(bool v);
        bool append_int32(int v);
        bool append_uint32(unsigned v);
        bool append_double(double v);
        /// Append a byte ('y'). Needed to build an 'ay' byte array such as the
        /// value passed to BlueZ GattCharacteristic1.WriteValue.
        bool append_byte(unsigned char v);

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
    /// true if the call was recognized + answered (via ctx.reply()/error()),
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

    /// Generic blocking method call (object-server companion to register_object).
    /// Sends `member` on `destination`/`path`/`interface`, marshalling the body
    /// via `build_args`, and blocks up to `timeout_ms` for the reply; the reply
    /// arguments are handed to `read_reply` (may be null to ignore the body).
    /// Returns false off Linux / without a connection / on transport or D-Bus
    /// error (e.g. the peer returned an error reply). This is the call needed to
    /// drive a handshake such as AT-SPI's Socket.Embed where the app both
    /// exports objects AND calls out to a registry.
    bool call_method(const std::string& destination, const std::string& path,
                     const std::string& interface, const std::string& member,
                     const std::function<void(Writer&)>& build_args,
                     const std::function<void(Reader&)>& read_reply,
                     int timeout_ms = 5000);

    // ── Signal subscription (broadcast, BYPASSES the object-path vtable) ─────
    //
    // D-Bus signals are broadcast messages, not method calls: they never reach
    // the object-path vtable / trampoline (which only routes method calls).
    // Receiving them requires (a) an org.freedesktop.DBus match rule so the
    // broker delivers them to this connection, and (b) a libdbus connection
    // FILTER that inspects each incoming message. Both are installed here.
    // Matching signals are routed to handlers during dispatch().

    /// Handler invoked for each matching signal. `args` is a Reader positioned at
    /// the signal body; it is valid only for the duration of the call.
    using SignalHandler = std::function<void(const std::string& path,
                                             const std::string& interface,
                                             const std::string& member,
                                             Reader& args)>;

    /// Subscribe to signals matching interface+member (member empty = any member
    /// of the interface). Adds the org.freedesktop.DBus match rule and routes
    /// matching signals to `handler` during dispatch(). Returns a token (>0) to
    /// pass to remove_signal_handler(); returns 0 off Linux / without a connection.
    unsigned add_signal_handler(const std::string& interface,
                                const std::string& member,
                                SignalHandler handler);
    /// Remove a signal handler previously returned by add_signal_handler().
    /// Returns false off Linux / for an unknown token. The match rule is left in
    /// place (harmless extra delivery; removing it precisely would require
    /// reference-counting identical rules across tokens).
    bool remove_signal_handler(unsigned token);

    /// Call org.freedesktop.DBus.ObjectManager.GetManagedObjects on
    /// destination/path and invoke `per_object(object_path, interface_name)` for
    /// every (object, interface) pair discovered. Property values are skipped
    /// (callers re-query specific props via call_method as needed). Returns false
    /// off Linux / on transport error.
    bool get_managed_objects(const std::string& destination,
                             const std::string& path,
                             const std::function<void(const std::string& obj_path,
                                                      const std::string& interface)>& per_object,
                             int timeout_ms = 5000);

    DBus(const DBus&) = delete;
    DBus& operator=(const DBus&) = delete;

private:
    /// Shared body of connect_session()/connect_system(): dlopen libdbus, resolve
    /// symbols, and open a private connection to `bus_type` (DBUS_BUS_SESSION = 0,
    /// DBUS_BUS_SYSTEM = 1). Idempotent via connected(). Linux-only; the non-Linux
    /// build never calls it (connect_session/connect_system are no-ops there).
    bool connect_bus(int bus_type);

    struct Impl;
    Impl* impl_ = nullptr;
};

/// Convert a file:// URI to a local filesystem path (percent-decoded). Returns
/// the input unchanged if it has no file:// scheme. Pure + testable everywhere.
std::string file_uri_to_path(const std::string& uri);

}  // namespace pulp::platform
