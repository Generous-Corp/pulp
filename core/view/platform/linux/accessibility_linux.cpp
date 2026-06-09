// Linux AT-SPI2 accessibility provider — direct D-Bus (no libatk).
//
// AT-SPI2 is a *wire protocol* over D-Bus: the application EXPORTS accessible
// objects, and the registry / screen reader (Orca) calls methods ON them
// (Accessible.GetRole, Accessible.GetChildren, Component.GetExtents, …). This
// TU implements that protocol directly over pulp::platform::DBus (libdbus-1
// dlopen'd, honest-fail), the same pattern the L6a portal client uses — so
// there is NO build-time dependency on libatk / libatspi and no LGPL-adjacent
// ATK runtime. Roles are mapped through the offline-tested atspi_mapping.hpp.
//
// AT-SPI lives on a SEPARATE "a11y" bus (org.a11y.Bus.GetAddress on the session
// bus → a fresh private connection). DBus::connect_a11y_bus() handles the
// discovery + switch; everything below talks to that connection.
//
// This slice (L7a-2) registers the process with the registry:
//   1. connect to the a11y bus,
//   2. export /org/a11y/atspi/accessible/root implementing
//      org.a11y.atspi.Accessible + org.a11y.atspi.Application (+ the mandatory
//      Introspectable + Properties interfaces),
//   3. perform the org.a11y.atspi.Socket.Embed handshake against the registry
//      so Orca discovers the process.
// The per-widget object tree (Accessible per View, Component extents) is L7b;
// Value + event signals are L7c — the notify_* hooks stay no-ops here.
//
// Run-loop requirement: the registry calls methods on us asynchronously, so the
// host must pump DBus::dispatch() periodically or those calls hang the AT. The
// handle exposes pump(); see init_accessibility()'s tail comment for the seam.

#if defined(__linux__) && !defined(__ANDROID__)

#include <pulp/view/accessibility_provider.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/platform/atspi_mapping.hpp>
#include <pulp/platform/dbus.hpp>
#include <pulp/runtime/log.hpp>

#include <memory>
#include <string>

namespace pulp::view {

namespace {

using pulp::platform::DBus;

// AT-SPI well-known names / paths (the published wire constants).
constexpr const char* kRootPath      = "/org/a11y/atspi/accessible/root";
constexpr const char* kNullPath      = "/org/a11y/atspi/null";
constexpr const char* kRegistryName  = "org.a11y.atspi.Registry";
constexpr const char* kIfaceAccessible   = "org.a11y.atspi.Accessible";
constexpr const char* kIfaceApplication  = "org.a11y.atspi.Application";
constexpr const char* kIfaceSocket       = "org.a11y.atspi.Socket";
constexpr const char* kIfaceProperties   = "org.freedesktop.DBus.Properties";
constexpr const char* kIfaceIntrospect   = "org.freedesktop.DBus.Introspectable";

// Pulp's toolkit identity reported via Application properties.
constexpr const char* kToolkitName    = "Pulp";
constexpr const char* kToolkitVersion = "1.0";
constexpr const char* kAtspiVersion   = "2.1";

// The exported root accessible + its owning a11y connection. Stored as the
// opaque handle returned by init_accessibility(). One per window/root.
struct AtspiProvider {
    DBus bus;                       // owns the a11y-bus connection
    View* root = nullptr;           // the View tree this root maps (children = L7b)

    // The registry's root accessible, captured from Socket.Embed: (bus_name,
    // object_path). Reported as this app root's Parent so the tree connects up.
    std::string registry_parent_name;
    std::string registry_parent_path;

    // Application.Id, assigned by the registry via Properties.Set during/after
    // Embed. -1 until set.
    int app_id = -1;

    // Append an AT-SPI object reference (so) = (bus_name, object_path) into a
    // reply. AT-SPI addresses every accessible as this struct.
    void append_object_ref(DBus::Writer& w, const std::string& name,
                           const std::string& path) {
        auto s = w.open_struct();
        DBus::Writer sw = w.sub(s);
        sw.append_string(name);
        sw.append_object_path(path.empty() ? kNullPath : path);
        w.close_container(s);
    }

    // GetState returns au (array of two uint32 = the 64-bit state bitfield).
    void append_state(DBus::Writer& w) {
        const atspi::StateSet st = atspi::default_states();
        auto a = w.open_array("u");
        DBus::Writer aw = w.sub(a);
        aw.append_uint32(st.low);
        aw.append_uint32(st.high);
        w.close_container(a);
    }

