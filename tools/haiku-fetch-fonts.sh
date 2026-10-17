#!/bin/sh
# haiku-fetch-fonts.sh -- copy a Haiku guest's font files to where the client
# looks first for the faces app_server names.
#
# Why: with RP_CAP_FONT_IDENTITY every RP_SET_FONT says which face app_server
# lays the text out with -- family, style, and the file's 'head' checksum and
# revision. app_server computes every advance from that file
# (RemoteDrawingEngine::DrawString returns ServerFont::StringWidth), so a
# client drawing from any other file puts its glyphs somewhere else. A host's
# "Noto Sans" is usually not Haiku's: hrev59996 ships the 2017 Noto release,
# a 2024 Linux distribution a different one with different metrics. The client
# tells you when that happens ("...the same face by name but not the same
# file..." / "...not installed here..."); this fetches the real ones.
#
#     tools/haiku-fetch-fonts.sh [--dest DIR] [--label NAME] [--ssh CMD] [--] [ssh args...]
#
#     tools/haiku-fetch-fonts.sh -- -i ~/.ssh/haiku_key user@10.0.0.9
#     tools/haiku-fetch-fonts.sh --ssh ./my-ssh-wrapper     # runs: CMD <remote command>
#
# Every directory GlobalFontManager scans is copied
# (src/servers/app/font/GlobalFontManager.cpp, _AddSystemPaths/_AddUserPaths):
# B_SYSTEM_FONTS_DIRECTORY, B_SYSTEM_NONPACKAGED_FONTS_DIRECTORY,
# B_USER_FONTS_DIRECTORY and B_USER_NONPACKAGED_FONTS_DIRECTORY, resolved on the
# guest with finddir. The guest needs nothing but sh, find and base64 -- a
# minimal image has no tar.
#
# --dest defaults to the directory the client searches (text_engine.cpp,
# fetched_font_dirs): $XDG_CACHE_HOME/haiku-remote/fonts (~/.cache/... when
# unset), ~/Library/Caches/haiku-remote/fonts on macOS. HAIKU_REMOTE_FONT_DIR
# names more directories for the client, searched before that one. Files land
# in DEST/LABEL/<guest path>; LABEL defaults to the guest's hrev, so two images
# can sit side by side -- the client tells them apart by checksum, not by
# directory.
set -eu

DEST=
LABEL=
SSH_CMD=ssh
while [ $# -gt 0 ]; do
	case "$1" in
	--dest) DEST=$2; shift 2 ;;
	--label) LABEL=$2; shift 2 ;;
	--ssh) SSH_CMD=$2; shift 2 ;;
	--) shift; break ;;
	-h|--help) sed -n '2,32p' "$0"; exit 0 ;;
	*) break ;;
	esac
done

if [ -z "$DEST" ]; then
	if [ "$(uname -s)" = Darwin ]; then
		DEST="$HOME/Library/Caches/haiku-remote/fonts"
	else
		DEST="${XDG_CACHE_HOME:-$HOME/.cache}/haiku-remote/fonts"
	fi
fi

# Runs on the guest. "## <hrev>" first, then for each file "== <path>" and its
# base64 -- a line base64 never produces, so the stream frames itself.
REMOTE='
printf "## %s\n" "$(uname -v | cut -d" " -f1)"
for which in B_SYSTEM_FONTS_DIRECTORY B_SYSTEM_NONPACKAGED_FONTS_DIRECTORY \
		B_USER_FONTS_DIRECTORY B_USER_NONPACKAGED_FONTS_DIRECTORY; do
	dir=$(finddir "$which" 2>/dev/null) || continue
	[ -d "$dir" ] || continue
	find "$dir" -type f | while IFS= read -r file; do
		printf "== %s\n" "$file"
		base64 "$file"
	done
done
'

work=$(mktemp -d "${TMPDIR:-/tmp}/haiku-fonts.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

# shellcheck disable=SC2086 # SSH_CMD may carry its own arguments.
if ! $SSH_CMD "$@" "$REMOTE" > "$work/stream"; then
	echo "haiku-fetch-fonts: the guest command failed (is this a Haiku guest you can ssh to?)" >&2
	exit 1
fi

guest_label=$(sed -n '1s/^## //p' "$work/stream")
[ -n "$LABEL" ] || LABEL=${guest_label:-guest}
case "$LABEL" in
*/*|.|..|'') echo "haiku-fetch-fonts: bad label '$LABEL'" >&2; exit 2 ;;
esac

# Split the stream. A guest path is used as a relative path under the label,
# and refused outright if it could climb out of it or needs quoting.
mkdir -p "$work/tree"
awk -v root="$work/tree" '
/^## / { next }
/^== / {
	if (out != "") close(out)
	path = substr($0, 4)
	if (path !~ /^\/[A-Za-z0-9._+ \/-]+$/ || path ~ /(^|\/)\.\.(\/|$)/) {
		print "haiku-fetch-fonts: skipping unsafe path " path > "/dev/stderr"
		out = ""
		next
	}
	sub(/^\/+/, "", path)
	out = root "/" path ".b64"
	dir = out
	sub(/\/[^\/]*$/, "", dir)
	system("mkdir -p \"" dir "\"")
	printf "" > out
	count++
	next
}
out != "" { print >> out }
END { printf "%d\n", count > (root "/.count") }
' "$work/stream"

decode() {
	base64 -d 2>/dev/null || base64 -D
}
find "$work/tree" -type f -name '*.b64' | while IFS= read -r encoded; do
	decode < "$encoded" > "${encoded%.b64}"
	rm -f "$encoded"
done

count=$(cat "$work/tree/.count")
rm -f "$work/tree/.count"
if [ "$count" -eq 0 ]; then
	echo "haiku-fetch-fonts: the guest reported no font files" >&2
	exit 1
fi

mkdir -p "$DEST"
target="$DEST/$LABEL"
rm -rf "$target.new"
mv "$work/tree" "$target.new"
rm -rf "$target"
mv "$target.new" "$target"
echo "haiku-fetch-fonts: $count files from $guest_label in $target"
