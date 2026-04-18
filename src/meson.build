milkdrop_exe = executable(
  'milkdrop',
  ['main.c', 'audio.c', 'audio_recovery.c', 'control.c', 'presets.c', 'quarantine.c', 'renderer.c', 'startup_gate.c'],
  include_directories: include_directories('.'),
  c_args: renderer_cargs,
  dependencies: renderer_deps,
  install: true,
)
