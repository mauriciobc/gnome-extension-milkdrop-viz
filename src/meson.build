executable(
  'milkdrop',
  ['main.c', 'audio.c', 'control.c', 'presets.c', 'quarantine.c', 'renderer.c'],
  include_directories: include_directories('.'),
  c_args: renderer_cargs,
  dependencies: renderer_deps,
  install: true,
)
