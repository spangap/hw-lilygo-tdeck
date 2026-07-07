/**
 * tdeck_lcd.cpp — T-Deck Plus on-device-UI input HAL: GT911 touch, trackball
 * pointer, centre/Home button, and the QWERTY keyboard. Lives under
 * esp-idf/main/conditional/spangap-lcd/, so it is compiled ONLY when spangap-lcd
 * is staged — no #if needed. Registered via two when: spangap/spangap-lcd hooks:
 *   tdeckLcdStart()  start: band — input HAL register (was tdeckInputRegister).
 *   tdeckLcdInit()   init:  band — keyboard bring-up    (was tdeckKeyboardInit).
 * Extracted verbatim from tdeck.cpp. tdeckI2cBus() and the BOARD_* pin macros
 * come from tdeck.h.
 */
#include "tdeck.h"
#include "tdeck_lcd.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd_input.h"
#include "lcd.h"
#include "storage.h"
#include "log.h"

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
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "lvgl.h"

/* Forward declarations so the HAL ops table + cross-calls resolve regardless
 * of definition order. */
static void tdeckInputInit(void);
static bool tdeckTouchRead(lcd_raw_pt_t* pts, int max, int* count);
static bool tdeckClickRead(void);
static void tdeckTrackballInit(void);
static bool tdeckPointerRead(int* x, int* y);
static void tdeckStandby(bool on);

/* GT911 handle, created in tdeckInputInit(); null if the controller didn't
 * answer (then tdeckTouchRead reports no touches and the indev never fires). */
static esp_lcd_touch_handle_t s_touch = nullptr;
/* True while in standby: the GT911 reads are gated off (only the centre button is
 * left live to wake the device). Set by tdeckStandby on the lcd task. */
static bool                   s_touchAsleep = false;

static void tdeckTouchInit(void) {
    i2c_master_bus_handle_t i2c = tdeckI2cBus();
    if (!i2c) return;

    /* Multi-touch is opt-in and ephemeral: a consumer (e.g. the maps app) sets
     * the runtime flag `tdeck.multi_touch` (no `s.` — not persisted, not a
     * setting) while it wants gestures. Watch it and flip the generic lcd
     * multipoint read mode. The GT911 is a 5-point controller. Runs on the lcd
     * task (board HAL init), same as the trackball subs above. */
    NOW_AND_ON_CHANGE("tdeck.multi_touch", { lcdTouchSetMultipoint(atoi(val) != 0); });

    esp_lcd_touch_config_t tcfg = {};
    /* Leave esp_lcd_touch at IDENTITY (native coords) and let the lcd component
     * rotate the points (lcdPanelOrientTouch) with the same CONFIG_LCD_ROTATION
     * it applies to the pixels — so the maxes are the native ranges, matching
     * CONFIG_LCD_NATIVE_WIDTH/HEIGHT. */
    tcfg.x_max         = CONFIG_LCD_NATIVE_WIDTH;
    tcfg.y_max         = CONFIG_LCD_NATIVE_HEIGHT;
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

    /* Probing the address the board did NOT latch makes esp_lcd_touch_gt911 and
     * the i2c IO log a failed read at ERROR level before the real address
     * answers — expected, not a fault. Mute those two tags across the probe,
     * then put back the level they had (the global default, barring an explicit
     * override). Our own warn() below still fires if BOTH addresses are silent. */
    const esp_log_level_t prevGt = esp_log_level_get("GT911");
    const esp_log_level_t prevIo = esp_log_level_get("lcd_panel.io.i2c");
    esp_log_level_set("GT911",            ESP_LOG_NONE);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);

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
            esp_log_level_set("GT911",            prevGt);   /* restore once found */
            esp_log_level_set("lcd_panel.io.i2c", prevIo);
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
            s_touch = tp;
            return;
        }
        esp_lcd_panel_io_del(tio);   /* free and try the other address */
    }
    esp_log_level_set("GT911",            prevGt);   /* both addresses silent — restore + report */
    esp_log_level_set("lcd_panel.io.i2c", prevIo);
    warn("touch: GT911 not found at 0x5D or 0x14\n");
    storageSet("tdeck.touch", "not found");
}

/* lcd_input.h touch_read: pop the GT911's points in NATIVE coords; the lcd
 * component applies the panel rotation. Returns false (no read) until the GT911
 * is up. */
