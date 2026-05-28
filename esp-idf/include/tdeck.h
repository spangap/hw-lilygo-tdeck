/**
 * tdeck.h — LilyGo T-Deck Plus board support for reticulous.
 *
 * What this module provides:
 *   - Compile-time hardware constants (pins, SPI host, TCXO voltage) for the
 *     selected board, chosen via `Kconfig.projbuild` → `CONFIG_RETICULOUS_BOARD_*`.
 *     Consumed by lora.cpp (the LoRa pin map), main.cpp, and the on-device UI.
 *   - The board bring-up API (`tdeckPreInit` / `tdeckPostInit`). tdeck.cpp owns
 *     and starts everything board-specific: the peripheral power rail, the
 *     shared-SPI CS park, the reset-on-off power-down hook, and — when the lcd
 *     component is built (`CONFIG_SPANGAP_LCD`) — the ST7789V display, GT911
 *     touch, trackball pointer, centre/Home button, and the ESP32-C3 QWERTY
 *     keyboard.
 *
 * Add a new board by adding its Kconfig `choice` entry and a constants block
 * here; the driver code in tdeck.cpp is otherwise board-agnostic where it can be.
 *
 * Runtime LoRa parameters (freq, BW, SF, CR, TXP, ...) are NOT here — they live
 * in storage at s.lora.* (see lora.cpp). Full reference: docs/tdeck.md.
 */
#pragma once

#include "driver/spi_common.h"
#include "sdkconfig.h"

