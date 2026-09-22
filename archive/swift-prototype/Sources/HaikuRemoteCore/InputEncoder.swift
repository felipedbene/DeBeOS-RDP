import Foundation

/// Haiku modifier bits (HaikuRemoteDesktop.js:189-204).
public struct HaikuModifiers: OptionSet {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    public static let shift = HaikuModifiers(rawValue: 0x0000_0001)
    public static let command = HaikuModifiers(rawValue: 0x0000_0002)
    public static let control = HaikuModifiers(rawValue: 0x0000_0004)
    public static let capsLock = HaikuModifiers(rawValue: 0x0000_0008)
    public static let scrollLock = HaikuModifiers(rawValue: 0x0000_0010)
    public static let numLock = HaikuModifiers(rawValue: 0x0000_0020)
    public static let option = HaikuModifiers(rawValue: 0x0000_0040)
    public static let menu = HaikuModifiers(rawValue: 0x0000_0080)
    public static let leftShift = HaikuModifiers(rawValue: 0x0000_0100)
    public static let rightShift = HaikuModifiers(rawValue: 0x0000_0200)
    public static let leftCommand = HaikuModifiers(rawValue: 0x0000_0400)
    public static let rightCommand = HaikuModifiers(rawValue: 0x0000_0800)
    public static let leftControl = HaikuModifiers(rawValue: 0x0000_1000)
    public static let rightControl = HaikuModifiers(rawValue: 0x0000_2000)
    public static let leftOption = HaikuModifiers(rawValue: 0x0000_4000)
    public static let rightOption = HaikuModifiers(rawValue: 0x0000_8000)
}

/// Haiku mouse button bits. These happen to coincide with the DOM's
/// `MouseEvent.buttons` values, which is why the reference client can pass them
/// through unchanged — but `NSEvent` reports buttons separately, so the mask has
/// to be built explicitly (PROTOCOL.md §8).
public struct HaikuButtons: OptionSet {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }
    public static let primary = HaikuButtons(rawValue: 1)
    public static let secondary = HaikuButtons(rawValue: 2)
    public static let tertiary = HaikuButtons(rawValue: 4)
}

/// Which macOS modifier should become Haiku's B_COMMAND_KEY.
///
/// Haiku's COMMAND sits physically where Alt is on a PC keyboard, and the
/// reference client duly maps browser Alt to it. On a Mac keyboard that position
/// is Option, so mapping Option→Command makes shortcuts land under the thumb
/// where a Haiku user expects. Mapping macOS Command instead feels natural to a
/// Mac user but collides with system shortcuts. Configurable because neither
/// choice is obviously right.
public enum CommandKeyMapping: String, CaseIterable {
    /// macOS ⌥ Option → Haiku COMMAND (matches physical PC layout).
    case optionIsCommand
    /// macOS ⌘ Command → Haiku COMMAND (matches Mac muscle memory).
    case commandIsCommand
}

/// Translates macOS input into RP_* messages.
///
/// Keeps the modifier mask itself, because the protocol makes modifiers
/// client-owned state: the client sends the complete new mask on any change and
/// the server just stores it (PROTOCOL.md §8).
public final class InputEncoder {
    public var mapping: CommandKeyMapping = .optionIsCommand
    public private(set) var modifiers: HaikuModifiers = []

    public init() {}

    /// Recomputes the mask from an `NSEvent.modifierFlags` raw value and returns
    /// a message if it changed. Deriving the whole mask from the flags avoids the
    /// drift you get from tracking individual key up/down events, which is a real
    /// problem on macOS where a modifier release can be missed when the app is
    /// not focused.
    public func modifiersChanged(nsFlagsRawValue flags: UInt) -> Data? {
        // NSEvent.ModifierFlags bit positions.
        let shiftMask: UInt = 1 << 17
        let controlMask: UInt = 1 << 18
        let optionMask: UInt = 1 << 19
        let commandMask: UInt = 1 << 20
        let capsLockMask: UInt = 1 << 16
        let functionMask: UInt = 1 << 23

        var m: HaikuModifiers = []
        if flags & shiftMask != 0 { m.insert(.shift); m.insert(.leftShift) }
        if flags & controlMask != 0 { m.insert(.control); m.insert(.leftControl) }
        if flags & capsLockMask != 0 { m.insert(.capsLock) }
        if flags & functionMask != 0 { m.insert(.menu) }

        let optionDown = flags & optionMask != 0
        let commandDown = flags & commandMask != 0
        switch mapping {
        case .optionIsCommand:
            if optionDown { m.insert(.command); m.insert(.leftCommand) }
            if commandDown { m.insert(.option); m.insert(.leftOption) }
        case .commandIsCommand:
            if commandDown { m.insert(.command); m.insert(.leftCommand) }
            if optionDown { m.insert(.option); m.insert(.leftOption) }
        }

        guard m != modifiers else { return nil }
        modifiers = m
        return ClientMessage.modifiersChanged(m.rawValue)
    }

