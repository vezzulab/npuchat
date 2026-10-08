#!/usr/bin/env bash
# Builds dist/NPU-Chat-x86_64.AppImage with linuxdeploy + its GTK plugin.
# Run it where the build dependencies are installed (on Fedora, inside the
# "npu-dev" toolbox: `toolbox run -c npu-dev packaging/build-appimage.sh`).
set -euo pipefail
cd "$(dirname "$0")/.."

tools="$PWD/tools"
mkdir -p "$tools" dist
if [ ! -x "$tools/linuxdeploy-x86_64.AppImage" ]; then
  curl -sSLo "$tools/linuxdeploy-x86_64.AppImage" \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
  curl -sSLo "$tools/linuxdeploy-plugin-gtk.sh" \
    https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh
  chmod +x "$tools"/linuxdeploy-*
fi
export PATH="$tools:$PATH"
export APPIMAGE_EXTRACT_AND_RUN=1 # no FUSE needed inside containers

rm -rf build-appimage AppDir
meson setup build-appimage --prefix=/usr --buildtype=release
meson compile -C build-appimage
DESTDIR="$PWD/AppDir" meson install -C build-appimage

# Bundle GTK 4 / libadwaita and their runtime data.
DEPLOY_GTK_VERSION=4 linuxdeploy-x86_64.AppImage --appdir AppDir --plugin gtk \
  --desktop-file AppDir/usr/share/applications/io.github.vezzulab.NpuChat.desktop \
  --icon-file AppDir/usr/share/icons/hicolor/scalable/apps/io.github.vezzulab.NpuChat.svg

# The GTK plugin forces GTK_THEME, which breaks libadwaita's styling
# (light/dark and accent colors). Let libadwaita handle theming.
sed -i '/GTK_THEME/d' AppDir/apprun-hooks/linuxdeploy-plugin-gtk.sh

OUTPUT=dist/NPU-Chat-x86_64.AppImage linuxdeploy-x86_64.AppImage --appdir AppDir --output appimage
rm -rf AppDir
echo "Built dist/NPU-Chat-x86_64.AppImage"
