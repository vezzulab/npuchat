#!/usr/bin/env bash
# Builds dist/NPU-Chat-x86_64.AppImage inside an Ubuntu 24.04 container
# (podman or docker). Building on an older base keeps the glibc requirement
# low (2.39), so the AppImage runs on Ubuntu 24.04+, Debian 13, Fedora 40+,
# Arch and other current distros.
set -euo pipefail
cd "$(dirname "$0")/.."

engine=$(command -v podman || command -v docker || true)
[ -n "$engine" ] || { echo "Install podman or docker first." >&2; exit 1; }

mkdir -p dist
"$engine" run --rm -v "$PWD":/src:Z -w /src docker.io/library/ubuntu:24.04 \
  bash packaging/appimage-inside.sh
echo "Built dist/NPU-Chat-x86_64.AppImage"
