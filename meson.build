project(
  'recgo',
  'rust',
  version: '0.1.6',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.59.0'
)

dependency('gtk4', version: '>= 4.14')
dependency('libadwaita-1', version: '>= 1.5')
dependency('gstreamer-1.0', version: '>= 1.20')
dependency('gstreamer-video-1.0', version: '>= 1.20')
dependency('gstreamer-gl-1.0', version: '>= 1.20')

cargo = find_program('cargo', required: true)

custom_target(
  'cargo-build',
  build_by_default: true,
  build_always_stale: true,
  output: ['recgo'],
  console: true,
  command: [
    cargo, 'build',
    '--manifest-path', meson.project_source_root() / 'Cargo.toml'
  ]
)
