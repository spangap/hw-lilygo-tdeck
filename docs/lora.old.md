# lora — LoRa transport task

`lora.cpp/h` (the [iface-lora](../../iface-lora) straddle) — RNS-over-LoRa transport.
It drives **any RadioLib LoRa chip** (SX126x, SX127x/RFM9x, SX128x, LR11x0,
LR2021), selected per-radio in Kconfig; the T-Deck Plus build below uses the
**SX1262** on the shared SPI bus. Owns the radio end-to-end: RadioLib + custom
[`EspIdfHal`](../main/esp_idf_hal.h), the chip's IRQ
ISR, RNode on-air framing with split reassembly, half-duplex
coordination, self-registration with rnsd as the `lora` iface. Pinned
to core 0 alongside rnsd + net, priority 2, 8 KB PSRAM stack (slightly
larger than other transports because of LoRa frame buffers + RadioLib
state machine).

For the persistent / ephemeral / CLI / panel / status-window contracts,
see `component-plan.md` §12.

## Hardware

The LoRa radio pins come from iface-lora's `CONFIG_LORA*` (set in
[`sdkconfig.defaults`](../sdkconfig.defaults)); the board's own peripheral
constants live in [`tdeck.h`](../main/tdeck.h). There is no board-select
Kconfig — hw-lilygo-tdeck is the T-Deck Plus. T-Deck Plus values:

| Define | T-Deck Plus |
|---|---|
| `BOARD_POWER_EN_PIN` | GPIO 10 |
| `BOARD_LORA_CS_PIN` | 9 |
| `BOARD_LORA_DIO1_PIN` | 45 |
| `BOARD_LORA_RST_PIN` | 17 |
| `BOARD_LORA_BUSY_PIN` | 13 |
| `BOARD_LORA_SPI_HOST` | SPI2_HOST |
| `BOARD_LORA_SCK_PIN` | 40 |
| `BOARD_LORA_MOSI_PIN` | 41 |
| `BOARD_LORA_MISO_PIN` | 38 |
| `CONFIG_LCD_CS_PIN` (lcd component) | 12 |
| `BOARD_LORA_TCXO_VOLTAGE` | 1.8 V |
| `BOARD_LORA_DIO2_RF_SWITCH` | 1 |

Heltec WiFi LoRa 32 V3 was evaluated and rejected: it has no PSRAM,
and the spangap platform requires PSRAM (PSRAM-backed ITS queues,
PSRAM task stacks, a 256 KB WebRTC router buffer). Running on a
no-PSRAM S3 would be a spangap-core fork, not a board entry. Octal
vs quad is the board's call — the T-Deck (S3R8) is octal, the later
Heltec V4 (S3R2) is quad; see the `hw-heltecv4` straddle.

Driver code is board-agnostic; RadioLib's `SX1262` class works the same wherever
the `CONFIG_LORA*` pins point.

## SPI is shared on T-Deck Plus

The FSPI bus on T-Deck Plus carries LCD (CS=12), microSD, **and** the
SX1262 (CS=9). Two consequences:

1. **`BOARD_POWER_EN_PIN = 10` must be driven HIGH at boot.** This pin
   gates the +3.3 V rail to display, SD, GPS *and* radio. Without it,
   SPI traffic to the SX1262 is just SPI traffic into a powered-down
   chip — `begin()` returns `RADIOLIB_ERR_SPI_CMD_TIMEOUT` (-706) or
   `RADIOLIB_ERR_CHIP_NOT_FOUND` (-2). The lora task does this itself
   if `BOARD_POWER_EN_PIN >= 0`, with a 100 ms settling delay (T-Deck
   Plus's TCXO regulator takes longer than RadioLib's 5 ms default
   wait after setting DIO3 as TCXO control — easier to fix at the
   rail).
2. **The LCD CS pin must be parked HIGH** so the ST7789V doesn't drive
   MISO on the shared bus while LoRa or SD is talking before the lcd
   component claims the panel. `tdeckPreInit()`'s power/CS block parks
   `CONFIG_LCD_CS_PIN` (and each `CONFIG_LORA*_CS_PIN`) HIGH at boot, ahead of
   the SD probe inside `spangapInit()`.

