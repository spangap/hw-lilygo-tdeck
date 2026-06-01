# maps — on-device offline map viewer

[`maps.cpp/h`](../main/maps.cpp) — a launcher program that draws pre-baked
slippy-map tiles read off the SD card, following the live GPS fix (or free-pan)
with a position marker. Nothing is rendered or fetched on the device: tiles are
made on a computer ([`scripts/maketiles.py`](../scripts/maketiles.py)) and copied
to the SD card. Gated on `CONFIG_SPANGAP_LCD`; both entry points are no-ops
without a display.

Features: follow-GPS with a **centre-me** button, **drag/trackball pan**,
**pinch-zoom** (multi-touch), and **overzoom fallback** (a missing tile at the
display zoom is filled by upscaling the nearest lower-zoom ancestor, so a coarse
base map shows through where local detail is absent instead of going grey).

Approach follows [`esp32_offline_osm`](https://github.com/mryndzionek/esp32_offline_osm):
vector→raster rendering and the heavy OSM dataset stay on the desktop; the MCU
only does file-read, JPEG-decode, and blit.

## Architecture

Two tasks, one shared structure:

- **Worker task** (`maps`, core 1, prio 1, 8 KB PSRAM) owns the slippy-map math,
  the view-centre / follow state, and a PSRAM **LRU tile cache** (16 slots ×
  256×256×2 B = ~2 MB; holds display tiles + overzoom ancestors). It subscribes
  to `gps.*` (centre) and `s.maps.*` (zoom / tile dir), and the LCD task feeds it
  pan deltas + a recentre flag; on any change it fetches the visible tiles (or
  their ancestors) from SD via the `fs` API (which proxies flash I/O safely off
  the PSRAM stack, and decodes `.jpg` via esp_jpeg), then hands a composite
  request to the LCD task with `lcdRun()`.
- **LCD task** owns the `lv_canvas`, the centre-me button, the drag handler, and
  the pinch gesture handler. `composite()` blits the cached tiles (scaled for
  overzoom) into the canvas buffer (plain `memcpy`/upscale, on the same task as
  LVGL render → no tearing), draws the marker, and toggles a status label.

The cache is shared (mutex); pan/recentre flags are shared (a portMUX). SD reads
+ JPEG decode happen *outside* the cache lock — only the slot swap is locked, so
a slow read never stalls the UI's composite.

The tile cache is the only shared state, guarded by a mutex. SD reads happen
*outside* the lock; only the cache-slot swap is locked, so a slow read never
stalls the UI's composite.

### Slippy-map math

Standard Web-Mercator tiles, 256 px each. `lon/lat → global pixel` at zoom `z`:
`x = (lon+180)/360 · 2^z · 256`, `y = (1 − ln(tan φ + sec φ)/π)/2 · 2^z · 256`.
The canvas is centred on that pixel; each visible tile `(z,x,y)` is blitted at
`x·256 − topleft_x, y·256 − topleft_y`, clipped to the canvas.

## Tile format on SD

Read from `s.maps.tiledir` (default `/sdcard/maps`), 256×256, two formats — the
device tries `.jpg` first, then `.bin`:

```
/sdcard/maps/<z>/<x>/<y>.jpg   JPEG (default) — decoded on the worker via esp_jpeg
/sdcard/maps/<z>/<x>/<y>.bin   raw little-endian RGB565, no header (131072 bytes)
```

JPEG is ~7–13× smaller (a 1 GB card reaches ~world-z8/9 vs ~world-z6 raw); the
worker decodes it off the UI thread. Raw `.bin` is little-endian because that is
LVGL's native canvas order (it byte-swaps for the ST7789 on flush — note this
differs from `esp32_offline_osm`'s `bin_565_swap`, a direct-to-panel path); the
JPEG decoder is told to emit little-endian to match. A missing tile triggers the
overzoom fallback (upscaled ancestor), or renders as background if none exists.

## Configuration & state

| Key | Default | Meaning |
|---|---|---|
| `s.maps.zoom` | `15` | slippy zoom level (1–19) |
| `s.maps.tiledir` | `/sdcard/maps` | SD path holding `<z>/<x>/<y>.bin` |

| Ephemeral | Values |
|---|---|
| `maps.state` | `ready` / `no tiles` / `no fix` / `no sd` |

## On-device program (`CONFIG_SPANGAP_LCD`)

- Launcher tile **Maps** with a globe icon
  ([`assets/lcd-icons/maps.svg`](../assets/lcd-icons/maps.svg) →
  `/fixed/lcd/icons/36x36/maps.bin`, rasterized by the existing icon hook).
- Full-screen canvas; red marker at the GPS position. When there's no fix / no
  SD / no tiles for the area, a status label says so.
- **Follow vs pan**: follows the GPS fix by default; a drag (touch or
  trackball-press-drag) free-pans (marker may leave the screen). The
  bottom-right **centre-me** button (`LV_SYMBOL_GPS`) re-locks to the fix.
- **Pinch-zoom**: while open, maps sets the ephemeral `tdeck.multi_touch` flag
  so the lcd reads both fingers; a pinch steps `s.maps.zoom` (the canvas
  live-scales for feedback, then the worker refetches at the new integer zoom).
- **Maps** Settings pane: `Zoom` slider, `Status` (`maps.state`), `Tile dir`.
- CLI `maps` prints state/sd/zoom/follow/tiledir + GPS & view centres;
  `maps center` recentres.

## Getting the map tiles

The device renders nothing — it only reads, JPEG-decodes, and blits pre-baked
raster tiles (see [Architecture](#architecture)). So every step that turns map
*data* into *pixels* runs on a computer; the SD card only ever holds finished
images.

### Vectors vs. bitmaps

```
OSM data (.pbf)   ──render──>   raster z/x/y tiles   ──maketiles.py──>   .jpg   ──copy──>   SD /sdcard/maps
  VECTORS         Mapnik+style    BITMAPS (PNG/JPG)    device format       (<z>/<x>/<y>.jpg)
```

This is the distinction people trip on:

- A **`.pbf` is vector data** — nodes, ways, relations and their tags
  (`highway=primary`, `name=…`). No pixels; it's the raw OSM source.
- **Tiles are bitmaps** — rendered pixels. The vector→raster **render** step (a
  render engine + a map *style*) is the heavy part of the pipeline, and it's
  exactly what lets the device stay dumb.

[`maketiles.py`](../scripts/maketiles.py) is only the *last* step — it transcodes
finished raster tiles into the device format and never sees vector data. So the
real question is always "where do the raster tiles come from?", and there are two
answers.

### Route A — from a tile provider (no rendering)

Easiest to hand to other people: point `maketiles.py` at a tile-server URL
template — your own server, or a keyed global provider (MapTiler, Thunderforest,
Stadia, …). No GIS toolchain, no `.pbf`; making tiles for "my city" is just a
bbox + an API key.

> **Never bulk-download `tile.openstreetmap.org`** — it violates the
> [OSM tile usage policy](https://operations.osmfoundation.org/policies/tiles/)
> and you'll be blocked; `maketiles.py` refuses that host outright. Note the
> policy is about *rendered tiles* — raw OSM **data** carries no such
> restriction (that's how a street index would be built).

```bash
./scripts/maketiles.py --src 'https://tiles.example/{z}/{x}/{y}.png?key=KEY' \
    --out ./sdmaps --bbox 13.30 52.45 13.50 52.60 --zoom 12 16 --quality 85
```

Cost: a third-party key plus their ToS / rate limits. Nothing to install.

### Route B — self-hosted render from `.pbf`

Owes nothing to anyone and works fully offline, but you run the renderer:

1. **Get the extract** — a region `.pbf` from
   [Geofabrik](https://download.geofabrik.de) (Berlin ≈ 70–90 MB); optionally
   trim to a bbox with `osmium extract`.
2. **Render `.pbf` → raster `<z>/<x>/<y>`** with **Mapnik + a style**
   (openstreetmap-carto). The classic stack is PostgreSQL + PostGIS + osm2pgsql
   + Mapnik + the carto style; the painless way to get all of it is a prebuilt
   Docker tile-server image (e.g. `overv/openstreetmap-tile-server`) — import the
   `.pbf`, then pre-seed the z/x/y range with `render_list`.
   > **`tilemaker` makes *vector* tiles, not raster** — it will not give you
   > bitmaps. For raster you need Mapnik (or a GL rasterizer on top of vector
   > tiles).
3. **Transcode** the rendered tree to the device format with `maketiles.py`
   (`--src` = the local tree; see below).

**How long?** The render is the *short* part — toolchain *setup* dominates.
Berlin to z16 on an M-series Mac:

| Step | Time |
|---|---|
| Download `.pbf` (70–90 MB) | ~1 min |
| osm2pgsql import (Berlin is small) | ~5–10 min |
| Render z0–16 (~16k tiles, ~12k at z16) | ~10–30 min (dense z16 is the slow part) |
| `maketiles.py` transcode | minutes |
| **Toolchain setup — one-time, per machine** | **~30–60 min (Docker image) … an afternoon (from scratch)** |

End-to-end for a city is under an hour *once the toolchain exists*; the
afternoon-long install is the real barrier — which is why Route A is so much
easier to put in an instruction sheet. z16 is a sane cap for a handheld: each
further level is ×4 (z18 ≈ 16× the tiles and disk).

### Transcode to the device format (`maketiles.py`)

`maketiles.py` (needs `pip install pillow`; `numpy` only for `--bin`) turns
raster tiles into `256×256` device tiles laid out as `<out>/<z>/<x>/<y>.jpg`,
clipped to the bbox + zoom range. It's identical for both routes — the only
difference is whether `--src` is a URL template (Route A) or a local rendered
tree (Route B):

```bash
# local rendered tree (Route B), JPEG q80 default:
./scripts/maketiles.py --src /maps/render --out ./sdmaps \
    --bbox 13.30 52.45 13.50 52.60 --zoom 12 16

# raw RGB565 instead of JPEG (zero on-device decode, ~7–13× bigger):
./scripts/maketiles.py --src /maps/render --out ./sdmaps --bin --bbox ... --zoom ...
```

Arguments: `--bbox MINLON MINLAT MAXLON MAXLAT` and `--zoom ZMIN ZMAX` bound what
gets produced; `--quality` (JPEG), `--bin` (raw); URL mode adds `--ua` and
`--delay` (be polite). A `maps.json` manifest is written alongside and **merged**
on re-run, so appending zoom levels / areas widens the recorded range instead of
clobbering it.

> Tiles must be **baseline** JPEG. The on-device decoder (esp_jpeg / TJpgDec) is
> baseline-only — progressive / arithmetic-coded / CMYK / 12-bit tiles are
> rejected, the cell stays blank, and a throttled `warn()` in `decodeJpg` names
> the offending file. `maketiles.py` (Pillow) writes baseline; if you source
> tiles elsewhere, re-encode baseline (`mogrify -interlace none`, or `jpegtran`).

### Copy to the SD card

Copy the contents of `--out` to the card so the device sees
`/sdcard/maps/<z>/<x>/<y>.jpg`. Done.

### Sizing & licensing

Berlin to z16 is ~300–500 MB as JPEG (~16k tiles). Raster multiplies with zoom —
you bound storage by bbox + zoom range, not by tiling the planet. A city across
the zooms you actually use is comfortable on an SD card; `--bin` trades ~7–13×
the size for zero on-device decode.

OSM data is **ODbL**: if you redistribute the tiles (or a derived street index)
you owe "© OpenStreetMap contributors" attribution and the share-alike terms.
Free to use — that's just the obligation.

## Scope & limitations

- **Pinch is stepped**, not continuous: it snaps to integer zoom levels (the
  canvas live-scales during the gesture for feedback, then refetches). Smooth
  fractional rendering is a heavier follow-up.
- Overzoom fallback is **ancestor-only** (upscales lower zooms); it never
  downscales a higher zoom into a missing lower one. Cap is `MAPS_MAXK` levels.
- The worker keeps recomputing on GPS updates even when the program isn't on
  screen. Cheap (SD reads/decodes only fire when the visible tile set changes),
  but not fully idle-when-hidden (no hide hook yet — also where `tdeck.multi_touch`
  would be cleared).

Natural next steps: continuous/fractional zoom, off-screen marker direction
arrow, idle-when-hidden, and a single-file tile container (PMTiles/MBTiles).

## Files

- [`main/maps.cpp`](../main/maps.cpp) / [`main/maps.h`](../main/maps.h) — worker task + launcher program.
- [`scripts/maketiles.py`](../scripts/maketiles.py) — desktop tile builder (JPEG/raw).
- [`assets/lcd-icons/maps.svg`](../assets/lcd-icons/maps.svg) — launcher icon.
- [`main/main.cpp`](../main/main.cpp) — `mapsInit()` / `mapsLcdRegister()`.
- spangap-core [`lcd.h`](../../spangap/spangap-core/include/lcd.h) — generic multi-touch API
  (`lcdTouchSetMultipoint`, `lcdTouchAddGestureHandler`) used for pinch.
- [`main/tdeck.cpp`](../main/tdeck.cpp) — watches `tdeck.multi_touch` → enables multipoint.
