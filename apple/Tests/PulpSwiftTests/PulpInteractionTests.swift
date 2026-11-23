import XCTest
@testable import PulpSwift

final class PulpInteractionTests: XCTestCase {
    func testSourceStampsAndNormalizesThroughRuntimeBridge() {
        var source = InteractionSource(sourceID: 7, surfaceSize: .init(width: 100, height: 200), clock: { 42 })
        let event = source.event(objectID: 3, phase: .press, point: .init(x: 25, y: 100), confidence: 0.8)
        XCTAssertEqual(event.sourceID, 7)
        XCTAssertEqual(event.epoch, 1)
        XCTAssertEqual(event.sequence, 1)
        XCTAssertEqual(event.providerTimeNanoseconds, 42)
        XCTAssertEqual(event.x, 0.25, accuracy: 0.0001)
        XCTAssertEqual(event.y, 0.5, accuracy: 0.0001)
    }

    func testStopPublishesCancel() {
        var source = InteractionSource(sourceID: 2, surfaceSize: .init(width: 10, height: 10), clock: { 1 })
        _ = source.event(objectID: 9, phase: .press, point: .init(x: 2, y: 3), confidence: 1)
        XCTAssertEqual(source.stop().phase, .cancel)
    }
}
