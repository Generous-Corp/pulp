#pragma once

// Locale-independent floating-point parsing that compiles on every toolchain
// Pulp builds with.
//
// `std::from_chars` is the natural choice for locale-independent number parsing
// (it always uses the C locale's decimal point, so a comma-decimal global
// locale cannot misparse "0.5"). Its *integer* overloads are universally
// implemented and we keep using them directly. Its *floating-point* overloads,
// however, are still `=delete`d placeholders in the libc++ shipped with some
// toolchains — notably the github-hosted macOS sanitizer image, where a
// `std::from_chars(first, last, a_float)` call hard-fails the build with
// "call to deleted function 'from_chars'". (The Mac Studio toolchain that backs
// the required `macos` gate *does* implement it, which is why the break only
// showed up on the advisory sanitizer lane.)
//
// To stay locale-independent without the fragile float overload, we parse
// against an explicit "C" locale: POSIX `uselocale()` around `std::strtod`,
// and `_strtod_l` on Windows. Plain `strtod` honors the *global* locale, so it
// alone is not safe — the explicit C locale is what makes this immune to a
// comma-decimal host.

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <clocale>
#else
#include <locale.h>
#endif

namespace pulp::format::detail {

// Parse a leading double out of [tok], using the C locale's '.' decimal point
// regardless of the process's global locale.
//
// Mirrors the parts of the `std::from_chars` contract the call sites rely on:
//   * `consumed` is the number of characters consumed from the start of `tok`
//     (0 == nothing parsed). `strtod` skips leading whitespace and accepts a
//     leading '+' or '-', matching the historical `strtod` behavior the CLAP
//     path documents.
//   * `range_error` is set when the magnitude over/underflowed (errno==ERANGE),
//     so callers can reject "1e999999" instead of writing a garbage value —
//     the same way `from_chars` reports `errc::result_out_of_range`.
//   * `out` is written only on a successful, in-range parse.
//
// Callers that need full-token consumption (e.g. rejecting "0.5foo" or a
// legacy comma-decimal "0,5") compare `consumed` against `tok.size()`.
struct DoubleParse {
    std::size_t consumed = 0;
    bool range_error = false;
};

inline DoubleParse parse_double_c_locale(std::string_view tok, double& out) {
    if (tok.empty()) return {};
    // strtod needs a NUL-terminated string; the call sites hand us views into
    // larger buffers (a `.pulpset` line, a CLAP text field), so copy the token.
    std::string buf(tok);

    errno = 0;
    char* end = nullptr;
    double value = 0.0;

#if defined(_WIN32)
    static _locale_t c_locale = ::_create_locale(LC_ALL, "C");
    value = ::_strtod_l(buf.c_str(), &end, c_locale);
#else
    // newlocale(LC_ALL_MASK, "C", ...) is guaranteed to succeed for "C".
    static ::locale_t c_locale = ::newlocale(LC_ALL_MASK, "C", static_cast<::locale_t>(0));
    const ::locale_t prev = ::uselocale(c_locale);
    value = std::strtod(buf.c_str(), &end);
    ::uselocale(prev);
#endif

    DoubleParse result;
    result.consumed = static_cast<std::size_t>(end - buf.c_str());
    if (result.consumed == 0) return result;  // no number at the start
    if (errno == ERANGE) {
        result.range_error = true;
        return result;
    }
    out = value;
    return result;
}

}  // namespace pulp::format::detail
