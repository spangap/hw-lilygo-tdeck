/**
 * tdeck_lcd.cpp — T-Deck Plus on-device-UI input HAL: GT911 touch, trackball
 * pointer, centre/Home button, the QWERTY keyboard and the lamp under its keys.
 * Lives under esp-idf/conditional/spangap-lcd/, so it is compiled ONLY when
 * spangap-lcd is staged — no #if needed. Reaches the boot through the
 * TdeckLcdInput service (tdeck_lcd.h):
 *   onStart()  start: band — input HAL register, before lcdInit().
 *   onInit()   init:  band — keyboard bring-up, once the lcd task exists.
 * tdeckI2cBus() and the BOARD_* pin macros come from tdeck.h.
 */
#include "tdeck.h"
#include "tdeck_lcd.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd_input.h"
#include "lcd_keys.h"
#include "lcd.h"
#include "storage.h"
#include "cli.h"
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
#include <cstring>
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
static void kbBacklightInit(void);             /* keyboard lamp: follow the screen (lcd task) */
static void kbBacklightSetLit(bool on);        /* keyboard lamp: the on/off latch (lcd task) */

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
/* Set by the wake ISR, cleared by the check it asks for. The poll task is woken
 * while parked by anything that notifies it — a keyboard-lamp push during the
 * fade to dark as much as a finger — and only the glass's own edge is grounds
 * for waking the GT911 up to ask it about a finger. */
static volatile bool          s_touchWakeFired = false;
/* The finger that woke the deck is swallowed until it lifts: waking is all it
 * does, exactly as the wake press is absorbed rather than clicking. */
static bool                   s_touchWakeAbsorb = false;

/* ---- touch latch: sampled on the poll task, read on the lcd task -------------
 * The GT911 read lives on the keyboard poll task, not the lcd task, so a touch
 * read never waits behind a render and never fights the render for the shared
 * I2C0 bus. tdeckTouchSample() (poll task) does the I2C read and stores the
 * sample here; tdeckTouchRead() (lcd task, the lcd_input.h touch_read) just
 * returns it — no hardware access.
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
 * GPIO 0 is the BOOT-strap pin, also the trackball centre-press (also wired to
 * the mic, whose clock the audio engine takes from GPIO 48, so audio never
 * drives GPIO 0). Pulled-up active-low. The board owns the
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
static lv_timer_t*    s_kbHoldTimer  = nullptr;   /* wake press still held at kStandbyHoldMs -> lamp */
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

/* The waking press, still held: light the keys. The same hold that puts the deck
 * to sleep brings the keyboard up on the way back, which leaves a plain tap
 * meaning what it has always meant — wake the screen, nothing else — so a deck
 * picked up for a glance costs no lamp and a deck picked up to type on asks for
 * one in the gesture that wakes it. The screen is never made to wait for this:
 * it comes up the instant the press lands, and only the keys watch the clock. */
static void btnKbLightCb(lv_timer_t*) {
    s_kbHoldTimer = nullptr;   /* the one-shot self-deleted after this fire */
    kbBacklightSetLit(true);
}

static void cancelKbHoldTimer(void) {
    if (s_kbHoldTimer) { lv_timer_delete(s_kbHoldTimer); s_kbHoldTimer = nullptr; }
}

