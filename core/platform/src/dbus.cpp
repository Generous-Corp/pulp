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
constexpr int kTypeString  = 's';
constexpr int kTypeObjPath = 'o';
constexpr int kTypeBool    = 'b';
constexpr int kTypeArray   = 'a';
constexpr int kTypeVariant = 'v';
constexpr int kTypeDictEnt = 'e';
constexpr int kTypeInvalid = '\0';
constexpr int kBusSession  = 0;
}  // namespace

// libdbus entry points resolved at runtime. Opaque structs as void*; the
// DBusMessageIter is an opaque fixed-size POD — libdbus documents it as a
// stack struct; we over-allocate a buffer to hold it across ABI versions.
struct DBus::Impl {
    void* handle = nullptr;       // dlopen handle
    void* conn = nullptr;         // DBusConnection*

    // Errors are a {const char* name; const char* message; ...} struct; we only
    // need to init/free/check it, so a generous opaque buffer suffices.
    struct ErrBuf { unsigned char bytes[64]; };
    // Iterators are documented as opaque; libdbus's is ~80-96 bytes. Over-size it.
    struct IterBuf { unsigned char bytes[128]; };

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
        return error_init && error_free && error_is_set && bus_get_private &&
               conn_close && conn_unref && set_exit_on_disconnect && add_match &&
               read_write && pop_message && send_with_reply_and_block &&
               msg_new_method_call && msg_unref && msg_is_signal && msg_get_path &&
               iter_init_append && iter_append_basic && iter_open_container &&
               iter_close_container && iter_init && iter_get_arg_type &&
               iter_recurse && iter_get_basic && iter_next;
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
};

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
        if (impl_->conn) { impl_->conn_close(impl_->conn); impl_->conn_unref(impl_->conn); }
        if (impl_->handle) dlclose(impl_->handle);
        delete impl_;
    }
}

bool DBus::connected() const { return impl_ && impl_->conn; }

bool DBus::connect_session() {
    if (connected()) return true;
    auto impl = new Impl();
    impl->handle = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_LOCAL);
    if (!impl->handle) impl->handle = dlopen("libdbus-1.so", RTLD_LAZY | RTLD_LOCAL);
    if (!impl->handle || !impl->resolve()) { delete impl; return false; }

    Impl::ErrBuf err;
    impl->error_init(&err);
    impl->conn = impl->bus_get_private(kBusSession, &err);
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
        unsigned b = v ? 1u : 0u;
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
bool DBus::library_available() { return false; }
DBus::DBus() = default;
DBus::~DBus() = default;
bool DBus::connected() const { return false; }
bool DBus::connect_session() { return false; }
std::optional<DBus::PortalResult> DBus::file_chooser(
    const std::string&, const std::string&,
    const std::map<std::string, std::string>&,
    const std::map<std::string, bool>&, int) { return std::nullopt; }
}  // namespace pulp::platform

#endif
