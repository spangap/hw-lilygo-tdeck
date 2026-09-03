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
#include "hal/gpio_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd_input.h"
#include "lcd.h"
#include "storage.h"
#include "log.h"
#include "pm.h"

#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_sleep.h"
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
static bool tdeckTouchSample(void);            /* poll task: read GT911 → latch */
static void tdeckTouchIsr(void*);              /* GT911 INT → wake the poll task (IRAM, see defn) */
static void tdeckTouchWakeArm(bool on);        /* standby: glass as a wake source */
static void tdeckTouchWakeCheck(void);         /* poll task: a touch woke us — really? */

/* GT911 handle, created in tdeckInputInit(); null if the controller didn't
 * answer (then tdeckTouchRead reports no touches and the indev never fires). */
static esp_lcd_touch_handle_t s_touch = nullptr;
/* True while in standby: the GT911 reads are gated off (only the centre button is
 * left live to wake the device). Set by tdeckStandby on the lcd task. */
static bool                   s_touchAsleep = false;

/* ---- wake on touch (s.lcd.wake_on_touch, off by default on this board) ----
 * A deck that rides in a pocket wants its centre button to be the only way
 * back — a bag full of touches would keep lighting the screen — so the key
 * ships off here and on a handheld ships on (the lcd component seeds it from
 * CONFIG_LCD_WAKE_ON_TOUCH_DEFAULT, and both boards carry the Display row).
 * When it IS on, standby leaves the GT911's INT armed as a light-sleep wake
 * source and the poll task, otherwise parked, samples the controller on that
 * wake and clears sys.standby if a finger is really there.
 *
 * The two traps the centre button's wake carries apply here identically: the
 * level, not an edge, because edges are invisible while the GPIO clock is
 * gated — and a level ISR that re-fires for the whole touch, so the ISR
 * silences the pin and the check below re-enables it. The armed level is
 * whatever the line is NOT resting at: GT911 INT polarity varies by
 * sub-revision (the awake wiring is ANYEDGE for the same reason), and arming
 * the resting level would wake the deck for ever. */
static bool                   s_touchWakeArmed = false;
static int                    s_touchWakeLevel = 0;
/* The finger that woke the deck is swallowed until it lifts: waking is all it
 * does, exactly as the wake press is absorbed rather than clicking. */
static bool                   s_touchWakeAbsorb = false;

/* ---- touch latch: sampled on the poll task, read on the lcd task -------------
 * The GT911 read moved off the lcd task onto the keyboard poll task, so a touch
 * read never waits behind a render and never fights the render for the shared
 * I2C0 bus (the two used to contend). tdeckTouchSample() (poll task) does the
 * I2C read and stores the sample here; tdeckTouchRead() (lcd task, the
 * lcd_input.h touch_read) just returns it — no hardware access.
 *
 * Missed-tap replay: if a whole tap completes (finger down then up, no real
 * movement) while the lcd task was too busy to read even one pressed sample, the
 * poll task latches a one-shot "pending tap" at the down point so it still lands
 * as a click. A gesture that moved past TOUCH_CLICK_R is a DRAG and is never
 * replayed this way — so a drag missed during a stall is simply lost, never
 * turned into a false click. All fields below are guarded by s_touchMux. */
static portMUX_TYPE           s_touchMux = portMUX_INITIALIZER_UNLOCKED;
static int16_t                s_tX[5], s_tY[5];        /* latest sample, native coords */
static int                    s_tCount = 0;            /* fingers in the latest sample */
static bool                   s_tapPending = false;    /* an unread tap waiting to replay */
static int16_t                s_tapX = 0, s_tapY = 0;  /* its down point */
static bool                   s_gestureConsumed = false; /* lcd read a press this gesture */
static bool                   s_touchDown = false;     /* poll task's view of finger-down */
/* Beyond this radius from the down point (native px) a gesture is a drag, not a
 * tap — matches the order of LVGL's own scroll threshold. */
static constexpr int          TOUCH_CLICK_R = 12;

