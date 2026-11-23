// swift-tools-version: 5.9
// Package.swift — Pulp Swift layer for Apple platforms
// Provides SwiftUI views and parameter bindings for plugin UIs

import Foundation
import PackageDescription

// The runtime headers live outside the package root, which SwiftPM rejects as a
// headerSearchPath, so the include directory is passed as an absolute -I flag.
let runtimeInclude = URL(fileURLWithPath: #filePath)
    .deletingLastPathComponent().deletingLastPathComponent()
    .appendingPathComponent("core/runtime/include").path

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
            name: "PulpInteractionBridge",
            path: "Sources/PulpInteractionBridge",
            publicHeadersPath: ".",
            cxxSettings: [.unsafeFlags(["-I", runtimeInclude])]
        ),
        .target(
            name: "PulpSwift",
            dependencies: ["PulpInteractionBridge"],
            path: "Sources/PulpSwift",
            exclude: [
                "PulpBridge.cpp",
                "PulpBridge.h",
            ],
            sources: [
                "PulpBridge.swift",
                "PulpParameter.swift",
                "PulpViews.swift",
                "PulpAudioSession.swift",
                "PulpMotionControlSource.swift",
                "PulpMotion.swift",
                "PulpMotionProbe.swift",
                "PulpInteraction.swift",
            ]
        ),
        .testTarget(
            name: "PulpSwiftTests",
            dependencies: ["PulpSwift"],
            path: "Tests/PulpSwiftTests"
        ),
    ],
    swiftLanguageVersions: [.v5],
    cxxLanguageStandard: .cxx20
)
