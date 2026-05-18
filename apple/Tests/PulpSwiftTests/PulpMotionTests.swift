import XCTest
@testable import PulpSwift

/// Exercises the SwiftPM-side facade. The C ABI / Coordinator round
/// trip is covered by `pulp-test-motion-swift-bridge` (C++ Catch2).
final class PulpMotionTests: XCTestCase {

    private final class RecordingBackend {
        var tracingEnabled = false
        var publishedValues: [(String, String, Double, Double, Int)] = []
        var publishedComponents:
            [(String, String, [(String, Double)], Double, Int)] = []
        var ambient: (String, String, String, Int)?
        var ambientCleared = 0
        var registerCalls: [(String, Int)] = []
        var updateCalls:
            [(Int32, String, Double, Double, Double, Double)] = []
        var detachCalls: [Int32] = []
        var nextId: Int32 = 1
    }

    private var rec: RecordingBackend!

    override func setUp() {
        super.setUp()
        rec = RecordingBackend()
        var backend = PulpMotionBackend()
        backend.isTracingEnabled = { [rec] in rec!.tracingEnabled }
        backend.publishValue = { [rec] v, m, val, eps, p in
            rec!.publishedValues.append((v, m, val, eps, p))
        }
        backend.publishComponents = { [rec] v, m, comps, eps, p in
            rec!.publishedComponents.append((v, m, comps, eps, p))
        }
        backend.setAmbientProvenance = { [rec] k, i, f, l in
            rec!.ambient = (k, i, f, l)
        }
        backend.clearAmbientProvenance = { [rec] in
            rec!.ambientCleared += 1
        }
        backend.registerGeometryTrace = { [rec] v, fps in
            rec!.registerCalls.append((v, fps))
            let id = rec!.nextId
            rec!.nextId += 1
            return rec!.tracingEnabled ? id : 0
        }
        backend.updateGeometry = { [rec] id, m, x, y, w, h in
            rec!.updateCalls.append((id, m, x, y, w, h))
        }
        backend.detachTrace = { [rec] id in
            rec!.detachCalls.append(id)
        }
        PulpMotionRuntime.installTestBackend(backend)
    }

    override func tearDown() {
        PulpMotionRuntime.installTestBackend(nil)
        rec = nil
        super.tearDown()
    }

    // MARK: - Off-by-default gate

    func testPublishValueIsNoOpWhenTracingDisabled() {
        rec.tracingEnabled = false
        PulpMotion.publishValue(view: "Card", metric: "opacity",
                                value: 0.5)
        XCTAssertTrue(rec.publishedValues.isEmpty)
    }

    func testPublishComponentsIsNoOpWhenTracingDisabled() {
        rec.tracingEnabled = false
        PulpMotion.publishComponents(view: "Card", metric: "pt",
                                     components: [("x", 1), ("y", 2)])
        XCTAssertTrue(rec.publishedComponents.isEmpty)
    }

    // MARK: - Publish forwarding

    func testPublishValueForwardsToBackend() {
        rec.tracingEnabled = true
        PulpMotion.publishValue(view: "Card", metric: "opacity",
                                value: 0.75, epsilon: 0.01, precision: 2)
        XCTAssertEqual(rec.publishedValues.count, 1)
        let call = rec.publishedValues[0]
        XCTAssertEqual(call.0, "Card")
        XCTAssertEqual(call.1, "opacity")
        XCTAssertEqual(call.2, 0.75, accuracy: 1e-9)
        XCTAssertEqual(call.3, 0.01, accuracy: 1e-9)
        XCTAssertEqual(call.4, 2)
    }

    func testPublishComponentsForwardsToBackend() {
        rec.tracingEnabled = true
        PulpMotion.publishComponents(view: "Card", metric: "frame",
                                     components: [("x", 10), ("y", 20)])
        XCTAssertEqual(rec.publishedComponents.count, 1)
        XCTAssertEqual(rec.publishedComponents[0].2.map { $0.0 }, ["x", "y"])
    }

    // MARK: - Ambient provenance

    func testAmbientProvenanceForwardsKindIdAndCaller() {
        PulpMotion.setAmbientProvenance(kind: "swiftui", id: "CardView")
        XCTAssertNotNil(rec.ambient)
        XCTAssertEqual(rec.ambient?.0, "swiftui")
        XCTAssertEqual(rec.ambient?.1, "CardView")
        // file / line auto-captured.
        XCTAssertFalse(rec.ambient?.2.isEmpty ?? true)
        XCTAssertGreaterThan(rec.ambient?.3 ?? 0, 0)
        PulpMotion.clearAmbientProvenance()
        XCTAssertEqual(rec.ambientCleared, 1)
    }

    // MARK: - Trace builder

