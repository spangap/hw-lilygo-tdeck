/**
 * tdeck.cpp — LilyGo T-Deck Plus board support, end to end.
 *
 * Single owner of all T-Deck Plus hardware bring-up. See tdeck.h for the API
 * contract and docs/tdeck.md for the module + hardware reference. Layout:
 *
 *   1. Peripheral power rail + shared-SPI CS park.
 *      Always compiled — SD and LoRa need the +3.3 V rail even with no on-device
 *      UI. Driven from tdeckStart() before spangapInit().
 *   2. [CONFIG_SPANGAP_LCD] ST7789V display + GT911 touch + trackball pointer +
 *      centre/Home button, registered as the lcd component's board HAL.
 *   3. [CONFIG_SPANGAP_LCD] QWERTY keyboard (ESP32-C3 @ I2C 0x55), end to end.
 *   4. The two-phase public API (tdeckStart / tdeckInit).
 */
#include "tdeck.h"
#include "log.h"            /* warn (i2c bus init) */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* =========================================================================
 * 1. Peripheral power rail + shared-SPI CS park
 *
 * Board prerequisites for the very first shared-SPI-bus access — which is
 * fs_mount_sd() *inside* spangapInit(). Both MUST happen before that:
 *
 *  1. Peripheral power rail. T-Deck (Plus) gates the +3.3 V rail to the
 *     SD card (and display/GPS/LoRa) behind BOARD_POWER_EN_PIN. Until it's
 *     driven HIGH the SD card is unpowered, so esp_vfs_fat_sdspi_mount()
 *     just times out (ESP_ERR_TIMEOUT). loraInit() also asserts this, but
 *     that runs long after spangapInit() — far too late for the SD probe.
 *  2. Idle CS park. The ST7789V shares the bus with no driver owning its
 *     CS yet; park it HIGH so it doesn't drive MISO during the SD/LoRa
 *     transactions. (Both go away when a display driver lands in spangap.)
 *
 * Re-asserting the power pin in loraInit() is a harmless idempotent no-op.
 * ========================================================================= */

static void tdeckPowerInit(void)
{
#if BOARD_POWER_EN_PIN >= 0
    gpio_config_t pwr = {};
    pwr.pin_bit_mask = 1ULL << BOARD_POWER_EN_PIN;
    pwr.mode         = GPIO_MODE_OUTPUT;
    pwr.pull_up_en   = GPIO_PULLUP_DISABLE;
    pwr.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pwr.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&pwr);
    gpio_set_level((gpio_num_t)BOARD_POWER_EN_PIN, BOARD_POWER_EN_ACTIVE ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(100));   /* 3.3 V rail settle (matches lora.cpp) */
#endif
    /* Park every shared-bus device's CS HIGH (deselected) so none of them
     * drive MISO during the SD probe. The SX1262's CS especially: now that
     * the power rail is up it is live, and loraInit() (which would own its
     * CS) runs long after the SD probe. */
    auto parkCsHigh = [](int pin) {
        if (pin < 0) return;
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << pin;
        cfg.mode         = GPIO_MODE_OUTPUT;
        cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type    = GPIO_INTR_DISABLE;
        gpio_config(&cfg);
        gpio_set_level((gpio_num_t)pin, 1);
    };
    /* The LCD CS pin comes from the lcd component's Kconfig (CONFIG_LCD_CS_PIN);
     * park it HIGH before the SD probe (inside spangapInit, before lcdInit claims
     * the pin) so the panel doesn't drive MISO. Defined only on an LCD build. */
#if defined(CONFIG_LCD_CS_PIN)
    parkCsHigh(CONFIG_LCD_CS_PIN);
#endif
    /* LoRa radio CS pins come from iface-lora's Kconfig (CONFIG_LORA*). Park
     * each configured radio's CS so it doesn't drive MISO during the SD
     * probe (which runs inside spangapInit, before loraInit owns them). */
#if defined(CONFIG_LORA0_CS_PIN)
    parkCsHigh(CONFIG_LORA0_CS_PIN);
#endif
#if defined(CONFIG_LORA1_CS_PIN)
    parkCsHigh(CONFIG_LORA1_CS_PIN);
#endif
#if defined(CONFIG_LORA2_CS_PIN)
    parkCsHigh(CONFIG_LORA2_CS_PIN);
#endif
#if defined(CONFIG_LORA3_CS_PIN)
    parkCsHigh(CONFIG_LORA3_CS_PIN);
#endif
}

/* =========================================================================
 * 1b. Shared I2C0 master bus
 *
 * Always compiled (not LCD-gated): the GT911 touch and QWERTY keyboard live
 * here only with CONFIG_SPANGAP_LCD, but the PCF8563 RTC (gps.cpp, 0x51) shares
 * the same bus on a headless build too. Created once; first caller wins.
 * ========================================================================= */

static i2c_master_bus_handle_t s_i2c = nullptr;   /* shared I2C0: touch + keyboard + RTC */

i2c_master_bus_handle_t tdeckI2cBus(void) {
    if (s_i2c) return s_i2c;
    i2c_master_bus_config_t bcfg = {};
    bcfg.i2c_port                     = I2C_NUM_0;
    bcfg.sda_io_num                   = (gpio_num_t)BOARD_TOUCH_I2C_SDA;
    bcfg.scl_io_num                   = (gpio_num_t)BOARD_TOUCH_I2C_SCL;
    bcfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bcfg.glitch_ignore_cnt            = 7;
    bcfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bcfg, &s_i2c) != ESP_OK) {
        warn("i2c: bus init failed\n");
        s_i2c = nullptr;
    }
    return s_i2c;
}

/* =========================================================================
 * On-device UI input HAL (touch / trackball / button / keyboard) now lives in
 * conditional/spangap-lcd/src/tdeck_lcd.cpp — compiled only when spangap-lcd is
 * staged, registered via the tdeckLcdStart / tdeckLcdInit when: hooks. No #if.
 *
 * Public API — the always-on board bring-up (see tdeck.h).
 * ========================================================================= */

void tdeckStart(void) {
    tdeckPowerInit();                       /* power rail + shared-SPI CS park */
    /* Create the shared I2C0 bus now, while we're still single-threaded — touch
     * (lcd task), keyboard (kbpoll task) and the RTC (gps task) all bring it up
     * lazily and could otherwise race i2c_new_master_bus() on the same port. */
    tdeckI2cBus();
}
