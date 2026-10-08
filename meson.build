project('npu-chat', 'c',
  version: '0.5.0',
  default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])

gnome = import('gnome')

add_project_arguments('-DNPU_CHAT_VERSION="@0@"'.format(meson.project_version()), language: 'c')

deps = [
  dependency('gtk4', version: '>= 4.14'),
  dependency('libadwaita-1', version: '>= 1.5'),
  dependency('libsoup-3.0'),
  dependency('json-glib-1.0'),
]

resources = gnome.compile_resources('resources',
  'data/npu-chat.gresource.xml',
  source_dir: 'data')

executable('npu-chat',
  'src/main.c', 'src/flm.c', 'src/markdown.c', 'src/i18n.c',
  'src/sysinfo.c', 'src/catalog.c', 'src/store.c', 'src/power.c',
  'src/assistants.c', 'src/templates.c', 'src/updater.c',
  resources,
  dependencies: deps,
  install: true)

install_data('data/io.github.vezzulab.NpuChat.desktop',
  install_dir: get_option('datadir') / 'applications')
install_data('data/io.github.vezzulab.NpuChat.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
