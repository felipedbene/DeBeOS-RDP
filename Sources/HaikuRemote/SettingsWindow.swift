import AppKit

/// Settings sheet.
///
/// The one thing this window has to make unambiguous is the connection mode,
/// because the two modes dial completely different endpoints and a wrong guess
/// looks like a hang:
///
///   Direct   Mac client ──tcp──▶ Haiku host:remotePort
///   Tunnel   Mac client ──tcp──▶ 127.0.0.1:localPort ──ssh -L──▶ Haiku:remotePort
///
/// Which fields matter depends on the mode, so the irrelevant ones are disabled
/// rather than left looking significant.
final class SettingsWindow: NSWindowController {
    private var fields: [String: NSTextField] = [:]
    private var modePopup: NSPopUpButton!
    private var mappingPopup: NSPopUpButton!
    private var autoReconnectBox: NSButton!
    private var followSizeBox: NSButton!
    private var pathLabel: NSTextField!
    /// Rows only the tunnel mode uses.
    private var tunnelOnlyKeys = ["user", "identity", "sshPort", "localPort"]
    private var labels: [String: NSTextField] = [:]

    convenience init() {
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 470, height: 470),
                         styleMask: [.titled, .closable],
                         backing: .buffered, defer: false)
        w.title = "Haiku Remote Settings"
        w.center()
        self.init(window: w)
        build()
        refreshMode()
    }

    private func build() {
        guard let content = window?.contentView else { return }
        var y: CGFloat = 424

        func row(_ label: String, _ key: String, _ value: String,
                 width: CGFloat = 250) {
            let l = NSTextField(labelWithString: label)
            l.frame = NSRect(x: 16, y: y, width: 140, height: 20)
            l.alignment = .right
            content.addSubview(l)
            labels[key] = l
            let f = NSTextField(string: value)
            f.frame = NSRect(x: 166, y: y - 2, width: width, height: 24)
            content.addSubview(f)
            fields[key] = f
            y -= 30
        }

        // -- mode ------------------------------------------------------------
        let ml = NSTextField(labelWithString: "Connection")
        ml.frame = NSRect(x: 16, y: y, width: 140, height: 20)
        ml.alignment = .right
        content.addSubview(ml)
        modePopup = NSPopUpButton(frame: NSRect(x: 166, y: y - 4,
                                                width: 280, height: 26))
        modePopup.addItems(withTitles: ["Direct — straight to the Haiku host",
                                        "SSH tunnel (key-based) + forward"])
        modePopup.selectItem(at: Settings.useTunnel ? 1 : 0)
        modePopup.target = self
        modePopup.action = #selector(modeChanged)
        content.addSubview(modePopup)
        y -= 30

        pathLabel = NSTextField(labelWithString: "")
        pathLabel.frame = NSRect(x: 166, y: y, width: 290, height: 18)
        pathLabel.font = .monospacedSystemFont(ofSize: 9, weight: .regular)
        pathLabel.textColor = .secondaryLabelColor
        content.addSubview(pathLabel)
        y -= 28

        // -- endpoint --------------------------------------------------------
        row("Haiku host", "host", Settings.host)
        row("Haiku port", "remotePort", String(Settings.remotePort), width: 90)
        row("SSH user", "user", Settings.user)
        row("Identity file (key)", "identity", Settings.identityFile)
        row("SSH port", "sshPort", String(Settings.sshPort), width: 90)
        row("Local port", "localPort", String(Settings.localPort), width: 90)
        row("Width", "width", String(Settings.width), width: 90)
        row("Height", "height", String(Settings.height), width: 90)

        // -- behaviour -------------------------------------------------------
        autoReconnectBox = NSButton(checkboxWithTitle: "Reconnect automatically if the link drops",
                                    target: nil, action: nil)
        autoReconnectBox.state = Settings.autoReconnect ? .on : .off
        autoReconnectBox.frame = NSRect(x: 166, y: y, width: 300, height: 20)
        content.addSubview(autoReconnectBox)
        y -= 26

        followSizeBox = NSButton(checkboxWithTitle: "Change resolution when the window is resized",
                                 target: nil, action: nil)
        followSizeBox.state = Settings.followWindowSize ? .on : .off
        followSizeBox.frame = NSRect(x: 166, y: y, width: 320, height: 20)
        content.addSubview(followSizeBox)
        y -= 32

        let kl = NSTextField(labelWithString: "Haiku COMMAND")
        kl.frame = NSRect(x: 16, y: y, width: 140, height: 20)
        kl.alignment = .right
        content.addSubview(kl)
        mappingPopup = NSPopUpButton(frame: NSRect(x: 166, y: y - 4,
                                                   width: 250, height: 26))
        mappingPopup.addItems(withTitles: ["macOS Option (⌥)", "macOS Command (⌘)"])
        mappingPopup.selectItem(at: Settings.commandMapping == .optionIsCommand
                                ? 0 : 1)
        content.addSubview(mappingPopup)

        let save = NSButton(title: "Save", target: self, action: #selector(save))
        save.frame = NSRect(x: 366, y: 12, width: 88, height: 30)
        save.keyEquivalent = "\r"
        content.addSubview(save)

        let cancel = NSButton(title: "Cancel", target: self,
                              action: #selector(cancel))
        cancel.frame = NSRect(x: 272, y: 12, width: 88, height: 30)
        content.addSubview(cancel)
    }

    @objc private func modeChanged() { refreshMode() }

    /// Grey out what the chosen mode does not use, and spell out the resulting
    /// path so it is obvious what will be dialled.
    private func refreshMode() {
        let tunnel = modePopup.indexOfSelectedItem == 1
        for key in tunnelOnlyKeys {
            fields[key]?.isEnabled = tunnel
            labels[key]?.textColor = tunnel ? .labelColor : .tertiaryLabelColor
        }
        let host = fields["host"]?.stringValue ?? ""
        let shown = host.isEmpty ? "<host>" : host
        let remote = fields["remotePort"]?.stringValue ?? "10900"
        let local = fields["localPort"]?.stringValue ?? "10900"
        pathLabel.stringValue = tunnel
            ? "client → 127.0.0.1:\(local) → ssh → \(shown):\(remote)"
            : "client → \(shown):\(remote)   (no ssh)"
    }

    @objc private func cancel() { window?.close() }

    @objc private func save() {
        func str(_ k: String) -> String {
            fields[k]?.stringValue.trimmingCharacters(in: .whitespaces) ?? ""
        }
        func int(_ k: String, _ fallback: Int) -> Int { Int(str(k)) ?? fallback }

        Settings.host = str("host")
        Settings.user = str("user")
        Settings.identityFile = str("identity")
        Settings.sshPort = int("sshPort", 22)
        Settings.remotePort = int("remotePort", 10900)
        Settings.localPort = int("localPort", 10900)
        Settings.width = int("width", 1280)
        Settings.height = int("height", 800)
        Settings.useTunnel = modePopup.indexOfSelectedItem == 1
        Settings.autoReconnect = autoReconnectBox.state == .on
        Settings.followWindowSize = followSizeBox.state == .on
        Settings.commandMapping = mappingPopup.indexOfSelectedItem == 0
            ? .optionIsCommand : .commandIsCommand
        window?.close()
    }
}
