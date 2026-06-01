/**
 * tdeck.cpp — LilyGo T-Deck Plus board support, end to end.
 *
 * Single owner of all T-Deck Plus hardware bring-up. See tdeck.h for the API
 * contract and docs/tdeck.md for the module + hardware reference. Layout:
 *
 *   1. Peripheral power rail + shared-SPI CS park + reset-on-off power-down.
 *      Always compiled — SD and LoRa need the +3.3 V rail even with no on-device
 *      UI. Driven from tdeckPreInit() before spangapInit().
 *   2. [CONFIG_SPANGAP_LCD] ST7789V display + GT911 touch + trackball pointer +
 *      centre/Home button, registered as the lcd component's board HAL.
 *   3. [CONFIG_SPANGAP_LCD] QWERTY keyboard (ESP32-C3 @ I2C 0x55), end to end.
 *   4. The two-phase public API (tdeckPreInit / tdeckPostInit).
 */
#include "tdeck.h"
#include "pm.h"             /* resetOnOffSetPowerOff */
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
    /* Release any deep-sleep pad hold left by a prior reset-on-off power-down
     * (resetOnOffHandler → tdeckPeripheralPowerOff). Without this, if the hold
     * survived the EN reset the rail would stay cut and the board would boot
     * with dead peripherals. Harmless no-op when no hold is active. */
    gpio_hold_dis((gpio_num_t)BOARD_POWER_EN_PIN);
    gpio_deep_sleep_hold_dis();

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
#ifdef BOARD_LCD_CS_PIN
    parkCsHigh(BOARD_LCD_CS_PIN);
#endif
#ifdef BOARD_LORA_CS_PIN
    parkCsHigh(BOARD_LORA_CS_PIN);
#endif
}

#if BOARD_POWER_EN_PIN >= 0
/* Reset-on-off power-down hook (resetOnOffHandler calls this just before deep
 * sleep). Drive the master peripheral rail to its inactive level and hold the
 * pad through deep sleep so display/SD/GPS/LoRa stay unpowered until the next
 * reset re-runs tdeckPowerInit(). */
