#pragma once

#include <pulp/view/window_host.hpp>
#include <pulp/view/theme.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

// ── Window type hints ───────────────────────────────────────────────────────

enum class WindowType {
    main,       ///< Primary editor window
    palette,    ///< Floating palette (stays on top, no taskbar entry)
    inspector,  ///< Resizable inspector panel
    popup,      ///< Ephemeral popup/dropdown (auto-dismiss on focus loss)
    dialog      ///< Modal or modeless dialog
};

// ── Screen / monitor information ────────────────────────────────────────────

struct ScreenInfo {
    int id = 0;                  ///< Platform screen index
    float x = 0, y = 0;         ///< Origin in global coordinates
    float width = 0;             ///< Logical width
    float height = 0;            ///< Logical height
    float scale_factor = 1.0f;   ///< HiDPI scale (e.g. 2.0 for Retina)
    bool is_primary = false;     ///< Whether this is the main/primary screen
    std::string name;            ///< Human-readable name (e.g. "Built-in Retina Display")
};

// ── Window state for save / restore ─────────────────────────────────────────

struct WindowState {
    float x = 0, y = 0;
    float width = 400, height = 300;
    int screen_id = 0;           ///< Which screen the window was on
    bool is_visible = true;
};

// ── Per-window record in the manager ────────────────────────────────────────

using WindowId = uint32_t;

struct WindowRecord {
    WindowId id = 0;
    WindowType type = WindowType::main;
    WindowId parent_id = 0;      ///< 0 = no parent (root window)
    WindowHost* host = nullptr;  ///< Non-owning — caller owns the WindowHost
    View* root_view = nullptr;   ///< Root view for this window
    WindowState state;
    bool alive = true;
};

// ── Inter-window message ────────────────────────────────────────────────────

struct WindowMessage {
    std::string type;            ///< Message type identifier
    std::string payload;         ///< JSON or string payload
    WindowId source = 0;         ///< Sender window (0 = broadcast)
};

using MessageHandler = std::function<void(const WindowMessage&)>;

// ── WindowManager ───────────────────────────────────────────────────────────
//
// Coordination layer for multiple windows. Manages:
//   - Registry of active windows
//   - Parent-child relationships and cascading close
//   - Theme propagation across all windows
//   - Inter-window message passing
//   - Screen/monitor enumeration
//   - Window state save/restore
//   - Shared GPU device reference for Dawn-based rendering
//
// Thread safety: all public methods are safe to call from the main thread.
// Audio-thread code should not call WindowManager directly.

class WindowManager {
public:
    WindowManager();
    ~WindowManager();

    WindowManager(const WindowManager&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;

    // ── Window registration ─────────────────────────────────────────────

    /// Register a window with the manager. Returns a unique WindowId.
    /// The caller retains ownership of host and root_view.
    WindowId register_window(WindowHost* host, View* root_view,
                             WindowType type, WindowId parent_id = 0);

    /// Remove a window from the registry. Does NOT destroy the WindowHost —
    /// that's the caller's responsibility.
    void unregister_window(WindowId id);

    /// Get the record for a window (nullptr if not found).
    const WindowRecord* window(WindowId id) const;

    /// Get all active window IDs.
    std::vector<WindowId> window_ids() const;

    /// Get children of a given window.
    std::vector<WindowId> children_of(WindowId parent_id) const;

    /// Count of active windows.
    size_t window_count() const;

    // ── Cascading close ─────────────────────────────────────────────────

    /// Close a window and all its children (depth-first).
    /// Calls request_close() on each WindowHost, then unregisters.
    void close_window(WindowId id);

    /// Set a callback invoked when a window is about to be unregistered.
    /// Useful for cleanup (e.g. saving window state before removal).
    void set_on_window_closed(std::function<void(WindowId)> cb);

    // ── Theme propagation ───────────────────────────────────────────────

    /// Set the shared theme. Propagates to all registered windows'
    /// root views via set_theme().
    void set_shared_theme(const Theme& theme);

    /// Get the current shared theme.
    const Theme& shared_theme() const;

    // ── Inter-window messaging ──────────────────────────────────────────

    /// Send a message to a specific window.
    void send_message(WindowId target, const WindowMessage& msg);

    /// Broadcast a message to all windows.
    void broadcast_message(const WindowMessage& msg);

    /// Register a message handler for a window. Replaces any previous handler.
    void set_message_handler(WindowId id, MessageHandler handler);

    // ── Screen enumeration ──────────────────────────────────────────────

    /// Enumerate available screens/monitors. Platform-specific.
    static std::vector<ScreenInfo> available_screens();

    /// Get info for the primary screen.
    static ScreenInfo primary_screen();

    // ── Window state save / restore ─────────────────────────────────────

    /// Snapshot the current state of all windows (positions, sizes, screens).
    std::unordered_map<WindowId, WindowState> save_all_states() const;

    /// Restore window state from a previous snapshot.
    /// Only applies to windows whose IDs match.
    void restore_state(WindowId id, const WindowState& state);

    // ── GPU device sharing ──────────────────────────────────────────────

    /// Set the shared GPU device handle (Dawn wgpu::Device*).
    /// All windows MUST share the same device. Each window gets its own
    /// surface/swapchain on this device. Phase 13 (Three.js bridge) will
    /// also use this shared device.
    void set_shared_gpu_device(void* device_handle);

    /// Get the shared GPU device handle. Returns nullptr if not set.
    void* shared_gpu_device() const;

    // ── Multi-window capability query ───────────────────────────────────

    /// Whether the current platform supports multi-window.
    /// Returns false on iOS, embedded, or platforms that restrict window
    /// creation (some plugin hosts).
    static bool is_multi_window_supported();

private:
    WindowId next_id_ = 1;

    // Ordered map would give deterministic iteration, but unordered is fine
    // for the expected window count (< 20).
    std::unordered_map<WindowId, WindowRecord> windows_;
    std::unordered_map<WindowId, MessageHandler> handlers_;

    Theme shared_theme_;
    void* shared_gpu_device_ = nullptr;

    std::function<void(WindowId)> on_window_closed_;

    // Collect children recursively (depth-first).
    void collect_descendants(WindowId id, std::vector<WindowId>& out) const;
};

} // namespace pulp::view
