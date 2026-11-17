#include "pulp/platform/dbus.hpp"

#include <cctype>

namespace pulp::platform {

// ── file:// URI → path (pure; available on every platform) ──────────────────
std::string file_uri_to_path(const std::string& uri) {
    const std::string scheme = "file://";
    if (uri.rfind(scheme, 0) != 0) return uri;
    // Strip scheme + optional host (file://host/path → /path; file:///path → /path).
    std::string rest = uri.substr(scheme.size());
    auto slash = rest.find('/');
    std::string path = (slash == std::string::npos) ? rest : rest.substr(slash);
    // Percent-decode.
    std::string out;
    out.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size() &&
            std::isxdigit(static_cast<unsigned char>(path[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(path[i + 2]))) {
            auto hex = [](char c) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return (c >= 'a') ? (c - 'a' + 10) : (c - '0');
            };
            out.push_back(static_cast<char>(hex(path[i + 1]) * 16 + hex(path[i + 2])));
            i += 2;
        } else {
            out.push_back(path[i]);
        }
    }
    return out;
}

}  // namespace pulp::platform

#if defined(__linux__)

#include <dlfcn.h>
#include <cstring>

namespace pulp::platform {

namespace {
// D-Bus type codes + bus type (avoid the build-time dbus header).
constexpr int kTypeByte    = 'y';
constexpr int kTypeString  = 's';
constexpr int kTypeObjPath = 'o';
constexpr int kTypeBool    = 'b';
constexpr int kTypeInt32   = 'i';
constexpr int kTypeUInt32  = 'u';
constexpr int kTypeDouble  = 'd';
constexpr int kTypeArray   = 'a';
constexpr int kTypeStruct  = 'r';   // STRUCT type code (open_container takes 'r')
constexpr int kTypeVariant = 'v';
constexpr int kTypeDictEnt = 'e';
constexpr int kTypeInvalid = '\0';
constexpr int kBusSession  = 0;   // DBUS_BUS_SESSION
constexpr int kBusSystem   = 1;   // DBUS_BUS_SYSTEM

// DBusHandlerResult values (dbus-shared.h).
constexpr int kHandlerHandled       = 0;   // DBUS_HANDLER_RESULT_HANDLED
constexpr int kHandlerNotYetHandled = 1;   // DBUS_HANDLER_RESULT_NOT_YET_HANDLED
}  // namespace

// libdbus entry points resolved at runtime. Opaque structs as void*; the
// DBusMessageIter is an opaque fixed-size POD — libdbus documents it as a
// stack struct; we over-allocate a buffer to hold it across ABI versions.
struct DBus::Impl {
    void* handle = nullptr;       // dlopen handle
    void* conn = nullptr;         // DBusConnection* (session or, after
                                  // connect_a11y_bus(), the a11y bus)
    bool on_a11y = false;         // true once switched to the a11y bus

    // Path → handler routing table for exported objects. The trampoline recovers
    // `this` from the vtable user_data and looks the path up here.
    std::map<std::string, IncomingHandler> handlers;

    // Errors are a {const char* name; const char* message; ...} struct; we only
    // need to init/free/check it, so a generous opaque buffer suffices.
    struct ErrBuf { unsigned char bytes[64]; };
    // Iterators are documented as opaque; libdbus's is ~80-96 bytes. Over-size it.
    struct IterBuf { unsigned char bytes[128]; };
    // DBusObjectPathVTable is { unregister_fn; message_fn; void* pad1..4 } —
    // two function pointers + four reserved void*. Over-allocate generously to
    // be robust across libdbus ABI versions (same discipline as the iter/err
    // buffers above).
    struct VTableBuf { unsigned char bytes[128]; };
    VTableBuf vtable{};   // single shared vtable; user_data discriminates by `this`

    using fn_error_init   = void (*)(void*);
    using fn_error_free   = void (*)(void*);
    using fn_error_is_set = unsigned (*)(void*);
    using fn_bus_get_priv = void* (*)(int, void*);
    using fn_conn_close   = void (*)(void*);
    using fn_conn_unref   = void (*)(void*);
    using fn_set_exit     = void (*)(void*, unsigned);
    using fn_add_match    = void (*)(void*, const char*, void*);
    using fn_read_write   = unsigned (*)(void*, int);
    using fn_pop          = void* (*)(void*);
    using fn_send_block   = void* (*)(void*, void*, int, void*);
    using fn_msg_new_call = void* (*)(const char*, const char*, const char*, const char*);
    using fn_msg_unref    = void (*)(void*);
    using fn_msg_is_signal = unsigned (*)(void*, const char*, const char*);
    using fn_msg_get_path = const char* (*)(void*);
    using fn_iter_init_app = void (*)(void*, void*);
    using fn_iter_append  = unsigned (*)(void*, int, const void*);
    using fn_iter_open    = unsigned (*)(void*, int, const char*, void*);
    using fn_iter_close   = unsigned (*)(void*, void*);
    using fn_iter_init    = unsigned (*)(void*, void*);
    using fn_iter_argtype = int (*)(void*);
    using fn_iter_recurse = void (*)(void*, void*);
    using fn_iter_getbasic = void (*)(void*, void*);
    using fn_iter_next    = unsigned (*)(void*);

    // ── object-server additions ──
    // DBusObjectPathMessageFunction: DBusHandlerResult (*)(conn, msg, user_data)
    using fn_msg_func     = int (*)(void*, void*, void*);
    // DBusObjectPathVTable layout we fill in by hand (no build-time header).
    struct VTable { void* unregister_function; fn_msg_func message_function;
                    void* pad1; void* pad2; void* pad3; void* pad4; };
    using fn_try_register = unsigned (*)(void*, const char*, const void*, void*, void*);
    using fn_unregister   = unsigned (*)(void*, const char*);
    using fn_msg_new_ret  = void* (*)(void*);
    using fn_msg_new_sig  = void* (*)(const char*, const char*, const char*);
    using fn_msg_new_err  = void* (*)(void*, const char*, const char*);
    using fn_conn_send    = unsigned (*)(void*, void*, void*);
    using fn_conn_flush   = void (*)(void*);
    using fn_read_write_dispatch = unsigned (*)(void*, int);
    using fn_msg_get_iface = const char* (*)(void*);
    using fn_msg_get_member = const char* (*)(void*);
    using fn_msg_get_sender = const char* (*)(void*);
    using fn_get_unique   = const char* (*)(void*);

