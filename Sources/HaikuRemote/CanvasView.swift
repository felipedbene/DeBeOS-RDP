import AppKit

/// Displays the session canvas and forwards input.
///
/// `isFlipped` is true so view coordinates match Haiku's top-left origin with y
/// increasing downward, which means mouse positions can go on the wire without
/// conversion.
final class CanvasView: NSView {
    var onSend: ((Data) -> Void)?
    let input = InputEncoder()

    private var frameImage: CGImage?
    private var cursorImage: CGImage?
    private var cursorHotspot = CGPoint.zero
    private var cursorPosition = CGPoint.zero
    private var cursorVisible = true
    private var trackingArea: NSTrackingArea?
    /// Buttons currently held, so RP_MOUSE_UP can report the remaining mask.
    private var heldButtons: Int = 0

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = CGColor(gray: 0, alpha: 1)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    func setFrameImage(_ image: CGImage) {
        frameImage = image
        needsDisplay = true
    }

    func setCursor(_ image: CGImage?, hotspot: CGPoint, position: CGPoint,
                   visible: Bool) {
        cursorImage = image
        cursorHotspot = hotspot
        cursorPosition = position
        cursorVisible = visible
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.setFillColor(CGColor(gray: 0, alpha: 1))
        ctx.fill(dirtyRect)

        guard let img = frameImage else { return }
        // The view is flipped but CGContext.draw expects y-up, so flip locally.
        // Without this the whole desktop renders upside down.
        let full = CGRect(x: 0, y: 0, width: CGFloat(img.width),
                          height: CGFloat(img.height))
        ctx.saveGState()
        ctx.interpolationQuality = .none
        // Flip about the *image* height, not the view's. The window is resizable
        // and the canvas only catches up when the resize ends, so during a drag
        // the two differ — flipping about the view would pin the desktop to the
        // bottom edge and slide it around as the window grows.
        ctx.translateBy(x: 0, y: CGFloat(img.height))
        ctx.scaleBy(x: 1, y: -1)
        ctx.draw(img, in: full)

        // The cursor is composited here rather than painted into the canvas:
        // nothing ever repaints what a cursor covered, so drawing it into the
        // framebuffer would leave a trail behind every movement.
        if cursorVisible, let c = cursorImage {
            let x = cursorPosition.x - cursorHotspot.x
            let y = cursorPosition.y - cursorHotspot.y
            let r = CGRect(x: x, y: CGFloat(img.height) - y - CGFloat(c.height),
                           width: CGFloat(c.width), height: CGFloat(c.height))
            ctx.draw(c, in: r)
        }
        ctx.restoreGState()
    }

    // MARK: - Tracking

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let t = trackingArea { removeTrackingArea(t) }
        let t = NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways,
                      .inVisibleRect],
            owner: self, userInfo: nil)
        addTrackingArea(t)
        trackingArea = t
    }

    private func pos(_ event: NSEvent) -> (Float, Float) {
        let p = convert(event.locationInWindow, from: nil)
        return (Float(p.x), Float(p.y))
    }

    // MARK: - Mouse

    override func mouseMoved(with event: NSEvent) {
        let (x, y) = pos(event)
        onSend?(input.mouseMoved(x: x, y: y))
    }

    override func mouseDragged(with event: NSEvent) { mouseMoved(with: event) }
    override func rightMouseDragged(with event: NSEvent) { mouseMoved(with: event) }
    override func otherMouseDragged(with event: NSEvent) { mouseMoved(with: event) }

    private func down(_ event: NSEvent, bit: Int) {
        window?.makeFirstResponder(self)
        heldButtons |= bit
        let (x, y) = pos(event)
        onSend?(input.mouseDown(x: x, y: y,
                                buttons: input.buttons(from: heldButtons),
                                clickCount: event.clickCount))
    }

    private func up(_ event: NSEvent, bit: Int) {
        heldButtons &= ~bit
        let (x, y) = pos(event)
        onSend?(input.mouseUp(x: x, y: y,
                              buttons: input.buttons(from: heldButtons)))
    }

    override func mouseDown(with event: NSEvent) { down(event, bit: 1) }
    override func mouseUp(with event: NSEvent) { up(event, bit: 1) }
    override func rightMouseDown(with event: NSEvent) { down(event, bit: 2) }
    override func rightMouseUp(with event: NSEvent) { up(event, bit: 2) }
    override func otherMouseDown(with event: NSEvent) { down(event, bit: 4) }
    override func otherMouseUp(with event: NSEvent) { up(event, bit: 4) }

    override func scrollWheel(with event: NSEvent) {
        // Use the line-based deltas so a trackpad's pixel-precise scrolling does
        // not swamp the link with hundreds of tiny wheel messages.
        let dx = Float(event.hasPreciseScrollingDeltas
                       ? event.scrollingDeltaX / 10.0 : event.scrollingDeltaX)
        let dy = Float(event.hasPreciseScrollingDeltas
                       ? event.scrollingDeltaY / 10.0 : event.scrollingDeltaY)
        guard dx != 0 || dy != 0 else { return }
        onSend?(input.mouseWheel(deltaX: dx, deltaY: dy))
    }

    // MARK: - Keyboard

    override func keyDown(with event: NSEvent) {
        // Deliberately not calling super: that would beep on every unhandled key.
        send(key: event, down: true)
    }

    override func keyUp(with event: NSEvent) {
        send(key: event, down: false)
    }

    override func flagsChanged(with event: NSEvent) {
        // Recompute the whole mask from the flags rather than tracking individual
        // modifier presses, which drifts when a release is missed.
        if let msg = input.modifiersChanged(
            nsFlagsRawValue: event.modifierFlags.rawValue) {
            onSend?(msg)
        }
    }

    private func send(key event: NSEvent, down: Bool) {
        let chars = event.characters ?? ""
        let bare = event.charactersIgnoringModifiers ?? chars
        onSend?(input.key(down: down, characters: chars,
                          charactersIgnoringModifiers: bare,
                          keyCode: event.keyCode))
    }

    /// Swallow command-key equivalents so they reach Haiku instead of being eaten
    /// by the menu bar — except Command-Q, which stays a local quit so the app is
    /// always escapable.
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        guard event.modifierFlags.contains(.command) else { return false }
        if event.charactersIgnoringModifiers?.lowercased() == "q" { return false }
        send(key: event, down: true)
        return true
    }
}