Bus init goes through spangap-core's `spiHelperInitBus` so future
LCD/SD drivers sharing the bus stay idempotent.

## EspIdfHal — RadioLib ↔ ESP-IDF glue

`EspIdfHal : public RadioLibHal` in
[`esp_idf_hal.cpp`](../main/esp_idf_hal.cpp). ~150 LOC of stateless
plumbing.

**SPI.** Each transfer goes through `spi_device_polling_transmit` for
low latency on small SX126x command words. RadioLib brackets all
transfers with `spiBeginTransaction` / `spiEndTransaction`; we hold
the bus across the pair with `spi_device_acquire_bus` /
`spi_device_release_bus` so multi-byte command-then-data exchanges
stay atomic on the shared FSPI bus.

**CS pin.** Driven by RadioLib via `digitalWrite` (we pass
`spics_io_num = -1` to the SPI driver). RadioLib (legitimately) wants
to hold CS low across what it considers two "transactions"; letting it
pulse CS itself avoids surprises.

**GPIO ISR.** `attachInterrupt(pin, cb, mode)` registers a per-pin
`void (*)(void)` callback. Trampoline `isrTrampoline(void*)` is
`IRAM_ATTR`, looks up `g_isrCb[pin]`, and **disables the GPIO interrupt
before invoking the callback** — harmless for edge-triggered users,
required for level-triggered ones. The consumer task re-enables via
`gpio_intr_enable` after servicing the peripheral that pulled the
line. (See "ISR — single notification, re-arm in the task" below.)

GPIO ISR service is installed lazily on first `init()` with
`ESP_INTR_FLAG_IRAM`.

**Pin-mode encoding.** `MODE_INPUT` / `MODE_OUTPUT` are
`(uint32_t)GPIO_MODE_INPUT/OUTPUT` so they pass straight to
`gpio_set_direction`. Edge constants likewise are
`GPIO_INTR_POSEDGE/NEGEDGE`. RadioLib stores them opaquely and feeds
them back when it wants to drive a pin — they don't need to match any
Arduino constants.

## RNode on-air framing

The SX126x physical layer caps each LoRa frame at **255 bytes**
(8-bit length register). Reticulum's protocol MTU is **500 bytes**. So
full-sized RNS packets must be split across multiple LoRa frames. This
is the **RNode on-air framing protocol** (defined in
`RNode_Firmware/Framing.h` / `Config.h`) — any device speaking
RNS-over-LoRa to RNode-equipped peers (RNodes, T-Beams, other
Reticulum-on-LoRa firmware) must use it.

```
[ 1 byte: header ][ ≤ 254 bytes: payload ]

header upper nibble (0xF0): random sequence id, 16 values
header bit 0       (0x01): RNODE_FLAG_SPLIT — part of a 2-frame split
```

- Single small RNS packet (≤254 B) → one LoRa frame, SPLIT=0.
- Large RNS packet (255–508 B) → two LoRa frames, both with same
  random seq nibble, both SPLIT=1, payloads concatenated by the
  receiver.
- Random seq nibble disambiguates interleaved splits from different
  senders. Reassembly timeout: 5 s (`SPLIT_RX_TIMEOUT_MS = 5000`).

Constants in `lora.cpp`:

```cpp
#define RNS_MTU             500    // upper bound on an RNS packet
#define RNODE_MAX_PAYLOAD   254    // 255 - 1 byte header
#define RNODE_FLAG_SPLIT    0x01
```

### Receive — `drainRadioIrq`

After DIO1 fires:

1. `s_radio->getPacketLength()`. Range check `1 ≤ len ≤
   1 + RNODE_MAX_PAYLOAD`; out of range → `startReceive` and bail.
2. `s_radio->readData(frame, len)` — fetches data and clears IRQ
   status. `RADIOLIB_ERR_CRC_MISMATCH` bumps `s_crcErr`; either way
   re-arm RX.
3. Bump `s_rxFrames`, cache RSSI/SNR.
4. Parse header: `seq = header & 0xF0`, `isSplit = header & 0x01`.

