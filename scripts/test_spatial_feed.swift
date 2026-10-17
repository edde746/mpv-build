@preconcurrency import AVFoundation
import Synchronization

// Only the renderer and the activator are replaced. The production bridge and
// SpatialAudioBridge's pinned passthrough sink and IEC unpacker are compiled
// alongside this file. The sink's public API names these types, so they are
// public too.
public enum SpatialAudioCodec: UInt32, Sendable { case trueHDAtmos = 1, dtsX = 2 }
public struct APACConfiguration: Sendable { public init() {} }
enum SpatialAudioError: Error, Sendable {
    case decoding(String), unsupported(String), discontinuity(String), encoding(String)
}
enum HarnessError: Error { case timeout(String) }
let mpSpatialSilentAtmos = Data()

public struct SpatialAudioRendererProgress: Sendable, Equatable {
    public let timelineStart: CMTime
    public let submittedEnd: CMTime
    public let isFinal: Bool
}

// Each enqueued packet plays for 100 ms.
actor SpatialAudioRenderer {
    struct Log: Sendable {
        var packets: [Data] = []
        var begins: [CMTime] = []
        var volume: Float = 1
    }
    static let log = Mutex(Log())
    // Models a route change that flushed part of the queue.
    static let queueLimit = Mutex<CMTime?>(nil)

    private let synchronizer: AVSampleBufferRenderSynchronizer
    private var timeline = CMTime.zero
    private var submittedEnd = CMTime.zero
    private var final = false
    private var latest: SpatialAudioRendererProgress?
    private var continuation: AsyncStream<SpatialAudioRendererProgress>.Continuation?

    init(codec: SpatialAudioCodec, configuration: APACConfiguration,
         synchronizer: AVSampleBufferRenderSynchronizer) throws {
        self.synchronizer = synchronizer
    }

    var isReadyToPlay: Bool { final || submittedEnd - timeline >= CMTime(value: 300, timescale: 1000) }
    var renderingStatus: AVQueuedSampleBufferRenderingStatus { .rendering }
    var queuedAhead: CMTime {
        let ahead = CMTimeMaximum(.zero, submittedEnd - synchronizer.currentTime())
        return Self.queueLimit.withLock { $0 }.map { CMTimeMinimum($0, ahead) } ?? ahead
    }

    func progress() -> AsyncStream<SpatialAudioRendererProgress> {
        continuation?.finish()
        let (stream, continuation) = AsyncStream.makeStream(of: SpatialAudioRendererProgress.self,
                                                            bufferingPolicy: .bufferingNewest(1))
        if let latest { continuation.yield(latest) }
        self.continuation = continuation
        return stream
    }

    private func report() {
        let progress = SpatialAudioRendererProgress(timelineStart: timeline, submittedEnd: submittedEnd, isFinal: final)
        latest = progress
        continuation?.yield(progress)
    }

    func begin(at time: CMTime) throws {
        timeline = time
        submittedEnd = time
        final = false
        Self.log.withLock { $0.begins.append(time) }
        report()
    }

    func enqueue(_ packet: Data, time: CMTime) async throws {
        Self.log.withLock { $0.packets.append(packet) }
        submittedEnd = submittedEnd + CMTime(value: 100, timescale: 1000)
        report()
    }

    func endOfStream() async throws {
        final = true
        report()
    }

    func setVolume(_ volume: Float) { Self.log.withLock { $0.volume = volume } }
    func setMuted(_ muted: Bool) {}
}

@MainActor
final class AtmosOutputActivator {
    static var attachments = 0
    static var detachments = 0
    init(silentAtmosStream: URL) {}
    func attach(to synchronizer: AVSampleBufferRenderSynchronizer) { Self.attachments += 1 }
    func detach() { Self.detachments += 1 }
}

@main
@MainActor
struct BridgeChecks {
    static var failures = 0

    static func check(_ condition: Bool, _ message: String) {
        if !condition {
            failures += 1
            print("FAIL: \(message)")
        }
    }

