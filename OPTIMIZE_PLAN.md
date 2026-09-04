# Performance Optimization Plan

## CRITICAL BUG FIXED (this session)

The XInternAtoms batching originally passed `(Atom*)` cast over an array of
*pointers to* Atom globals — XInternAtoms wrote the atom IDs into the pointer
array, not the globals, so **every batched atom stayed 0** (WM_PROTOCOLS,
_NET_WM_WINDOW_TYPE, tray atoms, everything). Consequences: WM protocol
matching dead, window-type checks dead, and any ClientMessage with
message_type=0 was treated as a tray dock request → infinite wrapper-creation
loop (bar middle wiped, exposure/reparent storms, close artifacts, wrong
window focus). Fixed by interning into a flat `Atom ids[]` array and copying
into the globals. Verified under Xvfb (bar renders fully, clean close, no
dock loop).

## Batch 1+2 (originally documented, re-verified/implemented this session)

All items below were found to be missing from the working tree and have been
re-implemented and verified in the current code:

- **#1** Compositor caches children list + per-window x/y/w/h/map_state in
  TrkWin (XQueryTree/XGetWindowAttributes only on structure change)
- **#2** XSync removed from drawframe / bar / iconbar / menu / dialog /
  drag-loop paths (XFlush instead; sync only at drag boundaries)
- **#5/#6** Root background picture + solid color cached (invalidated via
  compositor_bg_reloaded from bg_load)
- **#7** Hash table for wintoclient (O(1) lookup, registered at manage,
  removed at unmanage, fallback rescan on miss)
- **#9** Batch compositor repaints via defer_schedule (Map/Unmap/Expose no
  longer call compositor_repaint directly)
- **#12** Binary search for title truncation in drawframe
- **#13** O(N) master-stack layout (windows_left precomputed)
- **#14** Scaled icon mask built with XPutImage instead of per-pixel XDrawPoint
- **#15** manage(): window-type check moved to top (frame no longer leaked on
  skip); both WM_PROTOCOLS fetched with a single XGetWMProtocols call
- **#17** Geometric root-click lookup (XQueryPointer/XQueryTree removed)
- **#16** send_configure_notify computes root coords arithmetically
  (no XTranslateCoordinates)
- **#21** ~37 atoms interned with a single XInternAtoms call; XA_WM_NAME
  constant used for WM_NAME PropertyNotify
- **#22** amixer volume read cached with 2s TTL
- **#25** update_client_list uses a stack buffer for ≤128 clients
- **#27** XDefineCursor cached per frame window (skips redundant requests)
- **#29** Redundant title-row fill removed from drawframe
- **#33** (see #21)
- **#34** XRenderPictFormat cached in TrkWin at trk_add time

## New optimizations (this session, beyond the original plan)

### Compositor
- **Dirty-region repaint** — damage events accumulate a bounding-box region;
  background fill, per-window composite, and the final blit all operate on
  the dirty region only. A 1px cursor blink no longer repaints the screen.
  Full repaint on structure change / buffer realloc / explicit request.
- **Per-window opacity cached** in TrkWin; invalidated on PropertyNotify
  (`compositor_opacity_changed`). No XGetWindowProperty per window per frame.
- **Shared 1x1 alpha mask refilled only when alpha changes** (was every
  translucent window every frame).
- **Targeted damage subtract** — only the Damage handle that fired is
  subtracted (was O(N) subtracts per event).
- **Fade steps dirty only the fading window's rect** (`dirty_client`).
- **`compositor_repaint_full` / `compositor_dirty_window`** — menu and dialog
  paths mark their window dirty explicitly since nested event loops don't
  process damage events.
- **trk_mark_mapped lazily tracks never-seen windows** (tooltips, popups) so
  their rects are known and dirtied on map.

### Frames / layout
- **moveresizeframe skips unchanged geometry** (cached last_x/y/w/h) — focus
  changes no longer re-push configure requests.
- **place_client skips everything when computed geometry is unchanged** —
  arrange() on every focus change is now nearly free.
- **bevel_rect coalesces scanline runs** — each side emits ≤3 rects instead
  of one per scanline (drawframe previously sent 100+ small fills).
- **Title extents measured via binary search** (8 probes instead of O(n)).

### Bar / iconbar
- **Scaled icon cache per Client** (icon_scaled_pm/mask + size) — rebuilt only
  when the icon changes (WM_HINTS PropertyNotify invalidates) or size
  changes; freed on unmanage. XGetImage/Imlib2 work happens once, not per
  icon-bar redraw.
- **Iconbar back-buffer size tracked locally** (no XGetGeometry per draw).
- **Widget max-width extents cached per type** (invalidated on font change).
- **Workspace label extents cached** (horizontal + vertical bars).
- **Per-client name extents cached** in Client (invalidated by
  updatewindowname change detection).
- **Iconbar clip rect hoisted** out of the per-icon loop.
- **Tooltip unmap skipped when already hidden** (`tooltip_shown` flag).
- **updateiconbar no longer calls arrange()/drawbar()** — defer_flush owns
  that pass; double arrange/drawbar per flush eliminated.
- **swapbar now arranges explicitly** (was relying on updateiconbar side
  effect).

### Config / startup
- **cfg_reload skips when file mtime+size unchanged** (stat check).
- **cfg_set_defaults installed only on first init** (no alloc/copy/free
  churn per reload).
- **Tooltip font reopened only when changed** (tracks main-font changes too).
- **Workspace arrays realloc'd only when count changes.**
- **load_colors skips allocation when color name unchanged** (per-slot name
  cache; frees old pixel on actual change — also fixes a colormap leak on
  TrueColor-less visuals).
- **Keybind array grows with doubling capacity** (was realloc per bind).
- **icon_gc created once** (was created, freed, recreated in setup).

### Background
- **Patterns drawn via 2×2-cell tile pixmap + XSetTile + one fill**
  (checkerboard, dots — was thousands of per-cell requests).
- **Diagonal/crosshatch lines batched into XDrawSegments.**
- **Tiled image uses XSetTile + single fill** (was per-tile XCopyArea).
- **img_pm allocated only for BG_STRETCHED.**
- **_XROOTPMAP_ID atom cached.**

### Menus
- **Menu highlight changes repaint only the affected rows** (partial redraw
  instead of full menu re-render at mouse rate).
- **Menu extents cached** (invalidated via menu_extents_gen on reload).
- **Dialog cursors created once** (static cache).

### Misc
- **IPC accepted sockets set O_NONBLOCK** (accepted sockets don't inherit it
  from the listener — a stalling peer could block the WM event loop).
- **tray_reposition skips when positions unchanged** (was 2N move-resizes
  after every drawbar); tray_remove no longer reallocs per removal.

## Known remaining (intentional / low priority)
- **#8** Hash table for trk_find (linear scan of TrkWin array — small N)
- **#28** XGrabServer during drag (kept — UX semantics)
- **#30** Edge hit before button hit (intentionally skipped — buttons take
  priority)
- **#31** Persistent menu window (create/destroy per invocation remains)
- **#35** Vertical text per-character draws remain (pre-render would be a
  larger refactor)
- Two pre-existing unused-variable warnings in bar.c (arrow_bot/arrow_right
  in the vertical iconbar scroll code)