static bool tdeckTouchRead(lcd_raw_pt_t* pts, int max, int* count) {
    *count = 0;
    if (!s_touch || s_touchAsleep) return false;
    esp_lcd_touch_point_data_t pt[5] = {};
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(s_touch);
    esp_lcd_touch_get_data(s_touch, pt, &cnt, max < 5 ? (uint8_t)max : 5);
    int n = cnt > max ? max : cnt;
    for (int i = 0; i < n; i++) { pts[i].x = (int16_t)pt[i].x; pts[i].y = (int16_t)pt[i].y; }
    *count = n;
    return true;
}

/* ---- centre / Home button (GPIO 0): click, launcher, standby ----
 * GPIO 0 is the BOOT-strap pin, also the trackball centre-press (shared with the
 * mic, which reticulous never uses). Pulled-up active-low. The board owns the
 * timing and the four meanings of the press:
 *   - tap (< launcher_hold)                 -> a pointer click (lcd turns it into one)
 *   - hold launcher_hold ms                 -> back to the launcher (lcdGoHome)
 *   - hold launcher_hold + standby_hold ms  -> standby (set sys.standby)
 *   - any press while in standby            -> wake (clear sys.standby), absorbed
 * The two holds stack: a long hold goes Home first, then on to standby. When the
 * press starts already at the launcher the launcher tier is dead, so standby comes
 * at the shorter launcher_hold instead. The lcd component applies no timing of its
 * own. Thresholds are s.tdeck.*_hold_ms (live). */
static lv_timer_t*    s_homeTimer    = nullptr;   /* fires at launcher_hold -> lcdGoHome */
static lv_timer_t*    s_standbyTimer = nullptr;   /* fires at launcher_hold+standby_hold -> standby */
static bool           s_btnHeld      = false;     /* a press is in progress */
static bool           s_btnConsumed  = false;     /* a hold action fired -> release is not a click */
static bool           s_wakeAbsorb   = false;     /* swallow the rest of a transition press until release */
static volatile bool  s_standby      = false;     /* mirror of sys.standby (set on the lcd task) */
static int            s_launcherMs   = 300;       /* s.tdeck.launcher_hold_ms */
static int            s_standbyMs    = 1000;      /* s.tdeck.standby_hold_ms (added on top of launcher) */

static void tdeckButtonInit(void) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOARD_HOME_BTN_PIN;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_ANYEDGE;   /* wake on both press and release */
    gpio_config(&io);
    /* lcdInputISR wakes the lcd task on each edge; tdeckClickRead() (event mode)
     * runs the click/hold state machine from there. */
    gpio_isr_handler_add((gpio_num_t)BOARD_HOME_BTN_PIN, lcdInputISR, nullptr);
}

static void cancelHoldTimers(void) {
    if (s_homeTimer)    { lv_timer_delete(s_homeTimer);    s_homeTimer = nullptr; }
    if (s_standbyTimer) { lv_timer_delete(s_standbyTimer); s_standbyTimer = nullptr; }
}

/* Held to launcher_hold (lcd task, via lv_timer): go Home. The press may still be
 * down and hold on toward standby. */
static void btnHomeCb(lv_timer_t*) {
    s_homeTimer   = nullptr;   /* the one-shot self-deleted after this fire */
    s_btnConsumed = true;      /* release is no longer a click */
    lcdGoHome();
}

/* Held on to launcher_hold + standby_hold: enter standby. Swallow the rest of this
 * press so the still-down finger can't immediately wake it again. */
static void btnStandbyCb(lv_timer_t*) {
    s_standbyTimer = nullptr;
    s_btnConsumed  = true;
    s_btnHeld      = false;    /* this press is done as far as the state machine */
    s_wakeAbsorb   = true;     /* ignore the held finger until it lifts */
    storageSet("sys.standby", 1);
}

/* lcd_input.h click_read (lcd task): the click / hold / standby state machine. A
 * short press asserts the click for exactly one poll (the component forces the
 * follow-up read that lands the release → LVGL sees a click). */
