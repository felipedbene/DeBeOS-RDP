import Foundation

/// Manages the `ssh -L` child process.
///
/// Orphan avoidance is the whole point of this class: the tool runs unattended on
/// flaky networks, so a tunnel that outlives the app would hold the local port
/// and make the next connect fail in a confusing way. Three defences:
///
/// 1. `terminate()` on quit, with SIGTERM then SIGKILL escalation.
/// 2. The child PID is persisted, so a *later* launch can reap a tunnel left
///    behind by a crash or SIGKILL (which no in-process handler can catch).
/// 3. Before reaping, the recorded PID is verified to actually be our ssh
///    command, so we never signal an unrelated process that reused the PID.
public final class SSHTunnel {
    public struct Config: Equatable {
        public var host: String
        public var user: String
        public var identityFile: String?
        public var localPort: Int
        public var remoteHost: String
        public var remotePort: Int
        public var sshPort: Int

        public init(host: String, user: String, identityFile: String?,
                    localPort: Int, remoteHost: String = "127.0.0.1",
                    remotePort: Int, sshPort: Int = 22) {
            self.host = host
            self.user = user
            self.identityFile = identityFile
            self.localPort = localPort
            self.remoteHost = remoteHost
            self.remotePort = remotePort
            self.sshPort = sshPort
        }

        public var forwardSpec: String {
            "\(localPort):\(remoteHost):\(remotePort)"
        }

        public var arguments: [String] {
            var a = [
                "-N",                                    // no remote command
                "-T",                                    // no pty
                "-L", forwardSpec,
                "-p", String(sshPort),
                // Fail loudly if the forward cannot be established, instead of
                // sitting there looking connected with a dead port.
                "-o", "ExitOnForwardFailure=yes",
                // Detect a dead link reasonably fast on hotel wifi, and let ssh
                // exit so we can notice and restart rather than hanging.
                "-o", "ServerAliveInterval=15",
                "-o", "ServerAliveCountMax=3",
                "-o", "ConnectTimeout=15",
                "-o", "StrictHostKeyChecking=accept-new",
                "-o", "BatchMode=yes",
                // Never share or become a master: a shared connection can
                // outlive this process, which is exactly what we are avoiding.
                "-o", "ControlMaster=no",
                "-o", "ControlPath=none",
            ]
            if let id = identityFile, !id.isEmpty {
                a += ["-i", (id as NSString).expandingTildeInPath,
                      "-o", "IdentitiesOnly=yes"]
            }
            a.append("\(user)@\(host)")
            return a
        }
    }

    public enum State: Equatable {
        case idle
        case starting
        case running
        case failed(String)
        case stopped
    }

    private let pidKey = "HaikuRemote.tunnelPID"
    private let specKey = "HaikuRemote.tunnelSpec"

    private var process: Process?
    private var stderrPipe: Pipe?
    private let queue = DispatchQueue(label: "HaikuRemote.tunnel")

    public private(set) var state: State = .idle
    public var onStateChange: ((State) -> Void)?
    public var onLog: ((String) -> Void)?

    public init() {}

    /// Kills a tunnel left behind by a previous run. Safe to call at launch.
    public func reapOrphanFromPreviousRun() {
        let defaults = UserDefaults.standard
        guard let pid = defaults.object(forKey: pidKey) as? Int, pid > 1,
              let spec = defaults.string(forKey: specKey) else { return }
        defer {
            defaults.removeObject(forKey: pidKey)
            defaults.removeObject(forKey: specKey)
        }
        // Confirm the PID is still our ssh forward before signalling it: PIDs get
        // recycled, and killing a stranger would be a real bug.
        guard let cmd = Self.commandLine(forPID: pid) else { return }
        guard cmd.contains("ssh"), cmd.contains(spec) else {
            onLog?("recorded PID \(pid) is not our tunnel; leaving it alone")
            return
        }
        onLog?("reaping orphaned tunnel from a previous run (pid \(pid))")
        kill(pid_t(pid), SIGTERM)
        // Give it a moment, then make sure.
        DispatchQueue.global().asyncAfter(deadline: .now() + 1.0) {
            if Self.commandLine(forPID: pid)?.contains(spec) == true {
                kill(pid_t(pid), SIGKILL)
            }
        }
    }

