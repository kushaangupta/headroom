#!/bin/sh
# Install the Headroom top-bar extension for the current user (GNOME Shell 45-49).
set -eu
cd "$(dirname "$0")"
UUID=headroom@local
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$UUID"
mkdir -p "$DEST"
cp "$UUID"/metadata.json "$UUID"/extension.js "$UUID"/core.js "$UUID"/stylesheet.css "$DEST"/
echo "Installed to $DEST"
if [ "$(gsettings get org.gnome.shell disable-user-extensions 2>/dev/null)" = "true" ]; then
	gsettings set org.gnome.shell disable-user-extensions false
	echo "Note: GNOME user extensions were switched off; switched them on."
	echo "      To undo later: gsettings set org.gnome.shell disable-user-extensions true"
fi
if gnome-extensions enable "$UUID" 2>/dev/null; then
	echo "Enabled: look at the top bar."
else
	echo "GNOME has to notice the new extension first:"
	echo "  X11 session:     press Alt+F2, type r, press Enter"
	echo "  Wayland session: log out and back in"
	echo "then run:  gnome-extensions enable $UUID"
fi
