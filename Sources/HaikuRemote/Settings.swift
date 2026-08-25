import Foundation

/// Plain UserDefaults, as asked. No keychain: the only secret involved is an SSH
/// key path, and the key itself stays where ssh already keeps it.
struct Settings {
    private static let d = UserDefaults.standard

    private enum Key {
        static let host = "host"
        static let user = "user"
        static let identity = "identityFile"
        static let sshPort = "sshPort"
        static let remotePort = "remotePort"
        static let localPort = "localPort"
        static let width = "width"
        static let height = "height"
        static let useTunnel = "useTunnel"
        static let commandMapping = "commandMapping"
        static let autoReconnect = "autoReconnect"
        static let followWindowSize = "followWindowSize"
    }

    static func registerDefaults() {
        d.register(defaults: [
            Key.host: "",
            Key.user: "baron",                 // the Haiku images run as `baron`
            Key.identity: "~/.ssh/haiku-rdclient-ed25519",
            Key.sshPort: 22,
            // 10900 is what the Graviton images use (TARGET_SCREEN), not the
            // 10901 default baked into RemoteHWInterface.
            Key.remotePort: 10900,
            Key.localPort: 10900,
            Key.width: 1280,
            Key.height: 800,
            Key.useTunnel: true,
            Key.commandMapping: CommandKeyMapping.optionIsCommand.rawValue,
            Key.autoReconnect: true,
            Key.followWindowSize: true,
        ])
    }

    static var host: String {
        get { d.string(forKey: Key.host) ?? "" }
        set { d.set(newValue, forKey: Key.host) }
    }
    static var user: String {
        get { d.string(forKey: Key.user) ?? "baron" }
        set { d.set(newValue, forKey: Key.user) }
    }
    static var identityFile: String {
        get { d.string(forKey: Key.identity) ?? "" }
        set { d.set(newValue, forKey: Key.identity) }
    }
    static var sshPort: Int {
        get { d.integer(forKey: Key.sshPort) }
        set { d.set(newValue, forKey: Key.sshPort) }
    }
    static var remotePort: Int {
        get { d.integer(forKey: Key.remotePort) }
        set { d.set(newValue, forKey: Key.remotePort) }
    }
    static var localPort: Int {
        get { d.integer(forKey: Key.localPort) }
        set { d.set(newValue, forKey: Key.localPort) }
    }
    static var width: Int {
        get { max(320, d.integer(forKey: Key.width)) }
        set { d.set(newValue, forKey: Key.width) }
    }
    static var height: Int {
        get { max(240, d.integer(forKey: Key.height)) }
        set { d.set(newValue, forKey: Key.height) }
    }
    static var useTunnel: Bool {
        get { d.bool(forKey: Key.useTunnel) }
        set { d.set(newValue, forKey: Key.useTunnel) }
    }
    static var commandMapping: CommandKeyMapping {
        get {
            CommandKeyMapping(rawValue: d.string(forKey: Key.commandMapping) ?? "")
                ?? .optionIsCommand
        }
        set { d.set(newValue.rawValue, forKey: Key.commandMapping) }
    }

    /// Retry a dropped link automatically. On by default: the point of this
    /// client is unattended use over a link that comes and goes.
    static var autoReconnect: Bool {
        get { d.bool(forKey: Key.autoReconnect) }
        set { d.set(newValue, forKey: Key.autoReconnect) }
    }
    /// Re-advertise the display mode when the window is resized, rather than
    /// letterboxing the old resolution.
    static var followWindowSize: Bool {
        get { d.bool(forKey: Key.followWindowSize) }
        set { d.set(newValue, forKey: Key.followWindowSize) }
    }

    static var tunnelConfig: SSHTunnel.Config {
        SSHTunnel.Config(host: host, user: user,
                        identityFile: identityFile.isEmpty ? nil : identityFile,
                        localPort: localPort, remotePort: remotePort,
                        sshPort: sshPort)
    }
}