    static func commandLine(forPID pid: Int) -> String? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/ps")
        p.arguments = ["-p", String(pid), "-o", "command="]
        let out = Pipe()
        p.standardOutput = out
        p.standardError = Pipe()
        do { try p.run() } catch { return nil }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        guard p.terminationStatus == 0 else { return nil }
        let s = String(decoding: data, as: UTF8.self)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return s.isEmpty ? nil : s
    }

    public func start(_ config: Config) {
        stop()
        setState(.starting)

        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/ssh")
        p.arguments = config.arguments
        let err = Pipe()
        p.standardError = err
        p.standardOutput = Pipe()
        // ssh with BatchMode should never prompt, but give it a closed stdin so
        // it can never block waiting on one.
        p.standardInput = FileHandle.nullDevice
        stderrPipe = err

        err.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            guard !d.isEmpty else { return }
            let s = String(decoding: d, as: UTF8.self)
                .trimmingCharacters(in: .whitespacesAndNewlines)
            if !s.isEmpty { self?.onLog?("ssh: \(s)") }
        }

        p.terminationHandler = { [weak self] proc in
            guard let self else { return }
            self.stderrPipe?.fileHandleForReading.readabilityHandler = nil
            UserDefaults.standard.removeObject(forKey: self.pidKey)
            UserDefaults.standard.removeObject(forKey: self.specKey)
            if proc.terminationReason == .uncaughtSignal {
                self.setState(.stopped)
            } else if proc.terminationStatus != 0 {
                self.setState(.failed("ssh exited \(proc.terminationStatus)"))
            } else {
                self.setState(.stopped)
            }
        }

        do {
            try p.run()
        } catch {
            setState(.failed("could not launch ssh: \(error.localizedDescription)"))
            return
        }
        process = p
        UserDefaults.standard.set(Int(p.processIdentifier), forKey: pidKey)
        UserDefaults.standard.set(config.forwardSpec, forKey: specKey)
        onLog?("tunnel: ssh \(config.arguments.joined(separator: " "))")

        // ssh gives no "forward is up" signal, so poll the local port. This is
        // also what catches an auth failure quickly.
        waitForLocalPort(config.localPort, deadline: Date().addingTimeInterval(20))
    }

    private func waitForLocalPort(_ port: Int, deadline: Date) {
        queue.asyncAfter(deadline: .now() + 0.25) { [weak self] in
            guard let self else { return }
            guard let p = self.process, p.isRunning else { return }
            if Self.canConnect(port: port) {
                self.setState(.running)
                return
            }
            if Date() > deadline {
                self.setState(.failed("forward did not open within 20s"))
                self.stop()
                return
            }
            self.waitForLocalPort(port, deadline: deadline)
        }
    }

    static func canConnect(port: Int) -> Bool {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return false }
        defer { close(fd) }
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = UInt16(port).bigEndian
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        var tv = timeval(tv_sec: 1, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv,
                   socklen_t(MemoryLayout<timeval>.size))
        let result = withUnsafePointer(to: &addr) { ptr -> Int32 in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        return result == 0
    }

    public func stop() {
        guard let p = process else { return }
        process = nil
        stderrPipe?.fileHandleForReading.readabilityHandler = nil
        stderrPipe = nil
        guard p.isRunning else { return }
        p.terminate()   // SIGTERM
        let pid = p.processIdentifier
        // Escalate if it ignores SIGTERM, so quitting never leaves a tunnel up.
        DispatchQueue.global().asyncAfter(deadline: .now() + 2.0) {
            if p.isRunning { kill(pid, SIGKILL) }
        }
        UserDefaults.standard.removeObject(forKey: pidKey)
        UserDefaults.standard.removeObject(forKey: specKey)
    }

    private func setState(_ s: State) {
        guard state != s else { return }
        state = s
        DispatchQueue.main.async { [weak self] in self?.onStateChange?(s) }
    }

    public var isRunning: Bool { process?.isRunning ?? false }
}