    fn_error_init   error_init = nullptr;
    fn_error_free   error_free = nullptr;
    fn_error_is_set error_is_set = nullptr;
    fn_bus_get_priv bus_get_private = nullptr;
    fn_conn_close   conn_close = nullptr;
    fn_conn_unref   conn_unref = nullptr;
    fn_set_exit     set_exit_on_disconnect = nullptr;
    fn_add_match    add_match = nullptr;
    fn_read_write   read_write = nullptr;
    fn_pop          pop_message = nullptr;
    fn_send_block   send_with_reply_and_block = nullptr;
    fn_msg_new_call msg_new_method_call = nullptr;
    fn_msg_unref    msg_unref = nullptr;
    fn_msg_is_signal msg_is_signal = nullptr;
    fn_msg_get_path msg_get_path = nullptr;
    fn_iter_init_app iter_init_append = nullptr;
    fn_iter_append  iter_append_basic = nullptr;
    fn_iter_open    iter_open_container = nullptr;
    fn_iter_close   iter_close_container = nullptr;
    fn_iter_init    iter_init = nullptr;
    fn_iter_argtype iter_get_arg_type = nullptr;
    fn_iter_recurse iter_recurse = nullptr;
    fn_iter_getbasic iter_get_basic = nullptr;
    fn_iter_next    iter_next = nullptr;

    fn_try_register try_register_object_path = nullptr;
    fn_unregister   unregister_object_path = nullptr;
    fn_msg_new_ret  msg_new_method_return = nullptr;
    fn_msg_new_sig  msg_new_signal = nullptr;
    fn_msg_new_err  msg_new_error = nullptr;
    fn_conn_send    conn_send = nullptr;
    fn_conn_flush   conn_flush = nullptr;
    fn_read_write_dispatch read_write_dispatch = nullptr;
    fn_msg_get_iface  msg_get_interface = nullptr;
    fn_msg_get_member msg_get_member = nullptr;
    fn_msg_get_sender msg_get_sender = nullptr;
    fn_get_unique     get_unique_name = nullptr;

    // ── a11y-bus additions ──
    // dbus_connection_open_private(const char* address, DBusError*) → DBusConnection*
    using fn_open_private = void* (*)(const char*, void*);
    // dbus_bus_register(DBusConnection*, DBusError*) → dbus_bool_t
    using fn_bus_register = unsigned (*)(void*, void*);
    fn_open_private open_private = nullptr;
    fn_bus_register bus_register = nullptr;

    // ── signal-subscription additions ──
    // DBusHandleMessageFunction: DBusHandlerResult (*)(conn, msg, user_data)
    using fn_filter_func  = int (*)(void*, void*, void*);
    // dbus_connection_add_filter(conn, filter_fn, user_data, free_fn) → dbus_bool_t
    using fn_add_filter   = unsigned (*)(void*, void*, void*, void*);
    // dbus_connection_remove_filter(conn, filter_fn, user_data)
    using fn_remove_filter = void (*)(void*, void*, void*);
    fn_add_filter    add_filter = nullptr;
    fn_remove_filter remove_filter = nullptr;

    // Registered signal subscriptions. The filter trampoline walks these on each
    // incoming signal and invokes every handler whose interface matches and whose
    // member matches (or is empty = any member of the interface).
    struct SignalSub { std::string interface; std::string member; SignalHandler handler; };
    std::map<unsigned, SignalSub> signal_subs;
    unsigned next_signal_token = 0;
    bool filter_installed = false;   // the connection filter is lazily added once

    template <typename Fn>
    Fn sym(const char* name) { return reinterpret_cast<Fn>(dlsym(handle, name)); }

