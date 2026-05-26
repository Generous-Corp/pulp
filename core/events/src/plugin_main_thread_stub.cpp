// No-op plugin-main-thread backend for non-Apple platforms (item 6.4b macOS
// plan). Windows + Linux have their own message-loop integration paths and
// will get dedicated backends in gap-doc Phase 1.

#include <pulp/events/plugin_main_thread.hpp>

namespace pulp::events {

MainThreadDispatcher::Token register_plugin_backend() { return 0; }

bool unregister_plugin_backend(MainThreadDispatcher::Token token) {
    // A zero token is a benign no-op; any other value indicates a code path
    // that thought it had registered something we don't actually own — reject
    // it so the caller's bookkeeping doesn't drift silently.
    return token == 0;
}

bool plugin_backend_available() { return false; }

} // namespace pulp::events
