#!/bin/sh
# Remove the Headroom top-bar extension for the current user.
#   ./uninstall.sh           remove the extension, keep the settings
#   ./uninstall.sh --purge   also delete the settings (~/.config/headroom, shared with headroom.py)
set -eu
UUID=headroom@local
DIR="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$UUID"
CFG="${XDG_CONFIG_HOME:-$HOME/.config}/headroom"
gnome-extensions disable "$UUID" 2>/dev/null || true   # unloads it now, drops it from the enabled list
gnome-extensions uninstall "$UUID" 2>/dev/null || true # deletes it through GNOME Shell
rm -rf "$DIR"                                          # in case GNOME had not loaded it yet
echo "Removed the extension ($DIR)."
if [ "${1:-}" = "--purge" ]; then
	rm -rf "$CFG"
	echo "Deleted the settings ($CFG)."
else
	echo "Kept the settings in $CFG (shared with headroom.py). To delete them too: $0 --purge"
fi
