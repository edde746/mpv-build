import Foundation
import Synchronization

// Used serially by this harness; the production lease still uses its Mutex.
final class AVAudioSession: @unchecked Sendable {
    enum Category { case ambient, playback }
    enum Mode { case `default`, moviePlayback }
    struct CategoryOptions: OptionSet, Sendable {
        let rawValue: Int
        static let mixWithOthers = Self(rawValue: 1)
    }
    struct SetActiveOptions: OptionSet, Sendable {
        let rawValue: Int
        static let notifyOthersOnDeactivation = Self(rawValue: 1)
    }
    enum Failure: Error { case activation }

    static let shared = AVAudioSession()
    static func sharedInstance() -> AVAudioSession { shared }
    var category = Category.ambient
    var mode = Mode.default
    var categoryOptions: CategoryOptions = [.mixWithOthers]
    var supportsMultichannelContent = false
    var active = false
    var activationCalls = 0
    var deactivationCalls = 0
    var categoryCalls = 0
    var refuseActivation = false

    func setCategory(_ category: Category, mode: Mode, options: CategoryOptions = []) throws {
        self.category = category
        self.mode = mode
        categoryOptions = options
        categoryCalls += 1
    }
    func setSupportsMultichannelContent(_ enabled: Bool) throws {
        supportsMultichannelContent = enabled
    }
    func setActive(_ active: Bool, options: SetActiveOptions = []) throws {
        if active {
            activationCalls += 1
            if refuseActivation { throw Failure.activation }
        } else {
            deactivationCalls += 1
        }
        self.active = active
    }
}

// The runner appends the production SpatialSession enum here, then the checks.
