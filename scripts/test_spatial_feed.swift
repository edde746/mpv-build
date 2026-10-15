@preconcurrency import AVFoundation
import Synchronization

// Only the decoder/encoder and activator are replaced. The complete production
// bridge and the source-pinned IEC unpacker are compiled alongside this file.
enum SpatialAudioCodec: UInt32 { case trueHD = 1, dtsHD = 2 }
enum SpatialAudioError: Error { case decoding(String), invalidState(String) }
enum HarnessError: Error { case timeout(String), emptyRendererFlush }
let mpSpatialSilentAtmos = Data()

actor SpatialAudioRenderer {
    static let calls = Mutex((enqueues: 0, ends: 0, finalizing: 0))
    static let gate = Mutex((enqueue: false, end: false, afterFinal: false, afterEnqueue: false))
    static let finalOutput = Mutex<CMTime?>(nil)
    static let delivered = Mutex<[Data]>([])
    typealias Submission = @Sendable (CMTime, Bool, () -> Void) throws -> Bool
    typealias Completion = @Sendable () throws -> Void
    static let latestSubmission = Mutex<Submission?>(nil)
    static let latestCompletion = Mutex<Completion?>(nil)
    private var submission: Submission?
    private var completion: Completion?
    private var generation = 0
    private var enqueued = false
    private var ready = false
    private var end = CMTime.zero
    var isReadyToPlay: Bool { ready }

    init(codec: SpatialAudioCodec, synchronizer: AVSampleBufferRenderSynchronizer) throws {}
    func setVolume(_ volume: Float) {}
    func begin(at time: CMTime, submission: Submission? = nil, completion: Completion? = nil) throws {
        generation += 1
        enqueued = false; ready = false; end = time
        self.submission = submission; self.completion = completion
        Self.latestSubmission.withLock { $0 = submission }
        Self.latestCompletion.withLock { $0 = completion }
    }
    private func emit(_ packet: Data, duration: CMTime, final: Bool, generation current: Int) async throws {
        let next = end + duration
        while true {
            try Task.checkCancellation()
            guard generation == current else { throw CancellationError() }
            if let submission {
                if try submission(next, final, { Self.delivered.withLock { $0.append(packet) } }) { break }
                try await Task.sleep(for: .milliseconds(2))
            } else { Self.delivered.withLock { $0.append(packet) }; break }
        }
        end = next
    }
    func enqueue(_ packet: Data, time: CMTime) async throws {
        let current = generation
        Self.calls.withLock { $0.enqueues += 1 }
        if packet == Data([0x55]) {
            while !Self.gate.withLock({ $0.enqueue }) {
                try Task.checkCancellation()
                guard generation == current else { throw CancellationError() }
                try await Task.sleep(for: .milliseconds(2))
            }
        }
        // Synthetic output exceeds the host's resume lead; it models no codec latency.
        try await emit(packet, duration: packet == Data([0x70]) ? .zero : CMTime(value: 400, timescale: 1000), final: false, generation: current)
        while Self.gate.withLock({ $0.afterEnqueue }) {
            try Task.checkCancellation()
            guard generation == current else { throw CancellationError() }
            try await Task.sleep(for: .milliseconds(2))
        }
        enqueued = true
        if packet == Data([0x42]) { ready = true }
    }
    func endOfStream() async throws {
        let current = generation
        guard enqueued else { throw HarnessError.emptyRendererFlush }
        Self.calls.withLock { $0.finalizing += 1 }
        while Self.gate.withLock({ $0.end }) {
            try Task.checkCancellation()
            guard generation == current else { throw CancellationError() }
            try await Task.sleep(for: .milliseconds(2))
        }
        try Task.checkCancellation()
        guard generation == current else { throw CancellationError() }
        if let duration = Self.finalOutput.withLock({ $0 }) {
            try await emit(Data([0xfa]), duration: duration, final: true, generation: current)
        } else { try completion?() }
        while Self.gate.withLock({ $0.afterFinal }) {
            try Task.checkCancellation()
            guard generation == current else { throw CancellationError() }
            try await Task.sleep(for: .milliseconds(2))
        }
        Self.calls.withLock { $0.ends += 1 }
    }
}