static void tdeckPeripheralPowerOff(void)
{
    gpio_set_level((gpio_num_t)BOARD_POWER_EN_PIN, BOARD_POWER_EN_ACTIVE ? 0 : 1);
    gpio_hold_en((gpio_num_t)BOARD_POWER_EN_PIN);
    gpio_deep_sleep_hold_en();
}
#endif

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
 * 2. + 3. On-device UI: ST7789V display, GT911 touch, trackball pointer,
 *    centre/Home button (the lcd component's board HAL), and the QWERTY
 *    keyboard. Only compiled when the lcd module is enabled.
 * ========================================================================= */
#if CONFIG_SPANGAP_LCD

#include "lcd_board.h"
#include "lcd.h"
#include "spi_helper.h"
#include "storage.h"
#include "log.h"

#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/queue.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "lvgl.h"

#define BL_MODE   LEDC_LOW_SPEED_MODE
#define BL_TIMER  LEDC_TIMER_0
#define BL_CH     LEDC_CHANNEL_0

static esp_lcd_panel_handle_t  s_panel = nullptr;

/* Forward declarations so the HAL ops table + cross-calls resolve regardless
 * of definition order. */
static esp_lcd_panel_handle_t  tdeckLcdInit(esp_lcd_panel_io_handle_t* ioOut,
                                            int* wOut, int* hOut);
static void                    tdeckLcdShutdown(void);
static void                    tdeckLcdBacklight(uint8_t level);
static esp_lcd_touch_handle_t  tdeckTouchInit(void);
static void                    tdeckButtonInit(void);
static bool                    tdeckButtonRead(void);
static void                    tdeckTrackballInit(void);
static bool                    tdeckPointerRead(int* x, int* y);

static void backlightInit(void) {
    ledc_timer_config_t t = {};
    t.speed_mode      = BL_MODE;
    t.duty_resolution = LEDC_TIMER_8_BIT;       /* 0..255 */
    t.timer_num       = BL_TIMER;
    t.freq_hz         = 5000;
    /* RC_FAST (not APB) so the PWM can keep toggling through light sleep — see
     * tdeckLcdBacklight. RC_FAST is imprecise, but only duty matters here. */
    t.clk_cfg         = LEDC_USE_RC_FAST_CLK;
    ledc_timer_config(&t);

    /* Configure the channel exactly once — this is where GPIO42 gets reserved.
     * Brightness changes afterwards are ledc_set_duty only (tdeckLcdBacklight),
     * never another ledc_channel_config: re-running it re-reserves the pin and
     * logs "GPIO 42 is not usable, maybe conflict with others" on every call. */
    ledc_channel_config_t c = {};
    c.gpio_num   = BOARD_LCD_BL_PIN;
    c.speed_mode = BL_MODE;
    c.channel    = BL_CH;
    c.timer_sel  = BL_TIMER;
    c.hpoint     = 0;
    c.duty       = 0;                            /* start dark */
    c.sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE;
    ledc_channel_config(&c);
}

static esp_lcd_panel_handle_t tdeckLcdInit(esp_lcd_panel_io_handle_t* ioOut,
                                           int* wOut, int* hOut) {
    /* Shared SPI2 bus (idempotent — SD/LoRa may already have brought it up). */
    spi_bus_config_t bus = {};
    bus.sclk_io_num     = BOARD_LORA_SCK_PIN;
    bus.mosi_io_num     = BOARD_LORA_MOSI_PIN;
    bus.miso_io_num     = BOARD_LORA_MISO_PIN;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = 4096;
    spiHelperInitBus(BOARD_LORA_SPI_HOST, &bus);

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num       = BOARD_LCD_CS_PIN;
    io_cfg.dc_gpio_num       = BOARD_LCD_DC_PIN;
    io_cfg.pclk_hz           = BOARD_LCD_PCLK_HZ;
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;
    io_cfg.spi_mode          = 0;
    io_cfg.trans_queue_depth = 10;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)(intptr_t)BOARD_LORA_SPI_HOST,
                                 &io_cfg, &io) != ESP_OK) {
        err("lcd: panel-io init failed\n");
        return nullptr;
    }

    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num = -1;                     /* resets with the power rail */
    pcfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    pcfg.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7789(io, &pcfg, &s_panel) != ESP_OK) {
        err("lcd: st7789 init failed\n");
        return nullptr;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);    /* ST7789 panels need inversion */
    esp_lcd_panel_swap_xy(s_panel, true);         /* 240x320 -> 320x240 landscape */
    esp_lcd_panel_mirror(s_panel, true, false);   /* tweak if image is flipped */
    esp_lcd_panel_disp_on_off(s_panel, true);

    backlightInit();

    /* Shared GPIO ISR service for the input INT lines (touch / button / keyboard).
     * LoRa's DIO1 path also installs it with ESP_INTR_FLAG_IRAM — match the flag
     * so whichever runs first wins; the loser gets ESP_ERR_INVALID_STATE. The lcd
     * task can reach here before loraInit(), so we must not rely on LoRa for it. */
    esp_err_t isr = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE)
        warn("lcd: gpio isr service: %s\n", esp_err_to_name(isr));

    tdeckButtonInit();
    tdeckTrackballInit();
    /* The keyboard (I2C 0x55 + GPIO46 INT + its LVGL indev) comes up in
     * tdeckPostInit() after spangapInit() — it needs the lcd task to exist.
     * It shares this bus via tdeckI2cBus(). */

    if (ioOut) *ioOut = io;
    if (wOut)  *wOut  = BOARD_LCD_H_RES;
    if (hOut)  *hOut  = BOARD_LCD_V_RES;
    return s_panel;
}

static void tdeckLcdShutdown(void) {
    tdeckLcdBacklight(0);
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, false);
}

/* Panel standby for the lcd component's inactivity blank (backlight is cut by lcd
 * itself). disp_off retains GRAM, so wake is instant. The keyboard poll task is
 * left running — it already idles at 5 Hz (POLL_IDLE), enough for a keypress to
 * wake the screen via lcdNotifyActivity() in readCb. */
static void tdeckDisplayPower(bool on) {
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, on);
}

