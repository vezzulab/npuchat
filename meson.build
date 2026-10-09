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
  'src/assistants.c', 'src/templates.c', 'src/updater.c', 'src/rag.c', 'src/net.c', 'src/skills.c', 'src/copilotkey.c', 'src/calendar.c', 'src/calendar-ui.c', 'src/calendar-grid.c', 'src/calendar-quick.c', 'src/calendar-alerts.c',
  'src/calendar-settings.c', 'src/calendar-ics.c', 'src/calendar-print.c', 'src/calendar-subscribe.c', 'src/calendar-tools.c', 'src/calendar-caldav.c', 'src/calendar-accounts.c',
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
  executable('test-calendar',
    'tests/test-calendar.c', 'src/calendar.c',
    dependencies: deps)
  executable('test-settings', 'tests/test-settings.c', 'src/calendar-settings.c', dependencies: deps)
  executable('test-ics', 'tests/test-ics.c', 'src/calendar-ics.c', 'src/calendar.c', dependencies: deps)
  executable('test-alerts', 'tests/test-alerts.c', 'src/calendar-alerts.c', 'src/calendar.c', 'src/i18n.c', dependencies: deps)
  executable('test-subscribe', 'tests/test-subscribe.c', 'src/calendar-subscribe.c', 'src/calendar-ics.c', 'src/calendar.c', 'src/net.c', dependencies: deps)
  executable('test-caldav', 'tests/test-caldav.c', 'src/calendar-caldav.c', 'src/calendar-ics.c', 'src/calendar.c', 'src/net.c', dependencies: deps)
  executable('test-tools', 'tests/test-tools.c', 'src/calendar-tools.c', 'src/calendar-quick.c', 'src/calendar-settings.c', 'src/calendar.c', 'src/i18n.c', dependencies: deps)
  executable('test-quick',
    'tests/test-quick.c', 'src/calendar-quick.c', 'src/calendar.c',
    dependencies: deps)
  executable('test-skills',
    'tests/test-skills.c', 'src/skills.c', 'src/net.c', 'src/sysinfo.c', 'src/i18n.c', 'src/calendar.c', 'src/calendar-quick.c',
    'src/calendar-tools.c', 'src/calendar-settings.c',
    dependencies: deps)
endif