    func testTraceBuilderProducesExpectedMetricKinds() {
        @MotionTraceBuilder
        func build() -> [MotionMetric] {
            Trace.value("opacity", 0.5)
            Trace.geometry("frame")
            Trace.scrollGeometry("scroll")
        }
        let metrics = build()
        XCTAssertEqual(metrics.count, 3)
        if case .value(let v) = metrics[0].kind {
            XCTAssertEqual(v, 0.5)
        } else {
            XCTFail("expected .value")
        }
        if case .geometry(let props) = metrics[1].kind {
            XCTAssertEqual(props, [.minX, .minY, .width, .height])
        } else {
            XCTFail("expected .geometry")
        }
        if case .scrollGeometry = metrics[2].kind {
            // ok
        } else {
            XCTFail("expected .scrollGeometry")
        }
    }

    func testGeometryPropertyDefaultIsMinXMinYWidthHeight() {
        let m = Trace.geometry("frame")
        guard case .geometry(let props) = m.kind else {
            return XCTFail("expected .geometry")
        }
        XCTAssertEqual(props,
            [.minX, .minY, .width, .height])
    }

    // MARK: - Manual geometry probe

    func testGeometryProbeAttachAndUpdateAndDetach() {
        rec.tracingEnabled = true
        let probe = PulpMotionGeometryProbe(view: "CardView", fps: 30)
        XCTAssertTrue(probe.isAttached)
        XCTAssertEqual(rec.registerCalls.count, 1)
        XCTAssertEqual(rec.registerCalls[0].0, "CardView")
        XCTAssertEqual(rec.registerCalls[0].1, 30)

        probe.update(minX: 0, minY: 0, width: 100, height: 50)
        probe.update(minX: 0, minY: 10, width: 100, height: 50)
        XCTAssertEqual(rec.updateCalls.count, 2)
        XCTAssertEqual(rec.updateCalls[1].2, 0)
        XCTAssertEqual(rec.updateCalls[1].3, 10)

        probe.detach()
        XCTAssertFalse(probe.isAttached)
        XCTAssertEqual(rec.detachCalls.count, 1)
        // Idempotent.
        probe.detach()
        XCTAssertEqual(rec.detachCalls.count, 1)
    }

    func testGeometryProbeIsNoOpWhenTracingDisabled() {
        rec.tracingEnabled = false
        let probe = PulpMotionGeometryProbe(view: "CardView")
        XCTAssertFalse(probe.isAttached)
        probe.update(minX: 0, minY: 0, width: 10, height: 10)
        XCTAssertTrue(rec.updateCalls.isEmpty)
    }

    // MARK: - withAmbientProvenance race-guard (#2150)
    //
    // Pre-#2150, two SwiftUI view bodies in the same runloop tick that
    // both invoked `attachIfNeeded()` would race the process-wide
    // ambient slot — view A's `setAmbientProvenance` could be
    // immediately clobbered by view B's, then A's publishes would be
    // stamped with B's provenance. `PulpMotionRuntime.withAmbientProvenance`
    // serializes the set / body / clear triple so each attach observes
    // its own stamp.

    func testWithAmbientProvenanceSerializesSetClearAcrossThreads() async {
        rec.tracingEnabled = true

        // Record the sequence of ambient mutations (set / clear) the
        // backend observes. After two concurrent attaches, the
        // sequence must contain pairs of set→clear with no interleaved
        // sets — i.e. between every `set` and its matching `clear`,
        // no second `set` appears.
        let mutationLock = NSLock()
        var mutations: [String] = []
        var backend = PulpMotionRuntime.backend
        backend.setAmbientProvenance = { kind, id, _, _ in
            mutationLock.lock(); defer { mutationLock.unlock() }
            mutations.append("set:\(kind)/\(id)")
        }
        backend.clearAmbientProvenance = {
            mutationLock.lock(); defer { mutationLock.unlock() }
            mutations.append("clear")
        }
        PulpMotionRuntime.installTestBackend(backend)

        await withTaskGroup(of: Void.self) { group in
            for i in 0..<8 {
                group.addTask {
                    let viewName = "View\(i)"
                    PulpMotionRuntime.withAmbientProvenance(
                        kind: "swiftui", id: viewName
                    ) {
                        // Brief artificial work inside the critical
                        // section to widen the race window if the
                        // guard is removed.
                        for _ in 0..<200 { _ = viewName.uppercased() }
                    }
                }
            }
        }

        // Validate: every `set` is followed by exactly one `clear`
        // before the next `set`. Reject any nested `set:..`/`set:..`
        // pair — that would be the racing bug #2150 was filed for.
        var depth = 0
        for m in mutations {
            if m.hasPrefix("set:") {
                XCTAssertEqual(depth, 0,
                    "nested set with no intervening clear at \(m)")
                depth += 1
            } else if m == "clear" {
                XCTAssertEqual(depth, 1,
                    "clear without matching set")
                depth -= 1
            }
        }
        XCTAssertEqual(depth, 0, "trailing unmatched set")
        // 8 tasks → 16 mutations (8 set + 8 clear).
        XCTAssertEqual(mutations.count, 16)
    }
}