    // org.a11y.atspi.Accessible methods on the root.
    bool handle_accessible(DBus::CallContext& ctx) {
        const std::string& m = ctx.member();
        if (m == "GetRole") {
            // The root accessible is the APPLICATION.
            ctx.reply().append_uint32(atspi::kRoleApplication);
            return true;
        }
        if (m == "GetRoleName") {
            ctx.reply().append_string("application");
            return true;
        }
        if (m == "GetChildCount") {
            // L7a-2 exports only the root; the View subtree lands in L7b.
            ctx.reply().append_int32(0);
            return true;
        }
        if (m == "GetChildren") {
            // a(so) — empty for this slice.
            DBus::Writer w = ctx.reply();
            auto a = w.open_array("(so)");
            w.close_container(a);
            return true;
        }
        if (m == "GetState") {
            DBus::Writer w = ctx.reply();
            append_state(w);
            return true;
        }
        if (m == "GetInterfaces") {
            DBus::Writer w = ctx.reply();
            auto a = w.open_array("s");
            DBus::Writer aw = w.sub(a);
            aw.append_string(kIfaceAccessible);
            aw.append_string(kIfaceApplication);
            w.close_container(a);
            return true;
        }
        if (m == "GetAttributes") {
            // a{ss} — none.
            DBus::Writer w = ctx.reply();
            auto a = w.open_array("{ss}");
            w.close_container(a);
            return true;
        }
        return false;  // decline → UnknownMethod
    }

    // org.a11y.atspi.Application methods (Id is also a property; some clients
    // call GetApplicationBusAddress etc., not needed for discovery).
    bool handle_application(DBus::CallContext& ctx) {
        const std::string& m = ctx.member();
        if (m == "GetLocale") {
            // (u lc_category) -> s. Ignore the category; report the C locale.
            ctx.reply().append_string("C");
            return true;
        }
        if (m == "RegisterEventListener" || m == "DeregisterEventListener") {
            ctx.reply();  // empty ack
            return true;
        }
        return false;
    }

    // org.a11y.atspi.Socket.Embed handshake reply is initiated by US (a method
    // call out to the registry), not serviced here. But the registry may also
    // call Embed back / Unembed on us in some flows — ack benignly.
    bool handle_socket(DBus::CallContext& ctx) {
        const std::string& m = ctx.member();
        if (m == "Embed" || m == "Unembed") {
            // Reply with our own (so) root reference.
            DBus::Writer w = ctx.reply();
            append_object_ref(w, bus.unique_name(), kRootPath);
            return true;
        }
        return false;
    }

    // org.freedesktop.DBus.Properties for the root's Accessible + Application
    // interface properties. Get(iface, prop) -> v; GetAll(iface) -> a{sv};
    // Set(iface, prop, v) -> () (the registry sets Application.Id this way).
    bool handle_properties(DBus::CallContext& ctx) {
        const std::string& m = ctx.member();
        if (m == "Get") {
            std::string iface, prop;
            ctx.args().read_string(iface);
            ctx.args().read_string(prop);
            DBus::Writer w = ctx.reply();
            return append_property_variant(w, iface, prop);
        }
        if (m == "GetAll") {
            std::string iface;
            ctx.args().read_string(iface);
            DBus::Writer w = ctx.reply();
            auto a = w.open_array("{sv}");
            append_all_properties(w, a, iface);
            w.close_container(a);
            return true;
        }
        if (m == "Set") {
            std::string iface, prop;
            ctx.args().read_string(iface);
            ctx.args().read_string(prop);
            // The third arg is a variant holding the value; recurse into it.
            // Only Application.Id (i) is writable — the registry sets it to
            // assign this app's slot.
            if (iface == kIfaceApplication && prop == "Id") {
                DBus::Reader var;
                int v = 0;
                if (ctx.args().recurse(var) && var.read_int32(v)) app_id = v;
            }
            ctx.reply();  // empty success ack
            return true;
        }
        return false;
    }