static void tdeckTouchInit(void) {
    i2c_master_bus_handle_t i2c = tdeckI2cBus();
    if (!i2c) return;

    /* Multi-touch is opt-in and ephemeral: a consumer (e.g. the maps app) sets
     * the runtime flag `lcd.multi_touch` while it wants gestures, and the lcd
     * component's own subscription flips the multipoint read mode
     * (lcd_touch.cpp) — nothing to watch here. The GT911 is a 5-point
     * controller. */

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

    /* Every GT911-tagged driver line is dropped: a failed read or probe logs
     * three of them for one event, and the one worth keeping is our own code's
     * warn() (tdeckTouchSample names runtime read errors; the probe below
     * names a genuinely absent controller). The wrong-address half of the
     * probe and a not-yet-powered controller fail this way as a matter of
     * course. The i2c IO's own line rides a different tag, hence its own rule. */
    logRule("GT911: ", 'N');
    logRule("panel_io_i2c_rx_buffer", 'N');

    /* The GT911 shares the peripheral power rail and runs its own firmware;
     * right after a cold power-on it can miss the first probe on BOTH
     * addresses. Give the rail time and retry before declaring it absent. */
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(150));
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
                 * redundant edge just costs one empty read). The edge wakes the poll
                 * task (tdeckTouchIsr), which samples the GT911 and then bumps the lcd
                 * task (lcdTouchPoll) to read the latch — keeping the I2C read off the
                 * render path. Between edges the poll's own cadence sustains tracking.
                 * If the poll task doesn't exist yet (early boot) the ISR no-ops and
                 * the poll's timeout picks the finger up. */
                gpio_set_intr_type((gpio_num_t)BOARD_TOUCH_INT_PIN, GPIO_INTR_ANYEDGE);
                gpio_isr_handler_add((gpio_num_t)BOARD_TOUCH_INT_PIN, tdeckTouchIsr, nullptr);
                gpio_intr_enable((gpio_num_t)BOARD_TOUCH_INT_PIN);
                s_touch = tp;
                return;
            }
            esp_lcd_panel_io_del(tio);   /* free and try the other address */
        }
    }
    warn("touch: GT911 not found at 0x5D or 0x14 after 4 attempts\n");
    storageSet("tdeck.touch", "not found");
}

/* lcd_input.h touch_read (lcd task): return the sample the poll task latched, in
 * NATIVE coords (the lcd component applies the panel rotation) — no I2C here, so a
 * touch read never waits on the bus or behind a render. When there is no live
 * finger but a tap completed unseen (finger down+up during a render stall), replay
 * its press once; the next read then finds no finger → release → the click lands.
 * Returns false only before the GT911 is up. */
static bool tdeckTouchRead(lcd_raw_pt_t* pts, int max, int* count) {
    *count = 0;
    if (!s_touch) return false;
    taskENTER_CRITICAL(&s_touchMux);
    int n = s_tCount;
    if (n > max) n = max;
    if (n > 0) {
        s_gestureConsumed = true;      /* the lcd has now seen this gesture live */
        for (int i = 0; i < n; i++) { pts[i].x = s_tX[i]; pts[i].y = s_tY[i]; }
        *count = n;
    } else if (s_tapPending && max > 0) {
        s_tapPending = false;          /* one-shot: consume the replayed tap */
        pts[0].x = s_tapX; pts[0].y = s_tapY;
        *count = 1;
    }
    taskEXIT_CRITICAL(&s_touchMux);
    return true;
}

/* Poll task: read the GT911 once, latch the sample, and track the gesture so a
 * missed tap can be replayed (but a drag never is — see the latch note above).
 * Returns true when the lcd task should be bumped: a finger-down edge (start
 * tracking) or a tap to replay. The I2C read is done before the critical section
 * — esp_lcd_touch blocks on the bus and must not run under the spinlock. */
static bool tdeckTouchSample(void) {
    if (!s_touch || s_touchAsleep) return false;
    if (s_touchWakeAbsorb) {                 /* the waking finger: nothing until it lifts */
        esp_lcd_touch_point_data_t wpt[5] = {};
        uint8_t wcnt = 0;
        if (esp_lcd_touch_read_data(s_touch) == ESP_OK)
            esp_lcd_touch_get_data(s_touch, wpt, &wcnt, 5);
        if (wcnt == 0) s_touchWakeAbsorb = false;
        return false;
    }
    esp_lcd_touch_point_data_t pt[5] = {};
    uint8_t cnt = 0;
    /* A bus glitch loses one sample and the next poll picks the finger back up,
     * so this is a warning, not an error — and it is reported on the failing
     * edge only, since a wedged bus fails on every poll and would otherwise
     * flood the log at the poll cadence. */
    static bool readFailed = false;
    esp_err_t err = esp_lcd_touch_read_data(s_touch);
    if (err != ESP_OK) {
        if (!readFailed) warn("touch: GT911 read failed: %s\n", esp_err_to_name(err));
        readFailed = true;
        return false;                    /* keep the last latch; nothing new to show */
    }
    readFailed = false;
    esp_lcd_touch_get_data(s_touch, pt, &cnt, 5);
    int n = cnt > 5 ? 5 : cnt;

    static int16_t downX = 0, downY = 0;
    static bool    moved = false;
    bool bump = false;

    taskENTER_CRITICAL(&s_touchMux);
    s_tCount = n;
    for (int i = 0; i < n; i++) { s_tX[i] = (int16_t)pt[i].x; s_tY[i] = (int16_t)pt[i].y; }
    if (n > 0) {
        if (!s_touchDown) {                 /* finger-down edge: a new gesture */
            s_touchDown = true; moved = false;
            downX = s_tX[0]; downY = s_tY[0];
            s_gestureConsumed = false;
            bump = true;                     /* wake the lcd task to start tracking */
        } else if (!moved) {
            int dx = s_tX[0] - downX, dy = s_tY[0] - downY;
            if (dx * dx + dy * dy > TOUCH_CLICK_R * TOUCH_CLICK_R) moved = true;
        }
    } else if (s_touchDown) {               /* finger-up edge: gesture ended */
        s_touchDown = false;
        if (!moved && !s_gestureConsumed) { /* a tap the lcd never saw → replay it */
            s_tapPending = true; s_tapX = downX; s_tapY = downY;
            bump = true;
        }
    }
    taskEXIT_CRITICAL(&s_touchMux);
    return bump;
}

