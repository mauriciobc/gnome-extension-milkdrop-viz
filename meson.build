project(
  'milkdrop',
  'c',
  version: '0.1.0',
  meson_version: '>=0.63.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')

gtk_dep = dependency('gtk4', version: '>=4.10')
glib_dep = dependency('glib-2.0', version: '>=2.76')
gio_dep = dependency('gio-2.0', version: '>=2.76')
epoxy_dep = dependency('epoxy', version: '>=1.5')
egl_dep = dependency('egl')

projectm_dep = dependency('projectM-4', required: get_option('projectm'))
projectm_playlist_dep = dependency('projectM-4-playlist', required: get_option('projectm'))
pw_dep = dependency('libpipewire-0.3', required: get_option('pipewire'))
spa_dep = dependency('libspa-0.2', required: get_option('pipewire'))

have_projectm = projectm_dep.found() and projectm_playlist_dep.found()
have_pipewire = pw_dep.found() and spa_dep.found()

renderer_cargs = []
if have_projectm
  renderer_cargs += ['-DHAVE_PROJECTM=1']
else
  renderer_cargs += ['-DHAVE_PROJECTM=0']
  warning('Building without libprojectM support. Renderer will run in placeholder mode.')
endif

if have_pipewire
  renderer_cargs += ['-DHAVE_PIPEWIRE=1']
else
  renderer_cargs += ['-DHAVE_PIPEWIRE=0']
  warning('Building without PipeWire support. Audio capture is disabled.')
endif

renderer_deps = [gtk_dep, glib_dep, gio_dep, epoxy_dep, egl_dep]
if have_projectm
  renderer_deps += [projectm_dep, projectm_playlist_dep]
endif
if have_pipewire
  renderer_deps += [pw_dep, spa_dep]
endif

subdir('src')
subdir('tests')

install_data(
  'data/org.gnome.shell.extensions.milkdrop.gschema.xml',
  install_dir: join_paths(get_option('datadir'), 'glib-2.0', 'schemas'),
)

extension_dir = join_paths(
  get_option('datadir'),
  'gnome-shell',
  'extensions',
  'milkdrop@mauriciobc.github.io',
)

install_data('extension/milkdrop@mauriciobc.github.io/metadata.json', install_dir: extension_dir)
install_data('extension/milkdrop@mauriciobc.github.io/extension.js', install_dir: extension_dir)
install_data('extension/milkdrop@mauriciobc.github.io/prefs.js', install_dir: extension_dir)
install_data('extension/milkdrop@mauriciobc.github.io/constants.js', install_dir: extension_dir)
install_data('extension/milkdrop@mauriciobc.github.io/controlClient.js', install_dir: extension_dir)
