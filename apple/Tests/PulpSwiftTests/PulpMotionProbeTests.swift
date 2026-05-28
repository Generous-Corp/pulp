import XCTest
#if canImport(SwiftUI)
import SwiftUI
#endif
@testable import PulpSwift

final class PulpMotionProbeTests: XCTestCase {
    override func tearDown() {
        PulpMotionRuntime.installTestBackend(nil)
        super.tearDown()
    }

    func testGeometryProbeStaysDetachedWhenTracingIsDisabled() {
        let recorder = MotionBackendRecorder()
        recorder.tracingEnabled = false
        PulpMotionRuntime.installTestBackend(recorder.backend)

        let probe = PulpMotionGeometryProbe(view: "Disabled", fps: 12)
        probe.update(minX: 1, minY: 2, width: 3, height: 4)
        probe.detach()

        XCTAssertFalse(probe.isAttached)
        XCTAssertTrue(recorder.registrations.isEmpty)
        XCTAssertTrue(recorder.geometryUpdates.isEmpty)
        XCTAssertTrue(recorder.detachedTraceIds.isEmpty)
        XCTAssertTrue(recorder.events.isEmpty)
    }

    func testGeometryProbeRegistersUpdatesAndDetachesOnce() {
        let recorder = MotionBackendRecorder()
        recorder.nextTraceId = 42
        PulpMotionRuntime.installTestBackend(recorder.backend)

        let probe = PulpMotionGeometryProbe(view: "Meter", fps: 48, metric: "bounds")

        XCTAssertTrue(probe.isAttached)
        XCTAssertEqual(recorder.registrations.count, 1)
        XCTAssertEqual(recorder.registrations[0].view, "Meter")
        XCTAssertEqual(recorder.registrations[0].fps, 48)
        XCTAssertEqual(recorder.events, ["set:swiftui:Meter", "register:Meter:48", "clear"])

        probe.update(minX: 10, minY: 20, width: 300, height: 40)

        XCTAssertEqual(recorder.geometryUpdates.count, 1)
        XCTAssertEqual(recorder.geometryUpdates[0].traceId, 42)
        XCTAssertEqual(recorder.geometryUpdates[0].metric, "bounds")
        XCTAssertEqual(recorder.geometryUpdates[0].minX, 10)
        XCTAssertEqual(recorder.geometryUpdates[0].minY, 20)
        XCTAssertEqual(recorder.geometryUpdates[0].width, 300)
        XCTAssertEqual(recorder.geometryUpdates[0].height, 40)

        probe.detach()
        probe.detach()

        XCTAssertFalse(probe.isAttached)
        XCTAssertEqual(recorder.detachedTraceIds, [42])
    }

    func testGeometryProbeDeinitDetachesRegisteredTrace() {
        let recorder = MotionBackendRecorder()
        recorder.nextTraceId = 99
        PulpMotionRuntime.installTestBackend(recorder.backend)

        var probe: PulpMotionGeometryProbe? =
            PulpMotionGeometryProbe(view: "Deinit", fps: 30)
        XCTAssertTrue(probe?.isAttached == true)
        XCTAssertEqual(recorder.detachedTraceIds, [])

        probe = nil

        XCTAssertEqual(recorder.detachedTraceIds, [99])
    }

    func testGeometryProbeRegistrationDeclineClearsAmbientProvenance() {
        let recorder = MotionBackendRecorder()
        recorder.nextTraceId = 0
        PulpMotionRuntime.installTestBackend(recorder.backend)

        let probe = PulpMotionGeometryProbe(view: "Declined", fps: 24,
                                            metric: "layout")

        XCTAssertFalse(probe.isAttached)
        XCTAssertEqual(recorder.registrations.count, 1)
        XCTAssertEqual(recorder.registrations[0].view, "Declined")
        XCTAssertEqual(recorder.registrations[0].fps, 24)
        XCTAssertEqual(recorder.events, ["set:swiftui:Declined", "register:Declined:24", "clear"])

        probe.update(minX: 1, minY: 2, width: 3, height: 4)
        probe.detach()

        XCTAssertTrue(recorder.geometryUpdates.isEmpty)
        XCTAssertTrue(recorder.detachedTraceIds.isEmpty)
    }

