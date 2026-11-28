#ifdef _WIN32
#include <pulp/view/appearance_tracker.hpp>
#include <windows.h>

namespace pulp::view::platform {

Appearance detect_win_appearance() {
    HKEY key;
    LONG result = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &key);

    if (result != ERROR_SUCCESS)
        return Appearance::dark;

    DWORD value = 0;
    DWORD size = sizeof(value);
    result = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
                              reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);

    if (result == ERROR_SUCCESS && value == 1)
        return Appearance::light;

    return Appearance::dark;
}

} // namespace pulp::view::platform
#endif