    bool resolve() {
        error_init = sym<fn_error_init>("dbus_error_init");
        error_free = sym<fn_error_free>("dbus_error_free");
        error_is_set = sym<fn_error_is_set>("dbus_error_is_set");
        bus_get_private = sym<fn_bus_get_priv>("dbus_bus_get_private");
        conn_close = sym<fn_conn_close>("dbus_connection_close");
        conn_unref = sym<fn_conn_unref>("dbus_connection_unref");
        set_exit_on_disconnect = sym<fn_set_exit>("dbus_connection_set_exit_on_disconnect");
        add_match = sym<fn_add_match>("dbus_bus_add_match");
        read_write = sym<fn_read_write>("dbus_connection_read_write");
        pop_message = sym<fn_pop>("dbus_connection_pop_message");
        send_with_reply_and_block = sym<fn_send_block>("dbus_connection_send_with_reply_and_block");
        msg_new_method_call = sym<fn_msg_new_call>("dbus_message_new_method_call");
        msg_unref = sym<fn_msg_unref>("dbus_message_unref");
        msg_is_signal = sym<fn_msg_is_signal>("dbus_message_is_signal");
        msg_get_path = sym<fn_msg_get_path>("dbus_message_get_path");
        iter_init_append = sym<fn_iter_init_app>("dbus_message_iter_init_append");
        iter_append_basic = sym<fn_iter_append>("dbus_message_iter_append_basic");
        iter_open_container = sym<fn_iter_open>("dbus_message_iter_open_container");
        iter_close_container = sym<fn_iter_close>("dbus_message_iter_close_container");
        iter_init = sym<fn_iter_init>("dbus_message_iter_init");
        iter_get_arg_type = sym<fn_iter_argtype>("dbus_message_iter_get_arg_type");
        iter_recurse = sym<fn_iter_recurse>("dbus_message_iter_recurse");
        iter_get_basic = sym<fn_iter_getbasic>("dbus_message_iter_get_basic");
        iter_next = sym<fn_iter_next>("dbus_message_iter_next");

        // Object-server symbols.
        try_register_object_path = sym<fn_try_register>("dbus_connection_try_register_object_path");
        unregister_object_path = sym<fn_unregister>("dbus_connection_unregister_object_path");
        msg_new_method_return = sym<fn_msg_new_ret>("dbus_message_new_method_return");
        msg_new_signal = sym<fn_msg_new_sig>("dbus_message_new_signal");
        msg_new_error = sym<fn_msg_new_err>("dbus_message_new_error");
        conn_send = sym<fn_conn_send>("dbus_connection_send");
        conn_flush = sym<fn_conn_flush>("dbus_connection_flush");
        read_write_dispatch = sym<fn_read_write_dispatch>("dbus_connection_read_write_dispatch");
        msg_get_interface = sym<fn_msg_get_iface>("dbus_message_get_interface");
        msg_get_member = sym<fn_msg_get_member>("dbus_message_get_member");
        msg_get_sender = sym<fn_msg_get_sender>("dbus_message_get_sender");
        get_unique_name = sym<fn_get_unique>("dbus_bus_get_unique_name");

        // a11y-bus symbols (needed to open the separate accessibility daemon).
        open_private = sym<fn_open_private>("dbus_connection_open_private");
        bus_register = sym<fn_bus_register>("dbus_bus_register");

        // Signal-subscription symbols (connection message filter).
        add_filter = sym<fn_add_filter>("dbus_connection_add_filter");
        remove_filter = sym<fn_remove_filter>("dbus_connection_remove_filter");

        return error_init && error_free && error_is_set && bus_get_private &&
               conn_close && conn_unref && set_exit_on_disconnect && add_match &&
               read_write && pop_message && send_with_reply_and_block &&
               msg_new_method_call && msg_unref && msg_is_signal && msg_get_path &&
               iter_init_append && iter_append_basic && iter_open_container &&
               iter_close_container && iter_init && iter_get_arg_type &&
               iter_recurse && iter_get_basic && iter_next &&
               try_register_object_path && unregister_object_path &&
               msg_new_method_return && msg_new_signal && msg_new_error &&
               conn_send && conn_flush && read_write_dispatch &&
               msg_get_interface && msg_get_member && msg_get_sender &&
               get_unique_name && open_private && bus_register &&
               add_filter && remove_filter;
    }

    // Append one a{sv} entry: key (string) → variant of `vtype` holding `val`.
    void append_dict_entry(void* arr_iter, const char* key, int vtype, const void* val) {
        IterBuf entry, var;
        iter_open_container(arr_iter, kTypeDictEnt, nullptr, &entry);
        iter_append_basic(&entry, kTypeString, &key);
        const char vsig[2] = {static_cast<char>(vtype), '\0'};
        iter_open_container(&entry, kTypeVariant, vsig, &var);
        iter_append_basic(&var, vtype, val);
        iter_close_container(&entry, &var);
        iter_close_container(arr_iter, &entry);
    }

    // The single trampoline registered as DBusObjectPathVTable.message_function.
    // user_data is the Impl*; route the incoming method call to the handler
    // registered for the message's object path. Always reply or error — never
    // drop a call silently (an unanswered method call hangs the caller).
    static int trampoline(void* /*conn*/, void* msg, void* user_data) {
        auto* self = static_cast<Impl*>(user_data);
        if (!self || !msg) return kHandlerNotYetHandled;

        const char* path = self->msg_get_path(msg);
        if (!path) return kHandlerNotYetHandled;
        auto it = self->handlers.find(path);
        if (it == self->handlers.end()) return kHandlerNotYetHandled;

        // Build the argument reader over the incoming message.
        IterBuf args_iter;
        const bool has_args = self->iter_init(msg, &args_iter) != 0;

        DBus owner_view;             // lightweight non-owning DBus to host facades
        owner_view.impl_ = self;     // borrow; cleared before destruction below
        CallContext ctx(&owner_view, msg, has_args ? &args_iter : nullptr);
        const char* iface  = self->msg_get_interface(msg);
        const char* member = self->msg_get_member(msg);
        const char* sender = self->msg_get_sender(msg);
        ctx.path_      = path;
        ctx.interface_ = iface ? iface : "";
        ctx.member_    = member ? member : "";
        ctx.sender_    = sender ? sender : "";

        const bool recognized = it->second ? it->second(ctx) : false;

        int result = kHandlerHandled;
        if (!recognized) {
            // Decline → UnknownMethod error reply.
            void* err = self->msg_new_error(
                msg, "org.freedesktop.DBus.Error.UnknownMethod",
                "No such method");
            if (err) {
                self->conn_send(self->conn, err, nullptr);
                self->msg_unref(err);
            }
        } else if (ctx.reply_msg_) {
            // Handler built a reply (success or error) message.
            self->conn_send(self->conn, ctx.reply_msg_, nullptr);
            self->msg_unref(ctx.reply_msg_);
            ctx.reply_msg_ = nullptr;
        } else {
            // Recognized but produced no body → empty success reply.
            void* ret = self->msg_new_method_return(msg);
            if (ret) {
                self->conn_send(self->conn, ret, nullptr);
                self->msg_unref(ret);
            }
        }
        // Free the heap append-iterator reply() may have allocated.
        if (ctx.reply_iter_) {
            delete static_cast<IterBuf*>(ctx.reply_iter_);
            ctx.reply_iter_ = nullptr;
        }
        self->conn_flush(self->conn);
        owner_view.impl_ = nullptr;  // do NOT free the borrowed Impl on dtor
        return result;
    }

