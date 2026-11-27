#!/bin/sh
# Jurassic Park Operation Genesis.app, without the disk image:
#
#   curl -fsSL https://github.com/cpressland/jpog-recomp/releases/latest/download/install.sh | sh
#
# Puts the latest release in /Applications (~/Applications if that isn't
# writable) and opens it. Downloaded with curl, the app carries no quarantine,
# so macOS opens it without the "Open Anyway" step a browser download needs.
set -e
REPOSITORY=${REPOSITORY:-cpressland/jpog-recomp}
URL=${URL:-"https://github.com/$REPOSITORY/releases/latest/download/Jurassic-Park-Operation-Genesis.zip"}
APP="Jurassic Park Operation Genesis.app"

[ "$(uname -s)" = Darwin ] || { echo "Jurassic Park: Operation Genesis's app is for macOS." >&2; exit 1; }
[ "$(uname -m)" = arm64 ] || { echo "Jurassic Park: Operation Genesis needs a Mac with Apple silicon." >&2; exit 1; }
major=$(sw_vers -productVersion | cut -d. -f1)
[ "$major" -ge 26 ] || { echo "Jurassic Park: Operation Genesis needs macOS 26 or later (this Mac has $(sw_vers -productVersion))." >&2; exit 1; }

DEST=${DEST:-/Applications}
[ -w "$DEST" ] || { DEST="$HOME/Applications"; mkdir -p "$DEST"; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "Downloading Jurassic Park: Operation Genesis..."
curl -fL --progress-bar -o "$tmp/app.zip" "$URL"
ditto -x -k "$tmp/app.zip" "$tmp"
if [ -d "$DEST/$APP" ]; then
  osascript -e 'quit app "Jurassic Park Operation Genesis"' 2>/dev/null || true
  rm -rf "$DEST/$APP"
fi
mv "$tmp/$APP" "$DEST/"
echo "Installed $DEST/$APP"
[ "${OPEN:-1}" = 0 ] || open "$DEST/$APP"
