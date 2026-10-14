@preconcurrency import AVFoundation
import Synchronization

// Only the decoder/encoder and activator are replaced. The complete production
// bridge and the source-pinned IEC unpacker are compiled alongside this file.
enum SpatialAudioCodec: UInt32 { case trueHD = 1, dtsHD = 2 }
enum SpatialAudioError: Error { case decoding(String) }
enum HarnessError: Error { case timeout(String), emptyRendererFlush }
let mpSpatialSilentAtmos = Data()

actor SpatialAudioRenderer {
    static let calls = Mutex((enqueues: 0, ends: 0))
    private var enqueued = false
    var isReadyToPlay: Bool { false }

    init(codec: SpatialAudioCodec, synchronizer: AVSampleBufferRenderSynchronizer) throws {}
    func setVolume(_ volume: Float) {}
    func begin(at time: CMTime) throws { enqueued = false }
    func enqueue(_ packet: Data, time: CMTime) async throws {
        Self.calls.withLock { $0.enqueues += 1 }
        if packet == Data([0x55]) {
            // A reset must cancel an old feeder even while it is backpressured.
            try await Task.sleep(for: .seconds(10))
        }
        enqueued = true
    }
    func endOfStream() throws {
        guard enqueued else { throw HarnessError.emptyRendererFlush }
        Self.calls.withLock { $0.ends += 1 }
    }
}

@MainActor
final class AtmosOutputActivator {
    init(silentAtmosStream: URL) {}
    func attach(to synchronizer: AVSampleBufferRenderSynchronizer) {}
    func detach() {}
}

@main
@MainActor
struct FeederChecks {
    static func wait(_ label: String, until predicate: () -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(2))
        while !predicate() {
            guard ContinuousClock.now < deadline else { throw HarnessError.timeout(label) }
            try await Task.sleep(for: .milliseconds(2))
        }
    }

    static func burst(type: UInt16, payload: [UInt8]) -> Data {
        precondition(payload.count < 65_536 && payload.count.isMultiple(of: 2))
        var bytes: [UInt8] = [0x72, 0xf8, 0x1f, 0x4e, UInt8(type & 255), UInt8(type >> 8),
                             UInt8(payload.count & 255), UInt8(payload.count >> 8)]
        for index in stride(from: 0, to: payload.count, by: 2) {
            bytes += [payload[index + 1], payload[index]]
        }
        return Data(bytes)
    }

    static func matWithoutMajorSync() -> Data {
        var payload = [UInt8](repeating: 0, count: 61_424)
        payload.replaceSubrange(0..<20, with: [0x07, 0x9e, 0x00, 0x03, 0x84, 0x01, 0x01, 0x01,
                                             0x80, 0x00, 0x56, 0xa5, 0x3b, 0xf4, 0x81, 0x83,
                                             0x49, 0x80, 0x77, 0xe0])
        // One eight-byte access unit with no major-sync signature, then padding.
        payload.replaceSubrange(20..<28, with: [0, 4, 0, 0, 0x11, 0x22, 0x33, 0x44])
        var bytes = burst(type: 0x16, payload: payload)
        bytes.append(Data(repeating: 0, count: 61_440 - bytes.count))
        return bytes
    }

    static func dts(_ byte: UInt8) -> Data {
        burst(type: 0x11, payload: [0x01, 0, 0, 0, 0, 0, 0, 0, 0xfe, 0xfe, 0, 1, byte, 0])
    }

    static func write(_ bytes: Data, to sink: UnsafeMutableRawPointer) {
        guard !bytes.isEmpty else { return }
        bytes.withUnsafeBytes { mpSpatialWrite(sink, $0.baseAddress!, $0.count) }
    }

    static func main() async throws {
        let synchronizer = AVSampleBufferRenderSynchronizer()
        let pointer = Unmanaged.passUnretained(synchronizer).toOpaque()
        guard let sink = mpSpatialCreate(1, pointer) else { preconditionFailure("open failed") }
        let emptyTails: [(String, Data)] = [
            ("no input", Data()),
            ("filler", Data(repeating: 0, count: 256)),
            ("null burst", burst(type: 0, payload: [])),
            ("MAT before major sync", matWithoutMajorSync()),
        ]
        for (name, bytes) in emptyTails {
            mpSpatialBegin(sink, 0)
            write(bytes, to: sink)
            mpSpatialEnd(sink)
            try await wait(name) { mpSpatialReady(sink) || mpSpatialFailed(sink) }
            precondition(mpSpatialReady(sink) && !mpSpatialFailed(sink), "empty tail must drain: \(name)")
            let calls = SpatialAudioRenderer.calls.withLock { $0 }
            precondition(calls.enqueues == 0 && calls.ends == 0, "empty input must not flush a decoder")
            print("passed empty EOF: \(name)")
        }

        // Nonempty decoded input still flushes before readiness, even when the
        // renderer never reports its normal startup threshold reached.
        mpSpatialBegin(sink, 0)
        write(dts(0x7f), to: sink)
        mpSpatialEnd(sink)
        try await wait("renderer flush") { mpSpatialReady(sink) || mpSpatialFailed(sink) }
        precondition(mpSpatialReady(sink) && !mpSpatialFailed(sink))
        precondition(SpatialAudioRenderer.calls.withLock { $0.ends } == 1)

        mpSpatialBegin(sink, 0)
        write(dts(0x55), to: sink)
        try await wait("backpressured feeder") { SpatialAudioRenderer.calls.withLock { $0.enqueues } == 2 }
        mpSpatialBegin(sink, 0)
        try await Task.sleep(for: .milliseconds(30))
        precondition(!mpSpatialReady(sink) && !mpSpatialFailed(sink), "cancelled generation cannot finish its replacement")
        mpSpatialEnd(sink)
        try await wait("replacement EOF") { mpSpatialReady(sink) || mpSpatialFailed(sink) }
        precondition(mpSpatialReady(sink) && !mpSpatialFailed(sink))

        mpSpatialBegin(sink, 0)
        write(burst(type: 1, payload: []), to: sink)
        mpSpatialEnd(sink)
        try await wait("unsupported burst") { mpSpatialReady(sink) || mpSpatialFailed(sink) }
        precondition(mpSpatialFailed(sink) && !mpSpatialReady(sink), "actual unpacker errors must still fail")
        mpSpatialDestroy(sink)
        try await Task.sleep(for: .milliseconds(30)) // allow MainActor resource cleanup
        print("production feeder EOF, renderer flush, cancellation and failure checks passed")
    }
}
