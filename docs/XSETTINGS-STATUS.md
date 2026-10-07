# XSETTINGS

GDK's X11 backend reads many GTK settings — theme, font, icon theme, Xft
hinting/DPI, double-click and drag thresholds, toolbar style, … — from the
**XSETTINGS** protocol: the owner of `_XSETTINGS_S<screen>` exposes a
`_XSETTINGS_SETTINGS` property on its manager window, and GDK watches that
window for changes. There is no settings daemon inside a shim process, so
without this every GTK2 client fell back to gtkrc and built-in defaults.

The shim now **is** the manager. `src/xlib/xsettings.c` creates a manager
window, owns `_XSETTINGS_S0`, and publishes a settings list built from a small
config file; the `_XSETTINGS_SETTINGS` property is re-published with a new
serial when the file changes, and GDK clients update from the `PropertyNotify`.

## Config file

First match wins:

1. `$XLIB_WAYLAND_XSETTINGS`
2. `$XDG_CONFIG_HOME/xlib-wayland/xsettings`
3. `$HOME/.config/xlib-wayland/xsettings`

One setting per line; `#` starts a comment:

```
Net/ThemeName     = TraditionalOk
Gtk/FontName      = DejaVu Sans 11
Net/IconThemeName = mate
Gtk/ToolbarStyle  = 3
Xft/DPI           = 98304
```

A value is an integer when it is entirely a number (base 0), otherwise a
string; put it in double quotes to force a string. **Only settings present in
the file are published**, so an empty or absent file changes nothing — GTK2
keeps its normal defaults. Names are the XSettings names GDK maps, e.g.
`Net/ThemeName`, `Gtk/FontName`, `Gtk/IconSizes`, `Gtk/ToolbarStyle`,
`Xft/Antialias`, `Xft/Hinting`, `Xft/HintStyle`, `Xft/RGBA`, `Xft/DPI`,
`Net/DoubleClickTime`, `Net/DndDragThreshold`.

## Behaviour and limits

* `scripts/test-settings.sh` in `gtk2-wayland` proves a real `GtkSettings`
  reports the configured theme/font/icon.
* The manager is created without disturbing the sequential XID space, so the
  shim's window ids are unchanged.
* A real settings daemon running in the same process can still take the
  selection; the shim only re-asserts ownership when nobody else holds it.
* **Live reload is event-driven**: the shim stats the config file while the
  event pump runs, so a change is applied on the next event the client
  processes (in practice immediately for an interactive app; an idle app that
  receives nothing may wait). A periodic wakeup from the repeat-deadline thread
  would make this unconditional.
* Values come from the config file. Bridging a daemon's settings across shim
  processes — republish its `_XSETTINGS_SETTINGS` through the selection broker
  / `smprops` — and reading dconf/GSettings directly are both open.
