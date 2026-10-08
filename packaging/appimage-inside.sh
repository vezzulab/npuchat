#!/usr/bin/env bash
# Runs inside the Ubuntu 24.04 container started by build-appimage.sh.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential meson ninja-build pkg-config ca-certificates curl file \
  libgtk-4-dev libadwaita-1-dev libsoup-3.0-dev libjson-glib-dev libpoppler-glib-dev \
  libglib2.0-bin libgdk-pixbuf2.0-bin librsvg2-common glib-networking >/dev/null

tools=/tmp/tools
mkdir -p "$tools"
curl -sSLo "$tools/linuxdeploy-x86_64.AppImage" \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
curl -sSLo "$tools/linuxdeploy-plugin-gtk.sh" \
  https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh
chmod +x "$tools"/linuxdeploy-*
export PATH="$tools:$PATH"
export APPIMAGE_EXTRACT_AND_RUN=1 # no FUSE inside containers

build=/tmp/build
appdir=/tmp/AppDir
meson setup "$build" --prefix=/usr --buildtype=release
meson compile -C "$build"
DESTDIR="$appdir" meson install -C "$build"

# Bundle GTK 4 / libadwaita and their runtime data.
DEPLOY_GTK_VERSION=4 linuxdeploy-x86_64.AppImage --appdir "$appdir" --plugin gtk \
  --desktop-file "$appdir/usr/share/applications/io.github.vezzulab.NpuChat.desktop" \
  --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/io.github.vezzulab.NpuChat.svg"

# HTTPS for the update check: bundle GLib's TLS module and load only our own
# GIO modules, so host modules built against another GLib are never mixed in.
mkdir -p "$appdir/usr/lib/gio/modules"
cp /usr/lib/x86_64-linux-gnu/gio/modules/libgiognutls.so "$appdir/usr/lib/gio/modules/"
linuxdeploy-x86_64.AppImage --appdir "$appdir" --deploy-deps-only "$appdir/usr/lib/gio/modules"
cat > "$appdir/apprun-hooks/npu-chat-gio.sh" <<'HOOK'
export GIO_MODULE_DIR="$APPDIR/usr/lib/gio/modules"
HOOK

# The GTK plugin forces GTK_THEME, which breaks libadwaita's styling
# (light/dark and accent colors). Let libadwaita handle theming.
sed -i '/GTK_THEME/d' "$appdir/apprun-hooks/linuxdeploy-plugin-gtk.sh"

OUTPUT=/src/dist/NPU-Chat-x86_64.AppImage linuxdeploy-x86_64.AppImage --appdir "$appdir" --output appimage
chown "$(stat -c %u:%g /src)" /src/dist/NPU-Chat-x86_64.AppImage
