// PulpMotion.swift — SwiftUI / UIKit / AppKit facade over the Pulp
// motion observability bridge (see core/view/include/pulp/view/motion.hpp).
//
// Mirrors the trace-builder DSL used by other modern observability
// surfaces so a developer who has used those tools can pick this up
// with minimal friction:
//
//     view.pulpMotionTrace("Card") {
//         Trace.value("opacity", opacity)
//         Trace.geometry("frame", properties: [.minX, .minY, .width, .height])
//         Trace.scrollGeometry("scroll")
//     }
//
// All entry points are no-ops when the process-wide motion Coordinator
// has tracing disabled (the default). The Swift layer is intentionally
// a thin wrapper — the actual sampling, burst framing, fixture writing,
// and FrameClock binding all live in the C++ Coordinator.
//
// Bridging model:
//   - In production builds, an AUv3 / standalone host installs a
//     `PulpMotionBackend` that forwards every call into the C ABI in
//     `PulpBridge.h` (`pulp_motion_publish_value`, etc).
//   - In unit-test builds, `PulpMotionRuntime.installTestBackend(...)`
//     swaps in a Swift-only buffer so `swift test --package-path apple`
//     can exercise the facade without linking the C++ host.

import Foundation
#if canImport(SwiftUI)
import SwiftUI
#endif

// MARK: - Geometry properties (portable mirror)

/// Geometry property selector for `Trace.geometry(...)`. Mirrors the
/// names used by the C++ side (`pulp::view::motion::GeometryProperty`).
public struct MotionGeometryProperty: Equatable, Hashable, Sendable {
    public let rawValue: String
    public init(rawValue: String) { self.rawValue = rawValue }

    public static let minX   = MotionGeometryProperty(rawValue: "minX")
    public static let minY   = MotionGeometryProperty(rawValue: "minY")
    public static let maxX   = MotionGeometryProperty(rawValue: "maxX")
    public static let maxY   = MotionGeometryProperty(rawValue: "maxY")
    public static let midX   = MotionGeometryProperty(rawValue: "midX")
    public static let midY   = MotionGeometryProperty(rawValue: "midY")
    public static let width  = MotionGeometryProperty(rawValue: "width")
    public static let height = MotionGeometryProperty(rawValue: "height")
}

// MARK: - Trace metric DSL

/// A single metric inside a `pulpMotionTrace` block. Created with the
/// static factories on `Trace` (`.value(...)`, `.geometry(...)`,
/// `.scrollGeometry(...)`).
public struct MotionMetric {
    public enum Kind {
        case value(Double)
        case geometry(properties: [MotionGeometryProperty])
        case scrollGeometry
    }

    public let name: String
    public let kind: Kind
    public var epsilon: Double = 0.0001
    public var precision: Int = 3

    public init(name: String, kind: Kind,
                epsilon: Double = 0.0001, precision: Int = 3) {
        self.name = name
        self.kind = kind
        self.epsilon = epsilon
        self.precision = precision
    }
}

/// Public factory for `MotionMetric`. Mirrors the trace-builder names
/// developers expect.
public enum Trace {
    /// Single scalar metric — e.g. `Trace.value("opacity", opacity)`.
    public static func value(_ name: String, _ value: Double,
                             epsilon: Double = 0.0001,
                             precision: Int = 3) -> MotionMetric {
        MotionMetric(name: name, kind: .value(value),
                     epsilon: epsilon, precision: precision)
    }

    /// Geometry metric over the trace's host view — emitted as
    /// (minX, minY, width, height) by default. Add or remove properties
    /// to sample additional axes (`midX`, `maxY`, etc.).
    public static func geometry(_ name: String,
                                properties: [MotionGeometryProperty]
                                    = [.minX, .minY, .width, .height],
                                epsilon: Double = 0.1,
                                precision: Int = 2) -> MotionMetric {
        MotionMetric(name: name, kind: .geometry(properties: properties),
                     epsilon: epsilon, precision: precision)
    }

    /// Convenience for a scroll-container geometry trace — same
    /// underlying shape as `geometry(...)` but the name signals intent
    /// to readers and to downstream analysis tooling.
    public static func scrollGeometry(_ name: String,
                                      epsilon: Double = 0.1,
                                      precision: Int = 2) -> MotionMetric {
        MotionMetric(name: name, kind: .scrollGeometry,
                     epsilon: epsilon, precision: precision)
    }
}

@resultBuilder
public enum MotionTraceBuilder {
    public static func buildBlock(_ components: MotionMetric...) -> [MotionMetric] {
        components
    }
    public static func buildOptional(_ component: [MotionMetric]?) -> [MotionMetric] {
        component ?? []
    }
    public static func buildEither(first component: [MotionMetric]) -> [MotionMetric] {
        component
    }
    public static func buildEither(second component: [MotionMetric]) -> [MotionMetric] {
        component
    }
    public static func buildArray(_ components: [[MotionMetric]]) -> [MotionMetric] {
        components.flatMap { $0 }
    }
}

// MARK: - Runtime backend seam

/// Closure-bag the host app installs to bridge into the C ABI. Each
/// closure mirrors one entry point in `PulpBridge.h`. Default values
/// are no-ops so the package is testable without a host.
public struct PulpMotionBackend {
    public var isTracingEnabled: () -> Bool = { false }
    public var publishValue: (
        _ view: String, _ metric: String,
        _ value: Double, _ epsilon: Double, _ precision: Int) -> Void = { _,_,_,_,_ in }
    public var publishComponents: (
        _ view: String, _ metric: String,
        _ components: [(String, Double)],
        _ epsilon: Double, _ precision: Int) -> Void = { _,_,_,_,_ in }
    public var setAmbientProvenance: (
        _ kind: String, _ id: String, _ file: String, _ line: Int) -> Void = { _,_,_,_ in }
    public var clearAmbientProvenance: () -> Void = {}
    public var registerGeometryTrace: (
        _ view: String, _ fps: Int) -> Int32 = { _, _ in 0 }
    public var updateGeometry: (
        _ traceId: Int32, _ metric: String,
        _ minX: Double, _ minY: Double,
        _ width: Double, _ height: Double) -> Void = { _,_,_,_,_,_ in }
    public var detachTrace: (_ traceId: Int32) -> Void = { _ in }