    func testGeometryProbePublishesMultipleRectsUntilDetached() {
        let recorder = MotionBackendRecorder()
        recorder.nextTraceId = 314
        PulpMotionRuntime.installTestBackend(recorder.backend)

        let probe = PulpMotionGeometryProbe(view: "Canvas", fps: 60,
                                            metric: "scroll")

        probe.update(minX: 0, minY: 0, width: 100, height: 200)
        probe.update(minX: 5, minY: 10, width: 110, height: 210)
        probe.detach()
        probe.update(minX: 99, minY: 99, width: 99, height: 99)

        XCTAssertEqual(recorder.registrations.count, 1)
        XCTAssertEqual(recorder.registrations[0].view, "Canvas")
        XCTAssertEqual(recorder.registrations[0].fps, 60)
        XCTAssertEqual(recorder.geometryUpdates.count, 2)
        XCTAssertEqual(recorder.geometryUpdates.map(\.traceId), [314, 314])
        XCTAssertEqual(recorder.geometryUpdates.map(\.metric), ["scroll", "scroll"])
        XCTAssertEqual(recorder.geometryUpdates[1].minX, 5)
        XCTAssertEqual(recorder.geometryUpdates[1].minY, 10)
        XCTAssertEqual(recorder.geometryUpdates[1].width, 110)
        XCTAssertEqual(recorder.geometryUpdates[1].height, 210)
        XCTAssertEqual(recorder.detachedTraceIds, [314])
        XCTAssertFalse(probe.isAttached)
    }

    func testGeometryProbeDefaultOptionsUseFrameMetricAndThirtyFps() {
        let recorder = MotionBackendRecorder()
        recorder.nextTraceId = 515
        PulpMotionRuntime.installTestBackend(recorder.backend)

        let probe = PulpMotionGeometryProbe(view: "Defaulted")

        XCTAssertTrue(probe.isAttached)
        XCTAssertEqual(probe.name, "Defaulted")
        XCTAssertEqual(probe.metricName, "frame")
        XCTAssertEqual(recorder.registrations.count, 1)
        XCTAssertEqual(recorder.registrations[0].view, "Defaulted")
        XCTAssertEqual(recorder.registrations[0].fps, 30)
        XCTAssertEqual(recorder.events, ["set:swiftui:Defaulted", "register:Defaulted:30", "clear"])

        probe.update(minX: -1, minY: -2, width: 10, height: 20)
        XCTAssertEqual(recorder.geometryUpdates.count, 1)
        XCTAssertEqual(recorder.geometryUpdates[0].traceId, 515)
        XCTAssertEqual(recorder.geometryUpdates[0].metric, "frame")
        XCTAssertEqual(recorder.geometryUpdates[0].minX, -1)
        XCTAssertEqual(recorder.geometryUpdates[0].minY, -2)
        XCTAssertEqual(recorder.geometryUpdates[0].width, 10)
        XCTAssertEqual(recorder.geometryUpdates[0].height, 20)

        probe.detach()
        probe.detach()
        XCTAssertEqual(recorder.detachedTraceIds, [515])
        XCTAssertFalse(probe.isAttached)
    }

#if canImport(SwiftUI)
    @available(iOS 14.0, macOS 11.0, tvOS 14.0, watchOS 7.0, *)
    func testSwiftUIViewModifierStoresTraceConfiguration() {
        let metrics = [
            Trace.geometry("frame"),
            Trace.scrollGeometry("scroll"),
            Trace.value("opacity", 0.5, epsilon: 0.01, precision: 2),
        ]
        let modifier = PulpMotionTraceModifier(name: "Panel",
                                               fps: 24,
                                               metrics: metrics)

        XCTAssertEqual(modifier.name, "Panel")
        XCTAssertEqual(modifier.fps, 24)
        XCTAssertEqual(modifier.metrics.count, 3)
        XCTAssertEqual(modifier.metrics.map(\.name), ["frame", "scroll", "opacity"])

        _ = Text("Panel").pulpMotionTrace("Panel", fps: 24) {
            Trace.geometry("frame")
            Trace.value("opacity", 0.5)
        }
    }
#endif
}

private final class MotionBackendRecorder {
    struct Registration {
        let view: String
        let fps: Int
    }

    struct GeometryUpdate {
        let traceId: Int32
        let metric: String
        let minX: Double
        let minY: Double
        let width: Double
        let height: Double
    }

    var tracingEnabled = true
    var nextTraceId: Int32 = 7
    var registrations: [Registration] = []
    var geometryUpdates: [GeometryUpdate] = []
    var detachedTraceIds: [Int32] = []
    var events: [String] = []

    var backend: PulpMotionBackend {
        var backend = PulpMotionBackend()
        backend.isTracingEnabled = { [weak self] in
            self?.tracingEnabled ?? false
        }
        backend.setAmbientProvenance = { [weak self] kind, id, _, _ in
            self?.events.append("set:\(kind):\(id)")
        }
        backend.clearAmbientProvenance = { [weak self] in
            self?.events.append("clear")
        }
        backend.registerGeometryTrace = { [weak self] view, fps in
            guard let self else { return 0 }
            self.events.append("register:\(view):\(fps)")
            self.registrations.append(.init(view: view, fps: fps))
            return self.nextTraceId
        }
        backend.updateGeometry = { [weak self] traceId, metric, minX, minY, width, height in
            self?.geometryUpdates.append(.init(traceId: traceId,
                                               metric: metric,
                                               minX: minX,
                                               minY: minY,
                                               width: width,
                                               height: height))
        }
        backend.detachTrace = { [weak self] traceId in
            self?.detachedTraceIds.append(traceId)
        }
        return backend
    }
}
