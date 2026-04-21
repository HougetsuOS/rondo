# rondo

A tiling window manager for X11 that reproduces the visual style of the
Motif Window Manager (mwm / CDE).

Rondo combines keyboard-driven tiling with the classic 1990s Motif
aesthetic — 3D beveled borders, stretcher corners, mwm-style window
frames, and a CDE-like icon bar. Frame drawing algorithms are ported
directly from mwm's `WmGraphics.c`.

## Features

**Tiling**
- Master-stack layout (one master + vertically stacked clients)
- Binary tree layout (new windows split the focused cell, alternating
  vertical and horizontal splits per depth)
- Runtime layout switching (`Alt+Shift+t` by default)
- Adjustable master ratio (`Alt+h` / `Alt+l`)
- Floating mode per window, fullscreen toggle

**Window Frames**
- 3D beveled borders with asymmetric per-edge widths (mwm `WmRECESSED`)
- L-shaped stretcher corners ported from mwm `StretcherCorner()`
- Title bar with focus/unfocus color change
- Close, maximize, minimize, float buttons (right-aligned)
- Window menu button (left-aligned)
- Button press/depress visual feedback
- `_MOTIF_WM_HINTS` support — respects per-client decoration and
  function hints (no-decor, no-resize, no-minimize, no-maximize)

**Status Bar**
- Configurable position (top, bottom, left, right)
- Widgets: workspace buttons, layout indicator (M/T), focused window
  title, clock, system load, memory, disk, battery, volume, CPU,
  network, temperature
- Each widget aligned left or right independently
- ARGB transparency support (`#RRGGBBAA` colors)
- Toggle visibility at runtime

**Icon Bar**
- Appears when any window is minimized (CDE-style)
- Three display modes: icon, text, or icon-text
- Scrollable, with hover tooltips
- Configurable position and icon dimensions

**Compositor**
- Built-in compositor (XComposite + XDamage + XRender)
- Fade-in / fade-out animations on map, close, minimize, and
  workspace switch
- Per-window opacity via `_NET_WM_WINDOW_OPACITY`
- Detects and defers to an existing external compositor
- Toggleable at runtime

**Backgrounds**
- Solid color, 7 built-in patterns (checkerboard, stripes, dots, etc.),
  or image (centered, scaled, tiled, stretched, scale-filled)
- Sets `_XROOTPMAP_ID` for compatibility

**Menus & Dialogs**
- Window operations menu (from title bar menu button)
- Configurable root menu (right-click on desktop)
- mwm-style confirmation dialogs for restart and quit

**Move / Resize Feedback**
- XOR rubber-band outline during drag (mwm-style)
- Geometry indicator overlay (position or dimensions)

**ICCCM / EWMH**
- `WM_PROTOCOLS` (`WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`)
- `WM_STATE`, `WM_NORMAL_HINTS`, `WM_HINTS`, `WM_COLORMAP_WINDOWS`
- `_NET_CLIENT_LIST`, `_NET_CURRENT_DESKTOP`, `_NET_NUMBER_OF_DESKTOPS`,
  `_NET_WORKAREA`, `_NET_ACTIVE_WINDOW`, `_NET_WM_STATE_FULLSCREEN`,
  `_NET_WM_DESKTOP`, `_NET_WM_WINDOW_TYPE`
- Skips unmanaged types: dock, toolbar, utility, popup/dropdown menu,
  tooltip, notification

**IPC**
- Unix domain socket at `/tmp/.rondo-ipc-<display>`
- Commands: `reload`, `view <N>`, `move <N>`, `quit`, `arrange`,
  `float`, `fullscreen`

**Config**
- Scheme-like S-expression format (`~/.rondorc` or
  `~/.config/rondo/config.scm`)
- Hot reload without restart (`Alt+Shift+r`)
- All dimensions, colors, keybindings, bar widgets, and background
  settings configurable at runtime
- See `rondorc.example` for the full reference with comments

## Dependencies

- X11 (core, Xinerama, Xft, XComposite, XDamage, XFixes, XRender)
- Motif / LessTif (`libXm`, `libXt`)
- Imlib2
- PulseAudio (optional — for volume widget; `pactl` at runtime)

### Arch Linux

    pacman -S libx11 libxinerama libxft libxcomposite libxdamage libxfixes libxrender openmotif imlib2

### Debian / Ubuntu

    apt install libx11-dev libxinerama-dev libxft-dev libxcomposite-dev libxdamage-dev libxfixes-dev libxrender-dev libmotif-dev libimlib2-dev

## Building

    make

This produces the `rondo` binary. For the GUI configurator:

    cd rondomgr && make

## Installation

Copy `rondo` to your path and add it to `.xinitrc`:

    exec rondo

Copy `rondorc.example` to `~/.rondorc` and edit to taste.

## Default Keybindings

| Binding | Action |
|---|---|
| `Alt+Return` | Open terminal |
| `Alt+p` | Open launcher |
| `Alt+j` / `Alt+k` | Focus next / previous |
| `Alt+h` / `Alt+l` | Shrink / grow master area |
| `Alt+Shift+Return` | Swap focused into master |
| `Alt+f` | Toggle fullscreen |
| `Alt+Space` | Toggle floating |
| `Alt+Shift+c` | Close window |
| `Alt+1` – `Alt+9` | Switch workspace |
| `Alt+Shift+1` – `Alt+Shift+9` | Move window to workspace |
| `Alt+Tab` | Cycle windows |
| `Alt+Shift+Tab` | Lower window |
| `Alt+Shift+b` | Toggle bar |
| `Alt+Shift+t` | Cycle layout (master-stack / binary tree) |
| `Alt+Shift+o` | Toggle compositing |
| `Alt+Shift+r` | Reload config |
| `Alt+Shift+q` | Quit |

All keybindings are configurable in `~/.rondorc`.

## rondomgr

A Motif-based GUI for editing `~/.rondorc` and sending live reload
commands to rondo via IPC. Includes panels for dimensions, bar layout,
colors (with palette system), programs, compositing, and background
settings.

## License

MIT