/* Brightness change only — the channel is already configured (backlightInit), so
 * this is a plain duty update, no ledc_channel_config (which would re-reserve the
 * GPIO and warn). The lcd task holds no NO_LIGHT_SLEEP lock, so the channel is
 * KEEP_ALIVE (clocked from RC_FAST): a dimmed screen stays dimmed across light
 * sleep rather than freezing at a random duty phase, and a constant 0/full level
 * is held just the same. (The ESP32-S3 lacks SOC_LEDC_SUPPORT_SLEEP_RETENTION, so
 * the lower-power NO_ALIVE_ALLOW_PD mode is rejected outright and there's no
 * domain power-down to gain from anyway.) */
static void tdeckLcdBacklight(uint8_t level) {
    uint32_t duty = (level == 255) ? (1u << 8) : level;   /* 8-bit: 256 = true 100% */
    ledc_set_duty(BL_MODE, BL_CH, duty);
    ledc_update_duty(BL_MODE, BL_CH);
}

static esp_lcd_touch_handle_t tdeckTouchInit(void) {
    i2c_master_bus_handle_t i2c = tdeckI2cBus();
    if (!i2c) return nullptr;

    /* Multi-touch is opt-in and ephemeral: a consumer (e.g. the maps app) sets
     * the runtime flag `tdeck.multi_touch` (no `s.` — not persisted, not a
     * setting) while it wants gestures. Watch it and flip the generic lcd
     * multipoint read mode. The GT911 is a 5-point controller. Runs on the lcd
     * task (board HAL init), same as the trackball subs above. */
    NOW_AND_ON_CHANGE("tdeck.multi_touch", { lcdTouchSetMultipoint(atoi(val) != 0); });

    esp_lcd_touch_config_t tcfg = {};
    /* Leave esp_lcd_touch IDENTITY and rotate the raw GT911 coords ourselves in
     * touchReadCb. esp_lcd_touch mirrors the raw (pre-swap) coords using
     * x_max/y_max and only then swaps, so the maxes get mis-paired with the
     * swapped axes for this landscape orientation — no flag combo gets it right.
     * So x_max/y_max here are the NATIVE portrait ranges (240w x 320h). */
    tcfg.x_max         = BOARD_LCD_V_RES;   /* native width  = 240 */
    tcfg.y_max         = BOARD_LCD_H_RES;   /* native height = 320 */
    tcfg.rst_gpio_num  = (gpio_num_t)BOARD_TOUCH_RST_PIN;   /* -1 = none */
    tcfg.int_gpio_num  = (gpio_num_t)BOARD_TOUCH_INT_PIN;
    tcfg.flags.swap_xy  = 0;
    tcfg.flags.mirror_x = 0;
    tcfg.flags.mirror_y = 0;

    /* The GT911 latches its I2C address from the INT pin level at power-on
     * (low -> 0x5D, high -> 0x14). T-Deck has no dedicated touch reset, so the
     * address depends on the boot-time INT level and varies by sub-revision —
     * probe both, as docs/tdeck.md advises. */
    const uint8_t addrs[] = { ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
                              ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP };
    for (uint8_t addr : addrs) {
        esp_lcd_panel_io_handle_t tio = nullptr;
        /* Build the IO config by hand: the GT911 CONFIG macro uses out-of-order
         * designated initializers, which is a hard error in C++. */
        esp_lcd_panel_io_i2c_config_t io_cfg = {};
        io_cfg.dev_addr            = addr;
        io_cfg.scl_speed_hz        = 100000;
        io_cfg.control_phase_bytes = 1;
        io_cfg.dc_bit_offset       = 0;
        io_cfg.lcd_cmd_bits        = 16;
        io_cfg.flags.disable_control_phase = 1;
        if (esp_lcd_new_panel_io_i2c_v2(i2c, &io_cfg, &tio) != ESP_OK) continue;

        esp_lcd_touch_handle_t tp = nullptr;
        if (esp_lcd_touch_new_i2c_gt911(tio, &tcfg, &tp) == ESP_OK) {
            info("touch: GT911 ready @ 0x%02X\n", addr);
            char tch[20];
            snprintf(tch, sizeof(tch), "GT911 @ 0x%02X", addr);
            storageSet("tdeck.touch", tch);   /* surfaced in the T-Deck settings pane */
            /* esp_lcd_touch configured GPIO16 as input; take it interrupt-driven
             * ourselves (ANYEDGE — GT911 INT polarity is sub-rev dependent, and a
             * redundant edge just costs one empty read). lcdInputISR wakes the lcd
             * task to read the touch; touchReadCb sustains tracking from there. */
            gpio_set_intr_type((gpio_num_t)BOARD_TOUCH_INT_PIN, GPIO_INTR_ANYEDGE);
            gpio_isr_handler_add((gpio_num_t)BOARD_TOUCH_INT_PIN, lcdInputISR, nullptr);
            gpio_intr_enable((gpio_num_t)BOARD_TOUCH_INT_PIN);
            return tp;
        }
        esp_lcd_panel_io_del(tio);   /* free and try the other address */
    }
    warn("touch: GT911 not found at 0x5D or 0x14\n");
    storageSet("tdeck.touch", "not found");
    return nullptr;
}

