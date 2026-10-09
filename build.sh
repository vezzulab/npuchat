#!/usr/bin/env bash
# Builds NPU Chat and installs it for the current user (~/.local), so it shows
# up in the app menu. Uses the host compiler if the dev packages are present,
# otherwise the "npu-dev" toolbox container (see README).
set -euo pipefail
cd "$(dirname "$0")"

run() {
  if command -v cc >/dev/null && pkg-config --exists libadwaita-1 libsoup-3.0 json-glib-1.0; then
    "$@"
  else
    toolbox run -c npu-dev "$@"
  fi
}

[ -d build ] || run meson setup build
run meson compile -C build

install -Dm755 build/npu-chat ~/.local/bin/npu-chat
# Absolute path: desktop sessions often lack ~/.local/bin in PATH.
mkdir -p ~/.local/share/applications
sed "s|^Exec=.*|Exec=$HOME/.local/bin/npu-chat|" data/io.github.vezzulab.NpuChat.desktop \
  > ~/.local/share/applications/io.github.vezzulab.NpuChat.desktop
install -Dm644 data/io.github.vezzulab.NpuChat.svg \
  ~/.local/share/icons/hicolor/scalable/apps/io.github.vezzulab.NpuChat.svg
install -Dm644 data/io.github.vezzulab.NpuChat.Calendar.svg \
  ~/.local/share/icons/hicolor/scalable/apps/io.github.vezzulab.NpuChat.Calendar.svg
command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 >/dev/null 2>&1 || true
echo "Installed: run 'npu-chat' or open NPU Chat from the app menu."
