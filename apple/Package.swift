// swift-tools-version: 5.9
// Package.swift — Pulp Swift layer for Apple platforms
// Provides SwiftUI views and parameter bindings for plugin UIs

import PackageDescription

let package = Package(
    name: "PulpSwift",
    platforms: [
        .macOS(.v13),
        .iOS(.v16),
    ],
    products: [
        .library(name: "PulpSwift", targets: ["PulpSwift"]),
    ],
    targets: [
        .target(
            name: "PulpSwift",
            path: "Sources/PulpSwift",
            sources: ["PulpParameter.swift", "PulpViews.swift", "PulpAudioSession.swift"],
            publicHeadersPath: ".",
            cxxSettings: [
                .headerSearchPath("../../core/state/include"),
                .headerSearchPath("../../core/format/include"),
                .headerSearchPath("../../core/audio/include"),
                .headerSearchPath("../../core/midi/include"),
                .headerSearchPath("../../core/runtime/include"),
                .headerSearchPath("../../core/platform/include"),
            ]
        ),
    ],
    swiftLanguageModes: [.v5],
    cxxLanguageStandard: .cxx20
)
