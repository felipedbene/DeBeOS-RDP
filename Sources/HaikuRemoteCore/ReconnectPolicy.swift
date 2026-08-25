import Foundation

/// When and how often to retry a dropped link.
///
/// Kept out of the AppKit layer so it can be tested on its own — the delays are
/// the kind of thing that is easy to get subtly wrong (an unbounded doubling, or
/// a retry loop that never gives up on a typo'd hostname) and hard to notice by
/// hand, because you would have to sit and watch it for an hour.
public struct ReconnectPolicy: Equatable {
    /// Ceiling on the backoff. A laptop shut in a bag for an hour should come
    /// back within half a minute of being opened, not after a delay that kept
    /// doubling while it slept.
    public var maxDelay: TimeInterval = 30
    /// First retry delay; each subsequent attempt doubles it up to `maxDelay`.
    public var baseDelay: TimeInterval = 1
    /// How many times to try before concluding a link that has *never* worked is
    /// misconfigured rather than merely down. A link that has connected once is
    /// retried indefinitely instead.
    public var coldStartAttemptLimit = 5

    public init() {}

    /// `attempt` is 1-based.
    public func delay(attempt: Int) -> TimeInterval {
        let n = max(1, attempt)
        // Cap the exponent before computing the power: with a long enough outage
        // this is called with large attempt numbers, and pow() would overflow to
        // infinity long before min() got a chance to clamp it.
        let exponent = min(n - 1, 16)
        return min(maxDelay, baseDelay * pow(2.0, Double(exponent)))
    }

    /// Whether an attempt numbered `attempt` should be made at all.
    public func shouldRetry(attempt: Int, everConnected: Bool) -> Bool {
        if everConnected { return true }
        return attempt <= coldStartAttemptLimit
    }
}