/* ---- centre / Home button (GPIO 0): click, navigation, standby ----
 * GPIO 0 is the BOOT-strap pin, also the trackball centre-press (shared with the
 * mic, which reticulous never uses). Pulled-up active-low. The board owns the
 * timing and the five meanings of the press:
 *   - hold 300 ms                -> standby (set sys.standby)
 *   - one click                  -> a pointer click (lcd turns it into one)
 *   - two clicks                 -> the launcher (lcdGoHome)
 *   - three clicks               -> the running-app switcher (lcdShowRecents)
 *   - any press while in standby -> wake (clear sys.standby); the press still
 *                                   counts as the burst's first click, so a
 *                                   double or triple click wakes AND navigates
 * A hold has one tier and one meaning: sleep. Navigation is by click count, so it
 * reads the same from the launcher, an app, or the switcher — the lcd component
 * takes Home/Recents whatever is in the foreground.
 *
 * Clicks are counted while releases keep landing within kMulticlickMs of each
 * other and dispatched when that window closes, so a plain click costs one window
 * of latency — the price of a second click meaning something else. Three is the
 * maximum, so it dispatches on its own release without waiting.
 *
 * A burst that started by waking the device is marked, and its one-click case
 * dispatches nothing: that press meant "wake", and a click at the cursor is not
 * what the first touch of a sleeping device should do. Two and three still mean
 * what they always mean, so the same gesture reaches the launcher or the switcher
 * whether the screen was on or off.
 *
 * The lcd component applies no timing of its own, and neither threshold is a
 * setting: both are reflexes, not preferences, and a wrong value for either is
 * felt at once rather than tuned. */
static constexpr uint32_t kStandbyHoldMs = 300;   /* hold this long and the screen goes off */
static constexpr uint32_t kMulticlickMs  = 250;   /* a further click has to land within this */

static lv_timer_t*    s_standbyTimer = nullptr;   /* fires at kStandbyHoldMs -> standby */
static lv_timer_t*    s_clickTimer   = nullptr;   /* fires at kMulticlickMs -> dispatch the burst */
static int            s_clicks       = 0;         /* releases counted in the current burst */
static bool           s_clickAssert  = false;     /* one-shot: the next click_read() is a click */
static bool           s_btnHeld      = false;     /* a press is in progress */
static bool           s_btnConsumed  = false;     /* the hold fired -> release is not a click */
static bool           s_wakeAbsorb   = false;     /* swallow the rest of a transition press until release */
static bool           s_wakeClick    = false;     /* the absorbed press woke us: its release opens a burst */
static bool           s_wokeBurst    = false;     /* this burst's first click was the wake */
static volatile bool  s_standby      = false;     /* mirror of sys.standby (set on the lcd task) */
/* Standby wake: the button is re-armed as a genuine light-sleep wake source
 * (pmGpioWakeEnable — LOW_LEVEL, since edges are invisible while the GPIO clock
 * is gated, and sleep-isolation-exempt via gpio_sleep_sel_dis; same pattern as
 * LoRa's DIO1). Two button-specific wrinkles the radio pin doesn't have:
 *
 *  - A level-triggered ISR re-fires for as long as the line is held — DIO1
 *    drops within µs when the IRQ is read, a finger doesn't, so an unmitigated
 *    LOW_LEVEL ISR storms its core for the whole press (a >5 s hold would trip
 *    the task WDT). tdeckStandbyBtnISR therefore silences the pin on first fire;
 *    tdeckClickRead re-enables it after handling.
 *
 *  - Standby is usually *entered by a press* that is still down. The LOW_LEVEL
 *    wake can only be armed once that finger lifts, so s_standbyLock holds the
 *    CPU out of light sleep for just that entry-press window (previously it was
 *    held for the whole standby — the GPIO wake looked broken because the pin
 *    was sleep-isolated, before pmGpioWakeEnable learned gpio_sleep_sel_dis). */
