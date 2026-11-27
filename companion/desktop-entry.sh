#!/bin/sh
# Install a desktop entry for the debug build (`cargo run`), so Linux docks
# show the Dove icon. GNOME on Wayland ignores window icons: it takes the icon
# from the .desktop file named after the window's app id, which for this app is
# the binary name, goldfish-companion.
set -e
here=$(cd "$(dirname "$0")" && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
mkdir -p "$data/applications" "$data/icons/hicolor/256x256/apps"
cp "$here/src-tauri/icons/icon.png" "$data/icons/hicolor/256x256/apps/goldfish-companion.png"
cat > "$data/applications/goldfish-companion.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Dove
Comment=Watch-only wallet for Goldfish
Exec=$here/src-tauri/target/debug/goldfish-companion
Icon=goldfish-companion
StartupWMClass=goldfish-companion
Categories=Office;Finance;
EOF
echo "Installed $data/applications/goldfish-companion.desktop"