    // Connection-wide message FILTER for incoming signals. Unlike the object-path
    // trampoline above (method calls only), the filter sees every message —
    // including broadcast signals, which never hit the vtable. We route matching
    // signals to subscribed handlers and ALWAYS return NOT_YET_HANDLED so the
    // normal object-path dispatch (method-call routing) is unaffected.
    static int signal_filter(void* /*conn*/, void* msg, void* user_data) {
        auto* self = static_cast<Impl*>(user_data);
        if (!self || !msg) return kHandlerNotYetHandled;
        if (self->signal_subs.empty()) return kHandlerNotYetHandled;

        // Only consider signals. msg_is_signal(msg, iface, member) returns true
        // when both match; pass the message's own interface/member so it reduces
        // to a "is this a signal at all?" probe.
        const char* iface  = self->msg_get_interface(msg);
        const char* member = self->msg_get_member(msg);
        if (!iface || !member) return kHandlerNotYetHandled;
        if (!self->msg_is_signal(msg, iface, member)) return kHandlerNotYetHandled;

        const char* path = self->msg_get_path(msg);
        const std::string path_s   = path ? path : "";
        const std::string iface_s  = iface;
        const std::string member_s = member;

        // Snapshot the matching subscriptions before invoking any handler: a
        // handler may add/remove subscriptions, which would invalidate iterators
        // over signal_subs.
        std::vector<SignalHandler> to_invoke;
        for (auto& [token, sub] : self->signal_subs) {
            (void)token;
            if (sub.interface != iface_s) continue;
            if (!sub.member.empty() && sub.member != member_s) continue;
            if (sub.handler) to_invoke.push_back(sub.handler);
        }
        if (to_invoke.empty()) return kHandlerNotYetHandled;

        DBus owner_view;             // lightweight non-owning DBus to host Reader
        owner_view.impl_ = self;
        for (auto& h : to_invoke) {
            IterBuf args_iter;
            const bool has_args = self->iter_init(msg, &args_iter) != 0;
            Reader r(&owner_view, has_args ? &args_iter : nullptr);
            h(path_s, iface_s, member_s, r);
        }
        owner_view.impl_ = nullptr;  // borrowed; do NOT free on dtor
        return kHandlerNotYetHandled;  // never claim the message
    }
};

// ── Reader facade ───────────────────────────────────────────────────────────
bool DBus::Reader::read_string(std::string& out) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    auto& d = *owner_->impl_;
    const int t = d.iter_get_arg_type(iter_);
    if (t != kTypeString && t != kTypeObjPath) return false;
    const char* s = nullptr;
    d.iter_get_basic(iter_, &s);
    out = s ? s : "";
    d.iter_next(iter_);
    return true;
}
bool DBus::Reader::read_int32(int& out) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    auto& d = *owner_->impl_;
    if (d.iter_get_arg_type(iter_) != kTypeInt32) return false;
    int v = 0; d.iter_get_basic(iter_, &v); out = v; d.iter_next(iter_); return true;
}
bool DBus::Reader::read_uint32(unsigned& out) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    auto& d = *owner_->impl_;
    if (d.iter_get_arg_type(iter_) != kTypeUInt32) return false;
    unsigned v = 0; d.iter_get_basic(iter_, &v); out = v; d.iter_next(iter_); return true;
}
bool DBus::Reader::read_double(double& out) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    auto& d = *owner_->impl_;
    if (d.iter_get_arg_type(iter_) != kTypeDouble) return false;
    double v = 0; d.iter_get_basic(iter_, &v); out = v; d.iter_next(iter_); return true;
}
bool DBus::Reader::read_byte(unsigned char& out) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    auto& d = *owner_->impl_;
    if (d.iter_get_arg_type(iter_) != kTypeByte) return false;
    unsigned char v = 0; d.iter_get_basic(iter_, &v); out = v; d.iter_next(iter_); return true;
}
int DBus::Reader::arg_type() const {
    if (!owner_ || !owner_->impl_ || !iter_) return kTypeInvalid;
    return owner_->impl_->iter_get_arg_type(iter_);
}
bool DBus::Reader::next() {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    return owner_->impl_->iter_next(iter_) != 0;
}
bool DBus::Reader::recurse(Reader& out_sub) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    auto& d = *owner_->impl_;
    const int t = d.iter_get_arg_type(iter_);
    if (t != kTypeStruct && t != kTypeArray && t != kTypeVariant &&
        t != kTypeDictEnt) {
        return false;
    }
    auto buf = std::make_shared<Impl::IterBuf>();
    d.iter_recurse(iter_, buf.get());
    out_sub.owner_ = owner_;
    out_sub.iter_ = buf.get();
    out_sub.owned_iter_ = buf;   // keep the sub-iterator alive with the Reader
    return true;
}