    public init() {}
}

/// Process-wide accessor — the host wires its backend in at launch.
public enum PulpMotionRuntime {
    private static let lock = NSLock()
    /// Separate ambient-provenance lock so SwiftUI `PulpMotionProbe`
    /// attaches that briefly set + clear the C-side ambient slot can
    /// serialize without contending with backend installation. See
    /// `withAmbientProvenance(...)` below — issue #2150.
    private static let ambientLock = NSLock()
    private static var _backend = PulpMotionBackend()

    /// Install a backend (host app calls this at launch). Pass `nil`
    /// to revert to the no-op backend (useful in tests).
    public static func installBackend(_ backend: PulpMotionBackend?) {
        lock.lock(); defer { lock.unlock() }
        _backend = backend ?? PulpMotionBackend()
    }

    /// Alias for symmetry with `PulpBridgeRuntime.installTestBackend`.
    public static func installTestBackend(_ backend: PulpMotionBackend?) {
        installBackend(backend)
    }

    static var backend: PulpMotionBackend {
        lock.lock(); defer { lock.unlock() }
        return _backend
    }

    /// Run `body` with the process-wide ambient provenance set to
    /// (`kind`, `id`, `file`, `line`) and unconditionally cleared on
    /// exit. The set / body / clear triple runs under a dedicated lock
    /// so two SwiftUI view bodies in the same runloop tick can't
    /// interleave ambient slot mutations (issue #2150). The body should
    /// be short — typically just the publish calls that need the
    /// ambient stamp. The C ABI ambient slot is itself unsynchronized
    /// across runtimes, so this guard only protects Swift-side
    /// callers; it does not synchronize against arbitrary C++ writers.
    public static func withAmbientProvenance(
        kind: String, id: String,
        file: String = #fileID, line: Int = #line,
        _ body: () -> Void
    ) {
        ambientLock.lock(); defer { ambientLock.unlock() }
        let b = backend
        b.setAmbientProvenance(kind, id, file, line)
        body()
        b.clearAmbientProvenance()
    }
}

// MARK: - Public publish facade

/// SwiftUI / UIKit / AppKit-friendly entry points. Cheap to call when
/// tracing is off — the backend's `isTracingEnabled` is the first
/// branch and short-circuits the rest of the work.
public enum PulpMotion {

    /// True when the process-wide motion Coordinator is recording.
    /// Gate hot loops on this if even string-formatting the metric
    /// name is expensive.
    public static var isTracingEnabled: Bool {
        PulpMotionRuntime.backend.isTracingEnabled()
    }

    /// Publish a single scalar value.
    public static func publishValue(view: String,
                                    metric: String,
                                    value: Double,
                                    epsilon: Double = 0.0001,
                                    precision: Int = 3) {
        let b = PulpMotionRuntime.backend
        guard b.isTracingEnabled() else { return }
        b.publishValue(view, metric, value, epsilon, precision)
    }

    /// Publish a multi-component value. Components are sorted by name
    /// inside the coordinator so log lines stay stable.
    public static func publishComponents(view: String,
                                         metric: String,
                                         components: [(String, Double)],
                                         epsilon: Double = 0.0001,
                                         precision: Int = 3) {
        let b = PulpMotionRuntime.backend
        guard b.isTracingEnabled() else { return }
        b.publishComponents(view, metric, components, epsilon, precision)
    }

    /// Stamp every subsequent publish from this thread of work with
    /// the supplied provenance envelope. Pair with
    /// `clearAmbientProvenance()` to scope the stamp.
    public static func setAmbientProvenance(kind: String, id: String,
                                            file: String = #fileID,
                                            line: Int = #line) {
        PulpMotionRuntime.backend.setAmbientProvenance(kind, id, file, line)
    }

    /// Clear the ambient provenance slot.
    public static func clearAmbientProvenance() {
        PulpMotionRuntime.backend.clearAmbientProvenance()
    }

    /// Internal: register a geometry trace. Returns a positive trace_id
    /// or 0 when tracing is off / registration failed. Public because
    /// custom probe implementations (UIKit `layoutSubviews`, AppKit
    /// `viewDidLayout`) may want to bypass the SwiftUI modifier.
    public static func registerGeometryTrace(view: String,
                                             fps: Int = 30) -> Int32 {
        PulpMotionRuntime.backend.registerGeometryTrace(view, fps)
    }

    /// Internal: push a new geometry frame for a previously registered
    /// trace. `metricName` defaults to `"frame"`.
    public static func updateGeometry(traceId: Int32,
                                      metricName: String = "frame",
                                      minX: Double, minY: Double,
                                      width: Double, height: Double) {
        PulpMotionRuntime.backend.updateGeometry(
            traceId, metricName, minX, minY, width, height)
    }

    /// Internal: detach a previously registered trace. Idempotent.
    public static func detachTrace(_ traceId: Int32) {
        PulpMotionRuntime.backend.detachTrace(traceId)
    }
}

// The GeometryReader-backed SwiftUI probe lives in PulpMotionProbe.swift
// so this file stays platform-agnostic for the publish-side API.
