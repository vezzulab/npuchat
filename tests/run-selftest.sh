#!/usr/bin/env bash
# Runs the ASan + LeakSanitizer self-test against the mock FastFlowLM, inside
# the npu-dev toolbox (where libasan lives). Usage: tests/run-selftest.sh
set -euo pipefail
cd "$(dirname "$0")/.."

if [ ! -d build-test ]; then
  toolbox run -c npu-dev meson setup build-test -Dbuildtype=debug \
    -Db_sanitize=address -Dc_args=-DNPU_CHAT_SELFTEST -Dtests=true
fi
toolbox run -c npu-dev meson compile -C build-test

# The retrieval engine on its own, under ASan/LeakSanitizer.
toolbox run -c npu-dev env ASAN_OPTIONS=detect_leaks=1 ./build-test/test-rag
# The calculator and the other local skills.
toolbox run -c npu-dev env ASAN_OPTIONS=detect_leaks=1 ./build-test/test-skills
toolbox run -c npu-dev env ASAN_OPTIONS=detect_leaks=1 ./build-test/test-calendar

# A separate port and D-Bus session let this run next to a real NPU Chat.
port=52690

work=$(mktemp -d)
trap '[ -n "${KEEP_LOG:-}" ] && cp "$work/asan.log" "$KEEP_LOG"; rm -rf "$work"' EXIT
mkdir -p "$work/config/npu-chat"
printf '[chat]\nmodel=qwen3:8b\nlanguage=es\ntheme=dark\n' > "$work/config/npu-chat/settings.ini"

mkdir -p "$work/docs"
cat > "$work/docs/manual.md" <<'DOC'
# Manual de soporte

La garantía del producto cubre defectos de fabricación durante 24 meses desde la compra. No cubre daños por agua ni golpes.

Para solicitar un reembolso, envía el recibo y el producto sin usar dentro de los 30 días posteriores a la compra.

El router se reinicia manteniendo presionado el botón trasero durante 10 segundos hasta que las luces parpadeen.
DOC
printf 'Los envios internacionales tardan entre 7 y 15 dias habiles.\n' > "$work/docs/envios.txt"
printf '\000\001binary' > "$work/docs/roto.txt"

steps="send:Hola;wait;newchat;send:Otra;wait;newchat;send:Tercera;wait;open:1;delete:0;delete:0"
steps+=";models;sleep:3;closedlg;installed;sleep:2;closedlg;prefs;sleep:2;closedlg"
steps+=";pick:0;send:Me siento estresado;wait;newchat;mkassistant;sleep:1;send:Plan de comidas;wait"
steps+=";gallery;sleep:2;galleryadd:chef;galleryadd:linux;closedlg;assistmenu;sleep:1;popdown;pick:1;editor;sleep:1;closedlg;pick:-1;newchat"
steps+=";newchat;team:0,1;send:Quiero una rutina y un plan de comidas;wait;wait"
steps+=";send:PASSTEST algo mas;wait;wait;send:TAGTEST otra vez;wait;wait"
steps+=";send:Estratega, ¿cómo me organizo?;wait;toggle:2;send:¿Y para estudiar?;wait"
steps+=";clearassistant;galleryremove:chef;galleryremove:linux;newchat"
steps+=";newchat;import:$work/docs/manual.md|$work/docs/roto.txt;waitimport"
steps+=";send:¿Cuánto dura la garantía?;wait;send:NOMATCH pregunta sin relacion;wait"
steps+=";newlib:Envios;libimport:1:$work/docs/envios.txt;waitimport;attach:1;libdlg;sleep:1;rmdoc:1;dellib:1;closedlg"
steps+=";assistlib:0:0;newchat;pick:0;send:Pregunta con asistente;wait;pick:-1;newchat"
steps+=";newchat;skillsdlg;sleep:1;closedlg;skill:wikipedia:on;send:CALCTEST calcula;wait;send:BADTOOL;wait;send:STATUSTEST;wait"
steps+=";send:WIKITEST;wait;send:WIKI404;wait;skill:calculator:off;send:CALCTEST otra vez;wait;skill:calculator:on;send:DATESYS;wait;send:TOOLSYS;wait;send:HISTCHECK;wait;skill:current_datetime:off;send:DATESYS otra;wait;skill:current_datetime:on;newchat"
steps+=";lang:en;theme:light;sleep:1;send:English;wait;close"

