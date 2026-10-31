import XCTest
@testable import PulpSwift

final class PulpMotionControlSourceTests: XCTestCase {
    func testNormalizeTiltMapsGravityRange() {
        XCTAssertEqual(PulpMotionControlMath.normalizeTilt(yAcceleration: -9.80665)!, 0.0, accuracy: 0.0001)
        XCTAssertEqual(PulpMotionControlMath.normalizeTilt(yAcceleration: 0.0)!, 0.5, accuracy: 0.0001)
        XCTAssertEqual(PulpMotionControlMath.normalizeTilt(yAcceleration: 9.80665)!, 1.0, accuracy: 0.0001)
    }

    func testNormalizeTiltClampsAndRejectsMalformedInput() {
        XCTAssertEqual(PulpMotionControlMath.normalizeTilt(yAcceleration: 100.0), 1.0)
        XCTAssertEqual(PulpMotionControlMath.normalizeTilt(yAcceleration: -100.0), 0.0)
        XCTAssertNil(PulpMotionControlMath.normalizeTilt(yAcceleration: .infinity))
        XCTAssertNil(PulpMotionControlMath.normalizeTilt(yAcceleration: .nan))
        XCTAssertNil(PulpMotionControlMath.normalizeTilt(yAcceleration: 1.0, gravity: 0.0))
    }

    func testFramePreservesCoherentProviderMetadata() {
        let frame = PulpMotionControlFrame(
            sequence: 4,
            value: 0.75,
            confidence: 1.0,
            valid: true,
            timestampNanoseconds: 123
        )
        XCTAssertEqual(frame.sequence, 4)
        XCTAssertEqual(frame.value, 0.75)
        XCTAssertEqual(frame.confidence, 1.0)
        XCTAssertTrue(frame.valid)
        XCTAssertEqual(frame.timestampNanoseconds, 123)
    }
}