// ── Writer facade ───────────────────────────────────────────────────────────
bool DBus::Writer::append_string(const std::string& s) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    const char* c = s.c_str();
    return owner_->impl_->iter_append_basic(iter_, kTypeString, &c) != 0;
}
bool DBus::Writer::append_object_path(const std::string& p) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    const char* c = p.c_str();
    return owner_->impl_->iter_append_basic(iter_, kTypeObjPath, &c) != 0;
}
bool DBus::Writer::append_bool(bool v) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    const int b = v ? 1 : 0;
    return owner_->impl_->iter_append_basic(iter_, kTypeBool, &b) != 0;
}
bool DBus::Writer::append_int32(int v) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    return owner_->impl_->iter_append_basic(iter_, kTypeInt32, &v) != 0;
}
bool DBus::Writer::append_uint32(unsigned v) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    return owner_->impl_->iter_append_basic(iter_, kTypeUInt32, &v) != 0;
}
bool DBus::Writer::append_double(double v) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    return owner_->impl_->iter_append_basic(iter_, kTypeDouble, &v) != 0;
}
bool DBus::Writer::append_byte(unsigned char v) {
    if (!owner_ || !owner_->impl_ || !iter_) return false;
    return owner_->impl_->iter_append_basic(iter_, kTypeByte, &v) != 0;
}
DBus::Writer::Container DBus::Writer::open_array(const std::string& element_signature) {
    Container c;
    if (!owner_ || !owner_->impl_ || !iter_) return c;
    c.iter = new Impl::IterBuf();
    c.open = owner_->impl_->iter_open_container(
        iter_, kTypeArray, element_signature.c_str(), c.iter) != 0;
    if (!c.open) { delete static_cast<Impl::IterBuf*>(c.iter); c.iter = nullptr; }
    return c;
}
DBus::Writer::Container DBus::Writer::open_struct() {
    Container c;
    if (!owner_ || !owner_->impl_ || !iter_) return c;
    c.iter = new Impl::IterBuf();
    c.open = owner_->impl_->iter_open_container(iter_, kTypeStruct, nullptr, c.iter) != 0;
    if (!c.open) { delete static_cast<Impl::IterBuf*>(c.iter); c.iter = nullptr; }
    return c;
}
DBus::Writer::Container DBus::Writer::open_variant(const std::string& value_signature) {
    Container c;
    if (!owner_ || !owner_->impl_ || !iter_) return c;
    c.iter = new Impl::IterBuf();
    c.open = owner_->impl_->iter_open_container(
        iter_, kTypeVariant, value_signature.c_str(), c.iter) != 0;
    if (!c.open) { delete static_cast<Impl::IterBuf*>(c.iter); c.iter = nullptr; }
    return c;
}
DBus::Writer::Container DBus::Writer::open_dict_entry() {
    Container c;
    if (!owner_ || !owner_->impl_ || !iter_) return c;
    c.iter = new Impl::IterBuf();
    c.open = owner_->impl_->iter_open_container(iter_, kTypeDictEnt, nullptr, c.iter) != 0;
    if (!c.open) { delete static_cast<Impl::IterBuf*>(c.iter); c.iter = nullptr; }
    return c;
}
bool DBus::Writer::close_container(Container& c) {
    if (!owner_ || !owner_->impl_ || !iter_ || !c.open || !c.iter) return false;
    const bool ok = owner_->impl_->iter_close_container(iter_, c.iter) != 0;
    delete static_cast<Impl::IterBuf*>(c.iter);
    c.iter = nullptr;
    c.open = false;
    return ok;
}
DBus::Writer DBus::Writer::sub(Container& c) {
    return Writer(owner_, c.iter);
}

// ── CallContext ─────────────────────────────────────────────────────────────
DBus::Writer DBus::CallContext::reply() {
    if (!owner_ || !owner_->impl_ || answered_) return Writer(owner_, nullptr);
    answered_ = true;
    reply_msg_ = owner_->impl_->msg_new_method_return(msg_);
    if (!reply_msg_) return Writer(owner_, nullptr);
    // Append-iterator lives for the duration of the reply build. Stash it on the
    // heap so the returned Writer (and any sub-Writers) stay valid until the
    // trampoline sends the message. Freed in the trampoline after send.
    auto* it = new Impl::IterBuf();
    owner_->impl_->iter_init_append(reply_msg_, it);
    reply_iter_ = it;
    return Writer(owner_, it);
}
void DBus::CallContext::error(const std::string& name, const std::string& message) {
    if (!owner_ || !owner_->impl_ || answered_) return;
    answered_ = true;
    reply_msg_ = owner_->impl_->msg_new_error(msg_, name.c_str(), message.c_str());
}

bool DBus::library_available() {
    void* h = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = dlopen("libdbus-1.so", RTLD_LAZY | RTLD_LOCAL);
    if (!h) return false;
    dlclose(h);
    return true;
}

DBus::DBus() = default;

DBus::~DBus() {
    if (impl_) {
        if (impl_->conn) {
            // Drop the signal filter before tearing the connection down so its
            // user_data (this Impl) is never dereferenced after free.
            if (impl_->filter_installed && impl_->remove_filter) {
                impl_->remove_filter(impl_->conn,
                                     reinterpret_cast<void*>(&Impl::signal_filter),
                                     impl_);
            }
            impl_->conn_close(impl_->conn);
            impl_->conn_unref(impl_->conn);
        }
        if (impl_->handle) dlclose(impl_->handle);
        delete impl_;
    }
}

bool DBus::connected() const { return impl_ && impl_->conn; }

std::string DBus::unique_name() const {
    if (!connected() || !impl_->get_unique_name) return {};
    const char* n = impl_->get_unique_name(impl_->conn);
    return n ? std::string(n) : std::string();
}

bool DBus::connect_bus(int bus_type) {
    if (connected()) return true;
    auto impl = new Impl();
    impl->handle = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_LOCAL);
    if (!impl->handle) impl->handle = dlopen("libdbus-1.so", RTLD_LAZY | RTLD_LOCAL);
    if (!impl->handle || !impl->resolve()) { delete impl; return false; }

    Impl::ErrBuf err;
    impl->error_init(&err);
    impl->conn = impl->bus_get_private(bus_type, &err);
    if (!impl->conn || impl->error_is_set(&err)) {
        if (impl->conn) { impl->conn_close(impl->conn); impl->conn_unref(impl->conn); }
        impl->error_free(&err);
        dlclose(impl->handle);
        delete impl;
        return false;
    }
    impl->error_free(&err);
    impl->set_exit_on_disconnect(impl->conn, 0u);
    impl_ = impl;
    return true;
}

bool DBus::connect_session() { return connect_bus(kBusSession); }
bool DBus::connect_system() { return connect_bus(kBusSystem); }

bool DBus::a11y_connected() const { return impl_ && impl_->conn && impl_->on_a11y; }

