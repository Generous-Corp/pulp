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
// What this TU exports:
//   * /org/a11y/atspi/accessible/root — the APPLICATION accessible
//     (org.a11y.atspi.Accessible + org.a11y.atspi.Application). Its
//     GetChildren returns the top-level accessible Views.
//   * /org/a11y/atspi/accessible/<n> — one object PER accessible View
//     (org.a11y.atspi.Accessible + org.a11y.atspi.Component), built by a
//     depth-first walk of the View tree. The Socket.Embed handshake against
//     the registry links the application up to the desktop.
//
// The per-widget tree is rebuilt on accessibility_tree_changed(), which also
// emits org.a11y.atspi.Event.Object.ChildrenChanged. On top of Accessible +
// Component, a widget exports exactly the interfaces it can serve — decided by
// the shared accessibility_interfaces() predicate, so this bridge and the tests
// on the macOS gate agree about a TU that only ever compiles on Linux:
//
//   * org.a11y.atspi.Value — a NUMERIC range (slider / meter / progress bar
//     implementing AccessibilityValueInterface): CurrentValue / Minimum /
//     Maximum / MinimumIncrement through the Properties interface. CurrentValue
//     is writable and routes back into set_current_value, the same path a user
//     adjust takes.
//   * org.a11y.atspi.Text — a STRING value: a TextEditor's content (through
//     AccessibilityTextInterface) or a ComboBox's selected item (the
//     access_value slot). AT-SPI has no string-value interface, so a role that
//     exports neither Value nor Text reads back nothing at all — Orca announces
//     "entry" and the name, then silence. That was the state of every Pulp
//     ENTRY / COMBO_BOX before this interface was wired.
//
// A check/press state is not a value in AT-SPI — it is carried in the STATE
// bitfield (CHECKABLE / CHECKED / PRESSED / INDETERMINATE), see append_state.
// The notify_* hooks emit
// the matching org.a11y.atspi.Event.Object signals (StateChanged "focused",
// PropertyChange "accessible-value" / "accessible-name") so a listening registry
// re-reads the changed object.
//
// Tree-walk model (mirrors mac collect_accessible / Windows build_fragment_nodes):
// depth-first; every View with a non-none AccessRole becomes one accessible
// object; AccessRole::none containers are skipped as objects but recursed into,
// so a deeply-nested accessible descendant re-parents onto the nearest
// accessible ancestor (or onto the application root when there is none).
//
// Run-loop requirement: the registry calls methods on us asynchronously, so the
// host must pump DBus::dispatch() periodically or those calls hang the AT. The
// handle exposes pump(); see init_accessibility()'s tail comment for the seam.

#if defined(__linux__) && !defined(__ANDROID__)

#include <pulp/view/accessibility.hpp>
#include <pulp/view/accessibility_provider.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/platform/atspi_mapping.hpp>
#include <pulp/platform/dbus.hpp>
#include <pulp/runtime/log.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

namespace {

using pulp::platform::DBus;

// AT-SPI well-known names / paths (the published wire constants).
constexpr const char* kRootPath      = "/org/a11y/atspi/accessible/root";
constexpr const char* kNullPath      = "/org/a11y/atspi/null";
constexpr const char* kAccessiblePrefix = "/org/a11y/atspi/accessible/";
constexpr const char* kRegistryName  = "org.a11y.atspi.Registry";
constexpr const char* kIfaceAccessible   = "org.a11y.atspi.Accessible";
constexpr const char* kIfaceApplication  = "org.a11y.atspi.Application";
constexpr const char* kIfaceComponent    = "org.a11y.atspi.Component";
constexpr const char* kIfaceValue        = "org.a11y.atspi.Value";
constexpr const char* kIfaceText         = "org.a11y.atspi.Text";
constexpr const char* kIfaceSocket       = "org.a11y.atspi.Socket";
constexpr const char* kIfaceEventObject  = "org.a11y.atspi.Event.Object";
constexpr const char* kIfaceProperties   = "org.freedesktop.DBus.Properties";
constexpr const char* kIfaceIntrospect   = "org.freedesktop.DBus.Introspectable";

// Pulp's toolkit identity reported via Application properties.
constexpr const char* kToolkitName    = "Pulp";
constexpr const char* kToolkitVersion = "1.0";
constexpr const char* kAtspiVersion   = "2.1";

// org.a11y.atspi.Component CoordType: 0 = screen, 1 = window. Pulp's View
// bounds are window-relative (no live screen origin without the native window),
// so window coords are the honest answer; screen-coord GetExtents falls back to
// the same window-relative box (a host that knows the window origin can offset).
constexpr uint32_t kCoordWindow = 1;

// ── Accessible-tree node ─────────────────────────────────────────────────────
//
// A View's static place in the exported accessible tree, captured when the set
// is built. Children / parent are stored as indices into AtspiProvider::nodes_
// so navigation is pure arithmetic (no live View-tree walk per D-Bus call).
// index 0 is reserved for the application root; per-widget nodes start at 1.
struct AccessNode {
    View* view = nullptr;          // null only for the root node (index 0)
    int parent_index = 0;          // 0 ⇒ parent is the application root
    std::vector<int> children;     // indices into nodes_, in DFS order
};

// The exported accessible tree + its owning a11y connection. Stored as the
// opaque handle returned by init_accessibility(). One per window/root.
struct AtspiProvider {
    DBus bus;                       // owns the a11y-bus connection
    View* root = nullptr;           // the View tree this application maps

