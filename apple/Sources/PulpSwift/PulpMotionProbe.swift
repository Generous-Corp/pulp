// PulpMotionProbe.swift — SwiftUI view modifier that attaches a Pulp
// motion trace to a view and feeds GeometryReader-derived rects into
// `pulp_motion_update_geometry()` whenever the host view's frame
// changes.
//
// Off by default: registration short-circuits when the motion
// Coordinator has tracing disabled, so the modifier is safe to leave
// in production code paths.

import Foundation
#if canImport(SwiftUI)
import SwiftUI

@available(iOS 14.0, macOS 11.0, tvOS 14.0, watchOS 7.0, *)
public struct PulpMotionTraceModifier: ViewModifier {
    public let name: String
    public let fps: Int
    public let metrics: [MotionMetric]

    @State private var traceId: Int32 = 0

    public init(name: String, fps: Int, metrics: [MotionMetric]) {
        self.name = name
        self.fps = fps
        self.metrics = metrics
    }

    public func body(content: Content) -> some View {
        content
            .background(
                GeometryReader { proxy in
                    Color.clear
                        .preference(
                            key: PulpMotionFramePreferenceKey.self,
                            value: proxy.frame(in: .global)
                        )
                }
            )
            .onPreferenceChange(PulpMotionFramePreferenceKey.self) { rect in
                handleFrame(rect)
            }
            .onAppear { attachIfNeeded() }
            .onDisappear { detach() }
    }

    private func attachIfNeeded() {
        guard PulpMotion.isTracingEnabled, traceId == 0 else { return }
        let id = PulpMotion.registerGeometryTrace(view: name, fps: fps)
        guard id > 0 else { return }
        traceId = id

        // Walk the metric set; geometry / scrollGeometry metrics are
        // fed by the GeometryReader probe, scalar `value` metrics ride
        // the publish channel synchronously.
        //
        // SwiftUI may invoke `body` / `onAppear` from multiple view
        // bodies in the same runloop tick — two concurrent
        // `pulpMotionTrace` attaches would otherwise race the global
        // ambient provenance slot. Serialize the set / publish / clear
        // triple under `PulpMotionRuntime.withAmbientProvenance` so
        // each attach sees its own provenance stamp (issue #2150).
        PulpMotionRuntime.withAmbientProvenance(kind: "swiftui", id: name) {
            for metric in metrics {
                switch metric.kind {
                case .value(let v):
                    PulpMotion.publishValue(view: name, metric: metric.name,
                                            value: v,
                                            epsilon: metric.epsilon,
                                            precision: metric.precision)
                case .geometry, .scrollGeometry:
                    break  // fed by handleFrame / preference change
                }
            }
        }
    }

    private func detach() {
        if traceId != 0 {
            PulpMotion.detachTrace(traceId)
            traceId = 0
        }
    }

    private func handleFrame(_ rect: CGRect) {
        guard traceId != 0 else { return }
        for metric in metrics {
            switch metric.kind {
            case .geometry, .scrollGeometry:
                PulpMotion.updateGeometry(
                    traceId: traceId,
                    metricName: metric.name,
                    minX: Double(rect.minX),
                    minY: Double(rect.minY),
                    width: Double(rect.width),
                    height: Double(rect.height))
            case .value:
                break
            }
        }
    }
}

private struct PulpMotionFramePreferenceKey: PreferenceKey {
    static var defaultValue: CGRect = .zero
    static func reduce(value: inout CGRect, nextValue: () -> CGRect) {
        value = nextValue()
    }
}

@available(iOS 14.0, macOS 11.0, tvOS 14.0, watchOS 7.0, *)
public extension View {
    /// Attach a Pulp motion trace to this view. Geometry metrics are
    /// fed by a hidden GeometryReader probe; scalar metrics are
    /// published once at attach time (re-publish from your own state
    /// for live values).
    func pulpMotionTrace(
        _ name: String,
        fps: Int = 30,
        @MotionTraceBuilder _ build: () -> [MotionMetric]
    ) -> some View {
        modifier(PulpMotionTraceModifier(
            name: name, fps: fps, metrics: build()))
    }
}
#endif  // canImport(SwiftUI)

// MARK: - UIKit / AppKit probe helper (no SwiftUI dependency)

/// Manual probe for code paths that can't use the SwiftUI modifier
/// (UIView `layoutSubviews`, NSView `layout()`, plain C++ callbacks).
/// RAII: the handle's `deinit` detaches the trace.
public final class PulpMotionGeometryProbe {
    private var traceId: Int32 = 0
    public let name: String
    public let metricName: String

    public init(view name: String, fps: Int = 30,
                metric: String = "frame") {
        self.name = name
        self.metricName = metric
        if PulpMotion.isTracingEnabled {
            // Same race surface as `PulpMotionTraceModifier.attachIfNeeded`
            // (issue #2150): serialize the set / register / clear under
            // the runtime's ambient lock so concurrent UIKit / AppKit
            // probe constructions can't interleave ambient slot
            // mutations.
            PulpMotionRuntime.withAmbientProvenance(kind: "swiftui", id: name) {
                traceId = PulpMotion.registerGeometryTrace(view: name, fps: fps)
            }
        }
    }

    deinit { detach() }

    /// Push a new rect. Cheap when tracing is off (`traceId == 0`).
    public func update(minX: Double, minY: Double,
                       width: Double, height: Double) {
        guard traceId != 0 else { return }
        PulpMotion.updateGeometry(
            traceId: traceId, metricName: metricName,
            minX: minX, minY: minY, width: width, height: height)
    }

    /// Detach the trace early. Idempotent.
    public func detach() {
        if traceId != 0 {
            PulpMotion.detachTrace(traceId)
            traceId = 0
        }
    }

    /// True when a trace is currently registered.
    public var isAttached: Bool { traceId != 0 }
}
