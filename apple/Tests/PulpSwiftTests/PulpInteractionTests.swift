import XCTest
@testable import PulpSwift

final class PulpInteractionTests: XCTestCase {
    private var sent: [PulpInteractionEvent] = []

    override func setUp() {
        super.setUp()
        sent = []
        PulpInteractionRuntime.installTestBackend(PulpInteractionBackend(publish: { [unowned self] _, event in
            self.sent.append(event)
            return true
        }))
    }

    override func tearDown() {
        PulpInteractionRuntime.installTestBackend(nil)
        super.tearDown()
    }

    func testEventHasTheLayoutOfTheCStruct() {
        // The same literal offsets test/test_interaction_c.cpp checks in C++.
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.size, 48)
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.stride, 48)
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.offset(of: \.sourceId), 0)
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.offset(of: \.epoch), 4)
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.offset(of: \.sequence), 8)
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.offset(of: \.providerTimeNs), 16)
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.offset(of: \.objectId), 24)
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.offset(of: \.phase), 28)
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.offset(of: \.reserved), 29)
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.offset(of: \.x), 32)
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.offset(of: \.y), 36)
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.offset(of: \.strength), 40)
        XCTAssertEqual(MemoryLayout<PulpInteractionEvent>.offset(of: \.confidence), 44)
    }

    func testPhasesKeepTheWireValues() {
        XCTAssertEqual(PulpInteractionPhase.press.rawValue, 0)
        XCTAssertEqual(PulpInteractionPhase.hold.rawValue, 1)
        XCTAssertEqual(PulpInteractionPhase.move.rawValue, 2)
        XCTAssertEqual(PulpInteractionPhase.release.rawValue, 3)
        XCTAssertEqual(PulpInteractionPhase.cancel.rawValue, 4)
    }

    func testPressMoveReleaseForwardInOrderWithMonotonicSequence() {
        var now: UInt64 = 100
        let provider = PulpInteractionProvider(sourceId: 7, router: nil, epoch: 3, clock: { now += 1; return now })
        XCTAssertTrue(provider.press(object: 5, x: 0.2, y: 0.3, strength: 0.8, confidence: 0.9))
        XCTAssertTrue(provider.move(object: 5, x: 0.4, y: 0.5))
        XCTAssertTrue(provider.release(object: 5, x: 0.4, y: 0.5))

        XCTAssertEqual(sent.map { $0.phase }, [0, 2, 3])
        XCTAssertEqual(sent.map { $0.sequence }, [1, 2, 3])
        XCTAssertEqual(sent.map { $0.epoch }, [3, 3, 3])
        XCTAssertEqual(sent.map { $0.sourceId }, [7, 7, 7])
        XCTAssertEqual(sent.map { $0.objectId }, [5, 5, 5])
        XCTAssertEqual(sent.map { $0.providerTimeNs }, [101, 102, 103])
        XCTAssertEqual(sent[0].x, 0.2)
        XCTAssertEqual(sent[0].y, 0.3)
        XCTAssertEqual(sent[0].strength, 0.8)
        XCTAssertEqual(sent[0].confidence, 0.9)
    }

    func testAnEpochChangeRestartsSequenceAndSendsNoCancelOfItsOwn() {
        let provider = PulpInteractionProvider(sourceId: 7, router: nil, epoch: 1)
        provider.press(object: 5, x: 0.2, y: 0.3)
        provider.move(object: 5, x: 0.4, y: 0.5)
        provider.beginEpoch()
        provider.press(object: 9, x: 0.9, y: 0.1)

        // Swift forwards the new epoch's press and nothing else: the cancel of
        // object 5 is the router's tracker's to produce (see test_interaction_c.cpp).
        XCTAssertEqual(sent.map { $0.phase }, [0, 2, 0])
        XCTAssertEqual(sent.map { $0.epoch }, [1, 1, 2])
        XCTAssertEqual(sent.map { $0.sequence }, [1, 2, 1])
        XCTAssertFalse(sent.contains { $0.phase == PulpInteractionPhase.cancel.rawValue })
    }

    func testTheRouterVerdictIsReturnedUnchanged() {
        PulpInteractionRuntime.installTestBackend(PulpInteractionBackend(publish: { _, _ in false }))
        let provider = PulpInteractionProvider(sourceId: 1, router: nil)
        XCTAssertFalse(provider.press(object: 1, x: 2.0, y: 0.5))
    }

    func testOutOfRangeCoordinatesAreForwardedNotClamped() {
        let provider = PulpInteractionProvider(sourceId: 1, router: nil)
        provider.move(object: 1, x: 1.5, y: -0.25)
        XCTAssertEqual(sent.first?.x, 1.5)
        XCTAssertEqual(sent.first?.y, -0.25)
    }

    func testNormalizedMapsAViewPointToTheUnitSurface() {
        let top = PulpInteractionProvider.normalized(x: 50, y: 25, width: 200, height: 100)
        XCTAssertEqual(top.x, 0.25)
        XCTAssertEqual(top.y, 0.25)
        let flipped = PulpInteractionProvider.normalized(x: 50, y: 25, width: 200, height: 100, flipY: true)
        XCTAssertEqual(flipped.y, 0.75)
        let degenerate = PulpInteractionProvider.normalized(x: 1, y: 1, width: 0, height: 0)
        XCTAssertTrue(degenerate.x.isNaN && degenerate.y.isNaN)
    }
}
