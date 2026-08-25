import CoreGraphics
import Foundation
import Network

/// Raw TCP transport for the RP_* stream.
///
/// A plain socket is sufficient and correct: the server speaks no HTTP or
/// WebSocket, so websockify is only a browser shim (PROTOCOL.md §1.3). All
/// protocol work happens on one serial queue; snapshots are handed to the main
/// queue for display.
public final class RemoteConnection {
    public enum State: Equatable {
        case idle
        case connecting
        case handshaking
        case live
        case failed(String)
        case disconnected
    }

    private let queue = DispatchQueue(label: "HaikuRemote.session")
    private var connection: NWConnection?
    private let framer = MessageFramer()
    private var renderer: SessionRenderer?

    public private(set) var state: State = .idle
    public var onStateChange: ((State) -> Void)?
    public var onLog: ((String) -> Void)?
    /// Delivers a fresh composited frame on the main queue.
    public var onFrame: ((CGImage) -> Void)?
    public var onCursorChange: ((CGImage?, CGPoint, CGPoint, Bool) -> Void)?

    public private(set) var requestedWidth = 1280
    public private(set) var requestedHeight = 800

    private var dirty = false
    private var publishScheduled = false
    /// Cap redraw publishing; the wire can deliver far more updates than a
    /// display can show, and every snapshot costs a full buffer copy.
    private let publishInterval: TimeInterval = 1.0 / 60.0

    public init() {}

    public var stats: SessionRenderer.Stats? {
        queue.sync { renderer?.stats }
    }

    // MARK: - Lifecycle

    public func connect(host: String, port: UInt16, width: Int, height: Int) {
        queue.async {
            self.requestedWidth = width
            self.requestedHeight = height
            self.teardownLocked(notify: false)

            guard let canvas = Canvas(width: width, height: height) else {
                self.setState(.failed("could not allocate \(width)x\(height) canvas"))
                return
            }
            let r = SessionRenderer(canvas: canvas)
            r.delegate = self
            self.renderer = r
            self.framer.reset()

            let nwHost = NWEndpoint.Host(host)
            guard let nwPort = NWEndpoint.Port(rawValue: port) else {
                self.setState(.failed("bad port \(port)"))
                return
            }
            let params = NWParameters.tcp
            if let tcp = params.defaultProtocolStack.internetProtocol
                as? NWProtocolTCP.Options {
                // Input latency matters far more than packet efficiency here:
                // every keystroke and every blocking string-width reply is tiny.
                tcp.noDelay = true
            }
            let c = NWConnection(host: nwHost, port: nwPort, using: params)
            self.connection = c

            c.stateUpdateHandler = { [weak self] st in
                guard let self else { return }
                switch st {
                case .ready:
                    self.onLog?("TCP connected to \(host):\(port)")
                    self.setState(.handshaking)
                    self.sendLocked(ClientMessage.initConnection())
                    self.receiveLoop()
                case .failed(let e):
                    self.setState(.failed(e.localizedDescription))
                case .cancelled:
                    self.setState(.disconnected)
                case .waiting(let e):
                    self.onLog?("waiting: \(e.localizedDescription)")
                default:
                    break
                }
            }
            self.setState(.connecting)
            c.start(queue: self.queue)
        }
    }

    public func disconnect() {
        queue.async { self.teardownLocked(notify: true) }
    }

    private func teardownLocked(notify: Bool) {
        if let c = connection {
            c.stateUpdateHandler = nil
            c.cancel()
        }
        connection = nil
        renderer = nil
        if notify { setState(.disconnected) }
    }

    private func setState(_ s: State) {
        guard state != s else { return }
        state = s
        DispatchQueue.main.async { [weak self] in self?.onStateChange?(s) }
    }

    // MARK: - IO

    private func receiveLoop() {
        connection?.receive(minimumIncompleteLength: 1,
                            maximumLength: 1 << 18) { [weak self] data, _, done, err in
            guard let self else { return }
            if let err {
                self.setState(.failed(err.localizedDescription))
                return
            }
            if let data, !data.isEmpty {
                self.ingest([UInt8](data))
            }
            if done {
                self.onLog?("server closed the stream")
                self.setState(.disconnected)
                return
            }
            self.receiveLoop()
        }
    }

