recgo_resources = gnome.compile_resources(
  'recgo-resources',
  'recgo.gresource.xml',
  source_dir: meson.current_source_dir(),
)

install_data(
  'io.github.Antux912012.RecGo.desktop.in',
  rename: 'io.github.Antux912012.RecGo.desktop',
  install_dir: get_option('datadir') / 'applications',
)

install_data(
  'io.github.Antux912012.RecGo.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas',
)

install_data(
  'io.github.Antux912012.RecGo.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)

foreach size : ['32', '48', '64', '128', '256', '512']
  install_data(
    'icons' / 'io.github.Antux912012.RecGo_@0@x@0@.png'.format(size),
    rename: 'io.github.Antux912012.RecGo.png',
    install_dir: get_option('datadir') / 'icons' / 'hicolor' / '@0@x@0@'.format(size) / 'apps',
  )
endforeach

install_data(
  'icons' / 'io.github.Antux912012.RecGo.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)
