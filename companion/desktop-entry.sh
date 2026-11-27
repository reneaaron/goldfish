#!/bin/sh
# Install a desktop entry for the debug build (`cargo run`), so Linux docks
# show the Dove icon. GNOME on Wayland ignores window icons: it takes the icon
# from the .desktop file named after the window's app id, which for this app is
# the binary name, goldfish-companion.
set -e
here=$(cd "$(dirname "$0")" && pwd)
# Exec quotes the path for spaces; these would need escaping rules of their own.
case "$here" in
  *[\"\`\$\\%]*) echo "Path has a character Exec can't take: $here" >&2; exit 1 ;;
esac
case "$here" in
  */.claude/worktrees/*) echo "Warning: $here is a worktree; the dock entry stops working once it is removed. Run this from the main checkout to keep it." >&2 ;;
esac
data=${XDG_DATA_HOME:-$HOME/.local/share}
mkdir -p "$data/applications" "$data/icons/hicolor/256x256/apps"
cp "$here/src-tauri/icons/icon.png" "$data/icons/hicolor/256x256/apps/goldfish-companion.png"
cat > "$data/applications/goldfish-companion.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Dove
Comment=Watch-only wallet for Goldfish
Exec="$here/src-tauri/target/debug/goldfish-companion"
Icon=goldfish-companion
StartupWMClass=goldfish-companion
Categories=Office;Finance;
EOF
echo "Installed $data/applications/goldfish-companion.desktop"