static void armKbHoldTimer(void) {
    cancelKbHoldTimer();
    s_kbHoldTimer = lv_timer_create(btnKbLightCb, kStandbyHoldMs, nullptr);
    lv_timer_set_repeat_count(s_kbHoldTimer, 1);
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
            cancelKbHoldTimer();   /* lifted before the hold landed: no lamp */
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
            if (down) { s_wakeAbsorb = true; s_wakeClick = true; armKbHoldTimer(); }
            else      beginWakeBurst();   /* already lifted: a tap, so no lamp */
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

    /* The keyboard's own lamp rides on the screen's backlight from here. */
    kbBacklightInit();
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
 *   - A C3 with raw-matrix mode is driven in it: each poll reads the live switch
 *     state, and debounce, ghost rejection, the key map, modifiers and repeat
 *     are ours. An older C3 stays in key mode, whose 1-byte read is destructive
 *     (pops the key) with no peek and returns 0 when empty. A probe at bring-up
 *     tells the two apart.
 *
 * So a dedicated task polls the I2C off the lcd task, turns what it reads into
 * key codes in a queue, and bumps the lcd task (lcdRun) to drain them through
 * our LVGL keypad indev. The Mic key and Space are also reported as held keys
 * (lcd_keys.h); a claimed press is swallowed rather than typed.
 * We keep GPIO46 wired anyway: an edge wakes the poll early and is reported by
 * the kbint diagnostic (useful if a future firmware drives it).
 *
 * This same task also samples the GT911 touch (tdeckTouchSample) and writes the
 * keyboard's own lamp: they are the only peripherals on I2C0, so one task off
 * the lcd task keeps the reads out of the render path and leaves the bus with a
 * single owner. The touch INT (tdeckTouchIsr) and a finger-down both shorten the
 * scan cadence for smooth tracking; see below.
 *
 * Dependency is one-way: we call into lcd (lcdRun / lcdTouchPoll / lcdInputGroup /
 * lcdSetHasKeyboard / lcdKeyReport); the lcd component knows only the named
 * held keys we report, never the matrix or the key map. */
namespace {

i2c_master_dev_handle_t s_kbd        = nullptr;
QueueHandle_t           s_queue      = nullptr;   /* uint32_t key codes: poll task -> lcd task */
TaskHandle_t            s_pollTask   = nullptr;
lv_indev_t*             s_indev      = nullptr;    /* our keypad indev (lcd task) */
bool                    s_again      = false;      /* lcd task: more to drain this cycle */
bool                    s_drainOwed  = false;      /* poll task: queued bytes the lcd task
                                                      has not been bumped for yet */
volatile uint32_t       s_intCount   = 0;          /* for the kbint diagnostic */

/* Poll periods. In key mode the C3 holds only the last unread key (no buffer),
 * so any lazy backoff drops keystrokes under fast typing, and in raw mode the
 * C3's own scan takes ~35 ms, so polling faster gains nothing; standby parks the task, so the
 * always-on 30 ms scan costs nothing while the device sleeps, unless the talk key
 * is claimed (a live session), when it keeps scanning for that key alone. While a finger is
 * down we scan faster so touch tracking (a drag / scroll) stays smooth — the
 * component re-reads the latch every 10 ms, so refresh it at a matching rate. */
constexpr TickType_t POLL_PERIOD  = pdMS_TO_TICKS(30);
constexpr TickType_t TOUCH_PERIOD = pdMS_TO_TICKS(10);

/* ---- keyboard lamp ----------------------------------------------------------
 *
 * The lamps under the keys are the C3's, not ours: it owns the GPIO and the PWM,
 * and all we have is a two-byte write. So the lamp is driven by intent rather
 * than held at a level — we say what it should be at each moment the screen
 * changes, and say nothing in between.
 *
 * Whether it is wanted at all is a latch (s_kbLit), and only a HELD wake press
 * sets it: waking the deck for a glance should not light a keyboard nobody is
 * about to type on. Once lit it follows the screen's own duty, scaled to the
 * configured level — the same proportional cut at the dim step, dark by the time
 * the panel powers off, which also clears the latch. So each trip out of standby
 * asks for the lamp again. lcdBacklightOnChange feeds us every step of that on
 * the lcd task; we hand the wanted duty to the poll task, which owns I2C0 and
 * does the write on its next pass.
 *
 * A duty already written is never rewritten, and that is deliberate: the
 * keyboard's own Alt+B toggles the lamp behind our back — the C3 handles the
 * combo itself and sends no byte — so an Alt+B stands until the screen next
 * changes state, instead of being stamped back out within 30 ms. Alt+B lights to
 * the same configured level, which is what ALT_B_LEVEL keeps in sync. The one
 * write never skipped as a repeat is the one that takes the lamp out with the
 * screen: a deck must not sleep with lit keys, and an unseen Alt+B is exactly the
 * way it otherwise could. The lamp cannot be read back. The C3 toggles it on
 * Alt+B in raw-matrix mode too; there we see the combo as well and type
 * nothing for it. */
constexpr uint32_t KB_BL_PREVIEW_MS = 3000;   /* slider moved: hold it lit this long */

int            s_kbBlLevel   = 0;        /* s.tdeck.kb_backlight: duty when fully lit */
bool           s_kbLit       = false;    /* the lamp is wanted: set by a held wake press */
uint8_t        s_panelDuty   = 0;        /* live screen backlight, from the follow hook */
volatile int   s_kbBlWant    = -1;       /* duty the lcd task wants (-1 = nothing yet) */
int            s_kbBlSent    = -1;       /* duty the poll task last wrote */
volatile bool  s_kbBlForce   = false;    /* write it even if it repeats what we last sent */
volatile int   s_kbAltWant   = -1;       /* level Alt+B should light to */
int            s_kbAltSent   = -1;       /* level last written as that */
lv_timer_t*    s_kbBlPreview = nullptr;  /* live while the slider preview holds it lit */
bool           s_kbBlSeeded  = false;    /* the first apply is boot, not a slider move */

/* Poll task: push whatever the lcd task last asked for. Two independent writes,
 * each skipped when it would say what the C3 already knows. */
void kbBacklightPush() {
    if (!s_kbd) return;
    int alt = s_kbAltWant;
    if (alt > 30 && alt != s_kbAltSent) {    /* the C3 ignores 30 and below */
        uint8_t cmd[2] = { BOARD_KB_CMD_ALT_B_LEVEL, (uint8_t)alt };
        if (i2c_master_transmit(s_kbd, cmd, sizeof(cmd), 20) == ESP_OK) s_kbAltSent = alt;
    }
    int  want  = s_kbBlWant;
    bool force = s_kbBlForce;
    if (want < 0 || (want == s_kbBlSent && !force)) return;
    uint8_t cmd[2] = { BOARD_KB_CMD_BRIGHTNESS, (uint8_t)want };
    if (i2c_master_transmit(s_kbd, cmd, sizeof(cmd), 20) == ESP_OK) {
        s_kbBlSent  = want;
        s_kbBlForce = false;
    }
}

/* Lcd task: work out the duty the lamp should be at and wake the poll task if it
 * moved. While the slider preview holds, the level is shown as set — the point of
 * the preview is to see the level being chosen, not the level times a ratio. */
void kbBacklightApply() {
    int duty;
    if (s_kbBlPreview) {
        duty = s_kbBlLevel;                        /* the slider: as set, not ratio-scaled */
    } else if (!s_kbLit) {
        duty = 0;
    } else {
        int target = lcdBacklightTarget();
        duty = target > 0 ? (s_kbBlLevel * (int)s_panelDuty + target / 2) / target : 0;
    }
    if (duty > 255) duty = 255;
    /* Dark screen, dark keys — the one write that has to land whatever we believe
     * we last sent, because an Alt+B we never saw could have lit them. */
    if (duty == 0 && s_panelDuty == 0) s_kbBlForce = true;
    if (duty == s_kbBlWant && !s_kbBlForce) return;
    s_kbBlWant = duty;
    if (s_pollTask) xTaskNotifyGive(s_pollTask);   /* parked in standby too: it pushes on wake */
}

void kbPreviewDone(lv_timer_t*) {
    s_kbBlPreview = nullptr;       /* the one-shot self-deleted after this fire */
    kbBacklightApply();
}

/* Dragging the slider lights the keyboard at the level under the thumb, so the
 * choice can be made by looking at the keys rather than at a number. It lapses
 * on its own, back to whatever the screen says the lamp should be. Nobody is
 * looking at a dark screen, so a write to the key from the CLI while the deck
 * sleeps is not an invitation to light up a pocket. */
void kbBacklightPreview() {
    if (s_panelDuty == 0) return;
    if (s_kbBlPreview) { lv_timer_reset(s_kbBlPreview); return; }
    s_kbBlPreview = lv_timer_create(kbPreviewDone, KB_BL_PREVIEW_MS, nullptr);
    lv_timer_set_repeat_count(s_kbBlPreview, 1);
}

/* lcdBacklightOnChange target (lcd task): the screen moved, so the lamp does. A
 * screen that has gone out drops the latch with it — the lamp is not owed a
 * comeback on the next wake, which has its own hold to ask with. */
void kbBacklightFollow(uint8_t duty) {
    s_panelDuty = duty;
    if (duty == 0) s_kbLit = false;
    kbBacklightApply();
}

/* Poll task: hand a key code to the lcd task (kbDrain picks it up). */
void kbType(uint32_t code) {
    if (!code || !s_queue) return;
    xQueueSend(s_queue, &code, 0);
    s_drainOwed = true;
}

/* ---- the one-shot Ctrl lead-in, shared by both modes (poll task) ----
 * Armed by Alt-C in key mode and by Alt tapped on its own in raw mode: the next
 * lowercase letter within 1 s becomes Ctrl-<letter> (LCD_KEY_CTRL, which the
 * terminal turns into a control byte); arming it again while armed is Esc. */
constexpr int64_t CTRL_ARM_US = 1000 * 1000;
int64_t s_ctrlUntil = 0;

bool ctrlArmed(int64_t now) { return s_ctrlUntil && now < s_ctrlUntil; }

void ctrlLeadIn(int64_t now) {
    if (ctrlArmed(now)) { s_ctrlUntil = 0; kbType(LV_KEY_ESC); }
    else                s_ctrlUntil = now + CTRL_ARM_US;
}

/* ---- held keys (lcd_keys.h) ---- */

const char* namedKeyName(uint8_t k) { return k == LCD_KEY_PTT ? "ptt" : "space"; }

/* Report one edge of a named key; true = claimed, so swallow the press. */
bool namedKeyEdge(uint8_t k, bool down) {
    bool claimed = lcdKeyReport(k, down);
    dbg("key %s %s%s\n", namedKeyName(k), down ? "down" : "up", claimed ? " (claimed)" : "");
    return claimed;
}

/* A claimed press in standby wakes the screen on the same edge it keys with. */
void namedKeyWake() {
    if (s_standby) storageSet("sys.standby", 0);
}

/* Bit k: named key k held from the CLI (`key hold`); the poll task folds it
 * into what it reads, so it takes the keyboard's own path. */
volatile uint8_t s_simHeld = 0;

/* ---- key mode (C3 firmware without raw mode) ----------------------------- */

/* Key mode's Mic: alone it sends nothing, but Shift+Mic sends 0xE0 (the C3
 * subtracts 32 from the key's empty 0x00), a byte no character uses — so that
 * press toggles push-to-talk: down on one press, up on the next. An unclaimed
 * toggle reports its own up at once and stays off, and a latched one comes up
 * by itself when the last claim goes (kmPoll), so the next press keys again. */
bool s_kmPttLatched = false;

void kmByte(uint8_t b) {
    const int64_t now = esp_timer_get_time();
    if (b == 0xE0) {
        if (s_kmPttLatched) { s_kmPttLatched = false; namedKeyEdge(LCD_KEY_PTT, false); }
        else if (namedKeyEdge(LCD_KEY_PTT, true)) { s_kmPttLatched = true; namedKeyWake(); }
        else namedKeyEdge(LCD_KEY_PTT, false);
        return;
    }
    if (s_standby) return;                            /* in standby only the talk key is live */
    if (b == 0x0C) { ctrlLeadIn(now); return; }       /* Alt-C */
    const bool armed = ctrlArmed(now);
    s_ctrlUntil = 0;
    if (armed && b >= 'a' && b <= 'z') { kbType(LCD_KEY_CTRL | b); return; }
    switch (b) {
        case 13: case 10:  kbType(LV_KEY_ENTER);     return;
        case 8:  case 127: kbType(LV_KEY_BACKSPACE); return;
        case 27:           kbType(LV_KEY_ESC);       return;
        default:           if (b >= 0x20 && b < 0x7F) kbType(b); return;
    }
}

/* Key mode, each poll: drain up to a queueful — bounded so a wedged keyboard
 * that keeps returning a byte can't spin this task. */
void kmPoll() {
    if (s_kmPttLatched && !lcdKeyClaimed(LCD_KEY_PTT)) {
        s_kmPttLatched = false;
        namedKeyEdge(LCD_KEY_PTT, false);
    }
    /* A CLI hold of the talk key is a Shift+Mic toggle whenever the two differ. */
    static bool simPtt = false;
    const bool sim = s_simHeld & (1u << LCD_KEY_PTT);
    if (sim != simPtt) { simPtt = sim; if (sim != s_kmPttLatched) kmByte(0xE0); }
    for (int i = 0; i < 16; i++) {
        uint8_t b = 0;
        if (i2c_master_receive(s_kbd, &b, 1, 10) != ESP_OK || !b) break;
        dbg("kbread: 0x%02x\n", (unsigned)b);
        kmByte(b);
    }
}

/* ---- raw-matrix mode --------------------------------------------------------
 *
 * 5 columns x 7 rows; frame[c] bit r = switch (c,r) closed. Debounce: a press
 * counts on the first frame that shows it, a release only on the second frame
 * in a row that does (a single open sample mid-bounce is not a release). The
 * matrix has no diodes, so when three closed switches form three corners of a
 * rectangle the fourth reads closed too; any frame with two columns sharing two
 * or more rows is ambiguous, and its newly closed corners are dropped (keys
 * already held stay held).
 *
 * Key map. The caps' own legends, plus a Shift+Sym layer for what they lack:
 *   base        a-z, Space, Enter, Backspace, $
 *   Shift       A-Z  (other keys as base)
 *   Sym         the printed symbols: # 1 * 0 2 4 5 @ 8 7 3 / ( ? 9 6 _ : ) ! , ;
 *               + " - . ' ; Sym+Space = Tab (LV_KEY_NEXT), Sym+Backspace = Delete
 *   Shift+Sym   digits give the US shifted symbol (1! 2@ 3# 4$ 5% 6^ 7& 8* 9( 0)),
 *               t [  y ]  u {  i }  o =  g \  b |  n <  m >  k `  l ~ ;
 *               any other key gives its Sym symbol
 *   Alt         Alt+letter = Ctrl-letter; Alt tapped alone arms Ctrl for the
 *               next letter (twice = Esc). Alt+B is the lamp, toggled by the C3
 *               itself, and types nothing.
 * Mic types only with Sym ('0', Shift+Sym ')'): alone it is push-to-talk and
 * does nothing unless claimed. Printable keys, Backspace and Delete repeat
 * after REPEAT_DELAY_US, every REPEAT_US; Enter, Tab, Esc and Ctrl do not. */
constexpr int  KB_COLS = 5, KB_ROWS = 7;
constexpr int64_t REPEAT_DELAY_US = 500 * 1000;
constexpr int64_t REPEAT_US       = 60 * 1000;   /* two polls */

enum : uint8_t {   /* (col << 3) | row */
    KP_SYM = (0 << 3) | 2, KP_ALT = (0 << 3) | 4, KP_SPACE = (0 << 3) | 5, KP_MIC = (0 << 3) | 6,
    KP_LSHIFT = (1 << 3) | 6, KP_RSHIFT = (2 << 3) | 3, KP_ENTER = (3 << 3) | 3,
    KP_BACKSPACE = (4 << 3) | 3, KP_B = (3 << 3) | 4,
};

const char kBase[KB_COLS][KB_ROWS] = {
    { 'q', 'w', 0,   'a', 0,   ' ', 0   },
    { 'e', 's', 'd', 'p', 'x', 'z', 0   },
    { 'r', 'g', 't', 0,   'v', 'c', 'f' },
    { 'u', 'h', 'y', 0,   'b', 'n', 'j' },
    { 'o', 'l', 'i', 0,   '$', 'm', 'k' },
};
const char kSym[KB_COLS][KB_ROWS] = {
    { '#', '1', 0,   '*', 0,   0,   '0'  },
    { '2', '4', '5', '@', '8', '7', 0    },
    { '3', '/', '(', 0,   '?', '9', '6'  },
    { '_', ':', ')', 0,   '!', ',', ';'  },
    { '+', '"', '-', 0,   0,   '.', '\'' },
};
const char kShiftSym[KB_COLS][KB_ROWS] = {
    { 0,   '!', 0,   0,   0,   0,   ')'  },
    { '@', '$', '%', 0,   '*', '&', 0    },
    { '#', '\\','[', 0,   0,   '(', '^'  },
    { '{', 0,   ']', 0,   '|', '<', 0    },
    { '=', '~', '}', 0,   0,   '>', '`'  },
};

uint8_t s_kbDown[KB_COLS]   = {};   /* debounced state */
uint8_t s_kbUpPend[KB_COLS] = {};   /* read open once; a second open read releases */
uint8_t s_kbIgnore[KB_COLS] = {};   /* held at bring-up / wake: ignored until released */
bool    s_kbResync          = true; /* the next frame's held keys go to s_kbIgnore */
bool    s_altAlone          = false;/* Alt is down and nothing else has been pressed */
bool    s_swallowed[LCD_KEY_COUNT] = {};   /* this press of a named key was claimed */
int     s_repPos            = -1;   /* the repeating key, (col << 3) | row */
uint32_t s_repCode          = 0;
int64_t s_repNext           = 0;

bool isHeld(uint8_t pos) { return s_kbDown[pos >> 3] & (1u << (pos & 7)); }

/* The code a press produces under the modifiers held now (0 = nothing), and
 * whether it repeats. */
uint32_t rawCode(int c, int r, bool* repeats) {
    const uint8_t pos  = (uint8_t)((c << 3) | r);
    const bool shift   = isHeld(KP_LSHIFT) || isHeld(KP_RSHIFT);
    const bool sym     = isHeld(KP_SYM);
    const bool alt     = isHeld(KP_ALT);
    *repeats = false;
    if (pos == KP_ENTER) return LV_KEY_ENTER;
    if (pos == KP_BACKSPACE) { *repeats = true; return sym ? LV_KEY_DEL : LV_KEY_BACKSPACE; }
    if (pos == KP_SPACE && sym) return LV_KEY_NEXT;
    const char base = kBase[c][r];
    if (alt && base >= 'a' && base <= 'z') return LCD_KEY_CTRL | (uint32_t)base;
    char ch = base;
    if (sym) {
        ch = shift && kShiftSym[c][r] ? kShiftSym[c][r] : kSym[c][r];
        if (!ch) ch = (pos == KP_MIC) ? 0 : base;
    } else if (shift && base >= 'a' && base <= 'z') {
        ch = (char)(base - 'a' + 'A');
    }
    if (!ch) return 0;
    *repeats = true;
    return (uint32_t)(uint8_t)ch;
}

void stopRepeat() { s_repPos = -1; s_repCode = 0; }

/* A typing key went down (not a modifier, not a swallowed named key). */
void rawPress(int c, int r, int64_t now) {
    const uint8_t pos = (uint8_t)((c << 3) | r);
    if (isHeld(KP_ALT) && pos == KP_B) { stopRepeat(); return; }   /* the C3's lamp toggle */
    bool repeats = false;
    uint32_t code = rawCode(c, r, &repeats);
    if (ctrlArmed(now)) {
        s_ctrlUntil = 0;
        if (code >= 'a' && code <= 'z') { code = LCD_KEY_CTRL | code; repeats = false; }
    }
    stopRepeat();
    if (!code) return;
    kbType(code);
    if (repeats) { s_repPos = pos; s_repCode = code; s_repNext = now + REPEAT_DELAY_US; }
}

/* One debounced edge of switch (c,r). */
void rawEdge(int c, int r, bool down, int64_t now) {
    const uint8_t pos = (uint8_t)((c << 3) | r);
    /* In standby nothing types: a talk-key press is the only press that counts,
     * and releases still land so a named key held from before comes up. */
    if (s_standby && down && pos != KP_MIC) return;
    if (pos == KP_ALT) {
        if (down) s_altAlone = true;
        else if (s_altAlone) { s_altAlone = false; ctrlLeadIn(now); }
        return;
    }
    if (down) s_altAlone = false;
    if (pos == KP_SYM || pos == KP_LSHIFT || pos == KP_RSHIFT) return;

    if (pos == KP_MIC || pos == KP_SPACE) {
        const uint8_t k = pos == KP_MIC ? LCD_KEY_PTT : LCD_KEY_SPACE;
        if (down) {
            s_swallowed[k] = namedKeyEdge(k, true);
            if (s_swallowed[k]) { if (s_repPos >= 0) stopRepeat(); namedKeyWake(); return; }
        } else {
            namedKeyEdge(k, false);
            if (s_swallowed[k]) { s_swallowed[k] = false; return; }
        }
    }
    if (down) { if (!s_standby) rawPress(c, r, now); }
    else if (s_repPos == pos) stopRepeat();
}

/* Release everything held, as if every key lifted (the poll task parks in
 * standby and would otherwise sleep on a held push-to-talk). Whatever is still
 * held on the way back is ignored until it is released. */
void rawReleaseAll() {
    const int64_t now = esp_timer_get_time();
    s_altAlone = false;                 /* a release here is no Alt tap */
    for (int c = 0; c < KB_COLS; c++)
        for (int r = 0; r < KB_ROWS; r++)
            if (s_kbDown[c] & (1u << r)) {
                s_kbDown[c] &= (uint8_t)~(1u << r);
                rawEdge(c, r, false, now);
            }
    for (int c = 0; c < KB_COLS; c++) s_kbUpPend[c] = 0;
    stopRepeat();
    s_kbResync = true;
}

/* A valid frame (no bit 7 anywhere) through ghost rejection, debounce, edges
 * and repeat. Modifier edges go first so a key pressed in the same frame as its
 * Shift is shifted; releases go before presses. */
void rawFrame(const uint8_t in[KB_COLS], int64_t now) {
    uint8_t f[KB_COLS];
    memcpy(f, in, sizeof(f));

    if (s_kbResync) {
        s_kbResync = false;
        for (int c = 0; c < KB_COLS; c++) { s_kbIgnore[c] = f[c]; s_kbDown[c] = 0; s_kbUpPend[c] = 0; }
    }
    for (int c = 0; c < KB_COLS; c++) {
        s_kbIgnore[c] &= f[c];          /* released: no longer ignored */
        f[c] &= (uint8_t)~s_kbIgnore[c];
    }

    uint8_t ghost[KB_COLS] = {};       /* from the unmasked frame: an ignored key still ghosts */
    for (int a = 0; a < KB_COLS; a++)
        for (int b = a + 1; b < KB_COLS; b++) {
            uint8_t common = in[a] & in[b];
            if (__builtin_popcount(common) >= 2) { ghost[a] |= common; ghost[b] |= common; }
        }
    for (int c = 0; c < KB_COLS; c++) f[c] &= (uint8_t)~(ghost[c] & ~s_kbDown[c]);

    uint8_t pressed[KB_COLS], released[KB_COLS];
    for (int c = 0; c < KB_COLS; c++) {
        const uint8_t open = s_kbDown[c] & (uint8_t)~f[c];
        released[c]    = open & s_kbUpPend[c];
        s_kbUpPend[c]  = open & (uint8_t)~released[c];
        pressed[c]     = f[c] & (uint8_t)~s_kbDown[c];
    }
    static const uint8_t kMods[] = { KP_SYM, KP_ALT, KP_LSHIFT, KP_RSHIFT };
    for (uint8_t pos : kMods) {
        const int c = pos >> 3; const uint8_t bit = (uint8_t)(1u << (pos & 7));
        if (released[c] & bit) { released[c] &= (uint8_t)~bit; s_kbDown[c] &= (uint8_t)~bit; rawEdge(c, pos & 7, false, now); }
        if (pressed[c] & bit)  { pressed[c]  &= (uint8_t)~bit; s_kbDown[c] |= bit;            rawEdge(c, pos & 7, true, now); }
    }
    for (int c = 0; c < KB_COLS; c++)
        for (int r = 0; r < KB_ROWS; r++)
            if (released[c] & (1u << r)) { s_kbDown[c] &= (uint8_t)~(1u << r); rawEdge(c, r, false, now); }
    for (int c = 0; c < KB_COLS; c++)
        for (int r = 0; r < KB_ROWS; r++)
            if (pressed[c] & (1u << r)) { s_kbDown[c] |= (uint8_t)(1u << r); rawEdge(c, r, true, now); }

    if (s_repPos >= 0) {
        if (!isHeld((uint8_t)s_repPos)) stopRepeat();
        else if (now >= s_repNext) { kbType(s_repCode); s_repNext = now + REPEAT_US; }
    }
}

/* ---- mode probe and per-poll dispatch ---------------------------------------
 *
 * Which C3 is this? A raw-capable one (firmware from 2025-06-12) answers a
 * 5-byte read after [0x03] with five column bitmaps, and only rows 0..6 exist,
 * so bit 7 of every byte is 0. An older one ignores [0x03] and answers with one
 * key byte and then garbage. The probe runs inside the poll's own cadence:
 *   1. write [0x03] (retried each poll until the C3 ACKs);
 *   2. read 5 bytes each poll; PROBE_RUN identical frames in a row with bit 7
 *      clear is a raw C3. Identical rather than all-zero, so a key held through
 *      boot does not fail it (its bits are ignored until it is released); the
 *      first read after the switch may still carry the key-mode answer, and a
 *      run tolerates that;
 *   3. PROBE_FRAMES answered reads without such a run is a key-mode C3: write
 *      [0x04] (harmless to one that never left key mode) and stay there;
 *      PROBE_POLLS polls without enough answers is a C3 that is not talking —
 *      key mode too, which is what an absent keyboard costs nothing in.
 * Once in raw mode, RAW_LOST bad frames in a row (the C3 reset back to key
 * mode) re-send [0x03], and RAW_GIVEUP of them drops to key mode. */
constexpr int PROBE_RUN    = 3;
constexpr int PROBE_FRAMES = 20;
constexpr int PROBE_POLLS  = 100;
constexpr int RAW_LOST     = 3;
constexpr int RAW_GIVEUP   = 30;

enum KbMode : uint8_t { KB_PROBING, KB_RAW, KB_KEY };
KbMode  s_kbMode = KB_PROBING;
bool    s_probeSent = false;
int     s_probePolls = 0, s_probeFrames = 0, s_probeRun = 0, s_probeHigh = 0;
uint8_t s_probeLast[KB_COLS] = {};
int     s_rawBad = 0;

bool frameValid(const uint8_t f[KB_COLS]) {
    for (int c = 0; c < KB_COLS; c++) if (f[c] & 0x80) return false;
    return true;
}

bool kbCmd(uint8_t cmd) { return i2c_master_transmit(s_kbd, &cmd, 1, 20) == ESP_OK; }

void enterKeyMode(const char* why) {
    kbCmd(BOARD_KB_CMD_MODE_KEY);
    s_kbMode = KB_KEY;
    lcdKeySetLabel(LCD_KEY_PTT, "Mic");
    lcdKeyDeclare(LCD_KEY_PTT);
    lcdKeyUndeclare(LCD_KEY_SPACE);   /* key mode cannot see Space's up; raw mode may have declared it */
    info("keyboard: key mode, %s; push-to-talk is Shift+Mic, latching\n", why);
    storageSet("tdeck.keyboard", "key");
}

void probeStep() {
    s_probePolls++;
    if (!s_probeSent) {
        s_probeSent = kbCmd(BOARD_KB_CMD_MODE_RAW);
    } else {
        uint8_t f[KB_COLS];
        if (i2c_master_receive(s_kbd, f, sizeof(f), 10) == ESP_OK) {
            s_probeFrames++;
            if (!frameValid(f)) { s_probeHigh++; s_probeRun = 0; }
            else if (s_probeRun && !memcmp(f, s_probeLast, sizeof(f))) s_probeRun++;
            else s_probeRun = 1;
            memcpy(s_probeLast, f, sizeof(f));
            if (s_probeRun >= PROBE_RUN) {
                s_kbMode    = KB_RAW;
                s_kbResync  = true;
                lcdKeySetLabel(LCD_KEY_PTT, "Mic");
                lcdKeyDeclare(LCD_KEY_PTT);
                lcdKeyDeclare(LCD_KEY_SPACE);
                info("keyboard: raw-matrix mode (%d identical frames, bit 7 clear, after %d reads)\n",
                     PROBE_RUN, s_probeFrames);
                storageSet("tdeck.keyboard", "raw");
                rawFrame(f, esp_timer_get_time());
                return;
            }
        }
    }
    if (s_probeFrames >= PROBE_FRAMES) {
        char why[96];
        snprintf(why, sizeof(why), "C3 firmware has no raw mode (%d of %d frames had bit 7 set)",
                 s_probeHigh, s_probeFrames);
        enterKeyMode(why);
    } else if (s_probePolls >= PROBE_POLLS) {
        enterKeyMode("C3 did not answer the probe");
    }
}

void rawPoll() {
    uint8_t f[KB_COLS];
    if (i2c_master_receive(s_kbd, f, sizeof(f), 10) != ESP_OK) return;
    const int64_t now = esp_timer_get_time();
    if (!frameValid(f)) {
        s_rawBad++;
        if (s_rawBad == RAW_GIVEUP) { rawReleaseAll(); enterKeyMode("C3 stopped answering raw frames"); return; }
        if (s_rawBad % RAW_LOST == 0) {
            if (s_rawBad == RAW_LOST) warn("keyboard: bad raw frames; re-entering raw mode\n");
            kbCmd(BOARD_KB_CMD_MODE_RAW);
        }
        return;
    }
    s_rawBad = 0;
    const uint8_t sim = s_simHeld;
    if (sim & (1u << LCD_KEY_PTT))   f[KP_MIC >> 3]   |= (uint8_t)(1u << (KP_MIC & 7));
    if (sim & (1u << LCD_KEY_SPACE)) f[KP_SPACE >> 3] |= (uint8_t)(1u << (KP_SPACE & 7));
    static uint8_t last[KB_COLS] = {};
    if (memcmp(f, last, sizeof(f))) {
        dbg("kbraw: %02x %02x %02x %02x %02x\n", f[0], f[1], f[2], f[3], f[4]);
        memcpy(last, f, sizeof(f));
    }
    rawFrame(f, now);
}

/* Poll task, each awake pass. */
void kbPoll() {
    if (!s_kbd) return;
    /* kbint diagnostic: report a GPIO46 edge if one ever shows up. */
    static uint32_t lastInt = 0;
    uint32_t n = s_intCount;
    if (n != lastInt) { dbg("kbint: GPIO%d fired (total %u)\n", BOARD_KB_INT_PIN, (unsigned)n); lastInt = n; }
    switch (s_kbMode) {
        case KB_PROBING: probeStep(); break;
        case KB_RAW:     rawPoll();   break;
        case KB_KEY:     kmPoll();    break;
    }
}

/* Poll task, on the way into standby: nothing stays held while parked. */
void kbPark() {
    if (s_kbMode == KB_RAW) rawReleaseAll();
    else if (s_kbMode == KB_KEY && s_kmPttLatched) {
        s_kmPttLatched = false;
        namedKeyEdge(LCD_KEY_PTT, false);
    }
}

/* Whether standby keeps scanning: while the talk key is claimed it stays live
 * with the screen off, so one press both wakes the screen and keys. */
bool kbStandbyWatch() {
    return (s_kbMode == KB_RAW || s_kbMode == KB_KEY) && lcdKeyClaimed(LCD_KEY_PTT);
}

/* Poll task, entering the standby watch from awake: held keys stay tracked (a
 * held talk key keeps talking), but nothing repeats or reads as an Alt tap. */
void kbHush() {
    stopRepeat();
    s_altAlone = false;
}

/* lcdKeyOnClaimChange target (the claiming task): standby's scan depends on
 * the talk key's claim, so a parked poll task re-decides. */
void kbClaimChanged(uint8_t key, bool) {
    if (key == LCD_KEY_PTT && s_pollTask) xTaskNotifyGive(s_pollTask);
}

/* LVGL keypad read_cb (lcd task): one queued key code per call, press+release
 * synthesized over two reads; s_again asks kbDrain for another pass. */
void readCb(lv_indev_t*, lv_indev_data_t* data) {
    static uint32_t held = 0;
    if (held) { data->key = held; data->state = LV_INDEV_STATE_RELEASED; held = 0; s_again = true; return; }
    uint32_t k = 0;
    if (s_queue && xQueueReceive(s_queue, &k, 0) == pdTRUE && k) {
        s_again = true;
        /* Count the keystroke as activity (resets the inactivity blank timer).
         * If it woke the screen, swallow it — the key only served to wake. */
        if (lcdNotifyActivity()) { data->state = LV_INDEV_STATE_RELEASED; return; }
        data->key = k; data->state = LV_INDEV_STATE_PRESSED; held = k; return;
    }
    data->state = LV_INDEV_STATE_RELEASED;
}

/* lcd task (via lcdRun): create our keypad indev, joined to lcd's focus group. */
void kbCreateIndev(void*);

/* lcd task (via lcdRun): drain the queue through the indev. */
void kbDrain(void*) {
    /* Lazy create. TdeckLcdInput::onInit() fires lcdRun(kbCreateIndev) right after
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
        /* Standby: parked, the I2C keyboard is not scanned at all (the centre
         * button is the only thing left to wake the device); watching, while the
         * talk key is claimed, it is scanned for that key alone, whose press
         * keys and wakes at once. tdeckStandby unparks us with a notify, and a
         * claim change re-decides. Park on a clean self-check, not a suspend, so
         * we never stop mid-I2C-transaction holding the shared bus. */
        static bool parked = false, watching = false;
        if (s_standby) {
            if (kbStandbyWatch()) {
                if (!watching) { if (!parked) kbHush(); watching = true; parked = false; }
            } else if (!parked) {
                parked = true; watching = false; kbPark();
            }
            ulTaskNotifyTake(pdTRUE, watching ? POLL_PERIOD : portMAX_DELAY);
            /* The screen's fade to dark runs on past the moment we park, and the
             * lamp has to follow it all the way down, so a parked task still
             * pushes. Each step of the fade notifies us and we park again. */
            kbBacklightPush();
            /* Woken while parked by the glass's own edge (the centre button goes
             * to the lcd task): ask the controller whether a finger is really
             * there. */
            if (s_standby && s_touchWakeArmed && s_touchWakeFired) tdeckTouchWakeCheck();
            if (s_standby && watching && kbStandbyWatch()) kbPoll();
            continue;
        }
        /* Woken by a touch/keyboard INT, or times out to poll. Faster cadence
         * while a finger is down so touch tracking stays smooth. */
        ulTaskNotifyTake(pdTRUE, s_touchDown ? TOUCH_PERIOD : POLL_PERIOD);
        kbBacklightPush();                 /* before the standby check: the fade starts here */
        if (s_standby) continue;           /* entered standby during the wait */
        parked = watching = false;
        /* Touch: sample the GT911 and, on a finger-down edge or a tap to replay,
         * bump the lcd task to read its (event-mode) touch indev. */
        if (tdeckTouchSample()) lcdTouchPoll();
        /* Keyboard: probe, raw frame or key-mode drain, by mode. */
        kbPoll();
        /* Bump the lcd task to read our indev. The post is best-effort — a flash
         * flush can stall the lcd task past its send bound — and the bytes are in
         * the queue either way, so an owed drain is carried to the next pass (one
         * POLL_PERIOD away) rather than left for whenever someone presses another
         * key. kbDrain empties the whole queue, so one landing clears the debt. */
        if (s_drainOwed && lcdRun(kbDrain)) s_drainOwed = false;
    }
}

}  // namespace

