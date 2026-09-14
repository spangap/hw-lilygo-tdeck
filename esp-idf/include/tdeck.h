/**
 * tdeck.h — LilyGo T-Deck Plus board support for reticulous.
 *
 * What this module provides:
 *   - Compile-time hardware constants for the board's bespoke peripherals: the
 *     peripheral power rail, GT911 touch, optical trackball, centre/Home button,
 *     and QWERTY keyboard. Consumed by tdeck.cpp.
 *   - The board's bring-up services (`TdeckBoard`, `TdeckBattery`). tdeck.cpp
 *     owns and starts everything board-specific: the peripheral power rail, the
 *     shared-SPI CS park, and — when the lcd component is built — the input HAL
 *     (GT911 touch, trackball pointer, centre/Home button) and the ESP32-C3
 *     QWERTY keyboard.
 *
 * The display itself (SPI bus, ST7789 controller, backlight, orientation) is not
 * wired here: it is owned by the lcd component and configured through
 * CONFIG_LCD_* in sdkconfig.defaults. The LoRa radio pins likewise come from
 * iface-lora's CONFIG_LORA*, and the GNSS receiver's from gps's
 * CONFIG_GPS_* — all supplied by straddle.yaml's kconfig: block. So this header
 * carries only the board's own input pins. Runtime LoRa parameters (freq, BW,
 * SF, ...) live in storage at s.lora.* (see lora.cpp). Full reference:
 * docs/tdeck.md.
 */
#pragma once

#include "driver/i2c_master.h"
#include "sdkconfig.h"
#include "service.h"

/* The board's name as a person reads it — published to sys.board at init and
 * shown in the Hardware section of Settings, so the UI never spells a board
 * name of its own. */
#define BOARD_NAME              "LilyGO T-Deck"
/* Master peripheral power-enable pin. T-Deck (Plus) gates the +3.3 V rail to
 * display, SD, GPS *and* LoRa radio behind GPIO 10. Must be driven HIGH at boot
 * or SPI traffic to the SX1262 is just SPI traffic into a powered-down chip.
 * -1 = no such pin. */
#define BOARD_POWER_EN_PIN      10
#define BOARD_POWER_EN_ACTIVE   1   /* 1 = active high, 0 = active low */

/* Battery sense: VBAT through a 2:1 resistor divider (100k/100k) into GPIO 4
 * (ADC1). The divider sits on raw VBAT *upstream* of BOARD_POWER_EN_PIN — it has
 * no enable gate and conducts continuously (~21 uA @ 4.2 V) whenever a cell is
 * connected; only the case slide switch cuts it. So the ADC pin always carries
 * VBAT/2 and needs no power-up step. Read by the tdeck task (tdeck_lcd.cpp). */
#define BOARD_BAT_ADC           4

/* GT911 capacitive touch (optional sub-revision). Shares the on-board I2C bus
 * (keyboard is 0x55, touch 0x5D). No reset GPIO routed. */
#define BOARD_TOUCH_I2C_SDA     18
#define BOARD_TOUCH_I2C_SCL     8
#define BOARD_TOUCH_INT_PIN     16
#define BOARD_TOUCH_RST_PIN     (-1)

/* Home / centre button: GPIO 0 (the BOOT-strap pin, also the trackball
 * centre-press; shared with the mic, which reticulous never uses). Read as a
 * pulled-up active-low input after boot — tdeck_lcd.cpp gives it five meanings: a
 * standby_hold-ms hold enters standby, one click is a click, two clicks go Home
 * (lcdGoHome), three raise the app switcher (lcdShowRecents), and a press while in
 * standby wakes the device. Timings are s.tdeck.standby_hold_ms and
 * s.tdeck.multiclick_ms. */
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
 * 0x55. A 1-byte read returns the next pressed ASCII char (0 = none) and pops
 * it; only the last unread key is held. The C3 firmware reports press events
 * only — no key-up / hold / repeat — and never drives its INT (GPIO 46), so the
 * read is polled. Writes are commands: {0x01,duty} sets the keyboard backlight
 * (C3 GPIO 9, 0-255, 0 at power-on), {0x03}/{0x04} switch between raw-matrix
 * and ASCII key mode. See docs/tdeck.md for the full protocol and keycodes. */
#define BOARD_KB_ADDR           0x55
#define BOARD_KB_INT_PIN        46
/* Keyboard write commands. BRIGHTNESS sets the lamp duty now; ALT_B_LEVEL sets
 * the duty the keyboard's own Alt+B lights to, and the C3 ignores it at 30 or
 * below. Neither is readable back. */
#define BOARD_KB_CMD_BRIGHTNESS   0x01
#define BOARD_KB_CMD_ALT_B_LEVEL  0x02

/**
 * Board bring-up. TdeckBoard::onStart() is the always-on hardware bring-up: it
 * drives the peripheral power rail HIGH and parks the shared-SPI CS lines (the
 * first shared-bus access is fs_mount_sd() *inside* spangapInit()), so it runs
 * in the start band, before spangapInit().
 *
 * The on-device-UI input HAL (touch/trackball/button) and the QWERTY keyboard
 * live in conditional/spangap-lcd/src/tdeck_lcd.cpp and run via two
 * when: spangap/spangap-lcd companion (TdeckLcdInput) — onStart (start band,
 * input HAL register before lcdInit) and onInit (init band, keyboard, needs the
 * lcd task). Compiled and registered only when spangap-lcd is staged, so no #if
 * is needed anywhere.
 */
class TdeckBoard : public Service {
public:
    void onStart() override;   /* power rail + shared-SPI CS park, before spangapInit() */
    void onInit()  override;   /* publishes sys.board once storage is up */
};

/**
 * Battery monitor bring-up (onInit): configures the GPIO4 ADC, publishes an
 * initial battery.millivolt / battery.percent, and arms a once-a-minute
 * esp_timer to keep them fresh. init band (needs storage up). No task of its own
 * — the periodic timer callback does the sampling.
 */
class TdeckBattery : public Service {
public:
    void onInit() override;    /* ADC, first sample, and the 1/min timer */
};

/**
 * Shared I2C0 master bus (SDA=BOARD_TOUCH_I2C_SDA, SCL=BOARD_TOUCH_I2C_SCL).
 * Created lazily on first call; the first caller wins and all later callers get
 * the same handle. Home to the GT911 touch (0x5D) and QWERTY keyboard (0x55) —
 * conditional/spangap-lcd/ — and the ES7210 codec (0x40) — conditional/audio/.
 * Returns nullptr if the bus could not be created. Thread-safe at the IDF
 * driver layer.
 */
i2c_master_bus_handle_t tdeckI2cBus(void);