    // The registry's root accessible, captured from Socket.Embed: (bus_name,
    // object_path). Reported as the application root's Parent so the tree
    // connects up to the desktop.
    std::string registry_parent_name;
    std::string registry_parent_path;

    // Application.Id, assigned by the registry via Properties.Set during/after
    // Embed. -1 until set.
    int app_id = -1;

    // index → node. nodes_[0] is the application root (view == nullptr); every
    // other entry is one accessible View. View* → index lets event hooks and
    // GetIndexInParent resolve a View quickly.
    std::vector<AccessNode> nodes_;
    std::unordered_map<const View*, int> view_to_index_;

    // ── Path / addressing helpers ────────────────────────────────────────────

    std::string path_for_index(int index) const {
        if (index <= 0) return kRootPath;
        return std::string(kAccessiblePrefix) + std::to_string(index);
    }

    // Parse "/org/a11y/atspi/accessible/<n>" → index; root path → 0; -1 if not
    // one of our object paths.
    int index_for_path(const std::string& path) const {
        if (path == kRootPath) return 0;
        const std::string prefix = kAccessiblePrefix;
        if (path.size() <= prefix.size() ||
            path.compare(0, prefix.size(), prefix) != 0) {
            return -1;
        }
        const std::string tail = path.substr(prefix.size());
        if (tail.empty() || tail == "root") return 0;
        int v = 0;
        for (char c : tail) {
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        if (v < 0 || v >= static_cast<int>(nodes_.size())) return -1;
        return v;
    }

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
    // Reference to one of OUR objects, addressed at our own unique bus name.
    void append_self_ref(DBus::Writer& w, int index) {
        append_object_ref(w, bus.unique_name(), path_for_index(index));
    }

    // ── State ────────────────────────────────────────────────────────────────

    // GetState returns au (array of two uint32 = the 64-bit state bitfield).
    void append_state(DBus::Writer& w, int index) {
        atspi::StateSet st = atspi::default_states();
        if (index > 0 && index < static_cast<int>(nodes_.size())) {
            if (View* v = nodes_[static_cast<size_t>(index)].view) {
                if (v->focusable()) atspi::set_state(st, atspi::kStateFocusable);
                if (v->has_focus()) atspi::set_state(st, atspi::kStateFocused);
                // AT-SPI has no "value" for a checkbox: CHECKED / PRESSED /
                // INDETERMINATE STATE bits are how Orca announces it. Without
                // them a Pulp Checkbox announced "check box <name>" and no
                // state. Resolved through the same helper every other bridge
                // uses (accessibility_toggle_state).
                st = atspi::with_toggle_state(st, accessibility_toggle_state(*v),
                                              !v->access_checked().empty());
                st = atspi::with_editable(st, accessibility_text_editable(*v));
            }
        }
        auto a = w.open_array("u");
        DBus::Writer aw = w.sub(a);
        aw.append_uint32(st.low);
        aw.append_uint32(st.high);
        w.close_container(a);
    }

    // ── Tree construction (DFS with re-parenting onto nearest accessible) ─────

    void build_node(View& v, int parent_index) {
        int my_index = parent_index;
        if (is_accessibility_element(v)) {
            AccessNode node;
            node.view = &v;
            node.parent_index = parent_index;
            my_index = static_cast<int>(nodes_.size());
            nodes_.push_back(node);
            view_to_index_[&v] = my_index;
            nodes_[static_cast<size_t>(parent_index)].children.push_back(my_index);
        }
        for (size_t i = 0; i < v.child_count(); ++i) {
            if (View* child = v.child_at(i)) build_node(*child, my_index);
        }
    }

    void rebuild_tree() {
        nodes_.clear();
        view_to_index_.clear();
        // index 0 = the application root (no View; it IS the application).
        nodes_.push_back(AccessNode{});
        if (!root) return;
        // The root View itself is the application accessible (index 0), matching
        // snapshot_accessibility_tree / mac / Windows which never expose the
        // root View as a child object — its children become the top-level
        // accessibles.
        for (size_t i = 0; i < root->child_count(); ++i) {
            if (View* child = root->child_at(i)) build_node(*child, 0);
        }
    }

    // ── org.a11y.atspi.Accessible (any object) ────────────────────────────────

    bool handle_accessible(DBus::CallContext& ctx, int index) {
        const std::string& m = ctx.member();
        const bool is_root = (index == 0);
        AccessNode& node = nodes_[static_cast<size_t>(index)];

        if (m == "GetRole") {
            ctx.reply().append_uint32(
                is_root ? atspi::kRoleApplication
                        : atspi::role_to_atspi_role(node.view->access_role()));
            return true;
        }
        if (m == "GetRoleName") {
            ctx.reply().append_string(
                is_root ? "application"
                        : atspi::role_to_atspi_role_name(node.view->access_role()));
            return true;
        }
        if (m == "GetLocalizedRoleName") {
            ctx.reply().append_string(
                is_root ? "application"
                        : atspi::role_to_atspi_role_name(node.view->access_role()));
            return true;
        }
        if (m == "GetChildCount") {
            ctx.reply().append_int32(static_cast<int>(node.children.size()));
            return true;
        }
        if (m == "GetChildren") {
            // a(so) — child object references.
            DBus::Writer w = ctx.reply();
            auto a = w.open_array("(so)");
            DBus::Writer aw = w.sub(a);
            for (int child : node.children) append_self_ref(aw, child);
            w.close_container(a);
            return true;
        }
        if (m == "GetChildAtIndex") {
            int i = 0;
            ctx.args().read_int32(i);
            DBus::Writer w = ctx.reply();
            if (i >= 0 && i < static_cast<int>(node.children.size())) {
                append_self_ref(w, node.children[static_cast<size_t>(i)]);
            } else {
                append_object_ref(w, "", kNullPath);  // (so) null reference
            }
            return true;
        }
        if (m == "GetIndexInParent") {
            int result = -1;
            if (!is_root) {
                const AccessNode& parent =
                    nodes_[static_cast<size_t>(node.parent_index)];
                for (size_t i = 0; i < parent.children.size(); ++i) {
                    if (parent.children[i] == index) {
                        result = static_cast<int>(i);
                        break;
                    }
                }
            }
            ctx.reply().append_int32(result);
            return true;
        }
        if (m == "GetState") {
            DBus::Writer w = ctx.reply();
            append_state(w, index);
            return true;
        }
        if (m == "GetInterfaces") {
            DBus::Writer w = ctx.reply();
            auto a = w.open_array("s");
            DBus::Writer aw = w.sub(a);
            aw.append_string(kIfaceAccessible);
            if (is_root) {
                aw.append_string(kIfaceApplication);
            } else {
                aw.append_string(kIfaceComponent);
                // Exactly the interfaces the View can actually serve
                // (accessibility_interfaces): Value for a numeric range
                // (slider/meter/progress bar), Text for a string value — a
                // TextEditor's content or a ComboBox's selected item. AT-SPI has
                // no string-value interface, so a role like ENTRY or COMBO_BOX
                // with neither interface exported reads back NOTHING: Orca
                // announces "entry" plus the name and falls silent.
                const atspi_ifaces ifs = interfaces_for(index);
                if (ifs.value) aw.append_string(kIfaceValue);
                if (ifs.text)  aw.append_string(kIfaceText);
            }
            w.close_container(a);
            return true;
        }
        if (m == "GetApplication") {
            // (so) — the application root object.
            DBus::Writer w = ctx.reply();
            append_self_ref(w, 0);
            return true;
        }
        if (m == "GetAttributes") {
            // a{ss} — none.
            DBus::Writer w = ctx.reply();
            auto a = w.open_array("{ss}");
            w.close_container(a);
            return true;
        }
        if (m == "GetRelationSet") {
            // a(ua(so)) — none.
            DBus::Writer w = ctx.reply();
            auto a = w.open_array("(ua(so))");
            w.close_container(a);
            return true;
        }
        return false;  // decline → UnknownMethod
    }

    // ── org.a11y.atspi.Component (per-widget objects) ─────────────────────────
    //
    // Root-relative bounds: walk parent chain summing bounds().x/y (mirrors the
    // mac accessibilityFrame loop + the Windows view_to_screen_rect loop), then
    // the View's own width/height. These are window-relative coordinates.

    void compute_extents(View* v, int& x, int& y, int& w, int& h) {
        x = y = w = h = 0;
        if (!v) return;
        float rx = 0, ry = 0;
        for (View* cur = v; cur; cur = cur->parent()) {
            rx += cur->bounds().x;
            ry += cur->bounds().y;
        }
        x = static_cast<int>(rx);
        y = static_cast<int>(ry);
        w = static_cast<int>(v->bounds().width);
        h = static_cast<int>(v->bounds().height);
    }

    bool handle_component(DBus::CallContext& ctx, int index) {
        if (index == 0) return false;  // the application root has no geometry
        View* v = nodes_[static_cast<size_t>(index)].view;
        const std::string& m = ctx.member();

        if (m == "GetExtents") {
            // (u coord_type) -> (iiii x,y,w,h). We report window-relative box
            // regardless of the requested coord type (no live screen origin).
            int x, y, w, h;
            compute_extents(v, x, y, w, h);
            DBus::Writer rw = ctx.reply();
            auto s = rw.open_struct();
            DBus::Writer sw = rw.sub(s);
            sw.append_int32(x);
            sw.append_int32(y);
            sw.append_int32(w);
            sw.append_int32(h);
            rw.close_container(s);
            return true;
        }
        if (m == "GetPosition") {
            // (u coord_type) -> ii x,y
            int x, y, w, h;
            compute_extents(v, x, y, w, h);
            DBus::Writer rw = ctx.reply();
            rw.append_int32(x);
            rw.append_int32(y);
            return true;
        }
        if (m == "GetSize") {
            // () -> ii w,h
            int x, y, w, h;
            compute_extents(v, x, y, w, h);
            DBus::Writer rw = ctx.reply();
            rw.append_int32(w);
            rw.append_int32(h);
            return true;
        }
        if (m == "Contains") {
            // (ii x,y, u coord_type) -> b
            int px = 0, py = 0;
            ctx.args().read_int32(px);
            ctx.args().read_int32(py);
            int x, y, w, h;
            compute_extents(v, x, y, w, h);
            const bool inside = px >= x && px < x + w && py >= y && py < y + h;
            ctx.reply().append_bool(inside);
            return true;
        }
        if (m == "GetLayer") {
            // () -> u. ATSPI_LAYER_WIDGET = 3.
            ctx.reply().append_uint32(3u);
            return true;
        }
        if (m == "GrabFocus") {
            // () -> b. Request focus on the View; report success.
            if (v) v->set_focus(true);
            ctx.reply().append_bool(true);
            return true;
        }
        return false;
    }

    // ── org.a11y.atspi.Application (root only) ─────────────────────────────────

    bool handle_application(DBus::CallContext& ctx) {
        const std::string& m = ctx.member();
        if (m == "GetLocale") {
            ctx.reply().append_string("C");  // (u category) -> s
            return true;
        }
        if (m == "RegisterEventListener" || m == "DeregisterEventListener") {
            ctx.reply();  // empty ack
            return true;
        }
        return false;
    }

    // ── org.a11y.atspi.Socket (root only) ─────────────────────────────────────

    bool handle_socket(DBus::CallContext& ctx) {
        const std::string& m = ctx.member();
        if (m == "Embed" || m == "Unembed") {
            DBus::Writer w = ctx.reply();
            append_self_ref(w, 0);
            return true;
        }
        return false;
    }

    // ── org.freedesktop.DBus.Properties ───────────────────────────────────────

    bool handle_properties(DBus::CallContext& ctx, int index) {
        const std::string& m = ctx.member();
        if (m == "Get") {
            std::string iface, prop;
            ctx.args().read_string(iface);
            ctx.args().read_string(prop);
            DBus::Writer w = ctx.reply();
            return append_property_variant(w, index, iface, prop);
        }
        if (m == "GetAll") {
            std::string iface;
            ctx.args().read_string(iface);
            DBus::Writer w = ctx.reply();
            auto a = w.open_array("{sv}");
            append_all_properties(w, a, index, iface);
            w.close_container(a);
            return true;
        }
        if (m == "Set") {
            std::string iface, prop;
            ctx.args().read_string(iface);
            ctx.args().read_string(prop);
            // Application.Id (i) is writable — the registry sets it.
            if (index == 0 && iface == kIfaceApplication && prop == "Id") {
                DBus::Reader var;
                int v = 0;
                if (ctx.args().recurse(var) && var.read_int32(v)) app_id = v;
            }
            // Value.CurrentValue (d) is writable — an AT-driven adjust (Orca's
            // "increase/decrease") sets it; route to the View's value interface
            // along the SAME path a user adjust takes (set_current_value).
            else if (iface == kIfaceValue && prop == "CurrentValue") {
                if (AccessibilityValueInterface* vif = value_iface(index)) {
                    DBus::Reader var;
                    double v = 0.0;
                    if (ctx.args().recurse(var) && var.read_double(v)) {
                        vif->set_current_value(v);
                    }
                }
            }
            ctx.reply();  // empty success ack
            return true;
        }
        return false;
    }

    const std::string& name_for(int index) const {
        static const std::string kToolkit = kToolkitName;
        if (index == 0) return kToolkit;
        return nodes_[static_cast<size_t>(index)].view->access_label();
    }

    // The View→AccessibilityValueInterface accessor, identical to the one
    // accessibility_win.cpp (W6) uses: a dynamic_cast on the View. mac, Windows,
    // and Linux must agree on which Views are value-bearing, so all three resolve
    // it the same way — never via role inspection. Null for the application root
    // and for any View that does not implement the interface.
    AccessibilityValueInterface* value_iface(int index) const {
        if (index <= 0 || index >= static_cast<int>(nodes_.size())) return nullptr;
        View* v = nodes_[static_cast<size_t>(index)].view;
        return v ? dynamic_cast<AccessibilityValueInterface*>(v) : nullptr;
    }

    // The View behind an index, or nullptr for the application root.
    View* view_at(int index) const {
        if (index <= 0 || index >= static_cast<int>(nodes_.size())) return nullptr;
        return nodes_[static_cast<size_t>(index)].view;
    }

    // The optional AccessibilityTextInterface (a TextEditor). A ComboBox has
    // none — its string sits in the access_value slot — so every read goes
    // through the shared resolvers below, never through this pointer alone.
    AccessibilityTextInterface* text_iface(int index) const {
        View* v = view_at(index);
        return v ? dynamic_cast<AccessibilityTextInterface*>(v) : nullptr;
    }

    using atspi_ifaces = AccessibilityInterfaceSet;
    atspi_ifaces interfaces_for(int index) const {
        View* v = view_at(index);
        return v ? accessibility_interfaces(*v) : atspi_ifaces{};
    }

    // Text.GetText / Text.CharacterCount content — the SHARED resolver
    // (accessibility_text_content): text interface, else the access_value slot.
    std::string text_for(int index) const {
        View* v = view_at(index);
        return v ? accessibility_text_content(*v) : std::string{};
    }

    int caret_for(int index) const {
        if (AccessibilityTextInterface* tif = text_iface(index))
            return tif->get_selection().second;
        return 0;
    }

    // ── org.a11y.atspi.Text ───────────────────────────────────────────────────
    //
    // The string-value surface. Implemented: GetText, GetCharacterAtOffset,
    // GetNSelections / GetSelection, SetCaretOffset, plus the CharacterCount and
    // CaretOffset properties — enough for Orca to read an entry's content and
    // track the caret. NOT implemented: GetTextAtOffset / GetStringAtOffset word
    // and line granularity, text attributes, and bounded ranges (they need a
    // laid-out glyph run per line, which TextEditor does not expose yet), so
    // Orca's word/line navigation inside a field is still a gap.
    bool handle_text(DBus::CallContext& ctx, int index) {
        if (index == 0) return false;
        if (!interfaces_for(index).text) return false;
        const std::string& m = ctx.member();
        const std::string text = text_for(index);
        const int n = static_cast<int>(text.size());

        if (m == "GetText") {
            int start = 0, end = -1;
            ctx.args().read_int32(start);
            ctx.args().read_int32(end);
            if (start < 0) start = 0;
            if (end < 0 || end > n) end = n;   // -1 = "to the end", per AT-SPI
            const std::string slice =
                start >= end ? std::string{}
                             : text.substr(static_cast<size_t>(start),
                                           static_cast<size_t>(end - start));
            ctx.reply().append_string(slice);
            return true;
        }
        if (m == "GetCharacterCount") {   // some clients call it as a method
            ctx.reply().append_int32(n);
            return true;
        }
        if (m == "GetCaretOffset") {
            ctx.reply().append_int32(caret_for(index));
            return true;
        }
        if (m == "SetCaretOffset") {
            int off = 0;
            ctx.args().read_int32(off);
            bool ok = false;
            if (AccessibilityTextInterface* tif = text_iface(index)) {
                if (off >= 0 && off <= n) {
                    tif->set_selection(off, off);
                    ok = true;
                }
            }
            ctx.reply().append_bool(ok);
            return true;
        }
        if (m == "GetCharacterAtOffset") {
            int off = 0;
            ctx.args().read_int32(off);
            const int c = (off >= 0 && off < n)
                ? static_cast<int>(static_cast<unsigned char>(text[static_cast<size_t>(off)]))
                : 0;
            ctx.reply().append_int32(c);
            return true;
        }
        if (m == "GetNSelections") {
            int count = 0;
            if (AccessibilityTextInterface* tif = text_iface(index)) {
                const auto sel = tif->get_selection();
                count = sel.first != sel.second ? 1 : 0;
            }
            ctx.reply().append_int32(count);
            return true;
        }
        if (m == "GetSelection") {
            int which = 0;
            ctx.args().read_int32(which);
            int start = 0, end = 0;
            if (AccessibilityTextInterface* tif = text_iface(index); tif && which == 0) {
                const auto sel = tif->get_selection();
                start = sel.first;
                end = sel.second;
            }
            DBus::Writer w = ctx.reply();
            w.append_int32(start);
            w.append_int32(end);
            return true;
        }
        return false;
    }

    bool append_property_variant(DBus::Writer& w, int index,
                                 const std::string& iface,
                                 const std::string& prop) {
        const bool is_root = (index == 0);
        const AccessNode& node = nodes_[static_cast<size_t>(index)];
        if (iface == kIfaceAccessible) {
            if (prop == "Name") { return variant_string(w, name_for(index)); }
            if (prop == "Description") { return variant_string(w, ""); }
            if (prop == "Parent") {
                auto v = w.open_variant("(so)");
                DBus::Writer vw = w.sub(v);
                if (is_root) {
                    append_object_ref(vw, registry_parent_name,
                                      registry_parent_path);
                } else {
                    append_self_ref(vw, node.parent_index);
                }
                w.close_container(v);
                return true;
            }
            if (prop == "ChildCount") {
                return variant_int32(w, static_cast<int>(node.children.size()));
            }
        }
        if (is_root && iface == kIfaceApplication) {
            if (prop == "ToolkitName") { return variant_string(w, kToolkitName); }
            if (prop == "Version") { return variant_string(w, kToolkitVersion); }
            if (prop == "AtspiVersion") { return variant_string(w, kAtspiVersion); }
            if (prop == "Id") { return variant_int32(w, app_id); }
        }
        if (iface == kIfaceText && interfaces_for(index).text) {
            if (prop == "CharacterCount") {
                return variant_int32(w, static_cast<int>(text_for(index).size()));
            }
            if (prop == "CaretOffset") {
                return variant_int32(w, caret_for(index));
            }
        }
        if (iface == kIfaceValue) {
            if (AccessibilityValueInterface* vif = value_iface(index)) {
                if (prop == "CurrentValue") {
                    return variant_double(w, vif->get_current_value());
                }
                if (prop == "MinimumValue") {
                    return variant_double(w, vif->get_minimum_value());
                }
                if (prop == "MaximumValue") {
                    return variant_double(w, vif->get_maximum_value());
                }
                if (prop == "MinimumIncrement") {
                    return variant_double(w, vif->get_step_size());
                }
            }
        }
        return false;
    }

    void append_all_properties(DBus::Writer& w, DBus::Writer::Container& arr,
                               int index, const std::string& iface) {
        const bool is_root = (index == 0);
        const AccessNode& node = nodes_[static_cast<size_t>(index)];
        DBus::Writer aw = w.sub(arr);
        auto entry = [&](const char* key, auto appender) {
            auto e = aw.open_dict_entry();
            DBus::Writer ew = aw.sub(e);
            ew.append_string(key);
            appender(ew);
            aw.close_container(e);
        };
        if (iface == kIfaceAccessible) {
            const std::string nm = name_for(index);
            entry("Name", [&](DBus::Writer& ew) { variant_string(ew, nm); });
            entry("ChildCount", [&](DBus::Writer& ew) {
                variant_int32(ew, static_cast<int>(node.children.size()));
            });
        } else if (is_root && iface == kIfaceApplication) {
            entry("ToolkitName",
                  [&](DBus::Writer& ew) { variant_string(ew, kToolkitName); });
            entry("Version",
                  [&](DBus::Writer& ew) { variant_string(ew, kToolkitVersion); });
            entry("AtspiVersion",
                  [&](DBus::Writer& ew) { variant_string(ew, kAtspiVersion); });
            entry("Id", [&](DBus::Writer& ew) { variant_int32(ew, app_id); });
        } else if (iface == kIfaceText && interfaces_for(index).text) {
            const int count = static_cast<int>(text_for(index).size());
            const int caret = caret_for(index);
            entry("CharacterCount",
                  [&](DBus::Writer& ew) { variant_int32(ew, count); });
            entry("CaretOffset",
                  [&](DBus::Writer& ew) { variant_int32(ew, caret); });
        } else if (iface == kIfaceValue) {
            if (AccessibilityValueInterface* vif = value_iface(index)) {
                entry("CurrentValue", [&](DBus::Writer& ew) {
                    variant_double(ew, vif->get_current_value());
                });
                entry("MinimumValue", [&](DBus::Writer& ew) {
                    variant_double(ew, vif->get_minimum_value());
                });
                entry("MaximumValue", [&](DBus::Writer& ew) {
                    variant_double(ew, vif->get_maximum_value());
                });
                entry("MinimumIncrement", [&](DBus::Writer& ew) {
                    variant_double(ew, vif->get_step_size());
                });
            }
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
    static bool variant_double(DBus::Writer& w, double v) {
        auto var = w.open_variant("d");
        DBus::Writer vw = w.sub(var);
        vw.append_double(v);
        return w.close_container(var);
    }

    // ── org.freedesktop.DBus.Introspectable ───────────────────────────────────

    bool handle_introspect(DBus::CallContext& ctx, int index) {
        if (ctx.member() != "Introspect") return false;
        static const char* kRootXml =
            "<!DOCTYPE node PUBLIC "
            "\"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>"
            "<interface name=\"org.a11y.atspi.Accessible\"/>"
            "<interface name=\"org.a11y.atspi.Application\"/>"
            "<interface name=\"org.freedesktop.DBus.Properties\"/>"
            "<interface name=\"org.freedesktop.DBus.Introspectable\"/>"
            "</node>";
        static const char* kWidgetHead =
            "<!DOCTYPE node PUBLIC "
            "\"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>"
            "<interface name=\"org.a11y.atspi.Accessible\"/>"
            "<interface name=\"org.a11y.atspi.Component\"/>";
        static const char* kWidgetTail =
            "<interface name=\"org.freedesktop.DBus.Properties\"/>"
            "<interface name=\"org.freedesktop.DBus.Introspectable\"/>"
            "</node>";
        if (index == 0) {
            ctx.reply().append_string(kRootXml);
        } else {
            std::string xml = kWidgetHead;
            const atspi_ifaces ifs = interfaces_for(index);
            if (ifs.value) xml += "<interface name=\"org.a11y.atspi.Value\"/>";
            if (ifs.text)  xml += "<interface name=\"org.a11y.atspi.Text\"/>";
            xml += kWidgetTail;
            ctx.reply().append_string(xml);
        }
        return true;
    }

    // ── Dispatch (one handler per registered object path) ─────────────────────

    bool dispatch_object(DBus::CallContext& ctx, int index) {
        if (index < 0 || index >= static_cast<int>(nodes_.size())) return false;
        const std::string& iface = ctx.interface();
        if (iface == kIfaceAccessible)  return handle_accessible(ctx, index);
        if (iface == kIfaceComponent)   return handle_component(ctx, index);
        if (iface == kIfaceApplication) return index == 0 && handle_application(ctx);
        if (iface == kIfaceSocket)      return index == 0 && handle_socket(ctx);
        if (iface == kIfaceProperties)  return handle_properties(ctx, index);
        if (iface == kIfaceIntrospect)  return handle_introspect(ctx, index);
        // Legacy interface-less calls → best-effort Accessible.
        if (iface.empty()) return handle_accessible(ctx, index);
        return false;
    }

    // ── Object registration ───────────────────────────────────────────────────
    //
    // Each object path gets its own handler closure carrying its index. The
    // generic object server resolves the path → handler; we resolve member.

    bool export_all() {
        bool ok = true;
        for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
            const std::string path = path_for_index(i);
            const int index = i;
            ok = bus.register_object(
                     path,
                     [this, index](DBus::CallContext& ctx) {
                         return dispatch_object(ctx, index);
                     }) &&
                 ok;
        }
        return ok;
    }

    void unexport_all() {
        for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
            bus.unregister_object(path_for_index(i));
        }
    }

    // Emit org.a11y.atspi.Event.Object.ChildrenChanged from the root. The
    // AT-SPI event body is (siiv(so)): detail string, two ints, a variant, and
    // the source-app reference. We send a "children-changed" event so a
    // listening registry re-reads the tree. (Per-object value/state/focus
    // signals are L7c.)
    void emit_children_changed() {
        bus.emit_signal(
            kRootPath, kIfaceEventObject, "ChildrenChanged",
            [this](DBus::Writer& w) {
                w.append_string("");            // detail: "" (bulk add/remove)
                w.append_int32(0);              // detail1
                w.append_int32(0);              // detail2
                auto v = w.open_variant("i");   // any_data (unused) as i
                DBus::Writer vw = w.sub(v);
                vw.append_int32(0);
                w.close_container(v);
                append_self_ref(w, 0);          // source application ref (so)
            });
    }

    // ── Per-object Event.Object signals ──────────────────────────────────────
    //
    // Most object notifications we emit share the body signature (siiva{sv}):
    // a detail string, two int32 details, an any_data variant, and a trailing
    // a{sv} properties map. ChildrenChanged uses the separate (siiv(so)) body
    // above. The signal is emitted FROM the changed object's own path so a
    // listener knows which accessible changed; `member` is the event class
    // ("StateChanged" / "PropertyChange"). `fill_variant` writes the any_data
    // value (the new state/value/name); the helper frames the rest.
    void emit_object_event(int index, const char* member,
                           const std::string& detail, int detail1, int detail2,
                           const std::function<void(DBus::Writer&)>& fill_variant) {
        if (index < 0 || index >= static_cast<int>(nodes_.size())) return;
        bus.emit_signal(
            path_for_index(index), kIfaceEventObject, member,
            [&](DBus::Writer& w) {
                w.append_string(detail);
                w.append_int32(detail1);
                w.append_int32(detail2);
                fill_variant(w);                  // any_data (v)
                auto a = w.open_array("{sv}");     // properties a{sv} — empty
                w.close_container(a);
            });
    }

    // Resolve a target View to its exported object index, or -1 if the View is
    // not part of the exported tree (e.g. an AccessRole::none container, or a
    // View built after the last rebuild).
    int index_for_view(const View* v) const {
        auto it = view_to_index_.find(v);
        return it == view_to_index_.end() ? -1 : it->second;
    }

    // ── Socket.Embed handshake (links the app up to the registry) ─────────────

    bool embed_with_registry() {
        const std::string my_name = bus.unique_name();
        bool ok = bus.call_method(
            kRegistryName, kRootPath, kIfaceSocket, "Embed",
            [&](DBus::Writer& w) {
                auto s = w.open_struct();
                DBus::Writer sw = w.sub(s);
                sw.append_string(my_name);
                sw.append_object_path(kRootPath);
                w.close_container(s);
            },
            [&](DBus::Reader& r) {
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

    provider->rebuild_tree();
    if (!provider->export_all()) {
        runtime::log_warn("Linux AT-SPI: failed to export accessible objects");
        return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    }

    // Embed the application root with the registry (Socket.Embed (so)->(so)); a
    // failed Embed (registry absent / slow) is non-fatal — the objects stay
    // exported and the registry can still discover + query us once it appears.
    if (!provider->embed_with_registry()) {
        runtime::log_info("Linux AT-SPI: Socket.Embed not completed "
                          "(registry absent?); objects remain exported");
    }

    runtime::log_info(
        "Linux AT-SPI: exported {} accessible objects on a11y bus",
        provider->nodes_.size());

    // Run-loop seam: the registry/Orca call methods on the exported objects
    // asynchronously. The host MUST pump DBus::dispatch() periodically or those
    // calls hang the AT. The handle exposes pump(); a host with an event loop
    // calls accessibility_pump() each frame / on a timer (wired in the SDL
    // standalone host's run_event_loop). The X11 plugin-view host has no
    // internal loop because the DAW owns pumping.
    return provider.release();
}

void shutdown_accessibility(void* handle) {
    if (handle == reinterpret_cast<void*>(static_cast<uintptr_t>(1)) || !handle) {
        runtime::log_info("Linux AT-SPI: shutdown (no provider)");
        return;
    }
    auto* p = static_cast<AtspiProvider*>(handle);
    p->unexport_all();
    delete p;  // closes the a11y connection via DBus dtor
    runtime::log_info("Linux AT-SPI: accessible objects torn down");
}

// Pump the a11y connection so the registry's inbound method calls are serviced.
void accessibility_pump(void* handle) {
    if (handle == reinterpret_cast<void*>(static_cast<uintptr_t>(1)) || !handle) return;
    static_cast<AtspiProvider*>(handle)->bus.dispatch(0);
}

void accessibility_tree_changed(void* handle) {
    if (handle == reinterpret_cast<void*>(static_cast<uintptr_t>(1)) || !handle) return;
    auto* p = static_cast<AtspiProvider*>(handle);
    // Tear down the old per-widget objects, rebuild from the current View tree,
    // re-export, then tell any listener the structure changed.
    p->unexport_all();
    p->rebuild_tree();
    p->export_all();
    p->emit_children_changed();
}

// L7c event-raising surface. Each emits the matching org.a11y.atspi.Event.Object
// signal FROM the changed object's path so a listening registry/Orca re-reads it.
// No-op when there is no live provider (the benign sentinel handle / honest-fail
// path) or when the target View is not part of the exported tree.

void notify_accessibility_focus_changed(void* handle, View& target) {
    if (handle == reinterpret_cast<void*>(static_cast<uintptr_t>(1)) || !handle) return;
    auto* p = static_cast<AtspiProvider*>(handle);
    int index = p->index_for_view(&target);
    if (index < 0) return;
    // StateChanged: detail = state name ("focused"), detail1 = new value (1),
    // detail2 = 0, any_data = unused (variant<i>(0)). A blur would emit detail1=0;
    // the View hook fires on focus acquisition, so report focused=true.
    p->emit_object_event(index, "StateChanged", "focused", 1, 0,
                         [](DBus::Writer& w) {
                             auto v = w.open_variant("i");
                             DBus::Writer vw = w.sub(v);
                             vw.append_int32(0);
                             w.close_container(v);
                         });
}

void notify_accessibility_value_changed(void* handle, View& target) {
    if (handle == reinterpret_cast<void*>(static_cast<uintptr_t>(1)) || !handle) return;
    auto* p = static_cast<AtspiProvider*>(handle);
    int index = p->index_for_view(&target);
    if (index < 0) return;
    // PropertyChange: detail = "accessible-value", any_data = the new current
    // value (variant<d>). Resolve through the value interface; if the target is
    // not value-bearing, fall back to 0.0 (the event still tells the registry to
    // re-read).
    double current = 0.0;
    if (AccessibilityValueInterface* vif = p->value_iface(index)) {
        current = vif->get_current_value();
    }
    p->emit_object_event(index, "PropertyChange", "accessible-value", 0, 0,
                         [current](DBus::Writer& w) {
                             AtspiProvider::variant_double(w, current);
                         });
}

void notify_accessibility_name_changed(void* handle, View& target) {
    if (handle == reinterpret_cast<void*>(static_cast<uintptr_t>(1)) || !handle) return;
    auto* p = static_cast<AtspiProvider*>(handle);
    int index = p->index_for_view(&target);
    if (index < 0) return;
    // PropertyChange: detail = "accessible-name", any_data = the new name
    // (variant<s>).
    const std::string label = target.access_label();
    p->emit_object_event(index, "PropertyChange", "accessible-name", 0, 0,
                         [label](DBus::Writer& w) {
                             AtspiProvider::variant_string(w, label);
                         });
}

// ── Test-only seam ───────────────────────────────────────────────────────────
//
// The production provider connects to the desktop a11y bus (org.a11y.Bus),
// which does NOT exist under a headless `dbus-run-session`. The loopback test
// (test/test_atspi_tree.cpp) needs to exercise the *exact same* tree-build +
// per-object Accessible/Component handlers against a bus a CI client can also
// reach, so this seam builds an AtspiProvider on the plain SESSION bus instead
// and exports the identical object set. It returns the handle (usable with
// accessibility_pump / shutdown_accessibility) plus the server's unique bus
// name so the test client can address calls at us. Returns nullptr (and leaves
// out_bus_name empty) when no session bus is reachable. NOT used in production.
void* init_accessibility_on_session_bus_for_test(View& root,
                                                 std::string* out_bus_name) {
    if (out_bus_name) out_bus_name->clear();
    if (!DBus::library_available()) return nullptr;

    auto provider = std::make_unique<AtspiProvider>();
    provider->root = &root;
    if (!provider->bus.connect_session()) return nullptr;
    if (out_bus_name) *out_bus_name = provider->bus.unique_name();

    provider->rebuild_tree();
    if (!provider->export_all()) return nullptr;
    return provider.release();
}

}  // namespace pulp::view

#endif // __linux__