bool DBus::connect_a11y_bus() {
    if (a11y_connected()) return true;        // already switched over
    if (!connect_session()) return false;     // need the session bus to ask
    Impl& d = *impl_;

    // 1. Ask the session bus for the accessibility bus address:
    //    org.a11y.Bus . GetAddress() -> s   (on /org/a11y/bus).
    void* msg = d.msg_new_method_call(
        "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress");
    if (!msg) return false;

    Impl::ErrBuf err;
    d.error_init(&err);
    void* reply = d.send_with_reply_and_block(d.conn, msg, 5000, &err);
    d.msg_unref(msg);
    std::string address;
    if (reply && !d.error_is_set(&err)) {
        Impl::IterBuf it;
        if (d.iter_init(reply, &it) && d.iter_get_arg_type(&it) == kTypeString) {
            const char* a = nullptr;
            d.iter_get_basic(&it, &a);
            if (a) address = a;
        }
    }
    if (reply) d.msg_unref(reply);
    d.error_free(&err);
    if (address.empty()) return false;   // no a11y daemon (headless) → honest-fail

    // 2. Open a NEW private connection to that address and register with it.
    d.error_init(&err);
    void* a11y = d.open_private(address.c_str(), &err);
    if (!a11y || d.error_is_set(&err)) {
        if (a11y) { d.conn_close(a11y); d.conn_unref(a11y); }
        d.error_free(&err);
        return false;
    }
    d.error_free(&err);

    d.error_init(&err);
    const unsigned registered = d.bus_register(a11y, &err);
    if (!registered || d.error_is_set(&err)) {
        d.error_free(&err);
        d.conn_close(a11y);
        d.conn_unref(a11y);
        return false;
    }
    d.error_free(&err);
    d.set_exit_on_disconnect(a11y, 0u);

    // 3. Swap the active connection over to the a11y bus. Any objects registered
    //    on the session connection are dropped (none are at switch time — the
    //    AT-SPI provider exports onto the a11y bus only). The session connection
    //    served only to discover the address; close it.
    d.conn_close(d.conn);
    d.conn_unref(d.conn);
    d.conn = a11y;
    d.on_a11y = true;
    return true;
}

bool DBus::register_object(const std::string& path, IncomingHandler handler) {
    if (!connected()) return false;
    Impl& d = *impl_;

    // Fill the shared vtable in place: only message_function is needed (the
    // unregister hook is optional; we route everything through the trampoline).
    auto* vt = reinterpret_cast<Impl::VTable*>(&d.vtable);
    vt->unregister_function = nullptr;
    vt->message_function = &Impl::trampoline;
    vt->pad1 = vt->pad2 = vt->pad3 = vt->pad4 = nullptr;

    // A path already in the table is registered with libdbus already; updating
    // the handler is a pure in-table replace (the trampoline looks the path up
    // per call), so skip the libdbus registration in that case.
    const bool already_registered = d.handlers.count(path) != 0;
    d.handlers[path] = std::move(handler);
    if (already_registered) return true;

    Impl::ErrBuf err;
    d.error_init(&err);
    const unsigned ok = d.try_register_object_path(
        d.conn, path.c_str(), &d.vtable, &d /*user_data*/, &err);
    const bool failed = (ok == 0) || d.error_is_set(&err);
    d.error_free(&err);
    if (failed) {
        // First-time registration failed (OOM or libdbus rejected the path).
        // Honest-fail and drop the handler we speculatively inserted.
        d.handlers.erase(path);
        return false;
    }
    return true;
}

bool DBus::unregister_object(const std::string& path) {
    if (!connected()) return false;
    Impl& d = *impl_;
    auto it = d.handlers.find(path);
    if (it == d.handlers.end()) return false;
    d.handlers.erase(it);
    return d.unregister_object_path(d.conn, path.c_str()) != 0;
}

bool DBus::emit_signal(const std::string& path, const std::string& interface,
                       const std::string& member,
                       const std::function<void(Writer&)>& build_args) {
    if (!connected()) return false;
    Impl& d = *impl_;

    void* msg = d.msg_new_signal(path.c_str(), interface.c_str(), member.c_str());
    if (!msg) return false;

    Impl::IterBuf args;
    d.iter_init_append(msg, &args);
    if (build_args) {
        Writer w(this, &args);
        build_args(w);
    }

    const bool sent = d.conn_send(d.conn, msg, nullptr) != 0;
    d.msg_unref(msg);
    if (sent) d.conn_flush(d.conn);
    return sent;
}

bool DBus::dispatch(int timeout_ms) {
    if (!connected()) return false;
    // read_write_dispatch runs the libdbus dispatcher so registered object-path
    // handlers (the trampoline) actually fire. This is the object-server
    // consumption model; the portal file_chooser pump uses pop_message instead,
    // which removes messages BEFORE the dispatcher would route them. Do not
    // interleave the two on the same pending traffic.
    return impl_->read_write_dispatch(impl_->conn, timeout_ms) != 0;
}

bool DBus::call_method(const std::string& destination, const std::string& path,
                       const std::string& interface, const std::string& member,
                       const std::function<void(Writer&)>& build_args,
                       const std::function<void(Reader&)>& read_reply,
                       int timeout_ms) {
    if (!connected()) return false;
    Impl& d = *impl_;

    void* msg = d.msg_new_method_call(destination.c_str(), path.c_str(),
                                      interface.c_str(), member.c_str());
    if (!msg) return false;

    Impl::IterBuf args;
    d.iter_init_append(msg, &args);
    if (build_args) {
        Writer w(this, &args);
        build_args(w);
    }

    Impl::ErrBuf err;
    d.error_init(&err);
    void* reply = d.send_with_reply_and_block(d.conn, msg, timeout_ms, &err);
    d.msg_unref(msg);
    if (!reply || d.error_is_set(&err)) {
        if (reply) d.msg_unref(reply);
        d.error_free(&err);
        return false;
    }
    d.error_free(&err);

    if (read_reply) {
        Impl::IterBuf it;
        if (d.iter_init(reply, &it)) {
            Reader r(this, &it);
            read_reply(r);
        } else {
            Reader r(this, nullptr);
            read_reply(r);  // no body: a Reader over null returns false on reads
        }
    }
    d.msg_unref(reply);
    return true;
}

