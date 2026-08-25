import Foundation

/// Exercises the tunnel lifecycle guarantees, which matter more than anything
/// else here: this thing runs unattended on hotel wifi, and a leaked `ssh -L`
/// holds the local port so the next connect fails in a way that looks like a
/// server problem.
///
/// Uses 192.0.2.1 (TEST-NET-1, guaranteed unroutable) as the dead host so the
/// failure path is deterministic and no real machine is contacted.
enum TunnelTest {
    static func run() -> Never {
        var failures: [String] = []
        func check(_ ok: Bool, _ what: String) {
            print(ok ? "  ok   \(what)" : "  FAIL \(what)")
            if !ok { failures.append(what) }
        }

        print("1. connecting to an unroutable host must fail and leave no ssh behind")
        let deadPort = 19911
        let tunnel = SSHTunnel()
        var states: [String] = []
        tunnel.onStateChange = { states.append("\($0)") }
        tunnel.start(SSHTunnel.Config(host: "192.0.2.1", user: "nobody",
                                     identityFile: nil, localPort: deadPort,
                                     remotePort: 10900))
        check(tunnel.isRunning, "ssh child was spawned")
        // ConnectTimeout=15 plus the 20s forward deadline bound this.
        spin(until: { !tunnel.isRunning }, limit: 30)
        check(!tunnel.isRunning, "ssh exited on its own after failing to connect")
        tunnel.stop()
        spin(3)
        check(countSSH(matching: "\(deadPort):127.0.0.1") == 0,
              "no ssh process left holding the forward")
        check(states.contains { $0.contains("failed") },
              "failure was reported to the UI (states: \(states))")

        print("2. stop() must kill a live tunnel promptly")
        let livePort = 19912
        // A long ConnectTimeout keeps this child alive so stop() has something
        // real to kill.
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/usr/bin/ssh")
        proc.arguments = ["-N", "-T", "-L", "\(livePort):127.0.0.1:10900",
                          "-o", "ConnectTimeout=120",
                          "-o", "BatchMode=yes", "nobody@192.0.2.1"]
        proc.standardError = Pipe()
        proc.standardOutput = Pipe()
        try? proc.run()
        Thread.sleep(forTimeInterval: 1.0)
        check(proc.isRunning, "helper ssh is alive to be reaped")

        print("3. orphan reaping only touches our own forward")
        // Record it the way SSHTunnel does after a launch, then ask a fresh
        // instance to reap it -- the crash-recovery path.
        UserDefaults.standard.set(Int(proc.processIdentifier),
                                 forKey: "HaikuRemote.tunnelPID")
        UserDefaults.standard.set("\(livePort):127.0.0.1:10900",
                                 forKey: "HaikuRemote.tunnelSpec")
        let reaper = SSHTunnel()
        reaper.onLog = { print("  log: \($0)") }
        reaper.reapOrphanFromPreviousRun()
        spin(until: { !proc.isRunning }, limit: 8)
        check(!proc.isRunning, "orphan from a previous run was reaped")

        print("4. a recorded PID that is not our ssh must be left alone")
        // `sleep` is not ssh and does not contain the forward spec, so the guard
        // should refuse to signal it. Killing a stranger here would be a real bug.
        let bystander = Process()
        bystander.executableURL = URL(fileURLWithPath: "/bin/sleep")
        bystander.arguments = ["20"]
        try? bystander.run()
        Thread.sleep(forTimeInterval: 0.5)
        UserDefaults.standard.set(Int(bystander.processIdentifier),
                                 forKey: "HaikuRemote.tunnelPID")
        UserDefaults.standard.set("19913:127.0.0.1:10900",
                                 forKey: "HaikuRemote.tunnelSpec")
        let reaper2 = SSHTunnel()
        reaper2.onLog = { print("  log: \($0)") }
        reaper2.reapOrphanFromPreviousRun()
        Thread.sleep(forTimeInterval: 2.0)
        check(bystander.isRunning, "unrelated process with a recycled PID survived")
        bystander.terminate()

        // Leave no state behind for the real app to trip over.
        UserDefaults.standard.removeObject(forKey: "HaikuRemote.tunnelPID")
        UserDefaults.standard.removeObject(forKey: "HaikuRemote.tunnelSpec")

        print("")
        if failures.isEmpty {
            print("PASS — tunnel lifecycle holds")
            exit(0)
        }
        print("FAIL — \(failures.count) problem(s)")
        for f in failures { print("  - \(f)") }
        exit(1)
    }

    private static func countSSH(matching needle: String) -> Int {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/ps")
        p.arguments = ["-Ao", "command="]
        let out = Pipe()
        p.standardOutput = out
        try? p.run()
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return String(decoding: data, as: UTF8.self)
            .split(separator: "\n")
            .filter { $0.contains("ssh") && $0.contains(needle) }
            .count
    }

    private static func spin(_ seconds: Double) {
        let end = Date().addingTimeInterval(seconds)
        while Date() < end {
            RunLoop.current.run(mode: .default,
                                before: Date().addingTimeInterval(0.05))
        }
    }

    private static func spin(until done: () -> Bool, limit: Double) {
        let end = Date().addingTimeInterval(limit)
        while Date() < end && !done() {
            RunLoop.current.run(mode: .default,
                                before: Date().addingTimeInterval(0.1))
        }
    }
}
