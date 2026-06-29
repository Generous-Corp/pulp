// Tiny loadable module for test_reload_library.cpp. Built as a MODULE library;
// the test dlopens it by path (RELOAD_PROBE_PATH) and resolves these symbols.
// Mirrors the shape of a real logic library's C entry points.

#if defined(_WIN32)
#define PULP_PROBE_EXPORT extern "C" __declspec(dllexport)
#else
#define PULP_PROBE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

PULP_PROBE_EXPORT int pulp_reload_probe_answer() { return 42; }

PULP_PROBE_EXPORT const char* pulp_reload_probe_name() { return "reload-probe"; }