5. **Not split.** Deliver `frame+1, payloadLen` directly to rnsd; bump
   `s_rxBytes`.

6. **Split, no pending.** Stash `frame+1` in `s_splitBuf`, record
   `s_splitSeq = seq`, `s_splitPending = true`, set
   `s_splitDeadline = now + 5 s`.

7. **Split, matching seq.** Concatenate into `s_splitBuf`; deliver the
   joined buffer to rnsd; bump `s_rxBytes`; clear `s_splitPending`.

8. **Split, mismatched seq.** Different sender's split interleaved.
   Restart assembly on the new seq.

9. `startReceive` + `gpio_intr_enable(DIO1)` — re-arm both.

A 5 s timeout (`s_splitDeadline`) silently drops the half-assembled
buffer if the second frame never arrives, bumping `s_splitTimeouts`.

### Transmit — `sendRnsPacket`

```cpp
seq = (esp_random() & 0x0F) << 4;    // random 4-bit seq id

if (len ≤ RNODE_MAX_PAYLOAD) {
    frame[0] = seq;
    memcpy(frame+1, data, len);
    transmit(frame, 1+len);
} else {
    f1[0] = seq | RNODE_FLAG_SPLIT;  memcpy + transmit (1 + RNODE_MAX_PAYLOAD)
    f2[0] = seq | RNODE_FLAG_SPLIT;  memcpy + transmit (1 + remaining)
}
```

After each `transmit`, RadioLib leaves the radio in standby —
`startReceive` re-arms RX before returning.

`s_txBytes` accounts the **RNS** payload, not the per-frame bytes;
`s_txFrames` counts each LoRa frame separately.

### Half-duplex coordination

SX126x is half-duplex — transitioning to TX while a split RX is
pending would lose the second frame. `drainOneOutbound` early-outs on
`s_splitPending`:

```cpp
if (!s_running || s_splitPending || s_rnsdHandle < 0) return;
```

The outbound RNS packet sits in the ITS stream buffer (our outbound TX
queue). The task loop revisits once `s_splitPending` clears (either by
the second frame arriving or by `SPLIT_RX_TIMEOUT_MS` elapsing).

One outbound packet drained per loop turn so RX gets re-armed between
back-to-back transmissions. `drainRadioIrq` runs before
`drainOneOutbound`, so inbound IRQs queued in the SX126x FIFO get
serviced between TX bursts.

## ISR — single notification, re-arm in the task

DIO1 is configured rising-edge by RadioLib via
`setDio1Action(loraRadioIsr)`. The ISR body:

```cpp
static IRAM_ATTR void loraRadioIsr(void) {
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &hp);
    portYIELD_FROM_ISR(hp);
}
```

The trampoline in `EspIdfHal` disables the GPIO interrupt before
firing the callback (`gpio_intr_disable(pin)` — see the HAL section
above), so the ISR fires exactly once per packet. The task re-enables
via `gpio_intr_enable((gpio_num_t)BOARD_LORA_DIO1_PIN)` at the end of
`drainRadioIrq`. SX126x keeps DIO1 high until IRQ status is cleared
(`readData` does it internally) — without the disable+re-enable dance,
a level-trigger interpretation would re-fire continuously while the
line is asserted.

**ISR rules** (per plan §7):
- `IRAM_ATTR` mandatory — the ISR may fire while SPI flash is being
  accessed.
- **No SPI from the ISR.** Task-side reads `getIrqStatus` /
  `readData`, clears IRQ status, re-arms with `startReceive`.
- ISR body is exactly one notification — no `printf`, no logging.

`itsPoll` unblocks on the same task notification mechanism
(`ulTaskNotifyTake`), so this is the spangap canonical pattern — a
single wait point handling ITS, ISR, and deadlines simultaneously.

## Main loop