unsigned DBus::add_signal_handler(const std::string& interface,
                                  const std::string& member,
                                  SignalHandler handler) {
    if (!connected() || !handler) return 0;
    Impl& d = *impl_;

    // Install the connection filter once, on the first subscription.
    if (!d.filter_installed) {
        if (d.add_filter(d.conn, reinterpret_cast<void*>(&Impl::signal_filter),
                         &d /*user_data*/, nullptr /*free_fn*/) == 0) {
            return 0;  // OOM adding the filter → honest-fail
        }
        d.filter_installed = true;
    }

    // Tell the broker to deliver matching signals to this connection.
    std::string rule = "type='signal',interface='" + interface + "'";
    if (!member.empty()) rule += ",member='" + member + "'";
    Impl::ErrBuf err;
    d.error_init(&err);
    d.add_match(d.conn, rule.c_str(), &err);
    const bool match_failed = d.error_is_set(&err) != 0;
    d.error_free(&err);
    if (match_failed) return 0;

    const unsigned token = ++d.next_signal_token;
    d.signal_subs[token] = Impl::SignalSub{interface, member, std::move(handler)};
    return token;
}

bool DBus::remove_signal_handler(unsigned token) {
    if (!connected()) return false;
    Impl& d = *impl_;
    auto it = d.signal_subs.find(token);
    if (it == d.signal_subs.end()) return false;
    d.signal_subs.erase(it);
    // The org.freedesktop.DBus match rule is intentionally left in place: removing
    // it precisely would require ref-counting identical rules across tokens, and a
    // surplus rule only costs a few unmatched deliveries that the filter ignores.
    return true;
}

bool DBus::get_managed_objects(
    const std::string& destination, const std::string& path,
    const std::function<void(const std::string&, const std::string&)>& per_object,
    int timeout_ms) {
    if (!connected()) return false;
    // GetManagedObjects() -> a{oa{sa{sv}}}: a dict of object-path → (dict of
    // interface-name → (dict of property-name → variant)). We walk the outer
    // array of dict-entries, then each object's inner array of interface
    // dict-entries, reporting (object-path, interface-name). Property values are
    // skipped — callers re-query specific props via call_method as needed.
    return call_method(
        destination, path, "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects", /*build_args=*/nullptr,
        [&](Reader& reply) {
            if (reply.arg_type() != kTypeArray) return;
            Reader objects;
            if (!reply.recurse(objects)) return;          // → outer a{o...}
            while (objects.arg_type() == kTypeDictEnt) {
                Reader obj_entry;
                if (!objects.recurse(obj_entry)) break;    // → {o, a{sa{sv}}}
                std::string obj_path;
                if (!obj_entry.read_string(obj_path)) {    // object path 'o'
                    objects.next();
                    continue;
                }
                // obj_entry cursor now on the inner a{sa{sv}} (interfaces).
                if (obj_entry.arg_type() == kTypeArray) {
                    Reader ifaces;
                    if (obj_entry.recurse(ifaces)) {
                        while (ifaces.arg_type() == kTypeDictEnt) {
                            Reader iface_entry;
                            if (!ifaces.recurse(iface_entry)) break;  // → {s, a{sv}}
                            std::string iface_name;
                            if (iface_entry.read_string(iface_name) && per_object) {
                                per_object(obj_path, iface_name);
                            }
                            // The remaining a{sv} of props is skipped.
                            ifaces.next();
                        }
                    }
                }
                objects.next();
            }
        },
        timeout_ms);
}