    // Append a single property as a variant into `w`. Returns false (decline)
    // for unknown property so the trampoline replies UnknownMethod.
    bool append_property_variant(DBus::Writer& w, const std::string& iface,
                                 const std::string& prop) {
        if (iface == kIfaceAccessible) {
            if (prop == "Name") { return variant_string(w, kToolkitName); }
            if (prop == "Description") { return variant_string(w, ""); }
            if (prop == "Parent") {
                auto v = w.open_variant("(so)");
                DBus::Writer vw = w.sub(v);
                append_object_ref(vw, registry_parent_name, registry_parent_path);
                w.close_container(v);
                return true;
            }
            if (prop == "ChildCount") { return variant_int32(w, 0); }
        }
        if (iface == kIfaceApplication) {
            if (prop == "ToolkitName") { return variant_string(w, kToolkitName); }
            if (prop == "Version") { return variant_string(w, kToolkitVersion); }
            if (prop == "AtspiVersion") { return variant_string(w, kAtspiVersion); }
            if (prop == "Id") { return variant_int32(w, app_id); }
        }
        return false;
    }

    void append_all_properties(DBus::Writer& w, DBus::Writer::Container& arr,
                               const std::string& iface) {
        DBus::Writer aw = w.sub(arr);
        auto entry = [&](const char* key, auto appender) {
            auto e = aw.open_dict_entry();
            DBus::Writer ew = aw.sub(e);
            ew.append_string(key);
            appender(ew);
            aw.close_container(e);
        };
        if (iface == kIfaceAccessible) {
            entry("Name", [&](DBus::Writer& ew) { variant_string(ew, kToolkitName); });
            entry("ChildCount", [&](DBus::Writer& ew) { variant_int32(ew, 0); });
        } else if (iface == kIfaceApplication) {
            entry("ToolkitName",
                  [&](DBus::Writer& ew) { variant_string(ew, kToolkitName); });
            entry("Version",
                  [&](DBus::Writer& ew) { variant_string(ew, kToolkitVersion); });
            entry("AtspiVersion",
                  [&](DBus::Writer& ew) { variant_string(ew, kAtspiVersion); });
            entry("Id", [&](DBus::Writer& ew) { variant_int32(ew, app_id); });
        }
    }

    static bool variant_string(DBus::Writer& w, const std::string& s) {
        auto v = w.open_variant("s");
        DBus::Writer vw = w.sub(v);
        vw.append_string(s);
        return w.close_container(v);
    }
    static bool variant_int32(DBus::Writer& w, int v) {
        auto var = w.open_variant("i");
        DBus::Writer vw = w.sub(var);
        vw.append_int32(v);
        return w.close_container(var);
    }

    // org.freedesktop.DBus.Introspectable.Introspect -> s (XML). A minimal but
    // valid document advertising the interfaces we answer; the registry uses it
    // to know which interfaces to query.
    bool handle_introspect(DBus::CallContext& ctx) {
        if (ctx.member() != "Introspect") return false;
        static const char* kXml =
            "<!DOCTYPE node PUBLIC "
            "\"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>"
            "<interface name=\"org.a11y.atspi.Accessible\"/>"
            "<interface name=\"org.a11y.atspi.Application\"/>"
            "<interface name=\"org.freedesktop.DBus.Properties\"/>"
            "<interface name=\"org.freedesktop.DBus.Introspectable\"/>"
            "</node>";
        ctx.reply().append_string(kXml);
        return true;
    }

    // Single registered handler for the root path — dispatches by interface.
    bool dispatch_root(DBus::CallContext& ctx) {
        const std::string& iface = ctx.interface();
        if (iface == kIfaceAccessible)  return handle_accessible(ctx);
        if (iface == kIfaceApplication) return handle_application(ctx);
        if (iface == kIfaceSocket)      return handle_socket(ctx);
        if (iface == kIfaceProperties)  return handle_properties(ctx);
        if (iface == kIfaceIntrospect)  return handle_introspect(ctx);
        // Some clients send method calls with no interface (legacy). Try the
        // Accessible set as a best-effort fallback.
        if (iface.empty()) return handle_accessible(ctx);
        return false;
    }