static pm_lock_handle_t s_standbyLock = nullptr;
static bool             s_wakeArmed   = false;   /* LOW_LEVEL wake source live */
static volatile bool    s_wakePending = false;   /* the wake ISR fired: a press happened, even if the
                                                  * finger has since lifted (latched so a short press
                                                  * during the sleep-exit latency still wakes) */

/* First fire of the LOW_LEVEL wake press: silence the pin (see storm note
 * above), then the normal notify. LL register write, not gpio_intr_disable() —
 * the driver call takes a non-ISR spinlock. IRAM: the ISR service is installed
 * with ESP_INTR_FLAG_IRAM. */
static void IRAM_ATTR tdeckStandbyBtnISR(void* arg) {
    gpio_ll_intr_disable(&GPIO, (gpio_num_t)BOARD_HOME_BTN_PIN);
    s_wakePending = true;   /* a press occurred — latch it so a finger that lifts before
                             * the lcd task polls (sleep-exit latency) still wakes us */
    lcdInputISR(arg);
}

/* Arm the centre button as the light-sleep wake source (lcd task, in standby,
 * button up) and let the CPU sleep. Level semantics cover the handler-swap gap:
 * a press racing the swap still fires once LOW_LEVEL is set. */
static void tdeckWakeArm(void) {
    gpio_isr_handler_remove((gpio_num_t)BOARD_HOME_BTN_PIN);
    gpio_isr_handler_add((gpio_num_t)BOARD_HOME_BTN_PIN, tdeckStandbyBtnISR, nullptr);
    pmGpioWakeEnable(BOARD_HOME_BTN_PIN, GPIO_INTR_LOW_LEVEL);
    s_wakeArmed   = true;
    s_wakePending = false;   /* fresh arm: drop any stale latch */
    pmLockRelease(s_standbyLock);
}

/* Light-sleep wake backstop (IDLE-task context, on every light-sleep exit). The
 * LOW_LEVEL wake source (tdeckStandbyBtnISR) only latches the press if GPIO0 is
 * still low when the post-wake GPIO interrupt is sampled — but a press bounces,
 * and the ~1 ms sleep-exit window can land in a bounce-high gap, so the level ISR
 * never fires and the chip drops straight back to light sleep: the press woke the
 * chip but was dropped (the "several presses to wake"). Re-check on the wake
 * itself: if a GPIO wake finds us armed in standby with GPIO0 held low, latch the
 * press and notify the lcd task directly, independent of the ISR. A real (held)
 * press keeps GPIO0 low, so the chip re-wakes at once on each sleep attempt and
 * this catches it within a cycle; a DIO1 radio wake leaves GPIO0 high, ignored. */
static void tdeckSleepWake(int cause) {
    if (cause != ESP_SLEEP_WAKEUP_GPIO) return;
    if (!s_standby || !s_wakeArmed) return;
    if (gpio_get_level((gpio_num_t)BOARD_HOME_BTN_PIN) != 0) return;   /* GPIO0 high → not the button */
    s_wakePending = true;
    lcdInputSignal();
}

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
    pmOnLightSleepWake(tdeckSleepWake);    /* backstop the LOW_LEVEL standby wake */
}

static void btnClickCb(lv_timer_t*);   /* the multi-click window closing */

static void cancelStandbyTimer(void) {
    if (s_standbyTimer) { lv_timer_delete(s_standbyTimer); s_standbyTimer = nullptr; }
}

static void cancelClickBurst(void) {
    if (s_clickTimer) { lv_timer_delete(s_clickTimer); s_clickTimer = nullptr; }
    s_clicks    = 0;
    s_wokeBurst = false;
}

static void armClickWindow(void) {
    if (s_clickTimer) lv_timer_delete(s_clickTimer);
    s_clickTimer = lv_timer_create(btnClickCb, kMulticlickMs, nullptr);
    lv_timer_set_repeat_count(s_clickTimer, 1);
}