std::optional<DBus::PortalResult> DBus::file_chooser(
    const std::string& method, const std::string& title,
    const std::map<std::string, std::string>& options,
    const std::map<std::string, bool>& bool_options, int timeout_ms) {
    if (!connect_session()) return std::nullopt;
    Impl& d = *impl_;

    Impl::ErrBuf err;
    d.error_init(&err);
    // Catch the Response signal from the portal Request object.
    d.add_match(d.conn,
        "type='signal',interface='org.freedesktop.portal.Request',member='Response'",
        &err);
    if (d.error_is_set(&err)) { d.error_free(&err); return std::nullopt; }
    d.error_free(&err);

    void* msg = d.msg_new_method_call(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.FileChooser", method.c_str());
    if (!msg) return std::nullopt;

    Impl::IterBuf args;
    d.iter_init_append(msg, &args);
    const char* parent = "";          // no parent window handle
    const char* title_c = title.c_str();
    d.iter_append_basic(&args, kTypeString, &parent);
    d.iter_append_basic(&args, kTypeString, &title_c);
    // options a{sv}
    Impl::IterBuf opts;
    d.iter_open_container(&args, kTypeArray, "{sv}", &opts);
    for (const auto& [k, v] : options) {
        const char* vc = v.c_str();
        d.append_dict_entry(&opts, k.c_str(), kTypeString, &vc);
    }
    for (const auto& [k, v] : bool_options) {
        int b = v ? 1 : 0;
        d.append_dict_entry(&opts, k.c_str(), kTypeBool, &b);
    }
    d.iter_close_container(&args, &opts);

    d.error_init(&err);
    void* reply = d.send_with_reply_and_block(d.conn, msg, 10000, &err);
    d.msg_unref(msg);
    if (!reply || d.error_is_set(&err)) {
        if (reply) d.msg_unref(reply);
        d.error_free(&err);
        return std::nullopt;   // no portal / marshalling rejected → honest-fail
    }
    d.error_free(&err);

    // The reply carries the Request object path.
    std::string request_path;
    {
        Impl::IterBuf it;
        if (d.iter_init(reply, &it) && d.iter_get_arg_type(&it) == kTypeObjPath) {
            const char* p = nullptr;
            d.iter_get_basic(&it, &p);
            if (p) request_path = p;
        }
    }
    d.msg_unref(reply);
    if (request_path.empty()) return std::nullopt;

    // Pump the connection until the matching Response signal arrives.
    PortalResult result;
    int waited = 0;
    const int slice = 100;
    while (waited < timeout_ms) {
        d.read_write(d.conn, slice);
        void* sig = d.pop_message(d.conn);
        if (!sig) { waited += slice; continue; }
        const bool match =
            d.msg_is_signal(sig, "org.freedesktop.portal.Request", "Response") &&
            d.msg_get_path(sig) && request_path == d.msg_get_path(sig);
        if (!match) { d.msg_unref(sig); continue; }

        // Response: (u response, a{sv} results) where results["uris"] = as.
        Impl::IterBuf it;
        if (d.iter_init(sig, &it)) {
            // response code (u)
            unsigned code = 2;
            d.iter_get_basic(&it, &code);
            result.response = static_cast<int>(code);
            d.iter_next(&it);
            // results a{sv}
            if (d.iter_get_arg_type(&it) == kTypeArray) {
                Impl::IterBuf dict;
                d.iter_recurse(&it, &dict);
                while (d.iter_get_arg_type(&dict) == kTypeDictEnt) {
                    Impl::IterBuf entry;
                    d.iter_recurse(&dict, &entry);
                    const char* key = nullptr;
                    d.iter_get_basic(&entry, &key);
                    d.iter_next(&entry);  // → variant
                    if (key && std::strcmp(key, "uris") == 0 &&
                        d.iter_get_arg_type(&entry) == kTypeVariant) {
                        Impl::IterBuf var;
                        d.iter_recurse(&entry, &var);     // → array of string
                        if (d.iter_get_arg_type(&var) == kTypeArray) {
                            Impl::IterBuf strs;
                            d.iter_recurse(&var, &strs);
                            while (d.iter_get_arg_type(&strs) == kTypeString) {
                                const char* u = nullptr;
                                d.iter_get_basic(&strs, &u);
                                if (u) result.uris.push_back(u);
                                d.iter_next(&strs);
                            }
                        }
                    }
                    d.iter_next(&dict);
                }
            }
        }
        d.msg_unref(sig);
        return result;
    }
    return std::nullopt;  // timed out waiting for the user
}

}  // namespace pulp::platform

#else  // ── non-Linux: honest no-op ────────────────────────────────────────

namespace pulp::platform {
struct DBus::Impl {};

// Reader / Writer / CallContext facades are never constructed off Linux (no
// handler ever fires, emit_signal returns early), but their member functions
// must still link. Provide inert stubs.
bool DBus::Reader::read_string(std::string&) { return false; }
bool DBus::Reader::read_int32(int&) { return false; }
bool DBus::Reader::read_uint32(unsigned&) { return false; }
bool DBus::Reader::read_double(double&) { return false; }
bool DBus::Reader::read_byte(unsigned char&) { return false; }
int  DBus::Reader::arg_type() const { return 0; }
bool DBus::Reader::next() { return false; }
bool DBus::Reader::recurse(Reader&) { return false; }

bool DBus::Writer::append_string(const std::string&) { return false; }
bool DBus::Writer::append_object_path(const std::string&) { return false; }
bool DBus::Writer::append_bool(bool) { return false; }
bool DBus::Writer::append_int32(int) { return false; }
bool DBus::Writer::append_uint32(unsigned) { return false; }
bool DBus::Writer::append_double(double) { return false; }
bool DBus::Writer::append_byte(unsigned char) { return false; }
DBus::Writer::Container DBus::Writer::open_array(const std::string&) { return {}; }
DBus::Writer::Container DBus::Writer::open_struct() { return {}; }
DBus::Writer::Container DBus::Writer::open_variant(const std::string&) { return {}; }
DBus::Writer::Container DBus::Writer::open_dict_entry() { return {}; }
bool DBus::Writer::close_container(Container&) { return false; }
DBus::Writer DBus::Writer::sub(Container& c) { return Writer(owner_, c.iter); }

DBus::Writer DBus::CallContext::reply() { return Writer(owner_, nullptr); }
void DBus::CallContext::error(const std::string&, const std::string&) {}

bool DBus::library_available() { return false; }
DBus::DBus() = default;
DBus::~DBus() = default;
bool DBus::connected() const { return false; }
std::string DBus::unique_name() const { return {}; }
bool DBus::connect_bus(int) { return false; }
bool DBus::connect_session() { return false; }
bool DBus::connect_system() { return false; }
bool DBus::connect_a11y_bus() { return false; }
bool DBus::a11y_connected() const { return false; }
bool DBus::register_object(const std::string&, IncomingHandler) { return false; }
bool DBus::unregister_object(const std::string&) { return false; }
bool DBus::emit_signal(const std::string&, const std::string&, const std::string&,
                       const std::function<void(Writer&)>&) { return false; }
bool DBus::dispatch(int) { return false; }
bool DBus::call_method(const std::string&, const std::string&, const std::string&,
                       const std::string&, const std::function<void(Writer&)>&,
                       const std::function<void(Reader&)>&, int) { return false; }
unsigned DBus::add_signal_handler(const std::string&, const std::string&,
                                  SignalHandler) { return 0; }
bool DBus::remove_signal_handler(unsigned) { return false; }
bool DBus::get_managed_objects(
    const std::string&, const std::string&,
    const std::function<void(const std::string&, const std::string&)>&,
    int) { return false; }
std::optional<DBus::PortalResult> DBus::file_chooser(
    const std::string&, const std::string&,
    const std::map<std::string, std::string>&,
    const std::map<std::string, bool>&, int) { return std::nullopt; }
}  // namespace pulp::platform

#endif
