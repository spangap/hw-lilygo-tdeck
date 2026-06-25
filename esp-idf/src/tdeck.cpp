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
#include "storage.h"        /* battery.* ephemerals */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>

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

/* =========================================================================
 * Battery monitor — VBAT via the GPIO4 divider (see BOARD_BAT_ADC in tdeck.h).
 *
 * Always compiled (no UI dependency): a once-a-minute esp_timer samples the ADC
 * and publishes two ephemerals the rest of the system reacts to —
 *   battery.millivolt  — true VBAT in mV (pin reading × divider)
 *   battery.percent    — 0..100, via the measured discharge curve below
 * spangap-lcd's status bar subscribes to battery.percent for its icon; spangap-
 * core's `bat` CLI command prints both. No dedicated task — the periodic timer
 * callback does the read on the esp_timer task.
 * ========================================================================= */

namespace {

constexpr uint16_t BAT_MIN_MV    = 3040;           /* measured empty (3.04 V) */
constexpr uint16_t BAT_MAX_MV    = 4260;           /* measured full  (4.26 V) */
/* Divider ratio. The physical T-Deck divider is 100k/100k = 2.0 and the ADC is
 * curve-fit calibrated, so the true 2.0 applies — not Meshtastic's 2.11, which
 * compensates for an *uncalibrated* ADC. Trim NUM/DEN if a multimeter disagrees. */
constexpr uint32_t BAT_DIV_NUM   = 2, BAT_DIV_DEN = 1;
constexpr int      BAT_SAMPLES   = 16;             /* averaged per read — kills ADC jitter */
constexpr int64_t  BAT_PERIOD_US = 60LL * 1000000; /* re-sample cadence: every minute */

/* Battery voltage (scaled to 0..255 across [BAT_MIN_MV, BAT_MAX_MV]) at each
 * percent, measured on a real cell. Index i -> percent (100 - i); the array is
 * monotonic non-increasing, so the first entry <= our scaled reading gives the
 * percent. Input jitter is smoothed by the per-read averaging + EMA below, so
 * the raw curve's small edginess never reaches the published value. */
const uint8_t s_scaledVoltage[100] = {
    254, 242, 230, 227, 223, 219, 215, 213, 210, 207,
    206, 202, 202, 200, 200, 199, 198, 198, 196, 196,
    195, 195, 194, 192, 191, 188, 187, 185, 185, 185,
    183, 182, 180, 179, 178, 175, 175, 174, 172, 171,
    170, 169, 168, 166, 166, 165, 165, 164, 161, 161,
    159, 158, 158, 157, 156, 155, 151, 148, 147, 145,
    143, 142, 140, 140, 136, 132, 130, 130, 129, 126,
    125, 124, 121, 120, 118, 116, 115, 114, 112, 112,
    110, 110, 108, 106, 106, 104, 102, 101,  99,  97,
     94,  90,  81,  80,  76,  73,  66,  52,  32,   7,
};

adc_oneshot_unit_handle_t s_adc      = nullptr;
adc_cali_handle_t         s_adcCali  = nullptr;
adc_unit_t                s_adcUnit  = ADC_UNIT_1;
adc_channel_t             s_adcChan  = ADC_CHANNEL_3;   /* GPIO4; confirmed at init */
bool                      s_adcReady = false;
uint32_t                  s_mvEma    = 0;               /* smoothed VBAT, mV (0 = unset) */

uint8_t batteryPercent(uint16_t mv) {
    if (mv >= BAT_MAX_MV) return 100;
    if (mv <= BAT_MIN_MV) return 0;
    uint32_t scaled = (uint32_t)(mv - BAT_MIN_MV) * 256u / (BAT_MAX_MV - BAT_MIN_MV);
    for (uint8_t i = 0; i < 100; i++)
        if (s_scaledVoltage[i] <= scaled) return (uint8_t)(100 - i);
    return 0;
}

/* Sample, smooth, publish. Runs on the esp_timer task (and once at init). */
void batteryRead(void*) {
    if (!s_adcReady) return;
    int acc = 0, ok = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, s_adcChan, &raw) == ESP_OK) { acc += raw; ok++; }
    }
    if (!ok) return;
    int raw = acc / ok;
    int pinMv;
    if (!(s_adcCali && adc_cali_raw_to_voltage(s_adcCali, raw, &pinMv) == ESP_OK))
        pinMv = (int)((int64_t)raw * 3100 / 4095);      /* nominal 12-bit @ 12 dB */
    uint32_t mv = (uint32_t)pinMv * BAT_DIV_NUM / BAT_DIV_DEN;
    /* Light EMA across reads (~3-4 min at the 1/min cadence) so the icon and
     * percent don't wobble on noise; first reading seeds it directly (no lag). */
    s_mvEma = s_mvEma ? (s_mvEma * 3 + mv) / 4 : mv;
    uint16_t outMv = (uint16_t)s_mvEma;
    storageBegin();                                     /* one commit -> subscribers see both */
    storageSet("battery.millivolt", (int)outMv);
    storageSet("battery.percent",   (int)batteryPercent(outMv));
    storageEnd();
}

}  // namespace

/* init: hook — ADC bring-up, an initial reading, then the once-a-minute timer.
 * Runs after spangapInit() so storage is up for the ephemeral writes. */
void tdeckBatteryInit(void) {
    if (adc_oneshot_io_to_channel(BOARD_BAT_ADC, &s_adcUnit, &s_adcChan) != ESP_OK) {
        warn("battery: GPIO%d is not an ADC pin\n", BOARD_BAT_ADC);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = s_adcUnit;
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        warn("battery: adc unit init failed\n");
        return;
    }
    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten    = ADC_ATTEN_DB_12;        /* ~0..3.1 V pin range; VBAT/2 maxes ~2.13 V */
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(s_adc, s_adcChan, &ccfg) != ESP_OK) {
        warn("battery: adc channel config failed\n");
        return;
    }
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id  = s_adcUnit;
    cal.chan     = s_adcChan;
    cal.atten    = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_adcCali) != ESP_OK) {
        s_adcCali = nullptr;                /* fall back to nominal raw->mV scaling */
        warn("battery: adc calibration unavailable, using nominal scale\n");
    }
    s_adcReady = true;

    batteryRead(nullptr);                   /* publish an initial reading now */

    const esp_timer_create_args_t targs = { .callback = batteryRead, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "battery", .skip_unhandled_events = true };
    esp_timer_handle_t th = nullptr;
    if (esp_timer_create(&targs, &th) == ESP_OK)
        esp_timer_start_periodic(th, BAT_PERIOD_US);
    else
        warn("battery: timer create failed\n");
}

void tdeckStart(void) {
    tdeckPowerInit();                       /* power rail + shared-SPI CS park */
    /* Create the shared I2C0 bus now, while we're still single-threaded — touch
     * (lcd task), keyboard (kbpoll task) and the RTC (gps task) all bring it up
     * lazily and could otherwise race i2c_new_master_bus() on the same port. */
    tdeckI2cBus();
}
