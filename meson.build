project('npu-chat', 'c',
  version: '0.7.0',
  default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])

gnome = import('gnome')

add_project_arguments('-DNPU_CHAT_VERSION="@0@"'.format(meson.project_version()), language: 'c')

deps = [
  dependency('gtk4', version: '>= 4.14'),
  dependency('libadwaita-1', version: '>= 1.5'),
  dependency('libsoup-3.0'),
  dependency('json-glib-1.0'),
  meson.get_compiler('c').find_library('m'),
]

poppler = dependency('poppler-glib', required: false)
if poppler.found()
  deps += poppler
  add_project_arguments('-DHAVE_POPPLER=1', language: 'c')
endif

resources = gnome.compile_resources('resources',
  'data/npu-chat.gresource.xml',
  source_dir: 'data')

executable('npu-chat',
  'src/main.c', 'src/flm.c', 'src/markdown.c', 'src/i18n.c',
  'src/sysinfo.c', 'src/catalog.c', 'src/store.c', 'src/power.c',
  'src/assistants.c', 'src/templates.c', 'src/updater.c', 'src/rag.c', 'src/net.c', 'src/skills.c',
  resources,
  dependencies: deps,
  install: true)

install_data('data/io.github.vezzulab.NpuChat.desktop',
  install_dir: get_option('datadir') / 'applications')
install_data('data/io.github.vezzulab.NpuChat.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')

if get_option('tests')
  executable('test-rag',
    'tests/test-rag.c', 'src/rag.c',
    dependencies: deps)
  executable('test-skills',
    'tests/test-skills.c', 'src/skills.c', 'src/net.c', 'src/sysinfo.c', 'src/i18n.c',
    dependencies: deps)
endif
