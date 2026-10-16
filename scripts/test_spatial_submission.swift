import AVFoundation
import Synchronization

// These doubles shadow only the framework output and codec types. The complete
// production renderer, including retry/refill/generation logic, is compiled as-is.
final class AVSampleBufferAudioRenderer: @unchecked Sendable {
    struct State {
        var ready = true
        var enqueues = 0
        var stopAt: Int?
        var flushes: [@Sendable (Bool) -> Void] = []
    }
    static let control = Mutex(State())
    var status: AVQueuedSampleBufferRenderingStatus { .rendering }
    var error: Error? { nil }
    var hasSufficientMediaDataForReliablePlaybackStart: Bool { true }
    var isReadyForMoreMediaData: Bool { Self.control.withLock { $0.ready } }
    var volume: Float = 1
    var isMuted = false
    func enqueue(_ buffer: CMSampleBuffer) {
        Self.control.withLock { state in
            state.enqueues += 1
            if state.enqueues == state.stopAt { state.ready = false }
        }
    }
    func flush() {}
    func flush(fromSourceTime time: CMTime, completionHandler: @escaping @Sendable (Bool) -> Void) {
        Self.control.withLock { $0.flushes.append(completionHandler) }
    }
    static func finishFlushes() {
        let callbacks = control.withLock { s in defer { s.flushes = [] }; return s.flushes }
        for callback in callbacks { callback(true) }
    }
}
public final class AVSampleBufferRenderSynchronizer: @unchecked Sendable {
    let timebase: CMTimebase
    var delaysRateChangeUntilHasSufficientMediaData = false
    init() {
        var created: CMTimebase?
        precondition(CMTimebaseCreateWithSourceClock(allocator: kCFAllocatorDefault,
            sourceClock: CMClockGetHostTimeClock(), timebaseOut: &created) == noErr)
        timebase = created!
    }
    func currentTime() -> CMTime { CMTimebaseGetTime(timebase) }
    func setRate(_ rate: Float, time: CMTime) {
        if time.isValid { precondition(CMTimebaseSetTime(timebase, time: time) == noErr) }
        precondition(CMTimebaseSetRate(timebase, rate: Double(rate)) == noErr)
    }
    func addRenderer(_ renderer: AVSampleBufferAudioRenderer) {}
    func removeRenderer(_ renderer: AVSampleBufferAudioRenderer, at time: CMTime) {}
}
public enum SpatialAudioCodec { case trueHD }
enum SpatialAudioError: Error {
    case invalidConfiguration(String), invalidState(String), encoding(String), coreMedia(OSStatus)
}
public struct APACConfiguration {
    public init() {}
    struct Layout { var channelCount: Int { 2 } }
    var layout = Layout()
    var liveEncoding = true
    var gainDB: Float = 0
    var normalizesDialogue = false
    var hasValidEncoderSettings: Bool { true }
}
struct PCMBlock {
    let samples: [Float]
    var sampleRate: Int { 48_000 }
    var channels: Int { 2 }
    var frames: Int { samples.count / channels }
}
struct Decoder {
    init(codec: SpatialAudioCodec, layout: APACConfiguration.Layout, normalizesDialogue: Bool) throws {}
    func decode(_ bytes: UnsafeRawBufferPointer, end: Bool) throws -> [PCMBlock] {
        let packets = end ? 0 : Int(bytes.first ?? 0)
        return packets == 0 ? [] : [PCMBlock(samples: [Float](repeating: 0, count: packets * 2048))]
    }
}
struct APACEncoder {
    struct Packet { let data: Data; let isIndependent: Bool }
    static let settings = Mutex((priming: 0, finalPackets: 0))
    let format: CMAudioFormatDescription
    let primingFrames: Int
    init(configuration: APACConfiguration) throws {
        primingFrames = Self.settings.withLock { $0.priming }
        var asbd = AudioStreamBasicDescription(mSampleRate: 48_000, mFormatID: kAudioFormatMPEG4AAC,
            mFormatFlags: 0, mBytesPerPacket: 0, mFramesPerPacket: 1024,
            mBytesPerFrame: 0, mChannelsPerFrame: 2, mBitsPerChannel: 0, mReserved: 0)
        var description: CMAudioFormatDescription?
        let status = CMAudioFormatDescriptionCreate(allocator: kCFAllocatorDefault, asbd: &asbd,
            layoutSize: 0, layout: nil, magicCookieSize: 0, magicCookie: nil,
            extensions: nil, formatDescriptionOut: &description)
        guard status == noErr, let description else { throw SpatialAudioError.coreMedia(status) }
        format = description
    }
    func encode(_ samples: ArraySlice<Float>, final: Bool = false) throws -> [Packet] {
        let count = samples.count / 2048 + (final ? Self.settings.withLock { $0.finalPackets } : 0)
        return (0..<count).map { _ in Packet(data: Data([0x01, 0x02]), isIndependent: true) }
    }
}

