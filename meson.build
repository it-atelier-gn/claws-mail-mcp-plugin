project('mcp_plugin', 'c',
  version: '0.1.0',
  meson_version: '>=0.56.0',
  default_options: ['c_std=gnu99', 'buildtype=debugoptimized'],
)

cc = meson.get_compiler('c')
is_windows = host_machine.system() == 'windows'

add_project_arguments(['-Wall', '-Wextra', '-Wno-unused-parameter'], language: 'c')
if is_windows
  add_project_arguments('-D_CRT_RAND_S', language: 'c')
endif

glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gobject_dep = dependency('gobject-2.0')
gtk_dep = dependency('gtk+-3.0')
claws_dep_full = dependency('claws-mail')
claws_dep = claws_dep_full.partial_dependency(compile_args: true, includes: true)

plugin_deps = [glib_dep, gio_dep, gobject_dep, gtk_dep, claws_dep]
plugin_link_args = []

if is_windows
  claws_libdir = claws_dep_full.get_variable(pkgconfig: 'libdir')
  plugin_deps += cc.find_library('claws-mail', dirs: [claws_libdir])
  plugin_link_args += ['-Wl,--enable-auto-import']
endif

sources = files(
  'src/plugin.c',
  'src/mcp_server.c',
  'src/mcp_http.c',
  'src/mcp_json.c',
  'src/mcp_tools.c',
)

claws_plugindir = claws_dep_full.get_variable(pkgconfig: 'plugindir')

shared_module('mcp_plugin', sources,
  dependencies: plugin_deps,
  link_args: plugin_link_args,
  name_prefix: '',
  install: true,
  install_dir: claws_plugindir,
)

test_exe = executable('test_json',
  ['tests/test_json.c', 'src/mcp_json.c'],
  include_directories: include_directories('src'),
  dependencies: [glib_dep],
)
test('json', test_exe)

cppcheck = find_program('cppcheck', required: false)
if cppcheck.found()
  run_target('cppcheck',
    command: [cppcheck,
      '--enable=warning,performance,portability', '--std=c99',
      '--inline-suppr', '--suppress=missingIncludeSystem', '--suppress=missingInclude',
      '-DG_GSIZE_FORMAT="zu"', '-DG_GINT64_FORMAT="lld"',
      '--error-exitcode=1', '-q',
      '-I' + meson.current_source_dir() / 'src',
      meson.current_source_dir() / 'src',
    ],
  )
endif

clang_tidy = find_program('clang-tidy', required: false)
if clang_tidy.found()
  run_target('tidy',
    command: [clang_tidy, '-p', meson.current_build_dir()] + sources,
  )
endif