```cpp
for (;;) {
    if (s_configDirty) { s_configDirty = false; applyConfig(); }

    // IRQ was likely the wake source.
    if (s_running) drainRadioIrq();

    // 5 s timeout on stuck split-RX.
    if (s_splitPending && (int32_t)(now - s_splitDeadline) >= 0) {
        s_splitPending = false; s_splitTimeouts++;
    }

    // Re-register with rnsd if it dropped while we're enabled.
    if (s_running && s_rnsdHandle < 0 && s_enabled) registerWithRnsd();

    drainOneOutbound();
    publishStats();
    itsPoll(nextDeadline());
}
```

`nextDeadline`:
- If outbound queued AND radio free AND rnsd connected → return 0
  (drain immediately on next iteration).
- Otherwise, soonest of: split-RX deadline, 1 s stats-publish cap.

`s_configDirty` is flipped by `storageSubscribeChanges("s.lora",
onCfgChange)` which also `xTaskNotifyGive(s_task)` to drop out of
`itsPoll`.

## Config — `applyConfig` / `radioStart` / `radioStop`

`s.lora.enable=1` triggers `radioStart`; `s.lora.enable=0` triggers
`radioStop`. Any other `s.lora.*` change does `radioStop → radioStart`
(cheap, ~30 ms) — avoids tracking which fields changed.

### `radioStart`

Reads required config from storage. **No preselected defaults** for
frequency / bandwidth / SF / CR / TXP — refusing to bring the radio up
unconfigured matches the "no preselected defaults" rule in plan §12.4:

```cpp
freq_hz   = s.lora.frequency           (no default)
bw_hz     = s.lora.bandwidth           (default 125_000)
sf        = s.lora.spreading_factor    (default 7)
cr        = s.lora.coding_rate         (default 5 = 4/5)
txp       = s.lora.tx_power            (no default)
preamble  = s.lora.preamble            (default 12)
sync_word = s.lora.sync_word           (string, default "0x42";
                                        parsed with strtol(base=0))
```

Sync word is a string so the panel can accept hex (`0x42`) alongside
decimal. `0x42` is the RNode/Reticulum-on-LoRa convention.

Validation gates:
- `freq_hz > 0`, `bw_hz > 0`
- `sf ∈ [5, 12]`
- `cr ∈ [5, 8]`
- `txp ∈ [-9, 22]`
- `syncWord ∈ (0, 0xFF]` (fallback to `0x42`)

Fails any of these → log `info("not started: configure freq/bw/sf/cr/txp
first")`, `publishState("unconfigured")`, return false.

`radio.begin(freq_mhz, bw_khz, sf, cr, syncWord, txp, preamble,
TCXO_VOLTAGE, useRegulatorLDO=false)`. Common SX126x failure codes:

| Code | Meaning |
|---|---|
| -2   | `CHIP_NOT_FOUND` — no SPI response (power off, wrong wiring). |
| -12  | invalid frequency for SX1262 (out of 150–960 MHz). |
| -705 | `SPI_CMD_TIMEOUT` — got a command but couldn't complete (TCXO not stabilised, wrong band for matching network, PLL didn't lock). |
| -706 | `SPI_CMD_INVALID`. |
| -707 | `SPI_CMD_FAILED`. |

On success: optionally `setDio2AsRfSwitch(true)` (if
`BOARD_LORA_DIO2_RF_SWITCH`), publish `lora.chip = "SX1262"` and
`lora.bitrate_eff` from `computeBitrate(sf, bw_hz, cr)`:

```
bitrate (bits/s) = SF * (BW / 2^SF) * (4 / cr_denom)
```

Hook DIO1 ISR (`setDio1Action(loraRadioIsr)`), `startReceive()`,
`publishState("up")`, `registerWithRnsd()`.

If rnsd register fails, **radio stays up** (`s_running = true`) but
`lora.state = "rnsd_unavailable"`; the task loop retries
`registerWithRnsd` each iteration.

### `radioStop`

```cpp
pmGpioWakeDisable(BOARD_LORA_DIO1_PIN);
setDio1Action(nullptr);
radio.sleep(false);              // cold-start sleep, ~160 nA, ~5 ms wake
s_running = false;
s_splitPending = false; s_splitLen = 0;
deregisterFromRnsd();
publishState("down");
```

`retainConfig=false` on SX1262 sleep — we always re-apply config in
`radioStart`, so there's no value in warm-start retention.