static bool tdeckClickRead(void) {
    bool down = gpio_get_level((gpio_num_t)BOARD_HOME_BTN_PIN) == 0;   /* active-low */

    /* Swallow the remainder of a press that already caused a transition (woke us,
     * or was held into standby) until the finger lifts. */
    if (s_wakeAbsorb) {
        if (!down) s_wakeAbsorb = false;
        return false;
    }
    /* In standby the only live input is this button: a press just wakes (clears
     * sys.standby) and is absorbed — never a click or a hold. */
    if (s_standby) {
        if (down) { storageSet("sys.standby", 0); s_wakeAbsorb = true; }
        return false;
    }

    if (down) {
        if (!s_btnHeld) {
            s_btnHeld     = true;
            s_btnConsumed = false;
            uint32_t homeMs = s_launcherMs > 0 ? (uint32_t)s_launcherMs : 1;
            if (lcdAtLauncher()) {
                /* Already home: the launcher tier would be a no-op, so skip it and
                 * go straight to standby at the shorter launcher_hold — a quick hold
                 * from the launcher sleeps the device. */
                s_standbyTimer = lv_timer_create(btnStandbyCb, homeMs, nullptr);
                lv_timer_set_repeat_count(s_standbyTimer, 1);
            } else {
                uint32_t standbyMs = (uint32_t)(s_launcherMs + s_standbyMs);
                if (standbyMs == 0) standbyMs = 1;
                s_homeTimer = lv_timer_create(btnHomeCb, homeMs, nullptr);
                lv_timer_set_repeat_count(s_homeTimer, 1);
                s_standbyTimer = lv_timer_create(btnStandbyCb, standbyMs, nullptr);
                lv_timer_set_repeat_count(s_standbyTimer, 1);
            }
        }
        return false;                               /* never click while held */
    }
    cancelHoldTimers();
    bool click = (s_btnHeld && !s_btnConsumed);     /* short press → click on release */
    s_btnHeld = false;
    return click;
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
    NOW_AND_ON_CHANGE("s.tdeck.trackball_speed", { s_tbSpeed = atoi(val); });
    storageDefault("s.tdeck.trackball_accel_min", s_tbAccelMin);
    NOW_AND_ON_CHANGE("s.tdeck.trackball_accel_min", { s_tbAccelMin = atoi(val); });
    storageDefault("s.tdeck.trackball_accel_max", s_tbAccelMax);
    NOW_AND_ON_CHANGE("s.tdeck.trackball_accel_max", { s_tbAccelMax = atoi(val); });
    storageDefault("s.tdeck.trackball_smooth_ms", s_tbSmoothMs);
    NOW_AND_ON_CHANGE("s.tdeck.trackball_smooth_ms", { s_tbSmoothMs = atoi(val); });
    NOW_AND_ON_CHANGE("s.tdeck.pointer_visible_time",
                      { int s = atoi(val); lcdPointerSetVisibleMs(s < 0 ? -1 : s * 1000); });
}

