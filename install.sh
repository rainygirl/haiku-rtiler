#!/bin/sh
#
# Builds R Tiler and installs it into the Deskbar tray. Run on Haiku itself.
#
# Usage:
#   ./install.sh              build and install
#   ./install.sh --uninstall  remove the tray item and the binary
#   ./install.sh --build-only build in place, install nothing

set -e

APP="R Tiler"
SRC="RTiler.cpp"
RDEF="RTiler.rdef"
LIBS="-lbe -llocalestub -lroot"

APPS_DIR="$HOME/config/non-packaged/apps"
MENU_DIR="$HOME/config/non-packaged/data/deskbar/menu/Applications"

cd "$(dirname "$0")"

if [ "$(uname -s)" != "Haiku" ]; then
	echo "This builds a Haiku application; run it on Haiku." >&2
	exit 1
fi

if [ "$1" = "--uninstall" ]; then
	# Same ordering as install: remove the tray item, let Deskbar drop the
	# image, and only then delete the file it was loaded from.
	[ -x "$APPS_DIR/$APP" ] && "$APPS_DIR/$APP" --remove || true
	sleep 2
	rm -f "$APPS_DIR/$APP" "$MENU_DIR/$APP"
	echo "Removed $APP."
	exit 0
fi

echo "Compiling..."
g++ -O2 -o "$APP" "$SRC" $LIBS

# The signature has to be a resource, not an attribute: Deskbar stores the
# replicant's "add_on" signature and loads the binary back through the roster
# on every boot, and mimeset -F drops a signature attached with addattr.
echo "Attaching resources..."
rc -o RTiler.rsrc "$RDEF"
xres -o "$APP" RTiler.rsrc
mimeset -f "$APP"
rm -f RTiler.rsrc

if [ "$1" = "--build-only" ]; then
	echo "Built ./$APP (not installed)."
	exit 0
fi

echo "Installing..."
mkdir -p "$APPS_DIR" "$MENU_DIR"

# Take the tray item out BEFORE the binary is replaced. Deskbar keeps this
# executable loaded as an add-on, and the replicant's destructor is called
# through a vtable inside that image -- overwrite the file first and the
# removal jumps into an image that no longer matches, which takes the whole
# Deskbar down with a null instruction pointer.
if [ -x "$APPS_DIR/$APP" ]; then
	"$APPS_DIR/$APP" --remove >/dev/null 2>&1 || true
	sleep 2
fi

cp -f "$APP" "$APPS_DIR/$APP"
ln -sf "$APPS_DIR/$APP" "$MENU_DIR/$APP"
rm -f "$APP"

# Running it is what puts the item in the tray; it exits immediately after.
"$APPS_DIR/$APP"
