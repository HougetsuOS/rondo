# rondomgr Improvement Plan

Status: P1–P3 complete; P4 optional polish pending
Created: 2026-09-07

Audit findings from driving rondomgr under Xvfb and diffing `~/.rondorc`
before/after an "Apply & Reload".

## P1 — Config data loss (critical)

### 1.1 Custom `root-menu` destroyed on Apply ✅
- Reader skips the section (`cfg_skip_form()`); `save_config()` writes a
  hard-coded default menu.
- **Fix:** capture the raw text of the `root-menu` form on read; on save,
  emit the captured text verbatim instead of the default. A tree editor can
  come later.
- Verified live: user menu with custom entries replaced by default.

### 1.2 Binding mod mangling ✅
- `(bind (mod mod4) (key Return) (action spawn) (arg "xterm"))` round-trips
  as `(mod Alt)` — the global modkey clobbers per-binding mods. Also `(arg)`
  loses quoting.
- **Fix:** keep the literal mod token per binding on read; quote string args
  on write when they came quoted.

### 1.3 Unknown keys / comments discarded ✅
- Whole file regenerated from the GUI model. Anything rondomgr doesn't know
  (comments, future keys) is silently dropped.
- **Fix:** on read, record the byte ranges of unrecognized top-level forms
  and file comments; on save, re-emit them in their original order relative
  to the known keys. (Patch-in-place strategy, done as a "trailer" list to
  keep it simple: known keys first as today, then preserved raw forms.)

## P2 — Feedback & correctness

### 2.1 Validation on Apply ✅ (committed e0825c4)
- Validate hex colors (`#RGB/#RRGGBB/#RRGGBBAA` or X color names —
  `XParseColor` probe), numeric ranges, non-empty font/terminal strings.
- On failure: Motif warning dialog listing the problems; do not write.
- Also flag silently-invalid colors in the GUI (bad hex renders as white).

### 2.2 Reload feedback ✅
- `ipc_send("reload")` failure prints to stderr only. Show a dialog.
- rondo side: `reloadconfig` replies with ok/error over the IPC socket;
  rondomgr surfaces the message (e.g. parse errors with line numbers).

### 2.3 IPC extensions ✅ (committed f21d707)
- rondo IPC gains: `restart` (hand over like SIGTERM path), `reload-deep`
  (colors/fonts/patterns, not just numbers). rondo's `reload` now replies
  `OK` / `ERR <message>`.

## P3 — Editors

### 3.1 Bar-layout editor ✅ (this commit)
- Currently round-tripped but not editable. Add: list of (widget, align)
  rows with add/remove/up/down controls, options = ws, title, clock, load,
  mem, disk, bat, vol, cpu, net, temp, tray.

### 3.2 Color picker ✅ (committed a712ee0)
- Swatch button next to each color row opens a small Motif color dialog:
  hex entry (validated live), list of common X color names, OK/Cancel.
  Preview swatch updates live on valid hex.

### 3.3 Background extras ✅ (this commit)
- `bg_pattern_size` scale (rondo supports it; GUI missed it).
- Root background preview widget (checkerboard/diagonal/… pattern + colors).

## P4 — Polish (optional, unscheduled)

- Raw-config tab (multi-line XmText) for power users.
- Live icon-bar preview inside rondomgr.
- Menu editor (tree UI) replacing 1.1's verbatim round-trip.
- Remember window geometry; single-instance guard (two rondomgrs clobber).

## Verification

Every change gets an Xvfb round-trip test: write a config with custom
content → run rondomgr → Apply → diff the file to prove nothing is lost.