static bool tdeckPointerRead(int* x, int* y) {
    int c[TB_N];
    portENTER_CRITICAL(&s_tbMux);
    for (int i = 0; i < TB_N; i++) { c[i] = (int)s_tbCount[i]; s_tbCount[i] = 0; }
    portEXIT_CRITICAL(&s_tbMux);

    /* In standby the trackball is dead: the counts are drained (so they can't jump
     * on wake) but the cursor doesn't move and nothing scrolls — only the centre
     * button wakes us. */
    if (s_standby) { *x = s_ptrX < 0 ? 0 : s_ptrX; *y = s_ptrY < 0 ? 0 : s_ptrY; return false; }

    /* Clamp to the actual (post-rotation) panel; the lcd component owns the size. */
    int scrW = 0, scrH = 0;
    lcdDisplaySize(&scrW, &scrH);
    if (s_ptrX < 0) { s_ptrX = scrW / 2; s_ptrY = scrH / 2; }

    int dxp = c[TB_RIGHT] - c[TB_LEFT];     /* signed pulse delta this read */
    int dyp = c[TB_DOWN]  - c[TB_UP];

    /* Arrow-key mode (a program claimed it via lcdProgramScrollwheelArrows, e.g.
     * the on-device terminal): feed arrows to the focus group instead of moving
     * the pointer. Uses the raw per-read pulse delta, so it never sticks at a
     * screen edge the way the clamped pointer position would. */
    if (lcdScrollwheelArrowsActive()) {
        int n; uint32_t key;
        if (abs(dyp) >= abs(dxp)) { n = abs(dyp); key = dyp > 0 ? LV_KEY_DOWN  : LV_KEY_UP;   }
        else                      { n = abs(dxp); key = dxp > 0 ? LV_KEY_RIGHT : LV_KEY_LEFT; }
        if (n > 4) n = 4;                   /* cap a fast flick */
        lv_group_t* g = lcdInputGroup();
        for (int i = 0; i < n && g; i++) lv_group_send_data(g, key);
        *x = s_ptrX; *y = s_ptrY;           /* pointer stays put */
        return false;
    }

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

    /* Tuning aid (dbg-gated, rate-limited): read the live pulse rate to set
     * trackball_accel_min/max. Runs on the lcd task, so it logs under that tag. */
    static int64_t s_tbDbgUs = 0;
    if (pulses && now - s_tbDbgUs > 150000) {
        s_tbDbgUs = now;
        dbg("tball vel=%.0f/s accel=%.2f step=%.1f\n", (double)s_tbVel, (double)accel, (double)step);
    }

    /* Edge-pan: when the cursor is already pinned against a screen edge and the
     * trackball keeps pushing that way, the motion the clamp would otherwise
     * swallow scrolls the active widget (or the launcher) instead — so a
     * touchless board can reach offscreen content. The would-be step px become
     * the scroll distance, so panning tracks the pointer's own (accelerated)
     * speed. lcdScroll runs on the lcd task; pointer_read already does. */
    if      (dx > 0 && s_ptrX >= scrW - 1) lcdScroll(LCD_SCROLL_RIGHT, dx);
    else if (dx < 0 && s_ptrX <= 0)        lcdScroll(LCD_SCROLL_LEFT,  -dx);
    if      (dy > 0 && s_ptrY >= scrH - 1) lcdScroll(LCD_SCROLL_DOWN,  dy);
    else if (dy < 0 && s_ptrY <= 0)        lcdScroll(LCD_SCROLL_UP,    -dy);

    /* `moved` is the real position change after clamping — false once pinned at
     * an edge, so the cursor fades on its dwell timer while you keep panning. */
    int ox = s_ptrX, oy = s_ptrY;
    s_ptrX = std::clamp(s_ptrX + dx, 0, scrW - 1);
    s_ptrY = std::clamp(s_ptrY + dy, 0, scrH - 1);
    *x = s_ptrX;
    *y = s_ptrY;
    return (s_ptrX != ox || s_ptrY != oy);
}

/* lcd_input.h init hook — runs on the lcd task once the panel and the shared
 * GPIO ISR service are up. Wire the board's input: GT911 touch, centre button,
 * trackball. */
static void tdeckInputInit(void) {
    tdeckButtonInit();
    tdeckTrackballInit();
    tdeckTouchInit();

    /* Centre-button hold tiers + standby. The button (and the lcd inactivity
     * timeout) only set/clear the ephemeral sys.standby key; this subscription is
     * what actually sleeps/wakes the device — display off via lcdScreenSleep/Wake,
     * plus our own input (touch + keyboard scan) off. This init runs on the lcd
     * task, so the subscription dispatches there and lcdScreenSleep/Wake are safe. */
    NOW_AND_ON_CHANGE("s.tdeck.launcher_hold_ms", { s_launcherMs = atoi(val); });
    NOW_AND_ON_CHANGE("s.tdeck.standby_hold_ms", { s_standbyMs = atoi(val); });
    storageSubscribeChanges("sys.standby", ON_CHANGE { tdeckStandby(atoi(val) != 0); });
}

/* onStart — register this board's input HAL with the lcd component, before
 * spangapInit()/lcdInit(). The display itself is the component's (CONFIG_LCD_*);
 * we supply only input. */
