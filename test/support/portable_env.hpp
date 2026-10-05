#pragma once

// Set or remove an environment variable in a test, on every platform.
// MSVC's C runtime has no setenv/unsetenv; _putenv_s sets a variable, and an
// empty value removes it.

#include <cstdlib>

namespace pulp::test {

inline int set_env_var(const char* name, const char* value) {
#if defined(_WIN32)
    return _putenv_s(name, value);
#else
    return ::setenv(name, value, 1);
#endif
}

inline int unset_env_var(const char* name) {
#if defined(_WIN32)
    return _putenv_s(name, "");
#else
    return ::unsetenv(name);
#endif
}

}  // namespace pulp::test