/* The press that woke us is click one. It buys the same window as any other, so
 * a second or third landing inside it still reads as Home or the switcher. */
static void beginWakeBurst(void) {
    if (s_wokeBurst) return;   /* already counting from the wake; don't restart it */
    s_wokeBurst = true;
    s_clicks    = 1;
    armClickWindow();
}

/* Act on a closed burst (lcd task). One click is handed back to the lcd component
 * as a real click, at the cursor or on the focused item; two and three are
 * navigation, which the component routes wherever it is. */
static void dispatchClicks(int n) {
    bool woke   = s_wokeBurst;
    s_wokeBurst = false;
    if      (n <= 0) return;
    else if (n == 1) { if (!woke) { s_clickAssert = true; lcdInputSignal(); } }  /* -> tdeckClickRead */
    else if (n == 2) lcdGoHome();
    else             lcdShowRecents();
}

/* The multi-click window closed with no further press (lcd task, via lv_timer). */
static void btnClickCb(lv_timer_t*) {
    s_clickTimer = nullptr;    /* the one-shot self-deleted after this fire */
    int n = s_clicks;
    s_clicks = 0;
    dispatchClicks(n);
}

/* Held to standby_hold: enter standby. Swallow the rest of this press so the
 * still-down finger can't immediately wake it again. */
static void btnStandbyCb(lv_timer_t*) {
    s_standbyTimer = nullptr;
    s_btnConsumed  = true;
    s_btnHeld      = false;    /* this press is done as far as the state machine */
    s_wakeAbsorb   = true;     /* ignore the held finger until it lifts */
    cancelClickBurst();        /* clicks before the hold are not a navigation burst */
    storageSet("sys.standby", 1);
}

/* lcd_input.h click_read (lcd task): the click-count / hold / standby state
 * machine. A dispatched single click asserts for exactly one poll (the component
 * forces the follow-up read that lands the release → LVGL sees a click). */