/* Keyboard lamp, wired from the lcd task's input init (see the note above the
 * state). The subscription has to be made from the task that should receive it
 * and from one that outlives boot, which is why this is here and not in onInit
 * with the rest of the keyboard — the level is pure lcd-task policy, and none of
 * it needs the I2C device to exist yet. The first call is the stored value, not
 * somebody moving the slider, so it lights nothing. */
static void kbBacklightInit(void) {
    NOW_AND_ON_CHANGE("s.tdeck.kb_backlight", {
        (void)key;
        int level = atoi(val);
        s_kbBlLevel = level < 0 ? 0 : (level > 255 ? 255 : level);
        s_kbAltWant = s_kbBlLevel;              /* Alt+B lights to the same level */
        if (s_kbBlSeeded) kbBacklightPreview();
        s_kbBlSeeded = true;
        kbBacklightApply();
    });
    lcdBacklightOnChange(kbBacklightFollow);    /* calls back at once with the current duty */
}

/* The lamp's on/off latch (lcd task). Only the held wake press sets it; the
 * screen going dark clears it. */
static void kbBacklightSetLit(bool on) {
    if (s_kbLit == on) return;
    s_kbLit = on;
    kbBacklightApply();
}

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
    s_touchWakeFired = true;
    tdeckTouchIsr(arg);
}