/* Home/centre button on GPIO 0 (BOOT-strap pin, also the trackball centre-press;
 * shared with the mic, which reticulous never uses). Pulled-up active-low input;
 * the lcd component polls tdeckButtonRead() through a keypad indev. */
static void tdeckButtonInit(void) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOARD_HOME_BTN_PIN;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_ANYEDGE;   /* wake on both press and release */
    gpio_config(&io);
    /* lcdInputISR wakes the lcd task on each edge; buttonReadCb (event mode)
     * turns press+release into a click and a >=300ms hold into "go home". */
    gpio_isr_handler_add((gpio_num_t)BOARD_HOME_BTN_PIN, lcdInputISR, nullptr);
}

static bool tdeckButtonRead(void) {
    return gpio_get_level((gpio_num_t)BOARD_HOME_BTN_PIN) == 0;   /* active-low */
}

/* ---- trackball -> mouse pointer ----
 * Four direction lines, each a falling-edge pulse per unit of motion. The ISRs
 * just count (under a spinlock, shared with the reader); tdeckPointerRead()
 * integrates the counts into an absolute screen position. The lcd component owns
 * the cursor + visibility; we own the position (sensitivity, orientation, clamp). */
enum { TB_UP, TB_DOWN, TB_LEFT, TB_RIGHT, TB_N };
static uint32_t          s_tbCount[TB_N] = {0};   /* guarded by s_tbMux, not volatile */
static portMUX_TYPE      s_tbMux = portMUX_INITIALIZER_UNLOCKED;
static int               s_ptrX = -1, s_ptrY = -1;   /* -1 = uninit → centre on first read */
static int               s_tbSpeed    = 16;           /* s.tdeck.trackball_speed:     px/pulse at a full flick */
static int               s_tbAccelMin = 3;            /* s.tdeck.trackball_accel_min: pulses/s at/below which a tick = 1 px */
static int               s_tbAccelMax = 18;           /* s.tdeck.trackball_accel_max: pulses/s at which full speed is reached */
static int               s_tbSmoothMs = 150;          /* s.tdeck.trackball_smooth_ms: velocity EMA time constant */
static int64_t           s_tbLastUs = 0;              /* last read time, for velocity */
static float             s_tbVel = 0.0f;              /* smoothed pulse rate (pulses/sec) */

/* Acceleration model. Every knob is an s.tdeck.* config key, tunable live over the
 * CLI/browser (no sliders). The per-pulse step ramps linearly from 1 px when the
 * smoothed pulse rate is at/below trackball_accel_min (a slow, deliberate roll → one
 * pixel per tick) up to the full trackball_speed once it reaches trackball_accel_max
 * (a fast flick). The rate is smoothed by a trackball_smooth_ms time-constant EMA so
 * a single short gap during a slow roll can't spike one tick to full speed. Defaults
 * (min 3 / max 18 pulses/s) are measured T-Deck Plus values; re-dial with the dbg
 * line below (enable debug logging for the lcd task and roll once). */