    private func ingest(_ bytes: [UInt8]) {
        guard let r = renderer else { return }
        let frames: [MessageFramer.Frame]
        do {
            frames = try framer.feed(bytes)
        } catch {
            // A bad length means the stream is unusable; there is no resync point.
            onLog?("stream framing error: \(error) — disconnecting")
            teardownLocked(notify: true)
            return
        }
        for f in frames {
            r.handle(code: f.code, payload: f.payload)
            // The handshake ack is our cue to advertise a display mode, which is
            // what actually starts the session (PROTOCOL.md §4.1).
            if f.code == RP.initConnection {
                // Ask for the palette BEFORE advertising a display mode. The
                // mode change is what makes the server start drawing, and any
                // B_CMAP8 bitmap that arrives before the palette does cannot be
                // decoded. The reference client sends these the other way round
                // and is only saved by the browser being slow to start.
                // This narrows the window but does not close it: the protocol
                // gives no ordering guarantee, so decoding still warns if a
                // paletted bitmap beats the palette.
                sendLocked(ClientMessage.getSystemPalette())
                sendLocked(ClientMessage.updateDisplayMode(
                    width: requestedWidth, height: requestedHeight))
                setState(.live)
            }
            if f.code == RP.closeConnection {
                teardownLocked(notify: true)
                return
            }
        }
        schedulePublish()
    }

    /// Called from any queue. Serialises onto the session queue.
    public func send(_ data: Data) {
        queue.async { self.sendLocked(data) }
    }

    private func sendLocked(_ data: Data) {
        guard let c = connection else { return }
        c.send(content: data, completion: .contentProcessed { [weak self] err in
            if let err { self?.onLog?("send failed: \(err.localizedDescription)") }
        })
    }

    // MARK: - Frame publishing

    private func schedulePublish() {
        dirty = true
        guard !publishScheduled else { return }
        publishScheduled = true
        queue.asyncAfter(deadline: .now() + publishInterval) { [weak self] in
            guard let self else { return }
            self.publishScheduled = false
            guard self.dirty, let img = self.renderer?.canvas.snapshot() else {
                return
            }
            self.dirty = false
            DispatchQueue.main.async { self.onFrame?(img) }
        }
    }

    /// Changes resolution on the live connection, with no reconnect.
    ///
    /// `RP_UPDATE_DISPLAY_MODE` is not just a handshake step — the server accepts
    /// it at any time and answers with `_NotifyScreenChanged()` and a full
    /// repaint. The canvas has to be replaced first, because the repaint starts
    /// arriving immediately and would otherwise be clipped to the old size.
    public func changeDisplayMode(width: Int, height: Int) {
        queue.async {
            guard self.connection != nil else { return }
            guard width != self.requestedWidth || height != self.requestedHeight
            else { return }
            guard let canvas = Canvas(width: width, height: height) else {
                self.onLog?("cannot resize to \(width)x\(height)")
                return
            }
            self.requestedWidth = width
            self.requestedHeight = height
            self.renderer?.resize(to: canvas)
            self.onLog?("requesting \(width)x\(height)")
            self.sendLocked(ClientMessage.updateDisplayMode(width: width,
                                                            height: height))
            self.schedulePublish()
        }
    }

    /// Re-advertising the display mode is the only way to force a full repaint,
    /// since the server holds no pixels (PROTOCOL.md §9.2).
    public func requestFullRepaint() {
        queue.async {
            self.renderer?.resetForReconnect()
            self.sendLocked(ClientMessage.updateDisplayMode(
                width: self.requestedWidth, height: self.requestedHeight))
        }
    }
}

extension RemoteConnection: SessionRendererDelegate {
    public func rendererWantsToSend(_ data: Data) {
        // Already on the session queue when called from the renderer.
        sendLocked(data)
    }

    public func rendererDidUpdate(rect: CGRect?) {
        dirty = true
    }

    public func rendererCursorChanged() {
        guard let r = renderer else { return }
        let img = r.cursorImage
        let hot = r.cursorHotspot
        let pos = r.cursorPosition
        let vis = r.cursorVisible
        DispatchQueue.main.async { [weak self] in
            self?.onCursorChange?(img, hot, pos, vis)
        }
    }

    public func rendererLog(_ message: String) {
        onLog?(message)
    }
}