@MainActor
final class AtmosOutputActivator {
    static var attachments = 0
    init(silentAtmosStream: URL) {}
    func attach(to synchronizer: AVSampleBufferRenderSynchronizer) { Self.attachments += 1 }
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
        precondition(bytes.withUnsafeBytes { mpSpatialWrite(sink, $0.baseAddress!, $0.count) }, "input must fit the advertised capacity")
    }

    static func main() async throws {
        // Hold the main actor while the independent feeder reaches EOF. The
        // attachment task cannot run, so feeder completion alone is not ready.
        let deferredSynchronizer = AVSampleBufferRenderSynchronizer()
        guard let deferred = mpSpatialCreate(1, Unmanaged.passUnretained(deferredSynchronizer).toOpaque()) else {
            preconditionFailure("deferred open failed")
        }
        mpSpatialBegin(deferred, 0, 512 * 1024)
        write(dts(0x7f), to: deferred)
        mpSpatialEnd(deferred)
        let feederFinished = DispatchSemaphore(value: 0)
        let observer = Task.detached {
            while SpatialAudioRenderer.calls.withLock({ $0.ends }) == 0 {
                try await Task.sleep(for: .milliseconds(2))
            }
            feederFinished.signal()
        }
        precondition(feederFinished.wait(timeout: .now() + 2) == .success, "feeder runs independently of main actor")
        observer.cancel()
        precondition(AtmosOutputActivator.attachments == 0 && !mpSpatialReady(deferred))
        try await wait("deferred attachment") { mpSpatialReady(deferred) }
        precondition(AtmosOutputActivator.attachments == 1)
        mpSpatialDestroy(deferred)

        // Closing before main-actor setup must not attach or revive the sink.
        let closedSynchronizer = AVSampleBufferRenderSynchronizer()
        guard let closed = mpSpatialCreate(1, Unmanaged.passUnretained(closedSynchronizer).toOpaque()) else {
            preconditionFailure("closed open failed")
        }
        mpSpatialBegin(closed, 0, 512 * 1024)
        mpSpatialDestroy(closed)
        try await Task.sleep(for: .milliseconds(30))
        precondition(AtmosOutputActivator.attachments == 1)
        SpatialAudioRenderer.calls.withLock { $0 = (enqueues: 0, ends: 0, finalizing: 0) }
        print("activator attachment gates readiness; closed setup cannot attach")

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
            mpSpatialBegin(sink, 0, 512 * 1024)
            write(bytes, to: sink)
            mpSpatialEnd(sink)
            try await wait(name) { mpSpatialReady(sink) || mpSpatialFailed(sink) }
            precondition(mpSpatialReady(sink) && !mpSpatialFailed(sink), "empty tail must drain: \(name)")
            let calls = SpatialAudioRenderer.calls.withLock { $0 }
            precondition(calls.enqueues == 0 && calls.ends == 0, "empty input must not flush a decoder")
            print("passed empty EOF: \(name)")
        }

        // Readiness is submitted coverage, but file completion still waits for
        // the final flush, independently of the renderer's startup getter.
        mpSpatialBegin(sink, 0, 512 * 1024)
        write(dts(0x7f), to: sink)
        mpSpatialEnd(sink)
        try await wait("renderer flush") { mpSpatialFinished(sink) || mpSpatialFailed(sink) }
        precondition(mpSpatialReady(sink) && !mpSpatialFailed(sink))
        precondition(SpatialAudioRenderer.calls.withLock { $0.ends } == 1)

        // Actual SpatialOutput must allow playback before enqueue returns.
        // Otherwise a production renderer queue filling mid-batch deadlocks.
        mpSpatialBegin(sink, 0, 64)
        SpatialAudioRenderer.gate.withLock { $0.afterEnqueue = true }
        write(dts(0x42), to: sink)
        try await wait("submitted readiness before enqueue return") { mpSpatialReady(sink) }
        precondition(mpSpatialPending(sink) == dts(0x42).count)
        try await wait("submitted clock starts while feeder suspended") {
            _ = mpSpatialTime(sink, true, false)
            return CMTimebaseGetRate(synchronizer.timebase) == 1
        }
        _ = mpSpatialTime(sink, true, true)
        SpatialAudioRenderer.gate.withLock { $0.afterEnqueue = false }
        try await wait("submitted feeder returns") { mpSpatialPending(sink) == 0 }

        // Once playback is ready, a suspended renderer still holds its input
        // credit. Fill exactly the byte bound, retry a refused write, then
        // release the renderer and prove that accepted audio was not evicted.
        mpSpatialBegin(sink, 0, 64)
        write(dts(0x42), to: sink)
        try await wait("startup ready") { mpSpatialReady(sink) && mpSpatialPending(sink) == 0 }
        let countBeforeBlock = SpatialAudioRenderer.calls.withLock { $0.enqueues }
        write(dts(0x55), to: sink)
        try await wait("enqueue suspended") { SpatialAudioRenderer.calls.withLock { $0.enqueues } == countBeforeBlock + 1 }
        write(Data(repeating: 0, count: 64 - dts(0x55).count), to: sink)
        precondition(mpSpatialPending(sink) == 64)
        let refused = Data([0])
        precondition(!refused.withUnsafeBytes { mpSpatialWrite(sink, $0.baseAddress!, $0.count) })
        precondition(mpSpatialPending(sink) == 64 && !mpSpatialFailed(sink))
        SpatialAudioRenderer.gate.withLock { $0.enqueue = true }
        try await wait("credit returned") { mpSpatialPending(sink) == 0 }
        write(refused, to: sink)
        SpatialAudioRenderer.gate.withLock { $0.end = true }
        mpSpatialEnd(sink)
        try await wait("all input consumed") { mpSpatialPending(sink) == 0 }
        try await Task.sleep(for: .milliseconds(30))
        precondition(!mpSpatialFinished(sink), "EOF must wait for the suspended final flush")
        SpatialAudioRenderer.gate.withLock { $0.end = false }
        try await wait("final flush") { mpSpatialFinished(sink) }
        precondition(SpatialAudioRenderer.delivered.withLock { $0.filter { $0 == Data([0x55]) }.count } == 1)
        precondition(!refused.withUnsafeBytes { mpSpatialWrite(sink, $0.baseAddress!, $0.count) }, "finished input stays closed")
        print("bounded input includes suspended enqueue; no eviction; EOF waits for final flush")

        // Real CoreMedia time, synthetic output only. Timely holds preserve
        // the raw anchor and enough new lead resumes without resetting codec state.
        mpSpatialBegin(sink, 0, 512 * 1024)
        write(dts(0x42), to: sink)
        try await wait("clock startup") { mpSpatialReady(sink) && mpSpatialPending(sink) == 0 }
        precondition(mpSpatialSubmitted(sink) == 400_000_000)
        try await wait("early stop") {
            _ = mpSpatialTime(sink, true, false)
            return synchronizer.currentTime() > CMTime(value: 250, timescale: 1000) && CMTimebaseGetRate(synchronizer.timebase) == 0
        }
        let rawHeld = synchronizer.currentTime()
        precondition(rawHeld < CMTime(value: 400, timescale: 1000) && !mpSpatialFailed(sink))
        try await Task.sleep(for: .milliseconds(30))
        precondition(synchronizer.currentTime() == rawHeld, "ordinary hold must not rewind")
        write(dts(0x42), to: sink)
        try await wait("refill lead") { mpSpatialPending(sink) == 0 && CMTimebaseGetRate(synchronizer.timebase) == 1 }
        _ = mpSpatialTime(sink, true, true)
        try await wait("pause settled") { _ = mpSpatialTime(sink, true, true); return CMTimebaseGetRate(synchronizer.timebase) == 0 }
        let paused = mpSpatialTime(sink, true, true)
        try await Task.sleep(for: .milliseconds(30))
        precondition(mpSpatialTime(sink, true, true) == paused)
        try await wait("second early stop") {
            _ = mpSpatialTime(sink, true, false)
            return synchronizer.currentTime() > CMTime(value: 650, timescale: 1000) && CMTimebaseGetRate(synchronizer.timebase) == 0
        }
        mpSpatialEnd(sink) // zero-output completion releases the held final tail
        try await wait("short final tail") { _ = mpSpatialTime(sink, true, false); return mpSpatialFinished(sink) && mpSpatialDrained(sink) }
        precondition(mpSpatialTime(sink, true, false) == 800_000_000 && !mpSpatialFailed(sink))
        let staleSubmission = SpatialAudioRenderer.latestSubmission.withLock { $0! }
        let staleCompletion = SpatialAudioRenderer.latestCompletion.withLock { $0! }
        mpSpatialBegin(sink, 7_000_000_000, 512 * 1024)
        var staleEnqueued = false
        do {
            _ = try staleSubmission(CMTime(value: 8, timescale: 1), true) { staleEnqueued = true }
            preconditionFailure("stale submission must be cancelled")
        } catch is CancellationError {}
        do { try staleCompletion(); preconditionFailure("stale completion must be cancelled") }
        catch is CancellationError {}
        precondition(!staleEnqueued)
        mpSpatialEnd(sink)
        try await wait("empty reset") { mpSpatialFinished(sink) && mpSpatialDrained(sink) }
        precondition(mpSpatialTime(sink, true, false) == 7_000_000_000)

        // Separate sinks: terminal failure is sticky until mpv reconstructs the AO.
        for finalLater in [false, true] {
            let lateSync = AVSampleBufferRenderSynchronizer()
            let late = mpSpatialCreate(1, Unmanaged.passUnretained(lateSync).toOpaque())!
            mpSpatialBegin(late, 0, 512 * 1024)
            SpatialAudioRenderer.gate.withLock { $0.enqueue = false; $0.end = true }
            SpatialAudioRenderer.finalOutput.withLock { $0 = finalLater ? CMTime(value: 40, timescale: 1000) : nil }
            write(dts(0x42), to: late)
            try await wait("late startup") { mpSpatialReady(late) && mpSpatialPending(late) == 0 }
            let deliveredBefore = SpatialAudioRenderer.delivered.withLock { $0.count }
            let finalBefore = SpatialAudioRenderer.calls.withLock { $0.finalizing }
            if finalLater {
                mpSpatialEnd(late)
                try await wait("final encoding suspended") { SpatialAudioRenderer.calls.withLock { $0.finalizing } > finalBefore }
            } else { write(dts(0x55), to: late) }
            try await wait("late clock running") { _ = mpSpatialTime(late, true, false); return CMTimebaseGetRate(lateSync.timebase) == 1 }
            try await Task.sleep(for: .milliseconds(460)) // no AO polling; intentionally overrun
            if finalLater {
                _ = mpSpatialTime(late, true, false)
                precondition(!mpSpatialFailed(late), "finalization may still return empty")
            }
            SpatialAudioRenderer.gate.withLock { $0.enqueue = true; $0.end = false }
            try await wait("terminal overrun") { _ = mpSpatialTime(late, true, false); return mpSpatialFailed(late) }
            precondition(SpatialAudioRenderer.delivered.withLock { $0.count } == deliveredBefore, "late buffer cannot hide old overrun")
            precondition(lateSync.currentTime() >= CMTime(value: 400, timescale: 1000), "failure must not rewind")
            precondition(!dts(0x42).withUnsafeBytes { mpSpatialWrite(late, $0.baseAddress!, $0.count) })
            mpSpatialDestroy(late)
        }

        // Empty final encoding can finish after old coverage is exhausted; a
        // nonempty final callback declares its horizon before delayed feeder return.
        for nonempty in [false, true] {
            let finishSync = AVSampleBufferRenderSynchronizer()
            let finish = mpSpatialCreate(1, Unmanaged.passUnretained(finishSync).toOpaque())!
            mpSpatialBegin(finish, 0, 512 * 1024)
            SpatialAudioRenderer.gate.withLock { $0.end = !nonempty; $0.afterFinal = nonempty }
            SpatialAudioRenderer.finalOutput.withLock { $0 = nonempty ? CMTime(value: 40, timescale: 1000) : nil }
            write(dts(0x42), to: finish)
            try await wait("final startup") { mpSpatialReady(finish) && mpSpatialPending(finish) == 0 }
            let finalBefore = SpatialAudioRenderer.calls.withLock { $0.finalizing }
            mpSpatialEnd(finish)
            try await wait("finalization entered") { SpatialAudioRenderer.calls.withLock { $0.finalizing } > finalBefore }
            if nonempty { try await wait("final horizon") { mpSpatialSubmitted(finish) == 440_000_000 } }
            try await wait("final clock running") { _ = mpSpatialTime(finish, true, false); return CMTimebaseGetRate(finishSync.timebase) == 1 }
            try await Task.sleep(for: .milliseconds(480))
            _ = mpSpatialTime(finish, true, false)
            precondition(!mpSpatialFailed(finish) && !mpSpatialFinished(finish))
            SpatialAudioRenderer.gate.withLock { $0.end = false; $0.afterFinal = false }
            try await wait("delayed final completion") { _ = mpSpatialTime(finish, true, false); return mpSpatialFinished(finish) && mpSpatialDrained(finish) }
            precondition(!mpSpatialFailed(finish))
            mpSpatialDestroy(finish)
        }
        // Recheck OLD coverage after a slow framework call too. Throwing
        // after one enqueue is terminal; it must not permit another submission.
        let costlySync = AVSampleBufferRenderSynchronizer()
        let costly = mpSpatialCreate(1, Unmanaged.passUnretained(costlySync).toOpaque())!
        mpSpatialBegin(costly, 0, 512 * 1024)
        write(dts(0x42), to: costly)
        try await wait("costly startup") { mpSpatialReady(costly) && mpSpatialPending(costly) == 0 }
        try await wait("costly clock running") { _ = mpSpatialTime(costly, true, false); return CMTimebaseGetRate(costlySync.timebase) == 1 }
        let costlyHook = SpatialAudioRenderer.latestSubmission.withLock { $0! }
        var costlyCalls = 0
        do {
            _ = try costlyHook(CMTime(value: 800, timescale: 1000), false) { costlyCalls += 1; usleep(500_000) }
            preconditionFailure("post-enqueue overrun accepted")
        } catch {}
        precondition(costlyCalls == 1 && mpSpatialFailed(costly) && mpSpatialSubmitted(costly) == 400_000_000)
        do { _ = try costlyHook(CMTime(value: 800, timescale: 1000), true) { costlyCalls += 1 }; preconditionFailure("terminal retry accepted") }
        catch {}
        precondition(costlyCalls == 1)
        mpSpatialDestroy(costly)

        let primingSync = AVSampleBufferRenderSynchronizer()
        let priming = mpSpatialCreate(1, Unmanaged.passUnretained(primingSync).toOpaque())!
        mpSpatialBegin(priming, 3_000_000_000, 512 * 1024)
        SpatialAudioRenderer.finalOutput.withLock { $0 = .zero }
        write(dts(0x70), to: priming)
        mpSpatialEnd(priming)
        try await wait("priming-only final output") { mpSpatialFinished(priming) && mpSpatialDrained(priming) }
        precondition(mpSpatialSubmitted(priming) == 3_000_000_000 && !mpSpatialFailed(priming))
        mpSpatialDestroy(priming)
        SpatialAudioRenderer.finalOutput.withLock { $0 = nil }
        print("clock timely holds, terminal late packets, finalization races, pause/reset and stale events passed")

        SpatialAudioRenderer.gate.withLock { $0.enqueue = false }
        let beforeCancellation = SpatialAudioRenderer.calls.withLock { $0.enqueues }
        mpSpatialBegin(sink, 0, 512 * 1024)
        write(dts(0x55), to: sink)
        try await wait("backpressured feeder") { SpatialAudioRenderer.calls.withLock { $0.enqueues } == beforeCancellation + 1 }
        mpSpatialBegin(sink, 0, 512 * 1024)
        try await Task.sleep(for: .milliseconds(30))
        precondition(!mpSpatialReady(sink) && !mpSpatialFailed(sink), "cancelled generation cannot finish its replacement")
        precondition(mpSpatialPending(sink) == 0 && !mpSpatialFinished(sink), "cancelled generation cannot return replacement credit")
        mpSpatialEnd(sink)
        try await wait("replacement EOF") { mpSpatialReady(sink) || mpSpatialFailed(sink) }
        precondition(mpSpatialReady(sink) && !mpSpatialFailed(sink))

        mpSpatialBegin(sink, 0, 512 * 1024)
        write(burst(type: 1, payload: []), to: sink)
        mpSpatialEnd(sink)
        try await wait("unsupported burst") { mpSpatialReady(sink) || mpSpatialFailed(sink) }
        precondition(mpSpatialFailed(sink) && !mpSpatialReady(sink), "actual unpacker errors must still fail")
        mpSpatialDestroy(sink)
        try await Task.sleep(for: .milliseconds(30)) // allow MainActor resource cleanup
        print("production feeder EOF, renderer flush, cancellation and failure checks passed")
    }
}