static void IRAM_ATTR tboxIsr(void* arg) {
    portENTER_CRITICAL_ISR(&s_tbMux);
    s_tbCount[(int)(intptr_t)arg]++;
    portEXIT_CRITICAL_ISR(&s_tbMux);
    lcdInputISR(nullptr);                 /* wake the lcd task to read the pointer */
}

static void tdeckTrackballInit(void) {
    const struct { int pin; int dir; } lines[TB_N] = {
        { BOARD_TBOX_UP_PIN,    TB_UP    },
        { BOARD_TBOX_DOWN_PIN,  TB_DOWN  },
        { BOARD_TBOX_LEFT_PIN,  TB_LEFT  },
        { BOARD_TBOX_RIGHT_PIN, TB_RIGHT },
    };
    for (auto& l : lines) {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << l.pin;
        io.mode         = GPIO_MODE_INPUT;
        io.pull_up_en   = GPIO_PULLUP_ENABLE;     /* idles high, pulses low */
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type    = GPIO_INTR_NEGEDGE;      /* one count per pulse */
        gpio_config(&io);
        gpio_isr_handler_add((gpio_num_t)l.pin, tboxIsr, (void*)(intptr_t)l.dir);
    }

    /* reticulous owns the whole pointing device, so its settings live in s.tdeck.*.
     * trackball_speed is px/pulse at a full flick; accel_min/accel_max/smooth_ms tune
     * the acceleration curve (see the model note above) — all live-tunable, no
     * sliders. Dwell = seconds the cursor stays after activity (-1 = always); spangap
     * owns the cursor but not this policy, so we push it in via lcdPointerSetVisibleMs.
     * All run on the lcd task — the subs dispatch here. */
    storageDefault("s.tdeck.trackball_speed", s_tbSpeed);
    NOW_AND_ON_CHANGE("s.tdeck.trackball_speed", { s_tbSpeed = atoi(val); });
    storageDefault("s.tdeck.trackball_accel_min", s_tbAccelMin);
    NOW_AND_ON_CHANGE("s.tdeck.trackball_accel_min", { s_tbAccelMin = atoi(val); });
    storageDefault("s.tdeck.trackball_accel_max", s_tbAccelMax);
    NOW_AND_ON_CHANGE("s.tdeck.trackball_accel_max", { s_tbAccelMax = atoi(val); });
    storageDefault("s.tdeck.trackball_smooth_ms", s_tbSmoothMs);
    NOW_AND_ON_CHANGE("s.tdeck.trackball_smooth_ms", { s_tbSmoothMs = atoi(val); });
    storageDefault("s.tdeck.pointer_visible_time", 2);
    NOW_AND_ON_CHANGE("s.tdeck.pointer_visible_time",
                      { int s = atoi(val); lcdPointerSetVisibleMs(s < 0 ? -1 : s * 1000); });
}

