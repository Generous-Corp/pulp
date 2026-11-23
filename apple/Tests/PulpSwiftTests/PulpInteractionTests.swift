import XCTest
import PulpInteractionBridge
@testable import PulpSwift

final class PulpInteractionTests: XCTestCase {
    private func makeSource(clock: @escaping () -> UInt64 = { 42 }) -> InteractionSource {
        InteractionSource(sourceID: 7, surfaceSize: .init(width: 100, height: 200), clock: clock)
    }

    func testSourceStampsAndNormalizesThroughRuntimeBridge() {
        let source = makeSource()
        let event = source.event(objectID: 3, phase: .press, point: .init(x: 25, y: 100), confidence: 0.8)
        XCTAssertEqual(event.sourceID, 7)
        XCTAssertEqual(event.epoch, 1)
        XCTAssertEqual(event.sequence, 1)
        XCTAssertEqual(event.providerTimeNanoseconds, 42)
        XCTAssertEqual(event.x, 0.25, accuracy: 0.0001)
        XCTAssertEqual(event.y, 0.5, accuracy: 0.0001)
    }

    func testStopPublishesCancel() {
        let source = InteractionSource(sourceID: 2, surfaceSize: .init(width: 10, height: 10), clock: { 1 })
        _ = source.event(objectID: 9, phase: .press, point: .init(x: 2, y: 3), confidence: 1)
        XCTAssertEqual(source.stop().phase, .cancel)
    }

    func testRecorderRoundTripPreservesEveryField() {
        let source = makeSource()
        let batch = [
            source.event(objectID: 3, phase: .press, point: .init(x: 25, y: 100), confidence: 0.8),
            source.event(objectID: 3, phase: .move, point: .init(x: 50, y: 150), confidence: 0.6),
            source.event(objectID: 3, phase: .release, point: .init(x: 75, y: 50), confidence: 0.4),
        ]
        let recorder = InteractionReplayRecorder()
        XCTAssertTrue(recorder.record(batch))
        XCTAssertTrue(recorder.record([source.stop()]))
        XCTAssertEqual(recorder.batchCount, 2)
        for (i, expected) in batch.enumerated() {
            XCTAssertEqual(recorder.event(batch: 0, index: i), expected)
        }
        XCTAssertEqual(recorder.event(batch: 1, index: 0)?.phase, .cancel)
        XCTAssertNil(recorder.event(batch: 0, index: 3))
        XCTAssertNil(recorder.event(batch: 2, index: 0))
        XCTAssertNil(recorder.event(batch: -1, index: 0))
    }

    func testPhaseValuesMatchRuntimeWireValues() {
        XCTAssertEqual(InteractionPhase.press.rawValue, 0)
        XCTAssertEqual(InteractionPhase.cancel.rawValue, 4)
        XCTAssertEqual(InteractionPhase.invalid.rawValue, 5)
        let source = makeSource()
        let recorder = InteractionReplayRecorder()
        XCTAssertTrue(recorder.record([source.event(objectID: 1, phase: .release, point: .init(x: 1, y: 1), confidence: 1)]))
        XCTAssertEqual(recorder.event(batch: 0, index: 0)?.phase, .release)
    }

    func testRecorderRejectsOversizedAndEmptyBatchesWithoutRecording() {
        let source = makeSource()
        let event = source.event(objectID: 1, phase: .move, point: .init(x: 1, y: 1), confidence: 1)
        let recorder = InteractionReplayRecorder()
        XCTAssertFalse(recorder.record(Array(repeating: event, count: 17)))
        XCTAssertFalse(recorder.record(Array(repeating: event, count: 300)))
        XCTAssertFalse(recorder.record([]))
        XCTAssertEqual(recorder.batchCount, 0)
        XCTAssertTrue(recorder.record(Array(repeating: event, count: 16)))
        XCTAssertEqual(recorder.batchCount, 1)
    }

    func testRecorderRejectsBatchContainingInvalidPhaseAtomically() {
        let source = makeSource()
        let good = source.event(objectID: 1, phase: .press, point: .init(x: 1, y: 1), confidence: 1)
        let invalid = InteractionEvent(PulpInteractionEvent(
            source_id: 7, epoch: 1, object_id: 1, sequence: 9, provider_time_ns: 1,
            phase: PulpInteractionPhaseInvalid, x: 0.5, y: 0.5, confidence: 1))
        let recorder = InteractionReplayRecorder()
        XCTAssertFalse(recorder.record([good, invalid]))
        XCTAssertEqual(recorder.batchCount, 0)
    }

    func testNormalizeRejectsNonFiniteAndDegenerateSurfaces() {
        var x: Float = -1, y: Float = -1
        XCTAssertTrue(pulp_interaction_normalize(25, 100, 100, 200, &x, &y))
        XCTAssertEqual(x, 0.25, accuracy: 0.0001)
        XCTAssertTrue(pulp_interaction_normalize(500, -5, 100, 200, &x, &y))
        XCTAssertEqual(x, 1)
        XCTAssertEqual(y, 0)
        XCTAssertFalse(pulp_interaction_normalize(.nan, 1, 100, 200, &x, &y))
        XCTAssertFalse(pulp_interaction_normalize(1, 1, 0, 200, &x, &y))
        XCTAssertFalse(pulp_interaction_normalize(1, 1, 100, -1, &x, &y))
        XCTAssertFalse(pulp_interaction_normalize(1, 1, 100, 200, nil, &y))
    }

    func testValidateRejectsInvalidPhaseAndOutOfRangeValues() {
        func raw(phase: PulpInteractionPhase = PulpInteractionPhasePress, x: Float = 0.5, confidence: Float = 1,
                 sequence: UInt64 = 1) -> PulpInteractionEvent {
            PulpInteractionEvent(source_id: 7, epoch: 1, object_id: 1, sequence: sequence,
                                 provider_time_ns: 1, phase: phase, x: x, y: 0.5, confidence: confidence)
        }
        var good = raw()
        XCTAssertTrue(pulp_interaction_validate(&good))
        var invalidPhase = raw(phase: PulpInteractionPhaseInvalid)
        XCTAssertFalse(pulp_interaction_validate(&invalidPhase))
        var nanX = raw(x: .nan)
        XCTAssertFalse(pulp_interaction_validate(&nanX))
        var badConfidence = raw(confidence: 2)
        XCTAssertFalse(pulp_interaction_validate(&badConfidence))
        var zeroSequence = raw(sequence: 0)
        XCTAssertFalse(pulp_interaction_validate(&zeroSequence))
        XCTAssertFalse(pulp_interaction_validate(nil))
    }
}
