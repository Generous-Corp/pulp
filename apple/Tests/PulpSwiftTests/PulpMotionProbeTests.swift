import XCTest
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