static bool tdeckPointerRead(int* x, int* y) {
    int c[TB_N];
    portENTER_CRITICAL(&s_tbMux);
    for (int i = 0; i < TB_N; i++) { c[i] = (int)s_tbCount[i]; s_tbCount[i] = 0; }
    portEXIT_CRITICAL(&s_tbMux);

    if (s_ptrX < 0) { s_ptrX = BOARD_LCD_H_RES / 2; s_ptrY = BOARD_LCD_V_RES / 2; }

    int dxp = c[TB_RIGHT] - c[TB_LEFT];     /* signed pulse delta this read */
    int dyp = c[TB_DOWN]  - c[TB_UP];

    /* Pointer acceleration. Smooth the pulse rate with a *time-decayed* EMA: a
     * short gap barely moves it (steady feel under a continuous roll), a long gap
     * decays it toward zero (so the first nudge after a pause is precise, not a
     * leftover-velocity jump). The smoothed rate then maps to a per-pulse step
     * (below) that scales from 1 px up to the full trackball_speed. */
    int64_t now = esp_timer_get_time();
    int64_t dt  = now - s_tbLastUs;
    s_tbLastUs = now;
    if (dt < 1) dt = 1;

    int   pulses = abs(dxp) + abs(dyp);
    float vinst  = (float)pulses * 1e6f / (float)dt;                       /* pulses/sec */
    float tau    = (float)s_tbSmoothMs * 1000.0f;                          /* ms -> us */
    float decay  = tau / (tau + (float)dt);                                /* ~1 under a roll, ->0 after a gap */
    s_tbVel = decay * s_tbVel + (1.0f - decay) * vinst;

    /* 0 at/below accel_min (1 px/tick, precise), 1 at accel_max (full speed), linear
     * between — so a slow roll is pixel-exact and a fast flick hits trackball_speed. */
    float lo = (float)s_tbAccelMin, hi = (float)s_tbAccelMax;
    if (hi <= lo) hi = lo + 1.0f;
    float accel = (s_tbVel - lo) / (hi - lo);
    if (accel < 0.0f) accel = 0.0f; else if (accel > 1.0f) accel = 1.0f;
    float step  = 1.0f + (float)(s_tbSpeed - 1) * accel;                   /* px per pulse */

    int dx = (int)lroundf((float)dxp * step);
    int dy = (int)lroundf((float)dyp * step);
    bool moved = (dx != 0 || dy != 0);

    /* Tuning aid (dbg-gated, rate-limited): read the live pulse rate to set
     * trackball_accel_min/max. Runs on the lcd task, so it logs under that tag. */
    static int64_t s_tbDbgUs = 0;
    if (pulses && now - s_tbDbgUs > 150000) {
        s_tbDbgUs = now;
        dbg("tball vel=%.0f/s accel=%.2f step=%.1f\n", (double)s_tbVel, (double)accel, (double)step);
    }

    s_ptrX = std::clamp(s_ptrX + dx, 0, BOARD_LCD_H_RES - 1);
    s_ptrY = std::clamp(s_ptrY + dy, 0, BOARD_LCD_V_RES - 1);
    *x = s_ptrX;
    *y = s_ptrY;
    return moved;
}

/* Built-in "T-Deck" Settings panel (root level): the trackball + display knobs
 * that are this board's own. trackball_speed is ours (s.tdeck.*); the cursor
 * dwell and backlight are spangap's generic lcd keys, just surfaced here. */
static void tdeckSettingsPane(void* arg) {
    lv_obj_t* p = (lv_obj_t*)arg;
    /* Board probe results up top: which GPS receiver we found (gps.cpp) and
     * whether the GT911 touch answered on I2C (set in tdeckTouchInit). */
    lcdSettingSection(p, "Board");
    lcdSettingValue  (p, "GPS",   "gps.model");
    lcdSettingValue  (p, "Touch", "tdeck.touch");
    lcdSettingSection(p, "GPS");
    lcdSettingSwitch (p, "Enable",       "s.gps.enable");
    lcdSettingSlider (p, "Interval (s)", "s.gps.interval", 1, 60);
    lcdSettingValue  (p, "Status",       "gps.state");   /* "power-cycle to wake" etc. */
    lcdSettingSection(p, "Trackball");
    lcdSettingSlider (p, "Pointer speed",    "s.tdeck.trackball_speed",      4, 40);
    lcdSettingSlider (p, "Cursor dwell (s)", "s.tdeck.pointer_visible_time", 1, 30);
    lcdSettingSection(p, "Display");
    lcdSettingSlider (p, "Backlight",        "s.lcd.backlight",              0, 255);
    lcdSettingSlider (p, "Sleep after (s)",  "s.lcd.inactivity_timeout",     0, 120);
}

/* Register this board's HAL with the lcd component. Called from tdeckPreInit()
 * before spangapInit(). */
static void tdeckLcdRegister(void) {
    static const lcd_board_t ops = {
        .init        = tdeckLcdInit,
        .shutdown    = tdeckLcdShutdown,
        .backlight   = tdeckLcdBacklight,
        .display_power = tdeckDisplayPower,
        .touch_init  = tdeckTouchInit,
        .button_read = tdeckButtonRead,
        .pointer_read = tdeckPointerRead,
    };
    lcdSetBoard(&ops);
    lcdRegisterSettings("T-Deck", "T-Deck", tdeckSettingsPane);
}

