cc = meson.get_compiler('c')

test_glib_dep = dependency('glib-2.0', version: '>=2.76')
test_gtk_dep = dependency('gtk4', version: '>=4.10')

# Match milkdrop link flags when tests compile src/audio.c or src/renderer.c with renderer_cargs.
test_src_link_deps = [glib_dep, gtk_dep]
if have_projectm
  test_src_link_deps += [projectm_dep, projectm_playlist_dep]
endif
if have_pipewire
  test_src_link_deps += [pw_dep, spa_dep]
endif

# Evita corrida entre binários de teste paralelos no mesmo milkdrop-0.sock (ver control_socket_path_for_monitor).
control_socket_test_env = ['MILKDROP_TEST_ISOLATE_SOCKET=1']

ring_buffer_test = executable(
  'test-ring-buffer',
  'test_ring_buffer.c',
  include_directories: include_directories('..', '../src'),
  dependencies: [test_glib_dep, test_gtk_dep],
)

test('ring-buffer', ring_buffer_test)

# projectM antes do teste FBO simples: em alguns drivers o FBO verde deixa o GL
# num estado em que o readback do projectM falha se correr logo em seguida.
if have_projectm
  m_dep = cc.find_library('m', required: true)
  gtk_glarea_projectm_deps = [glib_dep, gtk_dep, epoxy_dep, projectm_dep, projectm_playlist_dep, m_dep]
  gtk_glarea_projectm_test = executable(
    'test-gtk-glarea-projectm',
    'test_gtk_glarea_projectm.c',
    include_directories: include_directories('..', '../src'),
    c_args: renderer_cargs,
    dependencies: gtk_glarea_projectm_deps,
  )
  _pm_test_preset = join_paths(
    meson.project_source_root(),
    'reference_codebases',
    'projectm',
    'presets',
    'tests',
    '100-square.milk',
  )
  test(
    'gtk-glarea-projectm',
    gtk_glarea_projectm_test,
    env: [
      'MILKDROP_PM_TEST_PRESET=' + _pm_test_preset,
      'MILKDROP_FORCE_GL_API=1',
    ],
    is_parallel: false,
    timeout: 60,
  )
endif

# GtkGLArea internal FBO: clear + glReadPixels (same attach path as milkdrop).
gtk_glarea_fbo_deps = [glib_dep, gtk_dep, epoxy_dep]
gtk_glarea_fbo_test = executable(
  'test-gtk-glarea-fbo',
  'test_gtk_glarea_fbo.c',
  include_directories: include_directories('..', '../src'),
  dependencies: gtk_glarea_fbo_deps,
)

test(
  'gtk-glarea-fbo',
  gtk_glarea_fbo_test,
  is_parallel: false,
  timeout: 60,
  # Sem DISPLAY o teste faz g_test_skip (não falha o CI).
)

backend_test = executable(
  'test-backends',
  ['test_backends.c', '../src/audio.c', '../src/audio_recovery.c', '../src/control.c', '../src/renderer.c'],
  include_directories: include_directories('..', '../src'),
  c_args: renderer_cargs,
  dependencies: test_src_link_deps,
)

test('backends', backend_test, env: control_socket_test_env)

control_protocol_test = executable(
  'test-control-protocol',
  ['test_control_protocol.c', '../src/control.c'],
  include_directories: include_directories('..', '../src'),
  dependencies: [test_glib_dep, test_gtk_dep],
)

test('control-protocol', control_protocol_test)

preset_scan_test = executable(
  'test-presets',
  ['test_presets.c', '../src/presets.c'],
  include_directories: include_directories('..', '../src'),
  dependencies: [test_glib_dep, test_gtk_dep],
)

test('presets', preset_scan_test)

cream_preset_scan_test = executable(
  'test-cream-preset-dir-scan',
  ['test_cream_preset_dir_scan.c', '../src/presets.c'],
  include_directories: include_directories('..', '../src'),
  dependencies: [test_glib_dep, test_gtk_dep],
)

test(
  'cream-preset-dir-scan',
  cream_preset_scan_test,
  env: ['MILKDROP_TEST_PRESET_DIR=/usr/local/share/projectM/presets/cream-of-the-crop'],
  timeout: 300,
)