## rnsd registration

```cpp
rnsd_transport_t reg = {};
safeStrncpy(reg.name, "lora", ...);
reg.mtu     = RNS_MTU;
reg.bitrate = s_curBitrate;
reg.mode    = s_curMode;
reg.in = reg.out = 1;
reg.fwd = (mode == FULL || mode == GATEWAY) ? 1 : 0;
reg.rpt = 0;
s_rnsdHandle = itsConnect("rnsd", RNSD_PORT_TRANSPORT, &reg, sizeof(reg),
                          500ms, LORA_RNSD_CONNECT_REF,
                          onRnsdRecv, onRnsdDisconnect);
```

Mode comes from `s.lora.mode` (string: "full" / "gateway" /
"access_point" / "roaming" / "boundary"; default "gateway"). The
single `LORA_RNSD_CONNECT_REF = 1` is the ITS ref because there's
exactly one LoRa iface — unlike tcp, no `runtime_id` shuffling needed.

`onRnsdDisconnect` clears `s_rnsdHandle = -1`; the task loop re-connects
on the next iteration if still enabled. `onRnsdRecv` is the outbound
path — calls `drainOneOutbound()`.

## CLI — `lora`

```
lora               # status: state, chip, freq, BW, SF, CR, TXP, bitrate,
                   #   tx/rx frames+bytes, last RSSI/SNR, error counters
lora up | down     # toggle s.lora.enable
```

Plan §12.3 lists `lora freq|bw|sf|cr|txp|mode|rssi` shortcuts — those
are aliases for `set s.lora.<key>` and are not yet wired. Storage
editing works today.

## Storage keys

Persistent (set by `loraInit`, gated on `s.lora.version`):

```
s.lora.enable             0|1  (default 0)
s.lora.mode               "gateway"
s.lora.bandwidth          125000  (Hz)
s.lora.spreading_factor   7
s.lora.coding_rate        5  (4/5)
s.lora.preamble           12
s.lora.sync_word          "0x42"
# no defaults — user must pick:
s.lora.frequency          (Hz)
s.lora.tx_power           (dBm)
```

Ephemeral (`publishState` + `publishStats`):

```
lora.up                   0|1
lora.state                "unconfigured" | "error" | "up" | "down" |
                          "rnsd_unavailable"
lora.chip                 "SX1262"
lora.bitrate_eff          bits/sec
lora.stats.{tx_bytes,rx_bytes,tx_frames,rx_frames,
            crc_err,split_rx_timeout,rssi_last,snr_last}
```

## Gotchas

- **Power-enable pin first.** On any board where
  `BOARD_POWER_EN_PIN >= 0`, the radio is unreachable until that pin
  is HIGH. 100 ms settling delay before `radio.begin()` (T-Deck Plus
  TCXO regulator needs longer than RadioLib's 5 ms default).
- **Park the shared-bus LCD CS before `loraInit()`.** Otherwise the
  ST7789V drives MISO low during the first SPI exchange and `begin()`
  returns -706. Done by `main.cpp:parkSharedBusIdleCs()` today;
  removable once a display driver lands in spangap.
- **GPIO interrupt re-enable.** The HAL trampoline disables on every
  fire. `drainRadioIrq` must `gpio_intr_enable(DIO1)` to re-arm; miss
  it and the radio goes silent.
- **Half-duplex split coordination.** `s_splitPending` blocks all TX
  until the second frame arrives OR the 5 s timeout fires. Outbound
  bytes sit in the ITS stream buffer in the meantime — don't try to
  drain them in a tight loop.
- **`sync_word = 0x42` is the LoRa-Reticulum convention.** Default
  LoRa nets use `0x14` (public) or `0x12` (private mesh). Mismatched
  syncWord = silent radio (CRC pass on the receiver fails entirely;
  no frame ever surfaces).
- **No file I/O from this task.** PSRAM-stack, like every reticulous
  task. `info()` etc. only — no `printf`.
- **`radio.startReceive()` after every `transmit`.** RadioLib leaves
  the radio in standby; without re-arming, RX is dead until the next
  config reload.
