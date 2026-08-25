#!/usr/bin/env bash
#
# Build without SwiftPM.
#
# SwiftPM cannot run here: this machine has only Command Line Tools, whose
# libPackageDescription.dylib exports no Package.init symbol, so every manifest
# fails to link. Package.swift is kept for anyone with a full Xcode install, but
# this script is the path that actually works. Both produce the same sources.
#
# Usage:
#   ./build.sh test    # build and run the protocol test suite
#   ./build.sh app     # build HaikuRemote.app (menu-bar agent)
#   ./build.sh all     # both
#   ./build.sh install # build the app and copy it to /Applications
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

BUILD=build
CORE=(Sources/HaikuRemoteCore/*.swift)
APP=(Sources/HaikuRemote/*.swift)
TESTS=(Sources/HaikuRemoteTests/*.swift)

SDK="$(xcrun --show-sdk-path)"
COMMON=(-sdk "$SDK" -target arm64-apple-macos13.0 -O
        -framework CoreGraphics -framework CoreText -framework Foundation -framework ImageIO)

mkdir -p "$BUILD"

build_tests() {
	echo "== building tests =="
	# The test runner links the core sources directly rather than a module, so it
	# can reach internal declarations without an @testable import.
	swiftc "${COMMON[@]}" -framework Network \
		"${CORE[@]}" "${TESTS[@]}" \
		-o "$BUILD/haiku-remote-tests"
	echo "== running tests =="
	"$BUILD/haiku-remote-tests"
}

# Render the icon and pack an .icns. Drawn by tools/make-icon.swift so the art is
# source rather than a binary blob; sips fans it out to the sizes macOS wants.
build_icon() {
	local iconset="$BUILD/HaikuRemote.iconset"
	local out="$BUILD/HaikuRemote.icns"
	# Only rebuild when the generator changed; iconutil is slow enough to notice.
	if [ -f "$out" ] && [ "$out" -nt tools/make-icon.swift ]; then return; fi
	echo "== building icon =="
	swiftc -O -o "$BUILD/make-icon" tools/make-icon.swift
	"$BUILD/make-icon" "$BUILD/icon-1024.png" >/dev/null
	rm -rf "$iconset"; mkdir -p "$iconset"
	# The names are fixed by iconutil; 16..512 plus @2x.
	for size in 16 32 128 256 512; do
		sips -z $size $size "$BUILD/icon-1024.png" \
			--out "$iconset/icon_${size}x${size}.png" >/dev/null
		local double=$((size * 2))
		sips -z $double $double "$BUILD/icon-1024.png" \
			--out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
	done
	iconutil --convert icns "$iconset" --output "$out"
	echo "built $out"
}

build_app() {
	echo "== building app =="
	swiftc "${COMMON[@]}" -framework AppKit -framework Network \
		"${CORE[@]}" "${APP[@]}" \
		-o "$BUILD/HaikuRemote"

	build_icon

	local bundle="$BUILD/HaikuRemote.app"
	rm -rf "$bundle"
	mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources"
	cp "$BUILD/HaikuRemote" "$bundle/Contents/MacOS/HaikuRemote"
	cp "$BUILD/HaikuRemote.icns" "$bundle/Contents/Resources/HaikuRemote.icns"

	# No LSUIElement: this is a regular app with a Dock icon and a menu bar. The
	# status-bar glyph is still created at runtime, which does not require it.
	cat > "$bundle/Contents/Info.plist" <<-PLIST
	<?xml version="1.0" encoding="UTF-8"?>
	<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
	<plist version="1.0">
	<dict>
		<key>CFBundleName</key>              <string>HaikuRemote</string>
		<key>CFBundleDisplayName</key>       <string>Haiku Remote</string>
		<key>CFBundleIdentifier</key>        <string>dev.benfelip.HaikuRemote</string>
		<key>CFBundleExecutable</key>        <string>HaikuRemote</string>
		<key>CFBundlePackageType</key>       <string>APPL</string>
		<key>CFBundleShortVersionString</key><string>0.1</string>
		<key>CFBundleVersion</key>           <string>1</string>
		<key>LSMinimumSystemVersion</key>    <string>13.0</string>
		<key>CFBundleIconFile</key>          <string>HaikuRemote</string>
		<key>NSHighResolutionCapable</key>   <true/>
	</dict>
	</plist>
	PLIST

	echo "built $bundle"
	echo "run with: open $bundle    (or $bundle/Contents/MacOS/HaikuRemote for logs)"
}

# Copying by hand is how /Applications ended up 19 minutes behind build/ once,
# which looked exactly like "the app stopped working".
install_app() {
	build_app
	echo "== installing =="
	pkill -f "HaikuRemote.app/Contents/MacOS/HaikuRemote" 2>/dev/null || true
	rm -rf /Applications/HaikuRemote.app
	cp -R "$BUILD/HaikuRemote.app" /Applications/
	echo "installed /Applications/HaikuRemote.app"
}

case "${1:-all}" in
	test)    build_tests ;;
	app)     build_app ;;
	icon)    build_icon ;;
	install) install_app ;;
	all)     build_tests; build_app ;;
	*) echo "usage: $0 {test|app|icon|install|all}" >&2; exit 2 ;;
esac
