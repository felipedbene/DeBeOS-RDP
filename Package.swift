// swift-tools-version:6.0
import PackageDescription

let package = Package(
    name: "HaikuRemote",
    platforms: [.macOS(.v13)],
    targets: [
        // Protocol decode/encode and rendering. Kept free of AppKit-only
        // assumptions where practical so it can be unit tested.
        .target(name: "HaikuRemoteCore"),
        .executableTarget(name: "HaikuRemote", dependencies: ["HaikuRemoteCore"]),
        .testTarget(name: "HaikuRemoteCoreTests", dependencies: ["HaikuRemoteCore"]),
    ]
)
