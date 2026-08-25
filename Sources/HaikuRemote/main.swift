import AppKit
import CoreGraphics
import Foundation

/// The app. A normal windowed Mac app with a Dock icon and a full menu bar; the
/// menu-bar status item is kept as well, because a glance at the glyph is the
/// quickest way to see whether the link is up.
final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var statusItem: NSStatusItem!
    /// The Connect/Disconnect item appears in both the status menu and the
    /// Session menu, so the title has to be kept in step across all of them.
    private var connectItems: [NSMenuItem] = []
    private var statusLine: NSMenuItem!
    private var repaintItem: NSMenuItem!

    private let tunnel = SSHTunnel()
    private let connection = RemoteConnection()
    private var sessionWindow: NSWindow?
    private var canvasView: CanvasView?
    private var settingsWindow: SettingsWindow?

    private var wantConnected = false
    private var logLines: [String] = []
    var autoConnect = false

    /// Auto-reconnect bookkeeping. `everConnected` decides how stubborn to be:
    /// a link that worked once and dropped is worth retrying forever, but one
    /// that has never come up is far more likely to be a wrong host or a bad key,
    /// so that case gives up rather than hammering a misconfiguration all night.
    private var reconnectAttempt = 0
    private var everConnected = false
    private var pendingReconnect: DispatchWorkItem?
    private let reconnectPolicy = ReconnectPolicy()

    func applicationDidFinishLaunching(_ notification: Notification) {
        Settings.registerDefaults()

        // Refuse to be the second copy. Two agents cannot coexist: they share one
        // UserDefaults key for the tunnel PID, and the newcomer's
        // reapOrphanFromPreviousRun() cannot tell "orphan from a previous run"
        // from "the live tunnel of the instance already running" — both are our
        // ssh with a matching forward spec. So the second one kills the first
        // one's tunnel and steals its local port, and the first is left connected
        // to nothing. Observed exactly that.
        guard Self.acquireInstanceLock() else {
            log("another HaikuRemote is already running — exiting rather than "
                + "killing its tunnel")
            let alert = NSAlert()
            alert.messageText = "HaikuRemote is already running"
            alert.informativeText = """
                Look for the ◌ or ● glyph in the menu bar.

                A second copy would terminate the first one's SSH tunnel and take                 its local port, so this one is exiting.
                """
            alert.alertStyle = .informational
            alert.runModal()
            NSApp.terminate(nil)
            return
        }

        // Kill a tunnel left behind by a crash before we try to bind the same
        // local port, otherwise the forward silently fails.
        tunnel.reapOrphanFromPreviousRun()

        statusItem = NSStatusBar.system.statusItem(
            withLength: NSStatusItem.variableLength)
        statusItem.button?.title = "◌"
        statusItem.button?.toolTip = "Haiku Remote"

        let menu = NSMenu()
        statusLine = NSMenuItem(title: "Idle", action: nil, keyEquivalent: "")
        statusLine.isEnabled = false
        menu.addItem(statusLine)
        menu.addItem(.separator())

        let statusConnect = NSMenuItem(title: "Connect", action: #selector(toggle),
                                       keyEquivalent: "")
        statusConnect.target = self
        menu.addItem(statusConnect)
        connectItems.append(statusConnect)

        let showItem = NSMenuItem(title: "Show Desktop", action: #selector(showWindow),
                                 keyEquivalent: "")
        showItem.target = self
        menu.addItem(showItem)

        repaintItem = NSMenuItem(title: "Force Full Repaint",
                                 action: #selector(forceRepaint), keyEquivalent: "")
        repaintItem.target = self
        menu.addItem(repaintItem)

        menu.addItem(.separator())
        let settings = NSMenuItem(title: "Settings…", action: #selector(openSettings),
                                  keyEquivalent: ",")
        settings.target = self
        menu.addItem(settings)
        let copyLog = NSMenuItem(title: "Copy Log", action: #selector(copyLog),
                                 keyEquivalent: "")
        copyLog.target = self
        menu.addItem(copyLog)
        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Quit", action: #selector(quit),
                              keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
        statusItem.menu = menu

        buildMainMenu()
        wireCallbacks()
        log("ready — set a host in Settings, then Connect")

        if autoConnect {
            connect()
        } else if Settings.host.isEmpty {
            // First run. Previously the app just sat in the menu bar with no
            // window and no Dock icon, which is indistinguishable from failing to
            // launch — the single most confusing thing about it. Show the one
            // window that is useful with no configuration.
            log("no host configured — opening Settings")
            openSettings()
        }
    }

    /// Held for the process lifetime; the lock is released when we exit.
    private static var instanceLockFD: Int32 = -1

    /// `flock` on a file in Caches, rather than checking
    /// `NSWorkspace.runningApplications` for our bundle id: this binary gets run
    /// directly as well as via `open`, and a direct launch does not always
    /// register the way a bundled one does.
    private static func acquireInstanceLock() -> Bool {
        let path = NSHomeDirectory()
            + "/Library/Caches/dev.benfelip.HaikuRemote.lock"
        let fd = open(path, O_CREAT | O_RDWR, 0o644)
        // If the lock file cannot be opened at all, do not stand in the user's
        // way — a missing guard is better than a refusal to start.
        guard fd >= 0 else { return true }
        if flock(fd, LOCK_EX | LOCK_NB) != 0 {
            close(fd)
            return false
        }
        instanceLockFD = fd
        return true
    }

    /// Belt and braces: `stop()` is also called from `quit`, but this fires for
    /// paths that bypass it (Force Quit's SIGTERM, logout).
    func applicationWillTerminate(_ notification: Notification) {
        connection.disconnect()
        tunnel.stop()
    }

    private func wireCallbacks() {
        tunnel.onLog = { [weak self] m in
            DispatchQueue.main.async { self?.log(m) }
        }
        tunnel.onStateChange = { [weak self] st in
            guard let self else { return }
            switch st {
            case .running:
                self.log("tunnel up")
                // Only now is the local port real, so connect after this.
                self.startConnection()
            case .failed(let m):
                self.log("tunnel failed: \(m)")
                if !self.scheduleReconnect(reason: "tunnel failed") {
                    self.setStatus("Tunnel failed", symbol: "◍")
                    self.wantConnected = false
                    self.setConnectTitle("Connect")
                }
            case .stopped:
                self.log("tunnel stopped")
                if self.wantConnected {
                    if !self.scheduleReconnect(reason: "tunnel dropped") {
                        self.setStatus("Tunnel dropped", symbol: "◍")
                    }
                }
            default:
                break
            }
        }

        connection.onLog = { [weak self] m in
            DispatchQueue.main.async { self?.log(m) }
        }
        connection.onStateChange = { [weak self] st in
            guard let self else { return }
            switch st {
            case .connecting: self.setStatus("Connecting…", symbol: "◌")
            case .handshaking: self.setStatus("Handshaking…", symbol: "◌")
            case .live:
                self.reconnectAttempt = 0
                self.everConnected = true
                self.setStatus("Connected", symbol: "●")
                self.showWindow()
            case .failed(let m):
                self.log("connection failed: \(m)")
                if !self.scheduleReconnect(reason: "connection failed") {
                    self.setStatus("Failed: \(m)", symbol: "◍")
                }
            case .disconnected:
                // Only a drop we did not ask for is worth retrying; an explicit
                // Disconnect clears wantConnected before this arrives.
                if !self.scheduleReconnect(reason: "link dropped") {
                    self.setStatus("Disconnected", symbol: "◌")
                }
            case .idle:
                self.setStatus("Idle", symbol: "◌")
            }
        }
        connection.onFrame = { [weak self] img in
            self?.canvasView?.setFrameImage(img)
        }
        connection.onCursorChange = { [weak self] img, hot, pos, vis in
            self?.canvasView?.setCursor(img, hotspot: hot, position: pos,
                                        visible: vis)
        }
    }

    /// Builds the application menu bar.
    ///
    /// Not optional polish: the standard Edit menu is what gives text fields
    /// clipboard support. Without it `NSApp.mainMenu` is nil, no key equivalents
    /// are registered, and Cmd-C/Cmd-V simply do nothing anywhere in the app —
    /// including in the Settings fields where you have to paste a host and a key
    /// path. The items use the standard selectors with a nil target so AppKit
    /// routes them to whatever the first responder is, and auto-enables them.
    private func buildMainMenu() {
        let appName = "Haiku Remote"
        let main = NSMenu()

        // -- application menu ------------------------------------------------
        let appItem = NSMenuItem()
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About \(appName)",
                        action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
                        keyEquivalent: "")
        appMenu.addItem(.separator())
        let settingsItem = appMenu.addItem(withTitle: "Settings…",
                                           action: #selector(openSettings),
                                           keyEquivalent: ",")
        settingsItem.target = self
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide \(appName)",
                        action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        let hideOthers = appMenu.addItem(withTitle: "Hide Others",
                                         action: #selector(NSApplication.hideOtherApplications(_:)),
                                         keyEquivalent: "h")
        hideOthers.keyEquivalentModifierMask = [.command, .option]
        appMenu.addItem(withTitle: "Show All",
                        action: #selector(NSApplication.unhideAllApplications(_:)),
                        keyEquivalent: "")
        appMenu.addItem(.separator())
        // Our own quit, not NSApplication.terminate, so the ssh tunnel is torn
        // down rather than orphaned.
        let quitItem = appMenu.addItem(withTitle: "Quit \(appName)",
                                       action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        appItem.submenu = appMenu
        main.addItem(appItem)

        // -- session ---------------------------------------------------------
        let sessionItem = NSMenuItem()
        let session = NSMenu(title: "Session")
        let sessionConnect = session.addItem(withTitle: "Connect",
                                             action: #selector(toggle),
                                             keyEquivalent: "k")
        sessionConnect.target = self
        connectItems.append(sessionConnect)
        let show = session.addItem(withTitle: "Show Desktop",
                                   action: #selector(showWindow), keyEquivalent: "1")
        show.target = self
        let repaint = session.addItem(withTitle: "Force Full Repaint",
                                      action: #selector(forceRepaint),
                                      keyEquivalent: "r")
        repaint.target = self
        session.addItem(.separator())
        let copyLogItem = session.addItem(withTitle: "Copy Log",
                                          action: #selector(copyLog),
                                          keyEquivalent: "")
        copyLogItem.target = self
        session.addItem(.separator())
        session.addItem(withTitle: "Close Window",
                        action: #selector(NSWindow.performClose(_:)),
                        keyEquivalent: "w")
        sessionItem.submenu = session
        main.addItem(sessionItem)

        // -- edit ------------------------------------------------------------
        let editItem = NSMenuItem()
        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Undo", action: Selector(("undo:")),
                     keyEquivalent: "z")
        let redo = edit.addItem(withTitle: "Redo", action: Selector(("redo:")),
                                keyEquivalent: "z")
        redo.keyEquivalentModifierMask = [.command, .shift]
        edit.addItem(.separator())
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)),
                     keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)),
                     keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)),
                     keyEquivalent: "v")
        edit.addItem(withTitle: "Delete", action: #selector(NSText.delete(_:)),
                     keyEquivalent: "")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)),
                     keyEquivalent: "a")
        editItem.submenu = edit
        main.addItem(editItem)

        // -- window ----------------------------------------------------------
        let windowItem = NSMenuItem()
        let window = NSMenu(title: "Window")
        window.addItem(withTitle: "Minimize",
                       action: #selector(NSWindow.performMiniaturize(_:)),
                       keyEquivalent: "m")
        window.addItem(withTitle: "Zoom",
                       action: #selector(NSWindow.performZoom(_:)), keyEquivalent: "")
        window.addItem(.separator())
        window.addItem(withTitle: "Bring All to Front",
                       action: #selector(NSApplication.arrangeInFront(_:)),
                       keyEquivalent: "")
        windowItem.submenu = window
        main.addItem(windowItem)

        NSApp.mainMenu = main
        NSApp.windowsMenu = window
    }

    // MARK: - Actions

    @objc private func toggle() {
        if wantConnected {
            disconnect()
        } else {
            connect()
        }
    }

    private func connect() {
        // A host is needed either way now: in direct mode it is what we dial, and
        // in tunnel mode it is what ssh dials. Point it at 127.0.0.1 in direct
        // mode to talk to the mock server or a forward you opened yourself.
        guard !Settings.host.isEmpty else {
            log("no host set — open Settings first")
            openSettings()
            return
        }
        wantConnected = true
        reconnectAttempt = 0
        everConnected = false
        setConnectTitle("Disconnect")
        if Settings.useTunnel {
            setStatus("Starting tunnel…", symbol: "◌")
            tunnel.start(Settings.tunnelConfig)
        } else {
            startConnection()
        }
    }

    /// Dials whichever endpoint the current mode implies.
    ///
    /// - **Tunnel**: `ssh -L localPort:127.0.0.1:remotePort` is already up, so the
    ///   thing to connect to is the *local* end of that forward.
    /// - **Direct**: connect straight to the Haiku host's own port, with no ssh
    ///   in the path at all.
    ///
    /// Direct mode needs `app_server` to be listening on an interface we can
    /// reach. On the EC2 images it is **not**: `RemoteHWInterface` binds
    /// 127.0.0.1 deliberately, because the protocol has no authentication, so
    /// those hosts require tunnel mode. Direct mode is for a Haiku you control
    /// (a VM or a LAN box with the bind changed), or for `host = 127.0.0.1` when
    /// pointing at the mock server or a forward you opened yourself.
    private func startConnection() {
        let host = Settings.useTunnel ? "127.0.0.1" : Settings.host
        let port = Settings.useTunnel ? Settings.localPort : Settings.remotePort
        log("\(Settings.useTunnel ? "tunnel" : "direct") mode: connecting to "
            + "\(host):\(port)")
        connection.connect(host: host, port: UInt16(port),
                          width: Settings.width, height: Settings.height)
    }

    private func disconnect() {
        // Clear the intent first: the teardown below produces a .disconnected
        // callback, and wantConnected is what stops that being read as a drop
        // and retried.
        wantConnected = false
        cancelReconnect()
        setConnectTitle("Connect")
        connection.disconnect()
        tunnel.stop()
        setStatus("Idle", symbol: "◌")
    }

    // MARK: - Auto-reconnect

    /// Queues a retry. Returns false if no retry will happen, so the caller can
    /// show its own terminal status instead.
    @discardableResult
    private func scheduleReconnect(reason: String) -> Bool {
        guard wantConnected, Settings.autoReconnect else { return false }
        // Don't stack retries: several callbacks can report the same drop, since
        // the tunnel dying takes the connection down with it and both report.
        guard pendingReconnect == nil else { return true }

        guard reconnectPolicy.shouldRetry(attempt: reconnectAttempt + 1,
                                          everConnected: everConnected) else {
            log("giving up after \(reconnectAttempt) attempts without ever "
                + "connecting — check the host, key and port in Settings")
            setStatus("Failed: never connected", symbol: "◍")
            wantConnected = false
            setConnectTitle("Connect")
            return true
        }

        reconnectAttempt += 1
        let delay = reconnectPolicy.delay(attempt: reconnectAttempt)
        log("\(reason) — retrying in \(String(format: "%.0f", delay))s "
            + "(attempt \(reconnectAttempt))")
        setStatus("Reconnecting in \(Int(delay))s…", symbol: "◍")

        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.pendingReconnect = nil
            guard self.wantConnected else { return }
            self.log("reconnecting…")
            if Settings.useTunnel {
                // Always tear the old forward down first: if the ssh process is
                // still alive the new one cannot bind the same local port, and
                // the failure is silent.
                self.tunnel.stop()
                self.setStatus("Starting tunnel…", symbol: "◌")
                self.tunnel.start(Settings.tunnelConfig)
            } else {
                self.startConnection()
            }
        }
        pendingReconnect = work
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
        return true
    }

    private func cancelReconnect() {
        pendingReconnect?.cancel()
        pendingReconnect = nil
        reconnectAttempt = 0
    }


    @objc private func forceRepaint() {
        // Re-advertising the display mode is the only way to get a full repaint;
        // the server holds no pixels to resend (PROTOCOL.md §9.2).
        connection.requestFullRepaint()
        log("requested full repaint")
    }

    @objc private func showWindow() {
        if sessionWindow == nil {
            let size = NSSize(width: Settings.width, height: Settings.height)
            let w = NSWindow(contentRect: NSRect(origin: .zero, size: size),
                            styleMask: [.titled, .closable, .miniaturizable,
                                        .resizable],
                            backing: .buffered, defer: false)
            w.title = "Haiku"
            // Required, and the default is the wrong way round for ARC: a window
            // built with init(contentRect:) has isReleasedWhenClosed == true, so
            // AppKit releases it on close while ARC also releases `sessionWindow`.
            // The resulting over-release does not fault at the close — it faults
            // later, inside a CoreAnimation transaction commit while the window's
            // own close animation is being torn down, which reads as a random
            // SIGSEGV in objc_release with none of our code on the stack.
            w.isReleasedWhenClosed = false
            w.delegate = self
            w.contentMinSize = NSSize(width: 320, height: 240)
            w.center()
            let v = CanvasView(frame: NSRect(origin: .zero, size: size))
            v.input.mapping = Settings.commandMapping
            v.onSend = { [weak self] data in self?.connection.send(data) }
            w.contentView = v
            w.initialFirstResponder = v
            canvasView = v
            sessionWindow = w
        }
        canvasView?.input.mapping = Settings.commandMapping
        sessionWindow?.makeKeyAndOrderFront(nil)
        // A menu-bar agent is not "active" by default, so keyboard focus needs
        // an explicit nudge or keystrokes go nowhere.
        NSApp.activate(ignoringOtherApps: true)
        if let v = canvasView { sessionWindow?.makeFirstResponder(v) }
    }

    func windowWillClose(_ notification: Notification) {
        sessionWindow = nil
        canvasView = nil
    }

    /// Follow the window size by re-advertising the display mode.
    ///
    /// Deliberately at the *end* of a live resize rather than continuously: each
    /// change costs a full repaint of the whole desktop, and the server holds no
    /// framebuffer to help, so sending one per drag frame would flood the link.
    func windowDidEndLiveResize(_ notification: Notification) {
        guard Settings.followWindowSize, wantConnected,
              let view = canvasView else { return }
        let width = Int(view.bounds.width.rounded())
        let height = Int(view.bounds.height.rounded())
        guard width >= 320, height >= 240 else { return }
        connection.changeDisplayMode(width: width, height: height)
        // Remember it, so the next session opens at the size actually in use.
        Settings.width = width
        Settings.height = height
    }

    @objc private func openSettings() {
        if settingsWindow == nil { settingsWindow = SettingsWindow() }
        settingsWindow?.showWindow(nil)
        settingsWindow?.window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    @objc private func copyLog() {
        let text = logLines.joined(separator: "\n")
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    @objc private func quit() {
        connection.disconnect()
        tunnel.stop()
        NSApp.terminate(nil)
    }

    // MARK: - Status

    private func setConnectTitle(_ title: String) {
        for item in connectItems { item.title = title }
    }

    private func setStatus(_ text: String, symbol: String) {
        statusLine.title = text
        statusItem.button?.title = symbol
    }

    private func log(_ message: String) {
        let stamp = ISO8601DateFormatter().string(from: Date())
        let line = "\(stamp) \(message)"
        logLines.append(line)
        if logLines.count > 2000 { logLines.removeFirst(500) }
        FileHandle.standardError.write(Data((line + "\n").utf8))
    }
}

// MARK: - Entry point

Settings.registerDefaults()

let args = CommandLine.arguments

func flagValue(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    return args[i + 1]
}

if args.contains("--help") || args.contains("-h") {
    print("""
    HaikuRemote — native macOS client for Haiku's app_server remote protocol.

      (no arguments)          run as a menu-bar agent
      --autoconnect           connect immediately on launch
      --capture <file.png>    headless: connect, render, write a PNG, exit
      --input-test            headless: send a scripted burst of input events
      --drive                 headless: click/type on a LIVE desktop and verify
                              each input actually repainted the screen
      --out-dir <dir>         where --drive writes its frames (default /tmp)
      --resize-test           headless: change resolution on a live connection
      --to-width <n> --to-height <n>  target size for --resize-test
      --seconds <n>           how long to collect before capturing (default 3)
      --host <h>              override host (default 127.0.0.1 for capture)
      --port <n>              local port (default \(Settings.localPort))
      --width <n> --height <n>  display mode to request

    Point --port at an ssh -L forward, or at tools/rp_mock_server.py.
    """)
    exit(0)
}

if args.contains("--tunnel-test") {
    TunnelTest.run()
}

if args.contains("--input-test") {
    let port = UInt16(flagValue("--port") ?? "") ?? UInt16(Settings.localPort)
    let width = Int(flagValue("--width") ?? "") ?? Settings.width
    let height = Int(flagValue("--height") ?? "") ?? Settings.height
    let seconds = Double(flagValue("--seconds") ?? "") ?? 1.0
    let host = flagValue("--host") ?? "127.0.0.1"
    InputTest.run(host: host, port: port, width: width, height: height,
                  seconds: seconds)
}

if args.contains("--drive") {
    let port = UInt16(flagValue("--port") ?? "") ?? UInt16(Settings.localPort)
    let width = Int(flagValue("--width") ?? "") ?? Settings.width
    let height = Int(flagValue("--height") ?? "") ?? Settings.height
    let host = flagValue("--host") ?? "127.0.0.1"
    let outDir = flagValue("--out-dir") ?? "/tmp"
    Drive.run(host: host, port: port, width: width, height: height,
              outDir: outDir)
}

if args.contains("--resize-test") {
    let port = UInt16(flagValue("--port") ?? "") ?? UInt16(Settings.localPort)
    let width = Int(flagValue("--width") ?? "") ?? 800
    let height = Int(flagValue("--height") ?? "") ?? 600
    let toWidth = Int(flagValue("--to-width") ?? "") ?? (width + 224)
    let toHeight = Int(flagValue("--to-height") ?? "") ?? (height + 168)
    let seconds = Double(flagValue("--seconds") ?? "") ?? 2.0
    let host = flagValue("--host") ?? "127.0.0.1"
    ResizeTest.run(host: host, port: port, width: width, height: height,
                   toWidth: toWidth, toHeight: toHeight, seconds: seconds,
                   output: flagValue("--capture"))
}

if let out = flagValue("--capture") {
    let port = UInt16(flagValue("--port") ?? "") ?? UInt16(Settings.localPort)
    let width = Int(flagValue("--width") ?? "") ?? Settings.width
    let height = Int(flagValue("--height") ?? "") ?? Settings.height
    let seconds = Double(flagValue("--seconds") ?? "") ?? 3.0
    let host = flagValue("--host") ?? "127.0.0.1"
    Capture.run(host: host, port: port, width: width, height: height,
                seconds: seconds, output: out)
}

let app = NSApplication.shared
let delegate = AppDelegate()
delegate.autoConnect = args.contains("--autoconnect")
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
