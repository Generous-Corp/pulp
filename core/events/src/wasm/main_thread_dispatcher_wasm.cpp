// WebCLAP (wasm) shim for the two MainThreadDispatcher queries the state store
// consults on a parameter gesture. The full dispatcher (main_thread_dispatcher.cpp)
// uses C++ exceptions in its call_async/call_sync machinery and therefore cannot
// build under the wasm toolchain's -fno-exceptions; the WebCLAP module runs a
// single-threaded worklet where no main-thread backend is ever registered, so
// these are compile-time constants:
//
//   has_backend()    -> false   (register_plugin_backend is the no-op stub)
//   is_main_thread() -> false   (matches the real impl's no-backend answer)
//
// With has_backend() false, StateStore's "dispatch this notification to the main
// thread" guard is always false, so notifications run inline — correct for the
// worklet — and call_async is never reached at run time.

#include <pulp/events/main_thread_dispatcher.hpp>

namespace pulp::events {

bool MainThreadDispatcher::has_backend() { return false; }

bool MainThreadDispatcher::is_main_thread() { return false; }

}  // namespace pulp::events