enum ProbeError: Error { case terminal, timeout }
@main struct SubmissionChecks {
    static func wait(_ predicate: @Sendable () -> Bool) async throws {
        let deadline = ContinuousClock.now.advanced(by: .seconds(2))
        while !predicate() {
            guard ContinuousClock.now < deadline else { throw ProbeError.timeout }
            try await Task.sleep(for: .milliseconds(2))
        }
    }
    static var enqueues: Int { AVSampleBufferAudioRenderer.control.withLock { $0.enqueues } }
    static func fullQueueChecks() async throws {
        // Production renderer AND production host clock. The queue double
        // refuses more buffers mid-batch; only simulated playback frees it.
        for finalBatch in [false, true] {
            let sync = AVSampleBufferRenderSynchronizer()
            let clock = SpatialClock(sync)
            let renderer = try SpatialAudioRenderer(codec: .trueHD, synchronizer: sync)
            clock.begin(at: .zero, generation: 1)
            APACEncoder.settings.withLock { $0 = (priming: 0, finalPackets: finalBatch ? 24 : 0) }
            try await renderer.begin(at: .zero, submission: { end, final, enqueue in
                try clock.submit(through: end, isFinal: final, generation: 1, enqueue: enqueue)
            }, completion: { try clock.finishEmpty(generation: 1) })
            let before = enqueues
            AVSampleBufferAudioRenderer.control.withLock { $0.ready = true; $0.stopAt = before + 12 }
            let returned = Mutex(false)
            if finalBatch { clock.beginFinalization(generation: 1) }
            let batch = Task {
                if finalBatch { try await renderer.endOfStream() }
                else { try await renderer.enqueue(Data([24]), time: .zero) }
                returned.withLock { $0 = true }
            }
            try await wait { enqueues == before + 12 }
            precondition(!returned.withLock { $0 } && clock.ready)
            precondition(clock.submitted == CMTime(value: 12 * 1024, timescale: 48_000))
            _ = clock.position(playing: true)
            precondition(CMTimebaseGetRate(sync.timebase) == 1, "full batch must not block startup")
            try await wait { sync.currentTime() > .zero }
            AVSampleBufferAudioRenderer.control.withLock { $0.ready = true; $0.stopAt = nil }
            try await batch.value
            precondition(enqueues == before + 24 && clock.failure == nil)
            _ = clock.position(playing: false)
        }

        // No minimum native capacity is assumed. Test startup and a refill
        // after an early hold, including final flush blocked before its last
        // packet: insufficient lead must fail within a bounded requested wait.
        for (refill, finalBatch, priming) in [(false, false, 0), (false, true, 0),
                                             (true, false, 0), (true, true, 0), (false, true, 4096)] {
            let sync = AVSampleBufferRenderSynchronizer()
            let clock = SpatialClock(sync)
            let renderer = try SpatialAudioRenderer(codec: .trueHD, synchronizer: sync)
            clock.begin(at: .zero, generation: 1)
            APACEncoder.settings.withLock { $0 = (priming: priming, finalPackets: finalBatch ? 8 : 0) }
            AVSampleBufferAudioRenderer.control.withLock { $0.ready = true; $0.stopAt = nil }
            try await renderer.begin(at: .zero, submission: { end, final, enqueue in
                try clock.submit(through: end, isFinal: final, generation: 1, enqueue: enqueue)
            }, completion: { try clock.finishEmpty(generation: 1) })
            if refill {
                try await renderer.enqueue(Data([12]), time: .zero)
                try await wait {
                    _ = clock.position(playing: true)
                    return sync.currentTime() > CMTime(value: 150, timescale: 1000) &&
                        CMTimebaseGetRate(sync.timebase) == 0
                }
            }
            let before = enqueues
            AVSampleBufferAudioRenderer.control.withLock { $0.stopAt = before + 4 }
            if finalBatch { clock.beginFinalization(generation: 1) }
            let batch = Task {
                if finalBatch { try await renderer.endOfStream() }
                else { try await renderer.enqueue(Data([24]), time: .zero) }
            }
            try await wait { enqueues == before + 4 }
            // A user pause is not a queue-capacity timeout.
            _ = clock.position(playing: false)
            try await Task.sleep(for: .milliseconds(2050))
            _ = clock.position(playing: false)
            precondition(clock.failure == nil && CMTimebaseGetRate(sync.timebase) == 0)
            let deadline = ContinuousClock.now.advanced(by: .seconds(3))
            while clock.failure == nil && ContinuousClock.now < deadline {
                _ = clock.position(playing: true)
                try await Task.sleep(for: .milliseconds(5))
            }
            guard case .insufficientLead? = clock.failure else { preconditionFailure("full queue hung") }
            precondition(enqueues == before + 4 && CMTimebaseGetRate(sync.timebase) == 0)
            batch.cancel() // mirrors AO close after the terminal PCM reload request
            do { try await batch.value; preconditionFailure("blocked batch completed") }
            catch is CancellationError {}
        }
        AVSampleBufferAudioRenderer.control.withLock { $0.ready = true; $0.stopAt = nil }
        // A completed short flush needs no 250ms prebuffer; priming-only final
        // output is terminal at the begin origin and needs no clock movement.
        for priming in [0, 2048] {
            let sync = AVSampleBufferRenderSynchronizer()
            let clock = SpatialClock(sync)
            let renderer = try SpatialAudioRenderer(codec: .trueHD, synchronizer: sync)
            clock.begin(at: .zero, generation: 1)
            APACEncoder.settings.withLock { $0 = (priming: priming, finalPackets: 2) }
            try await renderer.begin(at: .zero, submission: { end, final, enqueue in
                try clock.submit(through: end, isFinal: final, generation: 1, enqueue: enqueue)
            }, completion: { try clock.finishEmpty(generation: 1) })
            clock.beginFinalization(generation: 1)
            try await renderer.endOfStream()
            precondition(clock.ready)
            try await wait { _ = clock.position(playing: true); return clock.drained }
            precondition(clock.failure == nil && clock.submitted ==
                CMTime(value: Int64(2048 - priming), timescale: 48_000))
        }
        APACEncoder.settings.withLock { $0 = (priming: 0, finalPackets: 0) }
        print("production mid-batch/final-flush startup and bounded insufficient-capacity fallback passed")
    }
    static func main() async throws {
        let renderer = try SpatialAudioRenderer(codec: .trueHD, synchronizer: AVSampleBufferRenderSynchronizer())
        let gate = Mutex(false)
        let attempts = Mutex(0)
        let accepted = Mutex<[(CMTime, Bool)]>([])
        let complete = Mutex(0)
        let hook: SpatialAudioRenderer.Submission = { end, final, enqueue in
            attempts.withLock { $0 += 1 }
            guard gate.withLock({ $0 }) else { return false }
            enqueue()
            accepted.withLock { $0.append((end, final)) }
            return true
        }
        try await renderer.begin(at: .zero, submission: hook, completion: { complete.withLock { $0 += 1 } })
        let blocked = Task { try await renderer.enqueue(Data([1]), time: .zero) }
        try await wait { attempts.withLock { $0 } > 1 }
        precondition(enqueues == 0 && accepted.withLock { $0.isEmpty })
        AVSampleBufferAudioRenderer.control.withLock { $0.ready = false }
        try await Task.sleep(for: .milliseconds(10))
        let whileNotReady = attempts.withLock { $0 }
        gate.withLock { $0 = true }
        try await Task.sleep(for: .milliseconds(20))
        precondition(enqueues == 0 && attempts.withLock { $0 } == whileNotReady)
        AVSampleBufferAudioRenderer.control.withLock { $0.ready = true }
        try await blocked.value
        precondition(enqueues == 1 && accepted.withLock { $0.count } == 1)
        try await renderer.endOfStream()
        precondition(complete.withLock { $0 } == 1)
        print("production submission: false retries without enqueue; true once; empty final completion")

        // Contract misuse is terminal, and never physically queues twice.
        for mode in 0..<5 {
            try await renderer.begin(at: .zero, submission: { _, _, enqueue in
                switch mode {
                case 0: return true
                case 1: enqueue(); return false
                case 2: enqueue(); enqueue(); return true
                case 3: throw ProbeError.terminal
                default: enqueue(); throw ProbeError.terminal
                }
            })
            let before = enqueues
            do { try await renderer.enqueue(Data([1]), time: .zero); preconditionFailure("misuse accepted") }
            catch {}
            precondition(enqueues - before == ((mode == 1 || mode == 2 || mode == 4) ? 1 : 0))
            do { try await renderer.enqueue(Data([1]), time: .zero); preconditionFailure("failed timeline continued") }
            catch {}
        }

        // Cancellation and reset invalidate an actor suspended by host refusal.
        for cancel in [true, false] {
            gate.withLock { $0 = false }; attempts.withLock { $0 = 0 }
            try await renderer.begin(at: .zero, submission: hook)
            let pending = Task { try await renderer.enqueue(Data([1]), time: .zero) }
            try await wait { attempts.withLock { $0 } > 0 }
            if cancel { pending.cancel() }
            else { try await renderer.begin(at: CMTime(value: 2, timescale: 1)) }
            do { try await pending.value; preconditionFailure("old retry survived") }
            catch is CancellationError {}
        }

        // A pending asynchronous refill runs before retry; replay is not a new
        // admission and does not advance the host horizon a second time.
        gate.withLock { $0 = true }; attempts.withLock { $0 = 0 }; accepted.withLock { $0 = [] }
        try await renderer.begin(at: .zero, submission: hook)
        try await renderer.enqueue(Data([1]), time: .zero)
        gate.withLock { $0 = false }
        let retry = Task { try await renderer.enqueue(Data([1]), time: .zero) }
        try await wait { attempts.withLock { $0 } > 1 }
        await renderer.refill(from: .zero)
        let atRefill = attempts.withLock { $0 }
        gate.withLock { $0 = true }
        try await Task.sleep(for: .milliseconds(20))
        precondition(attempts.withLock { $0 } == atRefill, "retry bypassed pending refill")
        AVSampleBufferAudioRenderer.finishFlushes()
        try await retry.value
        precondition(accepted.withLock { $0.count } == 2)
        let beforeStaleRefill = enqueues
        await renderer.refill(from: .zero)
        try await renderer.begin(at: CMTime(value: 5, timescale: 1))
        AVSampleBufferAudioRenderer.finishFlushes()
        try await Task.sleep(for: .milliseconds(20))
        precondition(enqueues == beforeStaleRefill, "old refill replayed after reset")

        // Final status is attached only to the last actual packet. A final
        // encode returning no packets uses completion, including a blank stream.
        APACEncoder.settings.withLock { $0 = (priming: 1024, finalPackets: 2) }
        accepted.withLock { $0 = [] }; complete.withLock { $0 = 0 }
        try await renderer.begin(at: .zero, submission: hook, completion: { complete.withLock { $0 += 1 } })
        try await renderer.endOfStream()
        let final = accepted.withLock { $0 }
        precondition(final.count == 2 && final[0].0 == .zero && !final[0].1 && final[1].1)
        precondition(final[1].0 == CMTime(value: 1024, timescale: 48_000) && complete.withLock { $0 } == 0)
        APACEncoder.settings.withLock { $0 = (priming: 1024, finalPackets: 1) }
        accepted.withLock { $0 = [] }
        try await renderer.begin(at: .zero, submission: hook)
        try await renderer.endOfStream()
        precondition(accepted.withLock { $0.count == 1 && $0[0].0 == .zero && $0[0].1 })
        APACEncoder.settings.withLock { $0 = (priming: 0, finalPackets: 0) }
        try await renderer.begin(at: .zero, submission: hook, completion: { complete.withLock { $0 += 1 } })
        try await renderer.endOfStream()
        precondition(complete.withLock { $0 } == 1)
        APACEncoder.settings.withLock { $0 = (priming: 0, finalPackets: 1) }
        gate.withLock { $0 = false }; attempts.withLock { $0 = 0 }
        accepted.withLock { $0 = [] }; complete.withLock { $0 = 0 }
        try await renderer.begin(at: .zero, submission: hook, completion: { complete.withLock { $0 += 1 } })
        let oldFinal = Task { try await renderer.endOfStream() }
        try await wait { attempts.withLock { $0 } > 0 }
        try await renderer.begin(at: CMTime(value: 9, timescale: 1))
        gate.withLock { $0 = true }
        do { try await oldFinal.value; preconditionFailure("stale final succeeded") }
        catch is CancellationError {}
        precondition(accepted.withLock { $0.isEmpty } && complete.withLock { $0 } == 0)
        try await fullQueueChecks()
        print("production renderer admission counts, terminal misuse, cancellation/reset, refill and final/priming/empty events passed")
    }
}
