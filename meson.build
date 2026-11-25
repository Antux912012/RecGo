project(
  'recgo',
  'c', 'vala',
  version: '0.3.1',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.62.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'optimization=2',
  ],
)

gnome = import('gnome')
cc = meson.get_compiler('c')
valac = meson.get_compiler('vala')

gtk4_dep = dependency('gtk4', version: '>= 4.14')
adw_dep = dependency('libadwaita-1', version: '>= 1.5')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
gst_dep = dependency('gstreamer-1.0', version: '>= 1.20')
gst_app_dep = dependency('gstreamer-app-1.0', version: '>= 1.20')
gst_video_dep = dependency('gstreamer-video-1.0', version: '>= 1.20')

m_dep = cc.find_library('m', required: false)
posix_dep = valac.find_library('posix')
recgo_gst_vapi = valac.find_library('recgo-gst', dirs: meson.current_source_dir() / 'src' / 'gst')

subdir('data')

conf_data = configuration_data()
conf_data.set('VERSION', meson.project_version())
config_vala = configure_file(
  input: 'src/config.vala.in',
  output: 'config.vala',
  configuration: conf_data,
)

c_sources = files(
  'src/gst/recgo-codecs.c',
  'src/gst/recgo-devices.c',
  'src/gst/recgo-video-fx.c',
  'src/gst/recgo-preview.c',
  'src/gst/recgo-recorder.c',
  'src/gst/recgo-editor.c',
)

vala_sources = files(
  'src/main.vala',
  'src/application.vala',
  'src/safe-settings.vala',
  'src/window.vala',
  'src/preview-widget.vala',
  'src/preferences-dialog.vala',
  'src/portal.vala',
  'src/timeline-widget.vala',
  'src/editor-view.vala',
)

executable(
  'recgo',
  [c_sources, vala_sources, config_vala, recgo_resources],
  include_directories: include_directories('src/gst'),
  dependencies: [
    gtk4_dep,
    adw_dep,
    gio_dep,
    gio_unix_dep,
    gst_dep,
    gst_app_dep,
    gst_video_dep,
    m_dep,
    posix_dep,
    recgo_gst_vapi,
  ],
  vala_args: [
    '--target-glib=2.76',
    '--vapidir=' + (meson.current_source_dir() / 'src' / 'gst'),
  ],
  c_args: [
    '-Wno-unused-parameter',
    '-Wno-incompatible-pointer-types',
  ],
  install: true,
)

test_gst = executable(
  'test-gst',
  files(
    'tests/test-gst.c',
    'src/gst/recgo-codecs.c',
    'src/gst/recgo-devices.c',
    'src/gst/recgo-video-fx.c',
    'src/gst/recgo-editor.c',
  ),
  include_directories: include_directories('src/gst'),
  dependencies: [gio_dep, gst_dep, gst_video_dep, m_dep],
)
test('gst-core', test_gst)

gnome.post_install(
  glib_compile_schemas: true,
  gtk_update_icon_cache: true,
  update_desktop_database: true,
)
