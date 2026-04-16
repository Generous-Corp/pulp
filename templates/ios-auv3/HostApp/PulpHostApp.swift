//
// SwiftUI entry point for a Pulp AUv3 host app (issue #250).
// Plug-in authors copy this into HostApp/ alongside ContentView.swift.
//

import SwiftUI

@available(iOS 15.0, macOS 12.0, *)
@main
struct PulpHostApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