set +e
toolbox run -c npu-dev env \
  PATH="$PWD/tests/mock:/usr/bin:/bin" \
  XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data" \
  MOCK_FLM_STATE="$work/installed.json" \
  NPU_CHAT_PORT=$port NPU_CHAT_WIKI_URL="http://127.0.0.1:$port" ASAN_OPTIONS=detect_leaks=1 SELFTEST="$steps" \
  timeout 120 dbus-run-session -- ./build-test/npu-chat > "$work/asan.log" 2>&1
status=$?
set -e

if [ $status -eq 124 ]; then
  echo "FAIL: self-test timed out" >&2
  exit 1
fi
if grep -q 'ERROR: AddressSanitizer' "$work/asan.log"; then
  grep -A30 'ERROR: AddressSanitizer' "$work/asan.log" >&2
  echo "FAIL: memory error" >&2
  exit 1
fi

# 1) Objects whose lifetime NPU Chat manages by hand (TRACK() in the code)
#    must all be gone at exit.
if ! grep -q 'SELFTEST-LIVE-DONE' "$work/asan.log"; then
  echo "FAIL: the app did not reach a clean shutdown" >&2
  exit 1
fi
if grep -q 'SELFTEST-LIVE ' "$work/asan.log"; then
  grep 'SELFTEST-LIVE ' "$work/asan.log" >&2
  echo "FAIL: objects leaked" >&2
  exit 1
fi

# Team chat: one chat holds replies from two different members. Authors are
# stored per message, so they survive the later "remove assistant" step.
found=0
for f in "$work"/data/npu-chat/chats/*.json; do
  n=$( { grep -o '"author" : "[^"]*"' "$f" || true; } | sort -u | wc -l)
  [ "$n" -ge 2 ] && found=1
done
[ "$found" -eq 1 ] || { echo "FAIL: no chat with replies from 2+ team members" >&2; exit 1; }

# A member that passes leaves nothing behind, and a copied "[Name]:" tag is stripped.
if grep -lq '\[PASS\]' "$work"/data/npu-chat/chats/*.json; then
  echo "FAIL: a [PASS] reply was saved" >&2; exit 1
fi
if grep -hq '"content" : "\[[^]]*\]:' "$work"/data/npu-chat/chats/*.json; then
  echo "FAIL: a speaker tag was kept in a reply" >&2; exit 1
fi
grep -lq 'Respuesta con etiqueta propia' "$work"/data/npu-chat/chats/*.json || { echo "FAIL: tagged reply missing" >&2; exit 1; }

# Documents: the question was searched, the passage reached the model and
# the answer cites it; an unmatched question is recorded as searched-with-no-hits.
doc_chat=$(grep -l '"manual.md"' "$work"/data/npu-chat/chats/*.json | head -1 || true)
[ -n "$doc_chat" ] || { echo "FAIL: no chat with document sources saved" >&2; exit 1; }
grep -q '"libs"' "$doc_chat" || { echo "FAIL: the chat did not keep its library" >&2; exit 1; }
grep -q 'Según tus documentos \[1\]' "$doc_chat" || { echo "FAIL: the answer did not use the sources" >&2; exit 1; }
grep -q 'No encontré eso en tus documentos' "$doc_chat" || { echo "FAIL: the no-match question was not handled" >&2; exit 1; }
if grep -q 'roto.txt' "$work"/data/npu-chat/libraries/*/meta.json; then
  echo "FAIL: a binary file was indexed" >&2; exit 1
fi

