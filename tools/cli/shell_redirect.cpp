#include "shell_redirect.hpp"

const char* stderr_to_null() {
#if defined(_WIN32)
    return " 2>NUL";
#else
    return " 2>/dev/null";
#endif
}

const char* output_to_null() {
#if defined(_WIN32)
    return " >NUL 2>&1";
#else
    return " >/dev/null 2>&1";
#endif
}