static bool tdeckClickRead(void) {
    bool down = gpio_get_level((gpio_num_t)BOARD_HOME_BTN_PIN) == 0;   /* active-low */

    /* Swallow the remainder of a press that already caused a transition (woke us,
     * or was held into standby) until the finger lifts. The lift of the press
     * that *entered* standby is also the moment the LOW_LEVEL wake source can
     * be armed (see tdeckWakeArm). */
    if (s_wakeAbsorb) {
        if (!down) {
            s_wakeAbsorb = false;
            if (s_standby && !s_wakeArmed) tdeckWakeArm();
            /* Only a press that WOKE us opens a burst — the one that put the
             * device to sleep is absorbed and counts as nothing. */
            if (s_wakeClick) { s_wakeClick = false; beginWakeBurst(); }
        }
        return false;
    }
    /* In standby the only live input is this button: a press just wakes (clears
     * sys.standby) and is absorbed — never a click or a hold. */
    if (s_standby) {
        if (!s_wakeArmed) {
            /* Entry press still down (or a programmatic standby raced a press):
             * s_standbyLock is holding sleep off until we can arm on release. */
            if (!down) tdeckWakeArm();
            return false;
        }
        /* A press occurred — still down, OR the ISR latched one that has since
         * lifted during the sleep-exit latency. Either wakes: polling only `down`
         * dropped short presses, so it took several tries to catch one still-down
         * at poll time. Clearing sys.standby runs tdeckStandby(false), which
         * restores the awake button config (as the down path already relied on). */
        if (down || s_wakePending) {
            s_wakePending = false;
            storageSet("sys.standby", 0);
            /* Still down: swallow until it lifts, and let that release open the
             * burst. Already lifted (the ISR latched a press we only see now):
             * there is no release left to wait for, so open it here. */
            if (down) { s_wakeAbsorb = true; s_wakeClick = true; }
            else      beginWakeBurst();
        }
        /* Woken with nothing to act on (an already-consumed blip, or a non-button
         * wake): the ISR silenced the pin, so re-arm it or the button goes deaf. */
        else gpio_intr_enable((gpio_num_t)BOARD_HOME_BTN_PIN);
        return false;
    }

    /* A dispatched single click, asserted for exactly one read. */
    if (s_clickAssert) { s_clickAssert = false; return true; }

    if (down) {
        if (!s_btnHeld) {
            s_btnHeld     = true;
            s_btnConsumed = false;
            /* The burst is still open — this press may be its second or third
             * click, so stop the window from closing under it. */
            if (s_clickTimer) { lv_timer_delete(s_clickTimer); s_clickTimer = nullptr; }
            s_standbyTimer = lv_timer_create(btnStandbyCb, kStandbyHoldMs, nullptr);
            lv_timer_set_repeat_count(s_standbyTimer, 1);
        }
        return false;                               /* never click while held */
    }

    cancelStandbyTimer();
    bool released = (s_btnHeld && !s_btnConsumed);  /* a press that wasn't held into standby */
    s_btnHeld = false;
    if (!released) return false;

    /* Three is as far as the counting goes, so it needs no window to close. */
    if (++s_clicks >= 3) { int n = s_clicks; s_clicks = 0; dispatchClicks(n); return false; }
    armClickWindow();
    return false;
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
static int               s_upCount  = 0;              /* UP pulses toward the caret walk-out */
static int64_t           s_upFirstUs = 0;             /* start of the current UP window */

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
    /* Default the ball cursor to always-on so it stays put in ball-cursor mode
     * (you can see where a click / the text caret sits). Editing hides it
     * explicitly (arrow mode), so it isn't in the way while typing. -1 = always. */
    storageDefault("s.tdeck.pointer_visible_time", -1);
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

    /* Arrow-key mode: feed arrows to the focus group instead of moving the
     * pointer. Two triggers — an app latch (LcdApp::setScrollwheelArrows, e.g.
     * the on-device terminal) or a live text caret (lcdCaretActive: editing a box,
     * so the ball drives the caret). Uses the raw per-read pulse delta, so it never
     * sticks at a screen edge the way the clamped pointer position would. */
    int cx = 0, cy = 0; bool atTop = false;
    bool caret = lcdCaretActive(&cx, &cy, &atTop);
    if (lcdScrollwheelArrowsActive() || caret) {
        int n; uint32_t key;
        if (abs(dyp) >= abs(dxp)) { n = abs(dyp); key = dyp > 0 ? LV_KEY_DOWN  : LV_KEY_UP;   }
        else                      { n = abs(dxp); key = dxp > 0 ? LV_KEY_RIGHT : LV_KEY_LEFT; }
        if (n > 4) n = 4;                   /* cap a fast flick */

        /* Walk-out: 3 quick UPs pushed against the top line drop edit mode and put
         * the ball-cursor back, parked on the caret. Any other motion breaks the
         * streak. Only while a caret is live (the program latch has no "top"). */
        if (caret && key == LV_KEY_UP && atTop && n > 0) {
            int64_t now = esp_timer_get_time();
            if (s_upCount == 0 || now - s_upFirstUs > 500000) { s_upCount = 0; s_upFirstUs = now; }
            s_upCount += n;
            if (s_upCount >= 3) {
                s_upCount = 0;
                lcdCaretRelease();
                s_ptrX = std::clamp(cx, 0, scrW - 1);
                s_ptrY = std::clamp(cy, 0, scrH - 1);
                *x = s_ptrX; *y = s_ptrY;
                return true;                /* moved → cursor glides to the caret */
            }
        } else {
            s_upCount = 0;
        }

        lv_group_t* g = lcdInputGroup();
        for (int i = 0; i < n && g; i++) lv_group_send_data(g, key);
        *x = s_ptrX; *y = s_ptrY;           /* pointer stays put */
        return false;
    }
    s_upCount = 0;                          /* not editing → no streak */

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

    /* Centre-button timings + standby. The button (and the lcd inactivity
     * timeout) only set/clear the ephemeral sys.standby key; this subscription is
     * what actually sleeps/wakes the device — display off via lcdScreenSleep/Wake,
     * plus our own input (touch + keyboard scan) off. This init runs on the lcd
     * task, so the subscription dispatches there and lcdScreenSleep/Wake are safe. */
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
 * indev. We keep GPIO46 wired anyway: an edge wakes the poll early and is
 * reported by the kbint diagnostic (useful if a future firmware drives it).
 *
 * This same task also samples the GT911 touch (tdeckTouchSample): touch and the
 * keyboard are the only two peripherals on I2C0, so scanning both from one task
 * off the lcd task removes the read from the render path and removes the bus
 * contention the two tasks used to have. The touch INT (tdeckTouchIsr) and a
 * finger-down both shorten the scan cadence for smooth tracking; see below.
 *
 * Dependency is one-way: we call into lcd (lcdRun / lcdTouchPoll / lcdInputGroup /
 * lcdSetHasKeyboard); the lcd component has no knowledge of the keyboard. */
namespace {

i2c_master_dev_handle_t s_kbd        = nullptr;
QueueHandle_t           s_queue      = nullptr;   /* bytes: poll task -> lcd task */
TaskHandle_t            s_pollTask   = nullptr;
lv_indev_t*             s_indev      = nullptr;    /* our keypad indev (lcd task) */
bool                    s_again      = false;      /* lcd task: more to drain this cycle */
volatile uint32_t       s_intCount   = 0;          /* for the kbint diagnostic */

/* Poll periods. The C3 holds only the last unread key (no buffer), so any lazy
 * backoff drops keystrokes under fast typing; standby parks the task, so the
 * always-on 30 ms scan costs nothing while the device sleeps. While a finger is
 * down we scan faster so touch tracking (a drag / scroll) stays smooth — the
 * component re-reads the latch every 10 ms, so refresh it at a matching rate. */
constexpr TickType_t POLL_PERIOD  = pdMS_TO_TICKS(30);
constexpr TickType_t TOUCH_PERIOD = pdMS_TO_TICKS(10);

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
    if (!s_pollTask) return;
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_pollTask, &hp);
    portYIELD_FROM_ISR(hp);
}