    static func wait(_ label: String, until predicate: () -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(3))
        while !predicate() {
            guard ContinuousClock.now < deadline else { throw HarnessError.timeout(label) }
            try await Task.sleep(for: .milliseconds(5))
        }
    }

    // A little-endian IEC 61937 burst, as mpv's spdif muxer writes it.
    static func burst(type: UInt16, payload: [UInt8]) -> Data {
        precondition(payload.count < 65_536 && payload.count.isMultiple(of: 2))
        var bytes: [UInt8] = [0x72, 0xf8, 0x1f, 0x4e, UInt8(type & 255), UInt8(type >> 8),
                              UInt8(payload.count & 255), UInt8(payload.count >> 8)]
        for index in stride(from: 0, to: payload.count, by: 2) {
            bytes += [payload[index + 1], payload[index]]
        }
        return Data(bytes)
    }

    // A type IV burst carrying a one-byte DTS frame.
    static func dts(_ byte: UInt8) -> Data {
        burst(type: 0x11, payload: [0x01, 0, 0, 0, 0, 0, 0, 0, 0xfe, 0xfe, 0, 1, byte, 0])
    }

    static func write(_ bytes: Data, to output: UnsafeMutableRawPointer) -> Int {
        bytes.withUnsafeBytes { mpSpatialWrite(output, $0.baseAddress!, $0.count) }
    }

    static func poll(_ output: UnsafeMutableRawPointer) -> (kind: Int32, ready: Bool, covered: Int64, end: Int64, final: Bool) {
        var ready = false, covered: Int64 = 0, end: Int64 = 0, final = false
        let kind = mpSpatialPoll(output, &ready, &covered, &end, &final)
        return (kind, ready, covered, end, final)
    }

    static func failure(_ output: UnsafeMutableRawPointer) -> String? {
        guard let reason = mpSpatialFailure(output) else { return nil }
        defer { free(reason) }
        return String(cString: reason)
    }

    static func main() async {
        do {
            try await run()
        } catch {
            failures += 1
            print("FAIL: \(error)")
        }
        print(failures == 0 ? "all passed" : "\(failures) FAILED")
        exit(failures == 0 ? 0 : 1)
    }

    static func run() async throws {
        let ms: Int64 = 1_000_000
        let synchronizer = AVSampleBufferRenderSynchronizer()
        guard let output = mpSpatialCreate(2, Unmanaged.passUnretained(synchronizer).toOpaque(), 1 << 20) else {
            check(false, "the output opens")
            return
        }
        try await wait("the activator attaches") { AtmosOutputActivator.attachments == 1 }

        // Nothing is known until the timeline has reset the renderer.
        mpSpatialBegin(output, 0)
        check(poll(output).covered == .min || poll(output).covered == 0, "no audio is covered yet")
        try await wait("the timeline resets the renderer") { poll(output).covered == 0 }
        check(!poll(output).ready && poll(output).kind == 0, "an empty timeline is neither ready nor failed")

        // Bursts reach the renderer as their DTS frames; coverage follows the
        // enqueued audio once the queue has been polled too.
        for byte: UInt8 in 1...5 {
            let burst = dts(byte)
            check(write(burst, to: output) == burst.count, "the sink takes a burst that fits")
        }
        try await wait("five frames are enqueued") { SpatialAudioRenderer.log.withLock { $0.packets.count } == 5 }
        check(SpatialAudioRenderer.log.withLock { $0.packets } == (1...5).map { Data([$0]) }, "frames arrive in order, unpacked")
        try await wait("coverage reaches the enqueued audio") { poll(output).covered == 500 * ms }
        try await wait("the sink reports readiness") { poll(output).ready }

        // The polled queue caps the coverage below what was enqueued.
        SpatialAudioRenderer.queueLimit.withLock { $0 = CMTime(value: 100, timescale: 1000) }
        try await wait("a shorter queue lowers the coverage") { poll(output).covered == 100 * ms }
        check(poll(output).end == 500 * ms, "the end is still what was enqueued")
        SpatialAudioRenderer.queueLimit.withLock { $0 = nil }
        try await wait("a refilled queue restores it") { poll(output).covered == 500 * ms }

        // Silence between bursts converts to nothing and fails nothing.
        check(write(Data(count: 30_000), to: output) == 30_000, "the sink takes silence")
        try await Task.sleep(for: .milliseconds(100))
        check(poll(output).kind == 0 && SpatialAudioRenderer.log.withLock { $0.packets.count } == 5,
              "silence neither enqueues audio nor fails")

        // The end counts once the queue has been measured after it, and a
        // later flush still lowers the coverage of the finished stream.
        mpSpatialEnd(output)
        try await wait("the end is final") { poll(output).final }
        check(poll(output).covered == 500 * ms && poll(output).end == 500 * ms, "a final stream is covered to its end")
        SpatialAudioRenderer.queueLimit.withLock { $0 = CMTime(value: 100, timescale: 1000) }
        try await wait("a flush lowers a final stream's coverage") { poll(output).covered == 100 * ms }
        check(poll(output).final && poll(output).end == 500 * ms, "but not its end")
        SpatialAudioRenderer.queueLimit.withLock { $0 = nil }

        // A new timeline forgets the previous one's queue.
        mpSpatialBegin(output, 5_000 * ms)
        let fresh = poll(output)
        check(!fresh.final && fresh.covered <= 5_000 * ms, "a new timeline starts uncovered")
        try await Task.sleep(for: .milliseconds(120))
        check(poll(output).covered == 5_000 * ms, "the old queue never covers the new timeline")

        // A pause burst is a gap: the timeline stops until the next begin.
        check(write(burst(type: 0x03, payload: [0, 0]), to: output) > 0, "the sink takes a pause burst")
        try await wait("the gap is reported") { poll(output).kind == 1 }
        check(failure(output) != nil, "the gap is described for the log")
        check(write(dts(9), to: output) == 0, "a stopped timeline takes nothing")
        mpSpatialBegin(output, 6_000 * ms)
        check(poll(output).kind == 0 && failure(output) == nil, "a new timeline clears the gap")

        // Anything else is a failure to fall back on.
        check(write(burst(type: 0x01, payload: [0, 0]), to: output) > 0, "the sink takes an AC-3 burst")
        try await wait("the failure is reported") { poll(output).kind == 2 }
        check(failure(output)?.contains("Unsupported") == true, "the failure says why")

        mpSpatialVolume(output, 0.5)
        try await wait("the volume reaches the renderer") { SpatialAudioRenderer.log.withLock { $0.volume } == 0.5 }

        let begins = SpatialAudioRenderer.log.withLock { $0.begins.count }
        mpSpatialDestroy(output)
        try await wait("closing detaches the activator") { AtmosOutputActivator.detachments == 1 }
        try await wait("closing resets the renderer") {
            SpatialAudioRenderer.log.withLock { $0.begins.count > begins && $0.begins.last == .zero }
        }
    }
}