audio_alignment_test = executable(
  'test-audio-alignment',
  ['test_audio_alignment.c', '../src/audio.c', '../src/audio_recovery.c'],
  include_directories: include_directories('..', '../src'),
  c_args: renderer_cargs,
  dependencies: test_src_link_deps,
)

test('audio-alignment', audio_alignment_test)

render_pipeline_test = executable(
  'test-render-pipeline',
  ['test_render_pipeline.c', '../src/renderer.c'],
  include_directories: include_directories('..', '../src'),
  c_args: renderer_cargs,
  dependencies: test_src_link_deps,
)

test('render-pipeline', render_pipeline_test)

tick_state_test = executable(
  'test-tick-state-machine',
  ['test_tick_state_machine.c', '../src/presets.c'],
  include_directories: include_directories('..', '../src'),
  dependencies: [test_glib_dep, test_gtk_dep],
)

test('tick-state-machine', tick_state_test)

control_state_test = executable(
  'test-control-state-flow',
  ['test_control_state_flow.c', '../src/control.c'],
  include_directories: include_directories('..', '../src'),
  dependencies: [test_glib_dep, test_gtk_dep],
)

test('control-state-flow', control_state_test, env: control_socket_test_env)

presets_edge_test = executable(
  'test-presets-edge-cases',
  ['test_presets_edge_cases.c', '../src/presets.c'],
  include_directories: include_directories('..', '../src'),
  dependencies: [test_glib_dep, test_gtk_dep],
)

test('presets-edge-cases', presets_edge_test)

startup_async_gate_test = executable(
  'test-startup-async-gate',
  'test_startup_async_gate.c',
  include_directories: include_directories('..', '../src'),
  dependencies: [test_glib_dep],
)

test('startup-async-gate', startup_async_gate_test)

audio_recovery_test = executable(
  'test-audio-recovery',
  ['test_audio_recovery.c', '../src/audio_recovery.c'],
  include_directories: include_directories('..', '../src'),
  dependencies: [test_glib_dep, test_gtk_dep],
)

test('audio-recovery', audio_recovery_test)

pause_policy_test = executable(
  'test-pause-policy',
  ['test_pause_policy.c', '../src/control.c'],
  include_directories: include_directories('..', '../src'),
  dependencies: [test_glib_dep, test_gtk_dep],
)

test('pause-policy', pause_policy_test, env: control_socket_test_env)

preset_quarantine_test = executable(
  'test-preset-quarantine',
  ['test_preset_quarantine.c', '../src/quarantine.c'],
  include_directories: include_directories('..', '../src'),
  dependencies: [test_glib_dep, test_gtk_dep],
)

test('preset-quarantine', preset_quarantine_test)

state_persistence_test = executable(
  'test-state-persistence',
  ['test_state_persistence.c', '../src/control.c'],
  include_directories: include_directories('..', '../src'),
  dependencies: [test_glib_dep, test_gtk_dep],
)

test('state-persistence', state_persistence_test, env: control_socket_test_env)

python3 = find_program('python3')
test(
  'scaffold-validation',
  python3,
  args: [files('validate_scaffold.py'), meson.project_source_root()],
)

# Smoke-run: built milkdrop with --verbose and playlist dir under /usr/local (common install prefix).
# Skips (exit 77) if the directory or a display is missing — does not fail CI.
if have_projectm
  bash = find_program('bash')
  test(
    'milkdrop-binary-verbose-local-presets',
    bash,
    args: [
      files('run_milkdrop_verbose_local_presets.sh'),
      milkdrop_exe,
    ],
    depends: milkdrop_exe,
    env: control_socket_test_env + ['MILKDROP_FORCE_GL_API=1'],
    is_parallel: false,
    timeout: 120,
  )
endif

# Reference renderer for blur validation tests
if have_projectm
  m_dep = cc.find_library('m', required: true)
  reference_renderer = executable(
    'reference_renderer',
    'reference_renderer.c',
    dependencies: [glib_dep, gtk_dep, epoxy_dep, projectm_dep, projectm_playlist_dep, m_dep],
    include_directories: include_directories('..', '../src'),
    install: false
  )
endif