/* Swap the GT911 INT between its awake wiring (ANYEDGE → the poll task) and the
 * standby wake source. Lcd task, from tdeckStandby. */
static void tdeckTouchWakeArm(bool on) {
    if (on == s_touchWakeArmed || !s_touch) return;
    gpio_isr_handler_remove((gpio_num_t)BOARD_TOUCH_INT_PIN);
    gpio_isr_handler_add((gpio_num_t)BOARD_TOUCH_INT_PIN,
                         on ? tdeckTouchWakeIsr : tdeckTouchIsr, nullptr);
    s_touchWakeFired = false;                            /* fresh arm: drop any stale latch */
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
    s_touchWakeFired = false;
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
 * centre button is left live, and a press of it clears the key to wake us; a
 * claimed talk key stays live too (pollTask), and its press wakes us the same way. */
static void tdeckStandby(bool on) {
    if (on == s_standby) return;
    s_standby = on;
    if (on) {
        s_touchAsleep = true;                          /* GT911 reads return nothing */
        cancelClickBurst();                            /* no burst survives into sleep */
        cancelKbHoldTimer();                           /* nor a hold waiting to light the keys */
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

/* `key`: the named held keys' state, and `key hold mic|space 1|0`, which holds
 * or lifts one as the poll task reads it — the same scan, debounce, standby
 * watch and report a finger on the key gets. */
static void cliKey(const char* args) {
    if (cliWantsHelp(args)) {
        cliPrintf("%-*s held keys; hold mic|space 1|0 presses or lifts one\n", CLI_HELP_COL, "key [hold <k> 1|0]");
        return;
    }
    char verb[8] = {}, name[8] = {};
    int  down = -1;
    if (sscanf(args, "%7s %7s %d", verb, name, &down) >= 1) {
        uint8_t k;
        if      (!strcmp(name, "mic") || !strcmp(name, "ptt")) k = LCD_KEY_PTT;
        else if (!strcmp(name, "space"))                        k = LCD_KEY_SPACE;
        else k = LCD_KEY_COUNT;
        if (!cliVerbIs(verb, "hold", 1) || k == LCD_KEY_COUNT || (down != 0 && down != 1)) {
            cliPrintf("usage: key hold mic|space 1|0\n");
            return;
        }
        if (down) s_simHeld = (uint8_t)(s_simHeld | (1u << k));
        else      s_simHeld = (uint8_t)(s_simHeld & ~(1u << k));
        if (s_pollTask) xTaskNotifyGive(s_pollTask);
        cliPrintf("%s %s\n", name, down ? "held" : "lifted");
        return;
    }
    static const char* const kModes[] = { "probing", "raw", "key" };
    cliPrintf("keyboard %s, %s\n", kModes[s_kbMode], s_standby ? "standby" : "awake");
    for (uint8_t k = 0; k < LCD_KEY_COUNT; k++)
        cliPrintf("  %-5s %s%s%s%s\n", namedKeyName(k),
                  lcdKeyDeclared(k) ? "declared" : "undeclared",
                  lcdKeyClaimed(k) ? ", claimed" : "",
                  lcdKeyDown(k) ? ", down" : "",
                  (s_simHeld & (1u << k)) ? ", held from cli" : "");
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

    /* GPIO46 keyboard INT (ANYEDGE + pull-up). Dead on current C3 firmware; if
     * it fires, kbIntIsr only wakes the poll task early. The gpio ISR
     * service is already installed by the board's lcd bring-up. */
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOARD_KB_INT_PIN;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_ANYEDGE;
    gpio_config(&io);
    gpio_isr_handler_add((gpio_num_t)BOARD_KB_INT_PIN, kbIntIsr, nullptr);

    s_queue = xQueueCreate(32, sizeof(uint32_t));
    lcdRun(kbCreateIndev);                  /* create the indev on the lcd task */
    /* Prio 6: one notch above the lcd task (prio 5) so the render can't starve
     * the poll on core 1 — the C3 holds only the last unread key, so a stalled
     * poll drops keystrokes, and a late INT-driven read comes back stale/garbled
     * (phantom keys). This task owns ALL of I2C0 (keyboard + touch), off the
     * lcd task, so nothing else contends for the bus — and outranking the
     * render is what keeps touch sampling on cadence while the lcd task is busy
     * (the whole point of moving the read here). The scans are a few short I2C
     * transactions at 10–30 ms, so preempting the render costs it nothing. */
    xTaskCreatePinnedToCore(pollTask, "kbpoll", 4096, nullptr, 6, &s_pollTask, 1);
    lcdKeyOnClaimChange(kbClaimChanged);    /* standby's talk-key watch follows the claim */
    cliRegisterCmd("key", cliKey);

    lcdSetHasKeyboard(true);                /* lcd: suppress the on-screen keyboard */
}