void pollTask(void*) {
    for (;;) {
        /* Parked in standby: stop scanning the I2C keyboard entirely (the centre
         * button is the only thing left to wake the device). tdeckStandby unparks
         * us with a notify. Park on a clean self-check, not a suspend, so we never
         * stop mid-I2C-transaction holding the shared bus. */
        if (s_standby) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            /* Woken while parked: with the glass armed as a wake source that
             * notify is a touch (the centre button goes to the lcd task), so
             * ask the controller whether a finger is really there. */
            if (s_standby && s_touchWakeArmed) tdeckTouchWakeCheck();
            continue;
        }
        /* Woken by a touch/keyboard INT, or times out to poll. Faster cadence
         * while a finger is down so touch tracking stays smooth. */
        ulTaskNotifyTake(pdTRUE, s_touchDown ? TOUCH_PERIOD : POLL_PERIOD);
        if (s_standby) continue;           /* entered standby during the wait */
        /* Touch: sample the GT911 and, on a finger-down edge or a tap to replay,
         * bump the lcd task to read its (event-mode) touch indev. */
        if (tdeckTouchSample()) lcdTouchPoll();
        /* Keyboard: drain up to a queueful per wake — bounded so a wedged keyboard
         * that keeps returning a byte can't spin this task. */
        bool got = false;
        for (int i = 0; i < 16; i++) {
            uint32_t k = i2cReadKey();
            if (!k) break;
            uint8_t b = (uint8_t)k;
            xQueueSend(s_queue, &b, 0);
            got = true;
        }
        if (got) lcdRun(kbDrain);           /* bump the lcd task to read our indev */
    }
}

}  // namespace

/* GT911 INT (GPIO16, ANYEDGE): just wake the poll task, which does the actual
 * GT911 read and then bumps the lcd task. No-op until the poll task exists (the
 * INT is wired during the lcd task's input init, which can precede the poll
 * task's creation in onInit — an early edge is harmless, the poll's timeout
 * catches the finger). IRAM: the shared ISR service is installed with
 * ESP_INTR_FLAG_IRAM. */
static void IRAM_ATTR tdeckTouchIsr(void*) {
    if (!s_pollTask) return;
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_pollTask, &hp);
    portYIELD_FROM_ISR(hp);
}

/* The standby wake variant: silence the pin on first fire (a level ISR re-fires
 * for as long as the finger holds INT at the armed level; tdeckTouchWakeCheck
 * re-enables it), then the same notify. LL register write, not
 * gpio_intr_disable() — the driver call takes a non-ISR spinlock. */
static void IRAM_ATTR tdeckTouchWakeIsr(void* arg) {
    gpio_ll_intr_disable(&GPIO, (gpio_num_t)BOARD_TOUCH_INT_PIN);
    tdeckTouchIsr(arg);
}

/* Swap the GT911 INT between its awake wiring (ANYEDGE → the poll task) and the
 * standby wake source. Lcd task, from tdeckStandby. */