#if defined(CONFIG_RETICULOUS_BOARD_TDECK_PLUS)

    #define BOARD_NAME              "T-Deck Plus"
    /* Master peripheral power-enable pin. T-Deck (Plus) gates the +3.3 V
     * rail to display, SD, GPS *and* LoRa radio behind GPIO 10. Must be
     * driven HIGH at boot or SPI traffic to the SX1262 is just SPI
     * traffic into a powered-down chip. -1 = no such pin on this board. */
    #define BOARD_POWER_EN_PIN      10
    #define BOARD_POWER_EN_ACTIVE   1   /* 1 = active high, 0 = active low */

    /* SX1262 module pins (passed to RadioLib's Module() ctor). */
    #define BOARD_LORA_CS_PIN       9
    #define BOARD_LORA_DIO1_PIN     45
    #define BOARD_LORA_RST_PIN      17
    #define BOARD_LORA_BUSY_PIN     13
    /* SPI bus (shared with display + SD on this board). */
    #define BOARD_LORA_SPI_HOST     SPI2_HOST   /* FSPI on ESP32-S3 */
    #define BOARD_LORA_SCK_PIN      40
    #define BOARD_LORA_MOSI_PIN     41
    #define BOARD_LORA_MISO_PIN     38
    /* ST7789V display — shares the SPI2 bus (SCK/MOSI/MISO above) with LoRa
     * + SD. CS=12; no dedicated reset (the panel resets with the +3.3 V rail
     * behind BOARD_POWER_EN_PIN), so the esp_lcd panel uses reset_gpio = -1.
     * tdeckPreInit() still parks CS HIGH at boot — needed before the SD probe,
     * which runs (inside spangapInit) before lcdInit() claims the pin. */
    #define BOARD_LCD_CS_PIN        12
    #define BOARD_LCD_DC_PIN        11
    #define BOARD_LCD_BL_PIN        42      /* backlight (LEDC PWM) */
    #define BOARD_LCD_H_RES         320     /* landscape (after swap_xy) */
    #define BOARD_LCD_V_RES         240
    #define BOARD_LCD_PCLK_HZ       (40 * 1000 * 1000)

    /* GT911 capacitive touch (optional sub-revision). Shares the on-board
     * I2C bus (keyboard is 0x55, touch 0x5D). No reset GPIO routed. */
    #define BOARD_TOUCH_I2C_SDA     18
    #define BOARD_TOUCH_I2C_SCL     8
    #define BOARD_TOUCH_INT_PIN     16
    #define BOARD_TOUCH_RST_PIN     (-1)

    /* Home / centre button: GPIO 0 (the BOOT-strap pin, also the trackball
     * centre-press; shared with the mic, which reticulous never uses). Read as a
     * pulled-up active-low input after boot — tdeck.cpp exposes it to the lcd
     * component, which makes a short press a click and a >=300ms hold "go home". */
    #define BOARD_HOME_BTN_PIN      0

    /* BlackBerry-style optical trackball: four direction lines, each pulsing
     * (active-low) as the ball rolls that way; tdeck.cpp counts falling edges
     * and integrates them into a mouse-cursor position. Centre-press is the Home
     * button above. Direction→pin per Meshtastic's t-deck variant; the actual
     * ball orientation is sub-revision dependent (docs/tdeck.md §1.2: a sample
     * had DOWN/RIGHT swapped) — flip these or the dx/dy signs if it feels wrong. */
    #define BOARD_TBOX_UP_PIN       3
    #define BOARD_TBOX_DOWN_PIN     15
    #define BOARD_TBOX_LEFT_PIN     1
    #define BOARD_TBOX_RIGHT_PIN    2

    /* QWERTY keyboard: an ESP32-C3 keyboard MCU on the shared I2C0 bus, I2C
     * slave 0x55. A 1-byte read returns the next pressed ASCII char (0 = none);
     * INT (GPIO 46) asserts when a key is buffered. The C3 firmware only reports
     * press events — no key-up / hold / repeat. */
    #define BOARD_KB_ADDR           0x55
    #define BOARD_KB_INT_PIN        46
    /* TCXO on this board is 1.8 V. */
    #define BOARD_LORA_TCXO_VOLTAGE 1.8f
    /* No external RX/TX RF switch; SX1262 drives DIO2 as the antenna switch. */
    #define BOARD_LORA_DIO2_RF_SWITCH 1

    /* GNSS receiver — pre-soldered on the Plus, hard-wired to the Grove header.
     * NMEA over UART 8N1. The chip is production-batch dependent: Quectel L76K
     * (default 9600) or u-blox MIA-M10Q (default 38400) — no host-visible id, so
     * gps.cpp autobauds and infers the model from the baud (docs/tdeck.md §1.3).
     * Powered off the shared BOARD_POWER_EN_PIN rail (no independent GPS enable);
     * PPS is not routed on the Plus. Host RX <- GPS TX = 44; host TX -> GPS RX = 43. */
    #define BOARD_GPS_UART_NUM      1
    #define BOARD_GPS_RX_PIN        44
    #define BOARD_GPS_TX_PIN        43

#else
    #error "Pick a board in menuconfig: Reticulous board → Target board"
#endif

/**
 * Board bring-up — two phases around spangapInit():
 *
 *   tdeckPreInit()   BEFORE spangapInit(). Drives the peripheral power rail HIGH
 *                    and parks the shared-SPI CS lines (the first shared-bus
 *                    access is fs_mount_sd() *inside* spangapInit()), installs the
 *                    reset-on-off peripheral power-down hook, and — when built
 *                    with CONFIG_SPANGAP_LCD — registers the display/touch/pointer
 *                    HAL so spangapInit()'s lcdInit() can bring the panel up.
 *
 *   tdeckPostInit()  AFTER spangapInit(). Brings up the QWERTY keyboard, which
 *                    needs the lcd task spangapInit() created (CONFIG_SPANGAP_LCD).
 *                    No-op otherwise.
 *
 * It can't collapse to one call: the power rail must be up before spangapInit()'s
 * SD probe and the HAL registered before its lcdInit(), but the keyboard needs
 * the lcd task that spangapInit() creates — so the bring-up straddles it.
 */
void tdeckPreInit(void);
void tdeckPostInit(void);
