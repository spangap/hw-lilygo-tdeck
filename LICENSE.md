# License

This repository, **hw-lilygo-tdeck** (the consumer "buildable straddle" that
bundles spangap + reticulous straddles into a flashable T-Deck S3 firmware
image and a browser SPA), is released under the **Apache License,
Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by reticulous project contributors.

## Third-party software

### Vendored in this repository

None directly. All third-party code that lands in the built artifacts comes
in through the straddles listed below, or as managed components / npm
packages declared at build time.

### Straddles consumed (see each repo's own `LICENSE.md` for details)

Pulled in via `straddle.yaml`'s `requires:` and staged at build time
by `spangap make`. All are Apache-2.0 spangap-project code; some carry
vendored third-party sub-components with their own licenses:

| Straddle | Notable embedded third-party | License of that code |
|---|---|---|
| `spangap/spangap-core` | (none vendored after the `esp_wireguard` move) | — |
| `spangap/spangap-net`, `spangap-lcd`, `spangap-web` | none | — |
| `spangap/wg` | trombik/esp_wireguard tree, NaCl curve25519 ref, poly1305-donna, x25519 by Cryptography Research, RFC 7693 blake2s | BSD-3-Clause / public domain / MIT |
| `spangap/acme`, `duckdns`, `ota`, `upnp` | none | — |
| `reticulous/rns` | microReticulum fork, microStore, ed25519-donna, x25519 (Mike Hamburg), bzip2 | Apache-2.0 / public domain / MIT / bzip2 license |
| `reticulous/iface-tcp`, `-auto`, `-espnow`, `-lxmf`, `-nomad`, `maps` | none | — |
| `reticulous/iface-lora` | RadioLib (pulled as managed dep, not vendored) | MIT |

### Firmware build-time dependencies

Declared in `esp-idf/main/idf_component.yml`. The component manager does not
recurse into locally-staged straddles, so this buildable straddle surfaces every
third-party managed dep used by any of the straddles it requires:

| Component | Source | License |
|---|---|---|
| ESP-IDF v5.5.4 (platform) | espressif/esp-idf | Apache-2.0 |
| `jgromes/radiolib` v7 (for iface-lora) | components.espressif.com / GitHub | **MIT** |
| `lvgl/lvgl` v9 (for spangap-lcd) | components.espressif.com / lvgl | **MIT** |
| `espressif/esp_lcd_touch_gt911` (pulls `esp_lcd_touch`) | components.espressif.com | Apache-2.0 |
| `espressif/esp_jpeg` (TJpgDec-based, for maps) | components.espressif.com | Apache-2.0 |
| `espressif/mdns` (via spangap-core/-net) | components.espressif.com | Apache-2.0 |
| `joltwallet/littlefs` (via spangap-core; wraps `littlefs-project/littlefs`) | components.espressif.com | BSD-3-Clause |

### Browser build-time dependencies

Declared in `web-interface/package.json`. The Quasar build bundles these
into the served SPA:

| Package | License |
|---|---|
| `vue` ^3.5                       | MIT |
| `quasar` ^2.17                   | MIT |
| `pinia` ^3.0                     | MIT |
| `vue-router` ^4.4                | MIT |
| `@quasar/extras`                 | MIT |
| `@xterm/xterm`, `@xterm/addon-fit` | MIT |
| `register-service-worker`        | MIT |
| `workbox-precaching`, `workbox-routing`, `workbox-strategies`, `workbox-build` | MIT |
| `vite`, `@quasar/app-vite`, `tsx`, `typescript` (dev) | MIT / Apache-2.0 (vite) |

Other transitive npm dependencies retain their upstream licenses; see
`web-interface/package-lock.json` for the full resolved tree.

### Map / tile data

Tile imagery rendered by the `maps` component is not shipped with this
repository. Distributors should attribute the underlying map data
appropriately (e.g. © OpenStreetMap contributors, ODbL).
