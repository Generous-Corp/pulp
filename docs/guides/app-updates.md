# In-app updates

A Pulp standalone app can check for and install new versions of itself with
[Sparkle 2](https://sparkle-project.org). One CMake call turns it on; the app
then offers updates in the two places macOS users look for them:

1. **The application menu**: **About <App>**, then **Check for Updates…**
   directly under it, then Settings…, Services, Hide and Quit.
2. **Settings**: an **Updates** group with an **Automatically check for
   updates** toggle, a **Check for Updates…** button, the current version and
   when the app last checked, and a short note saying where updates come from
   and how they install.

Plug-ins (AU, VST3, CLAP, AAX) never contain the updater and never show either
surface. They run inside a host's process, where an installer has no business
running; updating the app updates them, because the update is the whole
installer package.

## Turn it on

```cmake
pulp_add_sparkle(MyPlugin_Standalone
    FEED_URL         "https://github.com/me/myplugin/releases/latest/download/appcast.xml"
    PUBLIC_ED_KEY    "<44-character base64 Ed25519 public key>"
    RELEASES_URL     "https://github.com/me/myplugin/releases"
    INSTALLER        package          # the update is the signed .pkg
    AUTOMATIC_CHECKS ON)              # check on a schedule without asking first
```

That is all an app that uses Pulp's built-in Settings panel needs. The menu
item, the Updates tab and the automatic-check preference come with it. Keys,
packaging and publishing the feed are covered in
[Shipping](shipping.md#in-app-updates-for-a-standalone-app-sparkle-2).

| Argument | What it changes |
|---|---|
| `AUTOMATIC_CHECKS ON` | Scheduled checks are on from the first launch (the toggle starts on). Omit it and Sparkle asks the user on the second launch instead. Either way the user's choice is saved by Sparkle and the toggle shows it. |
| `RELEASES_URL` | Linked from the Settings note ("Updates download from MyPlugin's GitHub releases."). |
| `INSTALLER package` | The note says installing quits and reopens the app and asks for an administrator password, which is what a package update does. `app` (a replacement .app) says it quits and reopens, without the password. Omitted: the note says nothing about installing. |
| `AUTOMATIC_INSTALL ON` | Lets Sparkle install updates in the background. Off by default, and the note then says updates are never installed automatically. |

The note is generated from these facts, so it stays true for each app instead
of being copied from another one.

## A custom editor's own Settings

An editor that draws its own Settings (a JS/React document, or a native view
tree that replaces Pulp's panel) adds the same group itself. The updater lives
in the standalone; the editor code, shared by every format, asks whether one is
there.

**JS editor.** Register the bridge messages once, in the C++ that sets up the
editor's `EditorBridge`:

```cpp
#include <pulp/format/app_updates_bridge.hpp>

pulp::format::add_app_update_handlers(editor_bridge_);
```

and read them from the document with the `@pulp/react` client:

```js
import { useAppUpdates } from '@pulp/react';

function UpdatesGroup({ dispatch }) {
  const [status, updates] = useAppUpdates(dispatch, 1000);
  if (!status?.available) return null;          // a plug-in: render nothing
  return (
    <section>
      <label>
        <input type="checkbox" checked={status.automaticChecks}
               onChange={e => updates.setAutomatic(e.target.checked)} />
        Automatically check for updates
      </label>
      <button disabled={!status.canCheckNow} onClick={updates.check}>
        Check for Updates…
      </button>
      <p>{status.versionText} · {status.lastCheckText}</p>
      <p>{status.note} <a href={status.releasesUrl}>Releases</a></p>
    </section>
  );
}
```

`dispatch` is the function the editor already uses to reach its
`EditorBridge` (it takes `{type, payload, id}` JSON and returns the response).
Without a bundler, `appUpdatesClient(dispatch)` is the same thing as plain
promises, and the messages themselves are `pulp_updates_get`,
`pulp_updates_check` and `pulp_updates_set_automatic {on}`.

**Native editor.** Add the ready-made group:

```cpp
#include <pulp/format/app_updates_settings_view.hpp>

if (pulp::format::app_update_status().available)
    settings->add_child(std::make_unique<pulp::format::AppUpdatesSettingsView>());
```

Call its `refresh()` from the editor's idle/poll so the last-check time moves
after a scheduled check.

## Before and after, for an app author

| | Before | After |
|---|---|---|
| App menu | `pulp_add_sparkle()`; item placed above Settings…, no About | same call; About, then Check for Updates…, standard layout |
| Pulp Settings panel | nothing | Updates tab, automatic |
| Custom JS Settings | write an EditorBridge handler per action, read Sparkle through the Objective-C runtime yourself, hand-write the note | one `add_app_update_handlers()` line in C++, `useAppUpdates()` in the document |
| Plug-in builds | had to be excluded by hand | `available: false`, nothing renders |

## Development builds

A build without Sparkle (no `pulp_add_sparkle()`, or an app identity that reads
no feed) shows neither surface, so it can never update itself from the release
feed. To exercise the wiring anyway, launch it with
`PULP_STANDALONE_UPDATER=stub`: the menu item and Settings controls appear,
backed by a stub that never contacts a feed. A check only records its time, and
the note says the build has no update feed.

For a build that does embed Sparkle, scheduled checks start only when the app
carries a Developer ID signature; `PULP_STANDALONE_UPDATER=on` starts them in
an ad hoc build (for a loopback practice feed) and `=off` disables the updater.

## Plug-ins later

Nothing is built for plug-ins today. The service a plug-in's editor reads,
`pulp::format::AppUpdateService`, is deliberately backend-neutral: a future
plug-in service could read the same appcast without running any installer in
the host, report that a newer version exists, and have its
`check_for_updates()` open `releases_url` or ask the standalone app to update.
The editor code above would show it with no change.
