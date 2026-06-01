# maps v2 — overzoom, pan/zoom, JPEG tiles, multi-touch

> **Status: implemented** (all four phases). See [../maps.md](../maps.md) for the
> resulting component doc; this file is kept for the design rationale.

Builds on the v1 viewer ([../maps.md](../maps.md)): follow-GPS, fixed integer
zoom, raw RGB565 tiles, single-touch. Goal: a continuous-feeling map you can
pan and pinch-zoom, with a compact world base that "shows through" when local
detail is missing.

## The touch data-path decision (answers "is ephemeral cJSON too slow?")

**Yes — don't put touch through storage.** Ephemeral `gps.*`-style vars are a
cJSON patch → ITS aux → browser merge-patch per write: right for ~1 Hz state,
wrong for 60–100 Hz × up to 5 points. Multi-touch goes through a **direct
in-process callback in the lcd component**, dispatched on the lcd task at the
touch-poll cadence, zero serialization. The only thing in storage is the
*enable flag*, and that's **ephemeral and consumer-owned** — `tdeck.multi_touch`
(no `s.`: not persisted, not a user setting, not in the pane). A consumer sets
it to `1` while it needs gestures and `0` when done; nothing else reads or
writes it. Finger data never touches storage.

## Tile format → JPEG (with a raw fallback)

Raw RGB565 is 128 KiB/tile fixed; a 1 GB card = world only to ~z6. JPEG tiles
are ~5–25 KB (ocean ~1 KB), ~7–13× denser → **world to ~z8–9 in 1 GB**.

- **Device**: `ensureTile` tries `<dir>/<z>/<x>/<y>.jpg` first (decode → RGB565
  into the cache slot on the **worker** task, off the UI), falling back to
  `.bin` (the existing raw path). Decoder: esp_jpeg (TJpgDec) or LVGL's built-in
  — confirm at build; 256×256 decode is a few–tens of ms, fine on the worker.
- **Toolchain**: `maketiles.py` emits `.jpg` by default (Pillow, quality flag)
  or raw `.bin` with `--bin` — that's why the device keeps the raw path. Not a
  user migration concern (install base zero), just dev convenience.

## Overzoom fallback

When a tile at the display zoom `Z` is missing, blit the nearest existing
**ancestor** `(Z-k, x>>k, y>>k)`: take its `256/2^k`-square sub-region at
`(x mod 2^k, y mod 2^k)` and nearest-neighbour upscale `2^k×`. So with a z6
world base loaded, every covered location has *something* at every higher zoom
(blurry until local detail exists) instead of grey. Cap k (~5 levels) → grey
beyond. Both the worker (fetch) and `composite` (blit) walk ancestors; the
cache already keys on `(z,x,y)` so mixed-zoom entries coexist.

## Pan + "center me"

- maps gains a **view centre** (lat/lon) and a **follow** flag. Follow on:
  view = GPS each update (today's behaviour). Follow off: free pan.
- **1-finger drag** pans — handled via LVGL press/drag events on the canvas
  (single-pointer, **no multi-touch needed**); each delta moves the view centre.
  Trackball motion pans too when the map is active.
- The **marker** is drawn at the GPS pixel relative to the view (no longer
  always centre; may go off-screen — edge-arrow indicator is a later nicety).
- **"Center me" button**: a small LVGL button (corner overlay) → follow=true,
  recenter on GPS, refetch. Tapping it is the only thing that re-locks to GPS.

## Pinch zoom + multi-touch

- **spangap-core lcd** (generic, board-agnostic, no storage knowledge):
  `lcdTouchSetMultipoint(bool)` — read up to 5 points; **point 0 still feeds the
  LVGL pointer indev** so normal UI is unaffected; when ≥2 fingers are down,
  report the pointer RELEASED so LVGL doesn't also act on the moving finger.
  `lcdTouchAddGestureHandler(cb)` — fires with all points on the lcd task (small
  handler list so multiple consumers can subscribe).
- **tdeck.cpp** (owns the GT911): subscribes to the ephemeral `tdeck.multi_touch`
  flag → `lcdTouchSetMultipoint`. Not surfaced in the pane, not persisted.
- **maps**: registers a gesture handler; sets `tdeck.multi_touch=1` on program
  open and `0` on go-home. Pinch = **stepped integer zoom** — past a
  distance-ratio threshold step ±1 level and refetch — with live LVGL
  `transform_scale` on the canvas for smooth feedback, snapped on release.
  Overzoom fallback makes the new level non-grey instantly. (Continuous
  fractional rendering is a heavier follow-up.)

## Phasing (each independently shippable)

1. **Overzoom fallback** — maps.cpp only, no deps. Immediate "no grey" win.
2. **Pan + center-me** — view-centre/follow state, LVGL drag, button, trackball.
   Single-touch; works today.
3. **JPEG tiles** — device decode path + `.bin` fallback; `maketiles.py --jpg`.
   Makes the real world base (Natural Earth, PD) practical in ~1 GB.
4. **Pinch + multi-touch** — `s.tdeck.multi_touch`, lcd gesture API, maps pinch.

## Decisions (all locked)

- Pinch v1 = **stepped integer zoom** + live `transform_scale` feedback. ✓
- Multi-touch enable = **ephemeral `tdeck.multi_touch`**, consumer-owned, not in
  the pane, not persisted. Finger data via the lcd gesture callback. ✓
- JPEG decoder = **`espressif/esp_jpeg`** managed component, decode on the maps
  worker task (off the UI, decoupled from LVGL). ✓
