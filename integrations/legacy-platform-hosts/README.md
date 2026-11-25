# Legacy platform hosts

The Windows, Linux, iOS, Android and web UI hosts that have not been ported to
Vellum yet. They are kept here as porting reference. Each platform's subtree is
removed when that platform's Vellum host lands.

| Directory | What it holds | Built by |
|---|---|---|
| `win/` | HWND plug-in editor host, UIA accessibility, OLE drag and drop, WebView2 backend; `uia_mapping.hpp` and the `win_*.hpp` input headers | `CMakeLists.txt` here, into the core/view targets |
| `linux/` | X11 plug-in editor host, AT-SPI accessibility, XDND, WebKitGTK backend; the `atspi_*.hpp` headers | `CMakeLists.txt` here, into the core/view targets |
| `ios/` | UIKit window host, accessibility, plug-in view host, the AUv3 view controller | `CMakeLists.txt` here; the view controller through `tools/cmake/PulpAuv3.cmake` |
| `android/` | TalkBack accessibility, motion JNI bridge, the GPU surface app shell, Choreographer pacing | `tools/cmake/PulpAndroid.cmake` and the root `pulp-jni` target |
| `web/` | Browser window host and input; `web/web_event_translate.hpp` | `tools/cmake/PulpWebUi.cmake` |
| `tests/` | Tests that cover only these hosts | `test/cmake/view_host_tests.cmake`, `test/cmake/motion_tests.cmake` |

Public headers keep their `pulp/view/...` include spelling and install to the
same place in the SDK.

## Rules

- Nothing outside this directory may include, name or link what is here.
  `tools/scripts/legacy_platform_hosts_guard.py` enforces it in the pre-push
  hook, `tools/scripts/gates.sh` and the required `macos` ctest lane. The
  references that exist today are allowlisted with an exact count.
- Fix bugs here only when a platform lane needs it. New UI work goes to Vellum.
- Removing a platform: delete its subtree and its tests, drop its rows from the
  guard's allowlist, and remove the wiring that names it.