/* ---- QWERTY keyboard, end to end --------------------------------------------
 *
 * The keyboard is an ESP32-C3 MCU on the shared I2C0 bus (addr 0x55). Its quirks
 * make it a poor fit for the generic lcd input model, so it lives here in the
 * consumer rather than in spangap-core:
 *   - GPIO46 is wired as a "key buffered" interrupt but the stock/rgrizzell C3
 *     firmware never drives it (verified on hardware and in the C3 source), so an
 *     INT-only path reads nothing.
 *   - The 1-byte read is destructive (pops the key) with no peek and returns 0
 *     when empty, so we can't tell a key is pending without consuming it.
 *
 * So a dedicated low-prio task polls the I2C off the lcd task, buffers keys into
 * a queue, and bumps the lcd task (lcdRun) to drain them through our LVGL keypad
 * indev. We keep GPIO46 wired anyway: the first edge ever seen flips us from
 * polling to interrupt-driven (self-healing if a future firmware drives it).
 *
 * Dependency is one-way: we call into lcd (lcdRun / lcdInputGroup /
 * lcdSetHasKeyboard); the lcd component has no knowledge of the keyboard. */
namespace {

i2c_master_dev_handle_t s_kbd        = nullptr;
QueueHandle_t           s_queue      = nullptr;   /* bytes: poll task -> lcd task */
TaskHandle_t            s_pollTask   = nullptr;
lv_indev_t*             s_indev      = nullptr;    /* our keypad indev (lcd task) */
bool                    s_again      = false;      /* lcd task: more to drain this cycle */
volatile bool           s_intSeen    = false;      /* set once GPIO46 actually fires */
volatile uint32_t       s_intCount   = 0;          /* for the kbint diagnostic */

/* Adaptive poll: snappy right after a key, lazy when idle (the C3 buffers keys,
 * so a slow poll only delays the first keypress, never drops one). Once the INT
 * fires we drop to a long fallback — essentially interrupt-driven. */
constexpr TickType_t POLL_FAST = pdMS_TO_TICKS(30);
constexpr TickType_t POLL_IDLE = pdMS_TO_TICKS(200);
constexpr TickType_t INT_WAIT  = pdMS_TO_TICKS(2000);

uint32_t mapAsciiKey(uint32_t b) {
    switch (b) {
        case 13: case 10:  return LV_KEY_ENTER;
        case 8:  case 127: return LV_KEY_BACKSPACE;
        case 27:           return LV_KEY_ESC;
        default:           return (b >= 0x20 && b < 0x7F) ? b : 0;
    }
}

/* Raw destructive read of the next buffered key (0 = none). Runs on the poll
 * task (off the lcd task). */
uint32_t i2cReadKey() {
    if (!s_kbd) return 0;
    /* kbint diagnostic: report a GPIO46 edge if one ever shows up. */
    static uint32_t lastInt = 0;
    uint32_t now = s_intCount;
    if (now != lastInt) { dbg("kbint: GPIO%d fired (total %u)\n", BOARD_KB_INT_PIN, (unsigned)now); lastInt = now; }
    uint8_t b = 0;
    if (i2c_master_receive(s_kbd, &b, 1, 10) != ESP_OK) return 0;
    if (b) dbg("kbread: 0x%02x\n", (unsigned)b);
    return b;
}

/* LVGL keypad read_cb (lcd task): one queued byte per call, press+release
 * synthesized over two reads; s_again asks kbDrain for another pass. */
void readCb(lv_indev_t*, lv_indev_data_t* data) {
    static uint32_t held = 0;
    if (held) { data->key = held; data->state = LV_INDEV_STATE_RELEASED; held = 0; s_again = true; return; }
    uint8_t raw = 0;
    if (s_queue && xQueueReceive(s_queue, &raw, 0) == pdTRUE && raw) {
        s_again = true;
        uint32_t k = mapAsciiKey(raw);
        if (k) {
            /* Count the keystroke as activity (resets the inactivity blank timer).
             * If it woke the screen, swallow it — the key only served to wake. */
            if (lcdNotifyActivity()) { data->state = LV_INDEV_STATE_RELEASED; return; }
            data->key = k; data->state = LV_INDEV_STATE_PRESSED; held = k; return;
        }
    }
    data->state = LV_INDEV_STATE_RELEASED;
}

/* lcd task (via lcdRun): drain the queue through the indev. */
void kbDrain(void*) {
    if (!s_indev) return;
    do { s_again = false; lv_indev_read(s_indev); } while (s_again);
}

/* lcd task (via lcdRun): create our keypad indev, joined to lcd's focus group. */
void kbCreateIndev(void*) {
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_indev, readCb);
    lv_indev_set_group(s_indev, lcdInputGroup());
    lv_indev_set_mode(s_indev, LV_INDEV_MODE_EVENT);
}