# Skills: the model asked, the app ran the tool, and the answer used the result.
skill_chat=$(grep -l '"n" : "calculator"' "$work"/data/npu-chat/chats/*.json | head -1 || true)
[ -n "$skill_chat" ] || { echo "FAIL: no chat recorded a skill" >&2; exit 1; }
grep -q '"r" : "42"' "$skill_chat" || { echo "FAIL: the calculator did not return 42" >&2; exit 1; }
grep -q 'El resultado es 42' "$skill_chat" || { echo "FAIL: the answer did not use the tool result" >&2; exit 1; }
grep -q '"ok" : false' "$skill_chat" || { echo "FAIL: a failing tool was not recorded as failed" >&2; exit 1; }
grep -q 'Hubo un error con la herramienta' "$skill_chat" || { echo "FAIL: the model never saw the tool error" >&2; exit 1; }
grep -q '"n" : "system_status"' "$skill_chat" || { echo "FAIL: status skill missing" >&2; exit 1; }
[ "$(grep -c '"n" : "wikipedia"' "$skill_chat")" -ge 2 ] || { echo "FAIL: Wikipedia (exact title and the search fallback) did not both work" >&2; exit 1; }
grep -q 'Según Wikipedia' "$skill_chat" || { echo "FAIL: the Wikipedia result did not reach the answer" >&2; exit 1; }
# Earlier tool calls are replayed to the model, and a laptop reading is marked outdated.
grep -q 'Historial con herramientas: sí; caducado: sí' "$skill_chat" || { echo "FAIL: earlier tool calls were not replayed (or the battery reading was not marked outdated)" >&2; exit 1; }
# The date and time note reaches the model, and disappears when its skill is off.
grep -q 'Contexto de hora: sí' "$skill_chat" || { echo "FAIL: the date and time note was not added to the message" >&2; exit 1; }
grep -q 'Instrucción de herramientas: sí' "$skill_chat" || { echo "FAIL: the tool instruction was not sent" >&2; exit 1; }
grep -q 'Contexto de hora: no' "$skill_chat" || { echo "FAIL: the date note stayed with its skill off" >&2; exit 1; }
# The note is stored beside the message, never inside what is shown.
if grep -q '"content" : "[^"]*\[Context: it is now' "$skill_chat"; then echo "FAIL: the context note leaked into the visible message" >&2; exit 1; fi
grep -q '"note" : "\[Context: it is now' "$skill_chat" || { echo "FAIL: the note was not stored with the message" >&2; exit 1; }
# With the calculator switched off, the same request must not call it.
[ "$(grep -c '"n" : "calculator"' "$skill_chat")" -eq 2 ] || { echo "FAIL: a disabled skill was still used" >&2; exit 1; }

# 2) Plain memory leaks. GTK, GLib and fontconfig keep some one-time
# allocations until exit; a leak counts as ours when our code made the
# allocation directly, i.e. no event/signal dispatch sits between malloc and
# our first frame.
python3 -I - "$work/asan.log" <<'PY'
import re, sys
txt = open(sys.argv[1]).read()
blocks = [b for b in re.split(r'\n(?=(?:Direct|Indirect) leak of)', txt)
          if b.startswith(('Direct', 'Indirect'))]
dispatch = re.compile(r' in (g_signal_emit\w*|signal_emit\w*|g_closure_invoke|g_cclosure_\w+|g_main_\w+'
                      r'|g_application_\w+|_?gtk_widget_(?!\w*_new)\w+|_gtk_\w+|gtk_window_\w+|gdk_\w+|_gdk_\w+'
                      r'|gtk_at_context\w*|gtk_accessible\w*|g_dbus_\w+|g_task_\w+|g_source_\w+|gtk_css_\w+'
                      r'|gtk_style_\w+|gtk_settings_\w+|g_type_class_\w+|g_type_init\w*|g_io_module\w*'
                      r'|try_implementation|_g_io_modules\w*|gtk_init\w*|adw_init\w*|g_vfs_\w+)\b')
# Font and rendering libraries build one-time caches the first time something is
# laid out; an allocation made inside them is theirs even if our code asked for the text.
foreign = re.compile(r'/lib(pango|pangocairo|pangoft2|fontconfig|freetype|cairo|harfbuzz|pixman|fribidi|epoxy)')
ours = []
for b in blocks:
    frames = [l.strip() for l in b.split('\n') if re.search(r'#\d+ ', l)]
    idx = next((i for i, f in enumerate(frames) if '/src/' in f), None)
    if idx is None or idx > 14:
        continue
    if not any(dispatch.search(f) or foreign.search(f) for f in frames[:idx]):
        ours.append((b.split('\n')[0], frames[:idx + 1]))
for head, frames in ours:
    print(head, file=sys.stderr)
    for f in frames:
        print('   ', re.sub(r'0x[0-9a-f]+ in |\(BuildId.*', '', f), file=sys.stderr)
print(f"{len(blocks)} leak reports from system libraries, {len(ours)} allocated by NPU Chat code")
sys.exit(1 if ours else 0)
PY
echo "PASS"
