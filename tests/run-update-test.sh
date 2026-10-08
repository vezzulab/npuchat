#!/usr/bin/env bash
# Tests the self-update flow (download, SHA-256 check, file replacement)
# against a local fake "GitHub Releases" server, under ASan, inside the
# npu-dev toolbox. Usage: tests/run-update-test.sh
set -euo pipefail
cd "$(dirname "$0")/.."
toolbox run -c npu-dev meson compile -C build-test >/dev/null

work=$(mktemp -d)
trap 'kill $server 2>/dev/null || true; rm -rf "$work"' EXIT
mkdir -p "$work/srv" "$work/app" "$work/config"
port=52691
head -c 3000000 /dev/urandom > "$work/srv/NPU-Chat-x86_64.AppImage"
(cd "$work/srv" && sha256sum NPU-Chat-x86_64.AppImage > NPU-Chat-x86_64.AppImage.sha256)
cat > "$work/srv/latest.json" <<JSON
{ "tag_name": "v9.9.9", "html_url": "http://127.0.0.1:$port/",
  "assets": [
    { "name": "NPU-Chat-x86_64.AppImage", "browser_download_url": "http://127.0.0.1:$port/NPU-Chat-x86_64.AppImage" },
    { "name": "NPU-Chat-x86_64.AppImage.sha256", "browser_download_url": "http://127.0.0.1:$port/NPU-Chat-x86_64.AppImage.sha256" } ] }
JSON
python3 -I -m http.server "$port" --bind 127.0.0.1 --directory "$work/srv" >/dev/null 2>&1 &
server=$!
sleep 1

run_case () {
  echo "old version" > "$work/app/NPU-Chat.AppImage"
  toolbox run -c npu-dev env PATH="$PWD/tests/mock:/usr/bin:/bin" \
    XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data" MOCK_FLM_STATE="$work/inst.json" \
    NPU_CHAT_PORT=52690 APPIMAGE="$work/app/NPU-Chat.AppImage" \
    NPU_CHAT_UPDATE_URL="http://127.0.0.1:$port/latest.json" ASAN_OPTIONS=detect_leaks=1 \
    SELFTEST="checkupdate;sleep:2;update;sleep:4;closedlg;close" \
    timeout 60 dbus-run-session -- ./build-test/npu-chat > "$work/log" 2>&1 || true
  if grep -q 'ERROR: AddressSanitizer' "$work/log"; then grep -A20 'ERROR: AddressSanitizer' "$work/log"; exit 1; fi
  if grep -q 'SELFTEST-LIVE ' "$work/log"; then grep 'SELFTEST-LIVE ' "$work/log"; exit 1; fi
}

# 1) Valid release: the file must be replaced by the new one, executable.
run_case
cmp -s "$work/app/NPU-Chat.AppImage" "$work/srv/NPU-Chat-x86_64.AppImage" || { echo "FAIL: not replaced"; exit 1; }
[ -x "$work/app/NPU-Chat.AppImage" ] || { echo "FAIL: not executable"; exit 1; }
echo "ok: valid update installed"

# 2) Tampered download (checksum mismatch): the old file must stay untouched.
echo "0000000000000000000000000000000000000000000000000000000000000000  NPU-Chat-x86_64.AppImage" \
  > "$work/srv/NPU-Chat-x86_64.AppImage.sha256"
run_case
[ "$(cat "$work/app/NPU-Chat.AppImage")" = "old version" ] || { echo "FAIL: tampered update was installed"; exit 1; }
[ ! -e "$work/app/NPU-Chat.AppImage.new" ] || { echo "FAIL: temp file left behind"; exit 1; }
echo "ok: tampered update rejected"
echo PASS
