/**
 * tdeck.h — LilyGo T-Deck Plus board support for reticulous.
 *
 * What this module provides:
 *   - Compile-time hardware constants for the board's bespoke peripherals: the
 *     peripheral power rail, GT911 touch, optical trackball, centre/Home button,
 *     QWERTY keyboard, and GNSS. Consumed by tdeck.cpp and gps.cpp.
 *   - The board bring-up API (`tdeckPreInit` / `tdeckPostInit`). tdeck.cpp owns
 *     and starts everything board-specific: the peripheral power rail, the
 *     shared-SPI CS park, and — when the lcd component is built — the input HAL
 *     (GT911 touch, trackball pointer, centre/Home button) and the ESP32-C3
 *     QWERTY keyboard.
 *
 * The display itself (SPI bus, ST7789 controller, backlight, orientation) is not
 * wired here: it is owned by the lcd component and configured through
 * CONFIG_LCD_* in sdkconfig.defaults. The LoRa radio pins likewise come from
 * tr-lora's CONFIG_LORA*. So this header carries only the board's own
 * input / GNSS pins. Runtime LoRa parameters (freq, BW, SF, ...) live in storage
 * at s.lora.* (see lora.cpp). Full reference: docs/tdeck.md.
 */
#pragma once

#include "driver/i2c_master.h"
#include "sdkconfig.h"

#define BOARD_NAME              "T-Deck Plus"
/* Master peripheral power-enable pin. T-Deck (Plus) gates the +3.3 V rail to
 * display, SD, GPS *and* LoRa radio behind GPIO 10. Must be driven HIGH at boot
 * or SPI traffic to the SX1262 is just SPI traffic into a powered-down chip.
 * -1 = no such pin. */
#define BOARD_POWER_EN_PIN      10
#define BOARD_POWER_EN_ACTIVE   1   /* 1 = active high, 0 = active low */

/* GT911 capacitive touch (optional sub-revision). Shares the on-board I2C bus
 * (keyboard is 0x55, touch 0x5D). No reset GPIO routed. */
#define BOARD_TOUCH_I2C_SDA     18
#define BOARD_TOUCH_I2C_SCL     8
#define BOARD_TOUCH_INT_PIN     16
#define BOARD_TOUCH_RST_PIN     (-1)

/* Home / centre button: GPIO 0 (the BOOT-strap pin, also the trackball
 * centre-press; shared with the mic, which reticulous never uses). Read as a
 * pulled-up active-low input after boot — tdeck.cpp makes a short press a click
 * and a >=300ms hold "go home" (lcdGoHome). */
#define BOARD_HOME_BTN_PIN      0

/* BlackBerry-style optical trackball: four direction lines, each pulsing
 * (active-low) as the ball rolls that way; tdeck.cpp counts falling edges and
 * integrates them into a mouse-cursor position. Centre-press is the Home button
 * above. Direction→pin per Meshtastic's t-deck variant; the actual ball
 * orientation is sub-revision dependent (docs/tdeck.md §1.2: a sample had
 * DOWN/RIGHT swapped) — flip these or the dx/dy signs if it feels wrong. */
#define BOARD_TBOX_UP_PIN       3
#define BOARD_TBOX_DOWN_PIN     15
#define BOARD_TBOX_LEFT_PIN     1
#define BOARD_TBOX_RIGHT_PIN    2

/* QWERTY keyboard: an ESP32-C3 keyboard MCU on the shared I2C0 bus, I2C slave
 * 0x55. A 1-byte read returns the next pressed ASCII char (0 = none); INT
 * (GPIO 46) asserts when a key is buffered. The C3 firmware only reports press
 * events — no key-up / hold / repeat. */
#define BOARD_KB_ADDR           0x55
#define BOARD_KB_INT_PIN        46

/* GNSS receiver — pre-soldered on the Plus, hard-wired to the Grove header.
 * NMEA over UART 8N1. The chip is production-batch dependent: Quectel L76K
 * (default 9600) or u-blox MIA-M10Q (default 38400) — no host-visible id, so
 * gps.cpp autobauds and infers the model from the baud (docs/tdeck.md §1.3).
 * Powered off the shared BOARD_POWER_EN_PIN rail (no independent GPS enable);
 * PPS is not routed on the Plus. Host RX <- GPS TX = 44; host TX -> GPS RX = 43. */
#define BOARD_GPS_UART_NUM      1
#define BOARD_GPS_RX_PIN        44
#define BOARD_GPS_TX_PIN        43

/**
 * Board bring-up — two phases around spangapInit():
 *
 *   tdeckPreInit()   BEFORE spangapInit(). Drives the peripheral power rail HIGH
 *                    and parks the shared-SPI CS lines (the first shared-bus
 *                    access is fs_mount_sd() *inside* spangapInit()), and — when
 *                    built with CONFIG_SPANGAP_LCD — registers the input HAL so
 *                    the lcd task can wire it once the panel is up.
 *
 *   tdeckPostInit()  AFTER spangapInit(). Brings up the QWERTY keyboard, which
 *                    needs the lcd task spangapInit() created (CONFIG_SPANGAP_LCD).
 *                    No-op otherwise.
 *
 * It can't collapse to one call: the power rail must be up before spangapInit()'s
 * SD probe and the input HAL registered before its lcdInit(), but the keyboard
 * needs the lcd task that spangapInit() creates — so the bring-up straddles it.
 */
void tdeckPreInit(void);
void tdeckPostInit(void);

/**
 * Shared I2C0 master bus (SDA=BOARD_TOUCH_I2C_SDA, SCL=BOARD_TOUCH_I2C_SCL).
 * Created lazily on first call; the first caller wins and all later callers get
 * the same handle. Home to the GT911 touch (0x5D) and QWERTY keyboard (0x55)
 * when the lcd component is built, and the PCF8563 RTC (0x51) regardless — so
 * this accessor is always compiled, not gated on CONFIG_SPANGAP_LCD. Returns
 * nullptr if the bus could not be created. Thread-safe at the IDF driver layer.
 */
i2c_master_bus_handle_t tdeckI2cBus(void);
