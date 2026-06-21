// No-op plugin-main-thread backend for non-Apple platforms. Windows and Linux
// plugin hosts use their own message-loop integration paths, so this stub keeps
// adapter code cross-platform while leaving the dispatcher in its no-backend
// state unless a platform owner registers one.

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