void TdeckLcdInput::onStart() {
    static const lcd_input_t ops = {
        .init         = tdeckInputInit,
        .touch_read   = tdeckTouchRead,
        .pointer_read = tdeckPointerRead,
        .click_read   = tdeckClickRead,
    };
    lcdSetInput(&ops);
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
    /* Control prefix: the keyboard sends 0x0C for Alt-C; treat it as a one-shot
     * "next lowercase letter is Ctrl-<letter>" lead-in (within 1 s), encoded
     * with LCD_KEY_CTRL for the terminal to turn into a control byte. */
    static TickType_t ctrlUntil = 0;
    if (held) { data->key = held; data->state = LV_INDEV_STATE_RELEASED; held = 0; s_again = true; return; }
    uint8_t raw = 0;
    if (s_queue && xQueueReceive(s_queue, &raw, 0) == pdTRUE && raw) {
        s_again = true;
        uint32_t k;
        bool armed = ctrlUntil && (long)(ctrlUntil - xTaskGetTickCount()) > 0;
        if (raw == 0x0C) {                          /* prefix */
            if (armed) { ctrlUntil = 0; k = LV_KEY_ESC; }  /* Alt-C Alt-C -> ESC */
            else { ctrlUntil = xTaskGetTickCount() + pdMS_TO_TICKS(1000); k = 0; }  /* arm for 1 s */
        } else if (armed
                   && raw >= 'a' && raw <= 'z') {
            ctrlUntil = 0;
            k = LCD_KEY_CTRL | raw;                 /* Ctrl-<letter> */
        } else {
            ctrlUntil = 0;
            k = mapAsciiKey(raw);
        }
        if (k) {
            /* Count the keystroke as activity (resets the inactivity blank timer).
             * If it woke the screen, swallow it — the key only served to wake. */
            if (lcdNotifyActivity()) { data->state = LV_INDEV_STATE_RELEASED; return; }
            data->key = k; data->state = LV_INDEV_STATE_PRESSED; held = k; return;
        }
    }
    data->state = LV_INDEV_STATE_RELEASED;
}

/* lcd task (via lcdRun): create our keypad indev, joined to lcd's focus group. */
void kbCreateIndev(void*);

/* lcd task (via lcdRun): drain the queue through the indev. */
void kbDrain(void*) {
    /* Lazy create. tdeckLcdInit() fires lcdRun(kbCreateIndev) right after
     * spangapInit(), which can land before the lcd task has registered its
     * LCD_RUN_PORT aux handler — that aux send then fails ("unregistered port")
     * and the indev is never made, so s_indev stays null and every keypress is
     * dropped here. kbDrain runs on the lcd task only once it's fully up, so it
     * is the race-proof place to build the indev on demand (also self-heals if
     * it were ever deleted). */
    if (!s_indev) kbCreateIndev(nullptr);
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
    dbg("kbCreateIndev: s_indev=%p group=%p disp=%p\n",   /* TEMP probe */
        (void*)s_indev, (void*)lcdInputGroup(), (void*)lv_display_get_default());
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
        /* Parked in standby: stop scanning the I2C keyboard entirely (the centre
         * button is the only thing left to wake the device). tdeckStandby unparks
         * us with a notify. Park on a clean self-check, not a suspend, so we never
         * stop mid-I2C-transaction holding the shared bus. */
        if (s_standby) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); continue; }
        TickType_t wait = s_intSeen ? INT_WAIT : (active ? POLL_FAST : POLL_IDLE);
        ulTaskNotifyTake(pdTRUE, wait);    /* woken by the INT, or times out to poll */
        active = false;
        if (s_standby) continue;           /* entered standby during the wait */
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

/* sys.standby subscription target (lcd task). The lcd component only flips the
 * key — on the inactivity timeout or our centre button; we decide what the device
 * actually does: display off, GT911 reads gated, keyboard scan parked. Only the
 * centre button is left live, and a press of it clears the key to wake us. */
static void tdeckStandby(bool on) {
    if (on == s_standby) return;
    s_standby = on;
    if (on) {
        s_touchAsleep = true;                          /* GT911 reads return nothing */
        lcdScreenSleep();                              /* display off, backlight to 0 */
        info("standby\n");
    } else {
        s_touchAsleep = false;
        if (s_pollTask) xTaskNotifyGive(s_pollTask);   /* unpark the keyboard scan */
        lcdScreenWake();                               /* display on, backlight fade-in */
        info("wake\n");
    }
}

/* onInit — bring up the keyboard, AFTER spangapInit(): it needs the lcd task to
 * exist so it can create + drive its indev via lcdRun(). */
void TdeckLcdInput::onInit() {
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
    /* Prio 3: one notch above the lcd task (prio 2) so a long synchronous redraw
     * on it (e.g. a search-box list rebuild) can't starve the poll on core 1 —
     * the C3 holds only the last unread key, so a stalled poll drops keystrokes.
     * Not higher: the read takes the shared I2C0 bus (touch, audio codec), and
     * polling it hard at high prio would inject latency into those. */
    xTaskCreatePinnedToCore(pollTask, "kbpoll", 3072, nullptr, 3, &s_pollTask, 1);

    lcdSetHasKeyboard(true);                /* lcd: suppress the on-screen keyboard */
}
