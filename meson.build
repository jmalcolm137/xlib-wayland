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
xrender_dep = dependency('xrender')
m_dep       = cc.find_library('m', required: false)

wl_scan = find_program('wayland-scanner')
proto_dir = protos_dep.get_variable(pkgconfig: 'pkgdatadir')
xdg_xml  = proto_dir / 'stable/xdg-shell/xdg-shell.xml'
deco_xml = proto_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'
viewp_xml = proto_dir / 'stable/viewporter/viewporter.xml'
textinput_xml = proto_dir / 'unstable/text-input/text-input-unstable-v3.xml'
primary_xml = proto_dir / 'unstable/primary-selection/primary-selection-unstable-v1.xml'

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
  # zwp_text_input_v3: the client side of the Wayland input-method protocol.
  # XIM (src/xlib/xim.c) is a real bridge onto this, so a CDE/Motif or GTK2
  # text widget gets preedit and commit from a running IME (e.g. ibus) via the
  # compositor.
  custom_target('text-input-client-header',
    input: textinput_xml, output: 'text-input-unstable-v3-client-protocol.h',
    command: [wl_scan, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('text-input-client-code',
    input: textinput_xml, output: 'text-input-protocol.c',
    command: [wl_scan, 'private-code', '@INPUT@', '@OUTPUT@']),
  # PRIMARY selection (select-to-paste) on top of the primary-selection
  # protocol; the X PRIMARY selection is bridged in clipboard.c.
  custom_target('primary-selection-client-header',
    input: primary_xml, output: 'primary-selection-unstable-v1-client-protocol.h',
    command: [wl_scan, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('primary-selection-client-code',
    input: primary_xml, output: 'primary-selection-protocol.c',
    command: [wl_scan, 'private-code', '@INPUT@', '@OUTPUT@']),
]

inc = include_directories('src/xlib', 'src/raster', 'src/wayland', 'src/xft')

raster_sources = files('src/raster/raster_cairo.c')
wayland_sources = files('src/wayland/wl.c', 'src/wayland/surface.c',
                        'src/wayland/clipboard.c', 'src/wayland/wakeup.c')
xlib_sources = files(
  'src/xlib/aliases.c', 'src/xlib/atom.c', 'src/xlib/color.c', 'src/xlib/compat.c',
  'src/xlib/cursor.c', 'src/xlib/display.c', 'src/xlib/drawing.c', 'src/xlib/event.c',
  'src/xlib/font.c', 'src/xlib/geometry.c', 'src/xlib/image.c',
  'src/xlib/input.c', 'src/xlib/keymap.c', 'src/xlib/region.c',
  'src/xlib/resource.c', 'src/xlib/selection.c', 'src/xlib/window.c',
  'src/xlib/xim.c', 'src/xlib/xi2.c', 'src/xlib/xkb.c', 'src/xlib/xmb.c', 'src/xlib/xtcompat.c', 'src/xlib/xlibint.c',
  'src/xlib/xresources.c', 'src/xlib/xftbridge.c', 'src/xlib/render.c',
  'src/xlib/xshape.c', 'src/xlib/broker.c', 'src/xlib/smprops.c', 'src/xlib/dnd.c',
  'src/xlib/xsettings.c', 'src/xlib/xdnd.c', 'src/xlib/xfixes.c')

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
# The .pc versions are what consumers' configure scripts compare against.
# Xlib's real ABI/version is 1.x; many builds require `x11` and check nothing
# more, but reporting the project version (0.1.0) misleads version-gated checks
# and tools that display it.  Report the Xlib version we are ABI-compatible
# with.
pkg.generate(libx11,
  name: 'x11',
  filebase: 'x11',
  version: '1.8.13',
  description: 'Xlib API implemented on Wayland (Motif/Wayland)',
  subdirs: 'X11')

# An Xft built on the shim's own raster/font path.  There is no Render
# extension, and none is needed: the shim already rasterises text with
# FreeType/fontconfig/cairo, so libXft.so.2 here draws through the mw_xft_*
# bridge instead of uploading glyphs to a server.  Installed next to libX11 so
# that -lXft resolves to it.
libxft = shared_library('Xft',
  'src/xft/xft.c',
  include_directories: inc,
  dependencies: [fc_dep, ft_dep, cairo_dep, xrender_dep, m_dep],
  link_with: libx11,
  version: '2.0.0',
  soversion: '2',
  install: true)

# libXft's real version line is 2.3.x; GTK2 requires `xft >= 2.0.0`, so a
# .pc version of 0.1.0 (the project version) fails its configure outright.
pkg.generate(libxft,
  name: 'xft',
  filebase: 'xft',
  version: '2.3.6',
  description: 'Xft over the Motif/Wayland raster font path',
  requires: 'x11',
  subdirs: 'X11')

# RandR lives in its own library, as in Xorg.  We advertise 1.4 (the single
# monitor covers the wl_output; XRRGetOutputInfo reports its physical size, so
# GDK derives a real DPI).  Installing libXrandr.so.2 here shadows the host's
# -- the same soname -- so GTK2/GDK use this facade instead of issuing RandR
# wire requests that we do not implement (which made XRRQueryVersion return 0
# and left GDK without monitor millimetres).
libxrandr = shared_library('Xrandr',
  'src/xlib/xrandr.c',
  include_directories: inc,
  dependencies: [pixman_dep, m_dep],
  link_with: libx11,
  version: '2.0.2',
  soversion: '2',
  install: true)

pkg.generate(libxrandr,
  name: 'xrandr',
  filebase: 'xrandr',
  version: '1.4.0',
  description: 'X Resize and Rotate (RandR 1.4) over Wayland',
  requires: 'x11',
  subdirs: 'X11')

# libXfixes: GDK uses XFixes for clipboard owner-change notifications,
# regions and cursor changes.  Shadowing the host's libXfixes.so.3 (same
# soname) keeps GDK from sending XFixes protocol we do not speak; the work is
# in src/xlib/xfixes.c.
libxfixes = shared_library('Xfixes',
  'src/xfixes/xfixes.c',
  include_directories: inc,
  link_with: libx11,
  version: '6.0.2',
  soversion: '3',
  install: true)

pkg.generate(libxfixes,
  name: 'xfixes',
  filebase: 'xfixes',
  version: '6.0.2',
  description: 'XFixes over the Motif/Wayland shim',
  requires: 'x11',
  subdirs: 'X11')

# libXcursor: GDK loads themed cursors through it.  Named/shape lookups become
# Wayland cursor-theme lookups and image cursors become ARGB Wayland cursor
# buffers (src/xlib/cursor.c).
libxcursor = shared_library('Xcursor',
  'src/xcursor/xcursor.c',
  include_directories: inc,
  link_with: libx11,
  version: '1.2.3',
  soversion: '1',
  install: true)

pkg.generate(libxcursor,
  name: 'xcursor',
  filebase: 'xcursor',
  version: '1.2.3',
  description: 'X cursor loading over the Motif/Wayland shim',
  requires: 'x11',
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

  # XIM bridge: an input context that receives a scripted preedit and commit
  # from the headless compositor's text-input server (tests/test_xim.c).
  executable('test_xim', 'tests/test_xim.c',
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

  executable('clip_wl', 'tests/clip_wl.c', client_protos,
    include_directories: inc,
    dependencies: [wl_dep],
    install: false)

  # X XDND drop target for the XDND bridge test.
  executable('xdnd_x', 'tests/xdnd_x.c',
    include_directories: inc,
    link_with: libx11,
    install: false)

  # XFIXES / Xcursor facades: selection-owner notification + themed cursors.
  executable('xfixes_x', 'tests/xfixes_x.c',
    include_directories: inc,
    dependencies: [wl_dep],
    link_with: [libx11, libxfixes, libxcursor],
    install: false)

  # X XDND drag source (owns XdndSelection) for the X->Wayland XDND path.
  executable('xdnd_src_x', 'tests/xdnd_src_x.c',
    include_directories: inc,
    link_with: libx11,
    install: false)

  # Wayland drop target for the X->Wayland XDND path: maps a toplevel, accepts
  # the drag and receives the data the X source serves through the shim.
  executable('drag_target_wl', 'tests/drag_target_wl.c', client_protos,
    include_directories: inc,
    dependencies: [wl_dep],
    install: false)

  # Drag source for the Motif DnD bridge: a Wayland toplevel that starts a
  # drag on button press, so a CDE drop target can be exercised with the
  # virtual-pointer tools.
  executable('dnd_wl', 'tests/dnd_wl.c', client_protos,
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
    custom_target('text-input-server-header',
      input: textinput_xml, output: 'text-input-unstable-v3-server-protocol.h',
      command: [wl_scan, 'server-header', '@INPUT@', '@OUTPUT@']),
    custom_target('text-input-server-code',
      input: textinput_xml, output: 'text-input-protocol-server.c',
      command: [wl_scan, 'private-code', '@INPUT@', '@OUTPUT@']),
    custom_target('primary-selection-server-header',
      input: primary_xml, output: 'primary-selection-unstable-v1-server-protocol.h',
      command: [wl_scan, 'server-header', '@INPUT@', '@OUTPUT@']),
    custom_target('primary-selection-server-code',
      input: primary_xml, output: 'primary-selection-protocol-server.c',
      command: [wl_scan, 'private-code', '@INPUT@', '@OUTPUT@']),
  ]
  wl_server_dep = dependency('wayland-server')
  executable('headless-compositor',
    'tools/headless-compositor.c', server_protos,
    include_directories: inc,
    dependencies: [wl_server_dep, xkb_dep, dependency('libpng')],
    install: false)
endif