static void tdeckTouchWakeArm(bool on) {
    if (on == s_touchWakeArmed || !s_touch) return;
    gpio_isr_handler_remove((gpio_num_t)BOARD_TOUCH_INT_PIN);
    gpio_isr_handler_add((gpio_num_t)BOARD_TOUCH_INT_PIN,
                         on ? tdeckTouchWakeIsr : tdeckTouchIsr, nullptr);
    if (on) {
        s_touchWakeLevel = gpio_get_level((gpio_num_t)BOARD_TOUCH_INT_PIN) ? 0 : 1;
        pmGpioWakeEnable(BOARD_TOUCH_INT_PIN,
                         s_touchWakeLevel ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    } else {
        pmGpioWakeDisable(BOARD_TOUCH_INT_PIN);
        gpio_set_intr_type((gpio_num_t)BOARD_TOUCH_INT_PIN, GPIO_INTR_ANYEDGE);
    }
    gpio_intr_enable((gpio_num_t)BOARD_TOUCH_INT_PIN);
    s_touchWakeArmed = on;
}

/* Poll task, woken while parked in standby: is there really a finger on the
 * glass? Only that clears the key — the INT can fire for a glitch, and the
 * sample itself is never reported (the touch that wakes must not also click
 * whatever the dark screen was showing). */
static void tdeckTouchWakeCheck(void) {
    gpio_intr_enable((gpio_num_t)BOARD_TOUCH_INT_PIN);   /* the wake ISR silenced it */
    if (!s_touch || esp_lcd_touch_read_data(s_touch) != ESP_OK) return;
    esp_lcd_touch_point_data_t pt[5] = {};
    uint8_t cnt = 0;
    esp_lcd_touch_get_data(s_touch, pt, &cnt, 5);
    if (cnt == 0) return;
    s_touchWakeAbsorb = true;
    storageSet("sys.standby", 0);   /* tdeckStandby(false) does the rest */
}

/* sys.standby subscription target (lcd task). The lcd component only flips the
 * key — on the inactivity timeout or our centre button; we decide what the device
 * actually does: display off, GT911 reads gated, keyboard scan parked. Only the
 * centre button is left live, and a press of it clears the key to wake us. */
static void tdeckStandby(bool on) {
    if (on == s_standby) return;
    s_standby = on;
    if (on) {
        s_touchAsleep = true;                          /* GT911 reads return nothing */
        cancelClickBurst();                            /* no burst survives into sleep */
        /* Drop any latched finger so a press held at sleep time isn't read stale or
         * replayed as a tap on wake. The poll task parks itself on its next loop. */
        taskENTER_CRITICAL(&s_touchMux);
        s_tCount = 0; s_tapPending = false; s_gestureConsumed = false; s_touchDown = false;
        taskEXIT_CRITICAL(&s_touchMux);
        lcdScreenSleep();                              /* display off, backlight to 0 */
        /* Hold the CPU out of light sleep only until the LOW_LEVEL wake source
         * is armed — the entry press must lift first (see s_standbyLock). Armed
         * right away when standby came programmatically (cron/CLI, button up). */
        if (!s_standbyLock) pmLockCreate(PM_NO_LIGHT_SLEEP, "standby", &s_standbyLock);
        pmLockAcquire(s_standbyLock);
        if (gpio_get_level((gpio_num_t)BOARD_HOME_BTN_PIN) != 0) tdeckWakeArm();
        /* Read fresh at every standby, so a change made while the screen was on
         * is in force the moment it goes off. */
        if (storageGetInt("s.lcd.wake_on_touch", 0)) tdeckTouchWakeArm(true);
        info("standby\n");
    } else {
        if (s_wakeArmed) {
            /* Back to the awake config: plain ANYEDGE ISR, no sleep wake. (The
             * lock was already dropped when the wake source was armed.) */
            s_wakeArmed = false;
            pmGpioWakeDisable(BOARD_HOME_BTN_PIN);
            gpio_set_intr_type((gpio_num_t)BOARD_HOME_BTN_PIN, GPIO_INTR_ANYEDGE);
            gpio_isr_handler_remove((gpio_num_t)BOARD_HOME_BTN_PIN);
            gpio_isr_handler_add((gpio_num_t)BOARD_HOME_BTN_PIN, lcdInputISR, nullptr);
        } else {
            pmLockRelease(s_standbyLock);              /* never armed — still held */
        }
        tdeckTouchWakeArm(false);                      /* back to the awake INT wiring */
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
    /* Prio 6: one notch above the lcd task (prio 5) so the render can't starve
     * the poll on core 1 — the C3 holds only the last unread key, so a stalled
     * poll drops keystrokes, and a late INT-driven read comes back stale/garbled
     * (phantom keys). This task now owns ALL of I2C0 (keyboard + touch), off the
     * lcd task, so the two no longer contend for the bus — and outranking the
     * render is what keeps touch sampling on cadence while the lcd task is busy
     * (the whole point of moving the read here). The scans are a few short I2C
     * transactions at 10–30 ms, so preempting the render costs it nothing. */
    xTaskCreatePinnedToCore(pollTask, "kbpoll", 3072, nullptr, 6, &s_pollTask, 1);

    lcdSetHasKeyboard(true);                /* lcd: suppress the on-screen keyboard */
}
