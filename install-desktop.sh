#!/usr/bin/env bash
# Install the AppImage to ~/Applications and register a .desktop entry
# + icon so GNOME search (Super key) finds it.
set -euo pipefail
cd "$(dirname "$0")"

APPIMAGE_SRC=$(ls dist/Google\ Home-*.AppImage | sort -V | tail -1)
APPIMAGE_DEST="$HOME/Applications/google-home-x86_64.AppImage"
ICON_DEST="$HOME/.local/share/icons/hicolor/512x512/apps/google-home-desktop.png"
DESKTOP_DEST="$HOME/.local/share/applications/io.github.averagenative.googlehome.desktop"

mkdir -p "$HOME/Applications" "$(dirname "$ICON_DEST")" "$(dirname "$DESKTOP_DEST")"
# Copy then rename: cp onto a running AppImage fails with "Text file
# busy", but a rename swaps it while the old copy keeps running.
cp "$APPIMAGE_SRC" "$APPIMAGE_DEST.tmp"
chmod +x "$APPIMAGE_DEST.tmp"
mv -f "$APPIMAGE_DEST.tmp" "$APPIMAGE_DEST"
cp build/icon.png "$ICON_DEST"

cat > "$DESKTOP_DEST" <<EOF
[Desktop Entry]
Type=Application
Name=Google Home
Comment=Unofficial desktop wrapper for the Google Home web app
Exec=$APPIMAGE_DEST
Icon=google-home-desktop
Terminal=false
Categories=Utility;
Keywords=google;home;smart;devices;nest;
StartupWMClass=google-home
EOF

update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
gtk-update-icon-cache -t "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
echo "Installed: $DESKTOP_DEST -> $APPIMAGE_DEST"