    public func buttons(from pressedMask: Int) -> HaikuButtons {
        var b: HaikuButtons = []
        if pressedMask & 1 != 0 { b.insert(.primary) }
        if pressedMask & 2 != 0 { b.insert(.secondary) }
        if pressedMask & 4 != 0 { b.insert(.tertiary) }
        return b
    }

    public func mouseMoved(x: Float, y: Float) -> Data {
        ClientMessage.mouseMoved(x: x, y: y)
    }

    public func mouseDown(x: Float, y: Float, buttons: HaikuButtons,
                          clickCount: Int) -> Data {
        ClientMessage.mouseDown(x: x, y: y, buttons: buttons.rawValue,
                                clicks: Int32(max(1, clickCount)))
    }

    public func mouseUp(x: Float, y: Float, buttons: HaikuButtons) -> Data {
        ClientMessage.mouseUp(x: x, y: y, buttons: buttons.rawValue)
    }

    /// Haiku's wheel convention matches a "content moves with the wheel" model;
    /// macOS scrolling deltas are inverted relative to it, and natural scrolling
    /// inverts again. `scrollingDeltaY` is already sign-corrected by macOS, so
    /// negate once to get Haiku's direction.
    public func mouseWheel(deltaX: Float, deltaY: Float) -> Data {
        ClientMessage.mouseWheel(dx: -deltaX, dy: -deltaY)
    }

    /// Encodes a key event.
    ///
    /// `text` is the already-composed UTF-8 the keystroke produces, which is what
    /// the `bytes` field carries. `rawChar` and `key` are what Haiku's keymap
    /// layer wants; the reference client always sends rawChar=0 and a browser
    /// keyCode, which is why the browser demo's keyboard handling is imperfect
    /// (PROTOCOL.md §8). Sending a real raw character does better.
    public func key(down: Bool, characters: String,
                    charactersIgnoringModifiers: String,
                    keyCode: UInt16) -> Data {
        let text = characters
        // raw_char is the unmodified character, which is how Haiku identifies the
        // physical key independent of shift/option.
        let rawChar = Int32(charactersIgnoringModifiers.unicodeScalars.first?.value
                            ?? 0)
        let haikuKey = Int32(Self.haikuKeyCode(forMacVirtualKey: keyCode) ?? 0)
        return ClientMessage.key(down: down, text: text, rawChar: rawChar,
                                 key: haikuKey)
    }

    /// Maps macOS virtual key codes to Haiku key codes.
    ///
    /// Haiku key codes are positions in its own keymap table, not ASCII. Only the
    /// keys where the distinction matters are mapped; anything unmapped returns
    /// nil and the server falls back to the `bytes` field, which is enough for
    /// ordinary typing.
    public static func haikuKeyCode(forMacVirtualKey vk: UInt16) -> UInt16? {
        switch vk {
        case 0x35: return 0x01   // Escape
        case 0x7A: return 0x02   // F1
        case 0x78: return 0x03   // F2
        case 0x63: return 0x04   // F3
        case 0x76: return 0x05   // F4
        case 0x60: return 0x06   // F5
        case 0x61: return 0x07   // F6
        case 0x62: return 0x08   // F7
        case 0x64: return 0x09   // F8
        case 0x65: return 0x0A   // F9
        case 0x6D: return 0x0B   // F10
        case 0x67: return 0x0C   // F11
        case 0x6F: return 0x0D   // F12
        case 0x33: return 0x1E   // Backspace
        case 0x30: return 0x26   // Tab
        case 0x24: return 0x47   // Return
        case 0x31: return 0x5E   // Space
        case 0x7E: return 0x57   // Up
        case 0x7D: return 0x62   // Down
        case 0x7B: return 0x61   // Left
        case 0x7C: return 0x63   // Right
        case 0x73: return 0x20   // Home
        case 0x77: return 0x35   // End
        case 0x74: return 0x21   // Page Up
        case 0x79: return 0x36   // Page Down
        case 0x75: return 0x34   // Delete (forward)
        case 0x72: return 0x1F   // Insert / Help
        default: return nil
        }
    }
}
