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
            exclude: [
                "PulpBridge.cpp",
                "PulpBridge.h",
            ],
            sources: [
                "PulpBridge.swift",
                "PulpParameter.swift",
                "PulpViews.swift",
                "PulpAudioSession.swift",
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