void IRAM_ATTR kbIntIsr(void*) {
    s_intCount = s_intCount + 1;       /* plain store: '++' on volatile is deprecated */
    s_intSeen  = true;                 /* flip the poll loop to interrupt-driven */
    if (!s_pollTask) return;
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_pollTask, &hp);
    portYIELD_FROM_ISR(hp);
}

void pollTask(void*) {
    bool active = false;
    for (;;) {
        TickType_t wait = s_intSeen ? INT_WAIT : (active ? POLL_FAST : POLL_IDLE);
        ulTaskNotifyTake(pdTRUE, wait);    /* woken by the INT, or times out to poll */
        active = false;
        /* Drain up to a queueful per wake — bounded so a wedged keyboard that
         * keeps returning a byte can't spin this task. */
        for (int i = 0; i < 16; i++) {
            uint32_t k = i2cReadKey();
            if (!k) break;
            uint8_t b = (uint8_t)k;
            xQueueSend(s_queue, &b, 0);
            active = true;
        }
        if (active) lcdRun(kbDrain);        /* bump the lcd task to read our indev */
    }
}

}  // namespace

/* Bring up the keyboard. Called from tdeckPostInit() AFTER spangapInit() — it
 * needs the lcd task to exist so it can create + drive its indev via lcdRun(). */
static void tdeckKeyboardInit(void) {
    i2c_master_bus_handle_t bus = tdeckI2cBus();
    if (!bus) { warn("keyboard: no i2c bus\n"); return; }

    i2c_device_config_t dcfg = {};
    dcfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dcfg.device_address  = BOARD_KB_ADDR;
    dcfg.scl_speed_hz    = 100000;
    if (i2c_master_bus_add_device(bus, &dcfg, &s_kbd) != ESP_OK) {
        warn("keyboard: i2c add device failed\n");
        return;
    }

    /* GPIO46 keyboard INT (ANYEDGE + pull-up). Dead on current C3 firmware, but
     * if it ever fires kbIntIsr flips us to interrupt-driven. The gpio ISR
     * service is already installed by the board's lcd bring-up. */
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOARD_KB_INT_PIN;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_ANYEDGE;
    gpio_config(&io);
    gpio_isr_handler_add((gpio_num_t)BOARD_KB_INT_PIN, kbIntIsr, nullptr);

    s_queue = xQueueCreate(16, 1);
    lcdRun(kbCreateIndev);                  /* create the indev on the lcd task */
    xTaskCreatePinnedToCore(pollTask, "kbpoll", 3072, nullptr, 1, &s_pollTask, 1);

    lcdSetHasKeyboard(true);                /* lcd: suppress the on-screen keyboard */
}

#endif /* CONFIG_SPANGAP_LCD */

/* =========================================================================
 * 4. Public API — two phases around spangapInit() (see tdeck.h).
 * ========================================================================= */

void tdeckPreInit(void) {
    tdeckPowerInit();                       /* power rail + shared-SPI CS park */
    /* Create the shared I2C0 bus now, while we're still single-threaded — touch
     * (lcd task), keyboard (kbpoll task) and the RTC (gps task) all bring it up
     * lazily and could otherwise race i2c_new_master_bus() on the same port. */
    tdeckI2cBus();
#if BOARD_POWER_EN_PIN >= 0
    resetOnOffSetPowerOff(tdeckPeripheralPowerOff);
#endif
#if CONFIG_SPANGAP_LCD
    tdeckLcdRegister();                     /* display/touch/pointer HAL → lcd */
#endif
}

void tdeckPostInit(void) {
#if CONFIG_SPANGAP_LCD
    tdeckKeyboardInit();                    /* needs the lcd task spangapInit() made */
#endif
}
