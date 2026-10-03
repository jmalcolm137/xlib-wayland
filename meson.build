project('xlib-wayland', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=0.60',
  default_options: ['c_std=gnu11', 'warning_level=1', 'buildtype=release'])

cc = meson.get_compiler('c')

wl_dep      = dependency('wayland-client')
wcursor_dep = dependency('wayland-cursor')
xkb_dep     = dependency('xkbcommon')
cairo_dep   = dependency('cairo')
ft_dep      = dependency('freetype2')
fc_dep      = dependency('fontconfig')
pixman_dep  = dependency('pixman-1')
protos_dep  = dependency('wayland-protocols')
threads_dep = dependency('threads')
m_dep       = cc.find_library('m', required: false)

wl_scan = find_program('wayland-scanner')
proto_dir = protos_dep.get_variable(pkgconfig: 'pkgdatadir')
xdg_xml  = proto_dir / 'stable/xdg-shell/xdg-shell.xml'
deco_xml = proto_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'
viewp_xml = proto_dir / 'stable/viewporter/viewporter.xml'

client_protos = [
  custom_target('xdg-shell-client-header',
    input: xdg_xml, output: 'xdg-shell-client-protocol.h',
    command: [wl_scan, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('xdg-shell-client-code',
    input: xdg_xml, output: 'xdg-shell-protocol.c',
    command: [wl_scan, 'private-code', '@INPUT@', '@OUTPUT@']),
  custom_target('xdg-decoration-client-header',
    input: deco_xml, output: 'xdg-decoration-client-protocol.h',
    command: [wl_scan, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('xdg-decoration-client-code',
    input: deco_xml, output: 'xdg-decoration-protocol.c',
    command: [wl_scan, 'private-code', '@INPUT@', '@OUTPUT@']),
  custom_target('viewporter-client-header',
    input: viewp_xml, output: 'viewporter-client-protocol.h',
    command: [wl_scan, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('viewporter-client-code',
    input: viewp_xml, output: 'viewporter-protocol.c',
    command: [wl_scan, 'private-code', '@INPUT@', '@OUTPUT@']),
]

inc = include_directories('src/xlib', 'src/raster', 'src/wayland')

raster_sources = files('src/raster/raster_cairo.c')
wayland_sources = files('src/wayland/wl.c', 'src/wayland/surface.c',
                        'src/wayland/clipboard.c')
xlib_sources = files(
  'src/xlib/aliases.c', 'src/xlib/atom.c', 'src/xlib/color.c', 'src/xlib/compat.c',
  'src/xlib/cursor.c', 'src/xlib/display.c', 'src/xlib/drawing.c', 'src/xlib/event.c',
  'src/xlib/font.c', 'src/xlib/geometry.c', 'src/xlib/image.c',
  'src/xlib/input.c', 'src/xlib/keymap.c', 'src/xlib/region.c',
  'src/xlib/resource.c', 'src/xlib/selection.c', 'src/xlib/window.c',
  'src/xlib/xim.c', 'src/xlib/xkb.c', 'src/xlib/xmb.c', 'src/xlib/xtcompat.c', 'src/xlib/xlibint.c', 'src/xlib/xrandr.c',
  'src/xlib/xresources.c',
  'src/xlib/xshape.c')

# Installed as libX11.so.6 so that -lX11, dlopen("libX11.so.6") and the
# transitive dependency of libXext/libXrender/libcairo all resolve to us.
libx11 = shared_library('X11',
  raster_sources, wayland_sources, xlib_sources, client_protos,
  include_directories: inc,
  dependencies: [wl_dep, wcursor_dep, xkb_dep, cairo_dep, ft_dep, fc_dep,
                 pixman_dep, threads_dep, m_dep],
  version: '6.0.0',
  soversion: '6',
  install: true)

pkg = import('pkgconfig')
pkg.generate(libx11,
  name: 'x11',
  filebase: 'x11',
  description: 'Xlib API implemented on Wayland (Motif/Wayland)',
  subdirs: 'X11')

# ---------------------------------------------------------------- tests/tools

if get_option('tests')
  test_draw = executable('test_draw', 'tests/test_draw.c',
    include_directories: inc,
    dependencies: [wl_dep],
    link_with: libx11,
    install: false)

  test('unit', executable('test_core', 'tests/test_core.c',
      include_directories: inc, link_with: libx11))

  # Motif-free reproduction of the menu item-selection path: override-redirect
  # popup + XGrabPointer(owner_events=True) + item child windows.  Driven by
  # tests/popup.input against the headless compositor.
  executable('test_popup', 'tests/test_popup.c',
    include_directories: inc,
    dependencies: [wl_dep],
    link_with: libx11,
    install: false)

  # Format-32 property semantics (long array) and mask-based event selection.
  # Needs a compositor, so run-tests.sh drives it rather than meson test().
  executable('test_props', 'tests/test_props.c',
    include_directories: inc,
    dependencies: [wl_dep],
    link_with: libx11,
    install: false)

  # Clipboard bridge tests: an X client and a plain Wayland peer, run against
  # the same headless compositor by run-tests.sh.
  executable('clip_x', 'tests/clip_x.c',
    include_directories: inc,
    dependencies: [wl_dep],
    link_with: libx11,
    install: false)

  executable('clip_wl', 'tests/clip_wl.c',
    include_directories: inc,
    dependencies: [wl_dep],
    install: false)
endif

if get_option('tools')
  server_protos = [
    custom_target('xdg-shell-server-header',
      input: xdg_xml, output: 'xdg-shell-server.h',
      command: [wl_scan, 'server-header', '@INPUT@', '@OUTPUT@']),
    custom_target('xdg-shell-server-code',
      input: xdg_xml, output: 'xdg-shell-protocol-server.c',
      command: [wl_scan, 'private-code', '@INPUT@', '@OUTPUT@']),
    custom_target('xdg-decoration-server-header',
      input: deco_xml, output: 'xdg-decoration-server.h',
      command: [wl_scan, 'server-header', '@INPUT@', '@OUTPUT@']),
    custom_target('xdg-decoration-server-code',
      input: deco_xml, output: 'xdg-decoration-protocol-server.c',
      command: [wl_scan, 'private-code', '@INPUT@', '@OUTPUT@']),
    custom_target('viewporter-server-header',
      input: viewp_xml, output: 'viewporter-server.h',
      command: [wl_scan, 'server-header', '@INPUT@', '@OUTPUT@']),
    custom_target('viewporter-server-code',
      input: viewp_xml, output: 'viewporter-protocol-server.c',
      command: [wl_scan, 'private-code', '@INPUT@', '@OUTPUT@']),
  ]
  wl_server_dep = dependency('wayland-server')
  executable('headless-compositor',
    'tools/headless-compositor.c', server_protos,
    include_directories: inc,
    dependencies: [wl_server_dep, xkb_dep, dependency('libpng')],
    install: false)
endif