    // Perform the Socket.Embed handshake: tell the registry "here is my root";
    // it returns ITS root reference (so) which becomes our Parent. Returns true
    // on a well-formed reply.
    //
    //   org.a11y.atspi.Socket.Embed(plug (so)) -> socket (so)
    //
    // We pass our own (unique_name, /…/root); the registry replies with its
    // root accessible reference, which we record as this app root's Parent so
    // the accessible tree links up to the desktop. The registry typically also
    // sets Application.Id via Properties.Set afterwards (serviced by dispatch()).
    bool embed_with_registry() {
        const std::string my_name = bus.unique_name();
        bool ok = bus.call_method(
            kRegistryName, kRootPath, kIfaceSocket, "Embed",
            [&](DBus::Writer& w) {
                // arg: plug (so)
                auto s = w.open_struct();
                DBus::Writer sw = w.sub(s);
                sw.append_string(my_name);
                sw.append_object_path(kRootPath);
                w.close_container(s);
            },
            [&](DBus::Reader& r) {
                // reply: socket (so)
                DBus::Reader sub;
                if (r.recurse(sub)) {
                    std::string name, path;
                    sub.read_string(name);
                    sub.read_string(path);
                    registry_parent_name = name;
                    registry_parent_path = path;
                }
            });
        return ok;
    }
};

}  // namespace

void* init_accessibility(View& root, void* /*native_window*/) {
    // Honest-fail ladder: no libdbus, no session bus, or no a11y daemon → return
    // a benign non-null handle (matching the historic stub) so callers can
    // unconditionally pair it with shutdown_accessibility(), but do no work.
    if (!DBus::library_available()) {
        runtime::log_info("Linux AT-SPI: libdbus unavailable; accessibility disabled");
        return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    }

    auto provider = std::make_unique<AtspiProvider>();
    provider->root = &root;

    if (!provider->bus.connect_session() || !provider->bus.connect_a11y_bus()) {
        runtime::log_info("Linux AT-SPI: no a11y bus (headless?); accessibility disabled");
        return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    }

    AtspiProvider* p = provider.get();
    const bool exported = provider->bus.register_object(
        kRootPath, [p](DBus::CallContext& ctx) { return p->dispatch_root(ctx); });
    if (!exported) {
        runtime::log_warn("Linux AT-SPI: failed to export root accessible");
        return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    }

    // Embed our root with the registry (Socket.Embed (so)->(so)); the registry's
    // returned reference becomes our Parent and it assigns Application.Id via a
    // Properties.Set that dispatch() services. A failed Embed (registry absent /
    // slow) is non-fatal: the root stays exported and the registry can still
    // discover + query us once it appears.
    if (!provider->embed_with_registry()) {
        runtime::log_info("Linux AT-SPI: Socket.Embed not completed "
                          "(registry absent?); root remains exported");
    }

    runtime::log_info("Linux AT-SPI: registered root accessible on a11y bus");

    // Run-loop seam: the registry/Orca call methods on the exported root
    // asynchronously. The host MUST pump DBus::dispatch() periodically or those
    // calls hang the AT (and Orca shows nothing). The handle exposes pump();
    // a host with an event loop should call pump() each frame / on a timer.
    //
    // Wiring point: the X11 plugin-view host (plugin_view_host_linux.cpp) has no
    // internal loop (the DAW pumps), and the standalone SDL host
    // (sdl_window_host.cpp run_event_loop) does — the parent wires pump() into
    // whichever host owns this handle. Left to the host so this TU stays
    // loop-agnostic.
    return provider.release();
}

void shutdown_accessibility(void* handle) {
    // Distinguish the benign sentinel (uintptr 1) from a real provider pointer.
    if (handle == reinterpret_cast<void*>(static_cast<uintptr_t>(1)) || !handle) {
        runtime::log_info("Linux AT-SPI: shutdown (no provider)");
        return;
    }
    auto* p = static_cast<AtspiProvider*>(handle);
    p->bus.unregister_object(kRootPath);
    delete p;  // closes the a11y connection via DBus dtor
    runtime::log_info("Linux AT-SPI: root accessible torn down");
}

// Pump the a11y connection so the registry's inbound method calls are serviced.
// The host calls this from its event loop / timer. Safe on the sentinel handle.
void accessibility_pump(void* handle) {
    if (handle == reinterpret_cast<void*>(static_cast<uintptr_t>(1)) || !handle) return;
    static_cast<AtspiProvider*>(handle)->bus.dispatch(0);
}

void accessibility_tree_changed(void* /*handle*/) {
    // L7b: structural change → re-export the per-widget subtree and emit
    // children-changed on the root. No-op until the View subtree is exported.
}

// L7c event-raising surface. These emit org.a11y.atspi.Event.Object signals
// (StateChanged / PropertyChange) once the per-widget objects exist. No-op for
// L7a-2 (registration only).
void notify_accessibility_value_changed(void* /*handle*/, View& /*target*/) {}
void notify_accessibility_focus_changed(void* /*handle*/, View& /*target*/) {}
void notify_accessibility_name_changed(void* /*handle*/, View& /*target*/) {}

}  // namespace pulp::view

#endif // __linux__
