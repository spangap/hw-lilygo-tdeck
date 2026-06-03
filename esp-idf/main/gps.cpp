/**
 * gps — GNSS receiver task.
 *
 * The T-Deck Plus carries one of two receivers depending on production batch
 * (docs/tdeck.md §1.3), with nothing host-visible to tell them apart:
 *   - Quectel L76K       default 9600 baud
 *   - u-blox MIA-M10Q    default 38400 baud
 * Both emit standard NMEA 8N1 on the Grove-header UART. We only RX. On enable
 * the task autobauds (tries 38400 then 9600, locking on the first checksum-valid
 * NMEA sentence) and infers the model from the baud that worked.
 *
 * There is no independent GPS power switch on this board — the receiver hangs
 * off the shared BOARD_POWER_EN_PIN rail with the display/SD/LoRa, already
 * driven HIGH by tdeckPreInit(). We can't cut its power, so on disable we put
 * the chip into standby over the UART instead, then tear the UART down:
 *   - u-blox M10: UBX-RXM-PMREQ software backup — real low power, wakes on a
 *     UART RX edge (so on re-enable a dummy byte revives it; see gpsAutobaud).
 *   - L76K: the documented PCAS set has no UART-wakeable standby (its low-power
 *     is a FORCE_ON pin, not routed on the Plus). We send $PMTK225,4 deep backup
 *     anyway -- real power saving, but the only way back is a power cycle. So a
 *     backed-up L76K can't be revived over serial: on re-enable we publish a
 *     "power-cycle to wake" status (s_needsPowerCycle) instead of trying.
 * On (re-)enable gpsAutobaud sends a wake edge before listening (revives a
 * u-blox; a power-cycled chip is simply fresh).
 *
 * Loop: itsPoll is the single wait point (config changes + a ≤1 s backstop).
 * The receiver free-runs at 1 Hz with no interrupt line wired (no PPS on the
 * Plus), so a 1 Hz drain cadence *is* the event cadence — on each wake we drain
 * the UART non-blocking, fold every sentence into a working fix, and once
 * s.gps.interval seconds have elapsed publish the whole snapshot to gps.*.
 *
 * Time discipline (unless s.gps.ignore_clock is set — then the clock is left
 * entirely to ntp / manual set):
 *   - GPS: while the system clock is invalid (pre-2025) we accept *any* valid
 *     GPS date+time — no satellites/position required (the receiver streams time
 *     before it locks). Once valid we only re-discipline from a real positioned
 *     fix. Either way we parse the NMEA sub-second field and subtract a static
 *     per-model pipeline lag (no PPS on the Plus to measure it), write the
 *     corrected time to the system clock, and publish sys.time.valid. Receiving
 *     GPS time inhibits ntp; we hand control back to ntp if the clock goes
 *     invalid or no valid GPS time is seen for a while — 3 days with an RTC to
 *     hold time, but only 1 hour without one (the RC-oscillator clock drifts).
 *   - RTC (PCF8563): the *stock* T-Deck has none — its I2C0 bus is keyboard +
 *     touch only — so this is all auto-detected and dormant by default. If a
 *     PCF8563 is found at 0x51 (external on the Grove I2C, or a board rev that
 *     adds one) we adopt its time at boot, mirror every GPS set to it, and once
 *     a minute with no GPS coverage re-sync the system clock from it (logging
 *     the drift at debug) so the wall clock survives long fixless runs.
 *
 * Config:    s.gps.enable (0/1), s.gps.interval (seconds), s.gps.ignore_clock (0/1)
 * Ephemeral: gps.model gps.baud gps.state gps.fix gps.quality gps.lat gps.lon
 *            gps.alt gps.geoid gps.speed gps.course gps.sats_used gps.sats_view
 *            gps.hdop gps.vdop gps.pdop gps.snr gps.utc gps.fix_age
 */
#include "gps.h"
#include "tdeck.h"
#include "rtc.h"
#include "spangap.h"

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <time.h>
#include <sys/time.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

static const char* TAG = "gps";

/* tdeck.h keeps BOARD_GPS_UART_NUM a bare int (no driver/uart.h dependency);
 * the IDF UART API wants the enum, so type it here. */
static constexpr uart_port_t GPS_UART = static_cast<uart_port_t>(BOARD_GPS_UART_NUM);

#define GPS_VERSION 1

/* ─────────────── clock-discipline tunables ─────────────── */

static constexpr time_t  kValidEpoch  = 1735689600;          /* 2025-01-01 00:00 UTC: below = bogus */
static constexpr int64_t kStepThreshUs = 250000;             /* don't re-step the system clock for < this (serial jitter floor) */
static constexpr int64_t kHeartbeatUs  = 60ll  * 1000000;    /* RTC sync / ntp-staleness cadence */
static constexpr int64_t kCoverageUs   = 90ll  * 1000000;    /* "GPS time seen recently" window */
/* No GPS time for this long → hand the clock back to ntp. With an RTC the clock
 * is held on its crystal, so tolerate long GPS gaps; without one the clock rides
 * the ESP32 RC oscillator (huge drift, esp. across sleep), so reclaim it fast. */
static constexpr int64_t kNtpStaleRtcUs   = 3ll * 24 * 3600 * 1000000;  /* 3 days */
static constexpr int64_t kNtpStaleNoRtcUs = 3600ll * 1000000;           /* 1 hour */

/* Static best-guess for the lag from the GPS fix instant to the moment the
 * time-bearing NMEA sentence is fully in our hands: receiver solution + serial
 * drain + buffer dwell. No PPS on the T-Deck Plus, so this can't be measured —
 * these are per-model estimates (richer/faster u-blox vs slower 9600 L76K).
 * gpsDisciplineFromGps() adds the measured time-since-parse on top. */
static int64_t gpsPipelineLagUs(int baud) {
    if (baud == 38400) return 70000;    /* u-blox MIA-M10Q @ 38400 ≈ 70 ms */
    if (baud == 9600)  return 260000;   /* Quectel L76K     @ 9600  ≈ 260 ms */
    return 150000;
}

/* Baud candidates, richest receiver first; the first that yields a valid NMEA
 * sentence wins and identifies the chip (docs/tdeck.md §1.3). */
static constexpr int  kBauds[]      = { 38400, 9600 };
static constexpr int  kAutobaudMs   = 1500;   /* per-candidate listen window */
static constexpr int  kUartRxBuf    = 2048;   /* > 1 s of NMEA at 38400 */

/* ─────────────── working fix (folded from many sentences) ─────────────── */

struct GpsFix {
    bool    valid      = false;  /* RMC status == 'A' */
    int     quality    = 0;      /* GGA fix-quality indicator */
    int     fixType    = 1;      /* GSA: 1 none / 2 2D / 3 3D */
    bool    hasPos     = false;
    double  lat = 0, lon = 0;
    double  altMsl     = 0;      /* GGA altitude, m above MSL */
    double  geoid      = 0;      /* GGA geoid separation, m */
    double  speedKmh   = 0;      /* RMC/VTG */
    double  courseDeg  = 0;      /* RMC/VTG track made good */
    int     satsUsed   = 0;      /* GGA */
    int     satsInView = 0;      /* GSV */
    double  hdop = 0, vdop = 0, pdop = 0;
    int     snrMax     = 0;      /* best C/N0 this epoch (reset in drainUart) */
    int     yr = 0, mo = 0, dy = 0, hh = 0, mi = 0, se = 0;
    int     ms = 0;              /* RMC sub-second field (0-999), 0 if absent */
    bool    hasTime = false, hasDate = false;
    int64_t lastFixUs  = 0;      /* esp_timer at last valid (positioned) fix, 0 = never */
    int64_t timeRxUs   = 0;      /* esp_timer when the time field was parsed (for lag accounting) */
};

/* ─────────────── globals (single-task ownership) ─────────────── */

static TaskHandle_t  s_task     = nullptr;
static volatile bool s_cfgDirty = true;

static bool          s_enabled  = false;
static bool          s_running  = false;   /* UART installed + listening */
static int           s_baud     = 0;       /* last detected baud (kept across disable) */
static int           s_interval = 1;       /* s.gps.interval, seconds */
static bool          s_needsPowerCycle = false;  /* L76K put in FORCE-pin-only backup */

static GpsFix        s_fix;
static std::string   s_line;               /* incremental NMEA line assembly */
static int64_t       s_lastPublishUs = 0;

/* clock discipline (all touched only on the gps task) */
static bool          s_ignoreClock = false;     /* s.gps.ignore_clock: leave the clock to ntp/manual */
static int           s_rtcPresent  = -1;        /* -1 unknown, 0 absent, 1 present (probed once at boot) */
static int64_t       s_lastGpsDisciplineUs = 0; /* esp_timer when GPS last drove the clock, 0 = never */
static int64_t       s_lastHeartbeatUs = 0;     /* esp_timer of last gpsHeartbeat() */
static bool          s_ownsClock = false;       /* mirror of our last sys.time.ext write (avoid redundant writes) */

/* ─────────────── NMEA helpers ─────────────── */

/* "$....*HH" with HH = XOR of every byte between '$' and '*'. */
static bool nmeaChecksumOk(std::string_view s) {
    if (s.size() < 4 || s.front() != '$') return false;
    size_t star = s.rfind('*');
    if (star == std::string_view::npos || star + 3 > s.size()) return false;
    uint8_t sum = 0;
    for (size_t i = 1; i < star; i++) sum ^= (uint8_t)s[i];
    uint8_t want = (uint8_t)strtol(std::string(s.substr(star + 1, 2)).c_str(), nullptr, 16);
    return sum == want;
}

/* Split the body (between '$' and '*') into comma fields. */
static std::vector<std::string_view> nmeaFields(std::string_view s) {
    std::vector<std::string_view> f;
    size_t star = s.rfind('*');
    std::string_view body = s.substr(1, (star == std::string_view::npos ? s.size() : star) - 1);
    size_t start = 0;
    for (;;) {
        size_t c = body.find(',', start);
        f.push_back(body.substr(start, c == std::string_view::npos ? std::string_view::npos : c - start));
        if (c == std::string_view::npos) break;
        start = c + 1;
    }
    return f;
}

static double toD(std::string_view v) {
    return v.empty() ? 0.0 : strtod(std::string(v).c_str(), nullptr);
}
static int toI(std::string_view v) {
    return v.empty() ? 0 : (int)strtol(std::string(v).c_str(), nullptr, 10);
}

/* ddmm.mmmm + hemisphere -> signed decimal degrees. */
static double toDegrees(std::string_view v, std::string_view hemi) {
    if (v.empty()) return 0.0;
    double raw = toD(v);
    double deg = std::floor(raw / 100.0);
    double min = raw - deg * 100.0;
    double dd  = deg + min / 60.0;
    if (!hemi.empty() && (hemi[0] == 'S' || hemi[0] == 'W')) dd = -dd;
    return dd;
}

/* The sentence type is the last 3 chars of the talker+type token (GGA/RMC/...),
 * so we match on those regardless of the GP/GN/GL/GA/GB talker prefix. */
static bool typeIs(std::string_view tok, const char* type3) {
    return tok.size() >= 3 && tok.compare(tok.size() - 3, 3, type3) == 0;
}

/* Fold one checksum-valid sentence into the working fix. Returns true if it was
 * a sentence type we understand (used by autobaud to confirm a live receiver). */
static bool nmeaApply(std::string_view line, GpsFix& fix) {
    auto f = nmeaFields(line);
    if (f.empty()) return false;
    std::string_view t = f[0];

    if (typeIs(t, "RMC") && f.size() >= 10) {
        fix.valid = (!f[2].empty() && f[2][0] == 'A');
        // hhmmss(.sss)
        if (f[1].size() >= 6) {
            fix.hh = toI(f[1].substr(0, 2)); fix.mi = toI(f[1].substr(2, 2));
            fix.se = toI(f[1].substr(4, 2));
            /* sub-second fraction → ms: ".5"=500, ".05"=50, ".250"=250 */
            fix.ms = 0;
            if (f[1].size() > 7 && f[1][6] == '.') {
                std::string_view frac = f[1].substr(7);
                int scale = 100;
                for (size_t i = 0; i < frac.size() && scale > 0; i++) {
                    if (frac[i] < '0' || frac[i] > '9') break;
                    fix.ms += (frac[i] - '0') * scale; scale /= 10;
                }
            }
            fix.hasTime  = true;
            fix.timeRxUs = esp_timer_get_time();   /* anchor for lag accounting */
        }
        if (fix.valid) {
            fix.lat = toDegrees(f[3], f[4]);
            fix.lon = toDegrees(f[5], f[6]);
            fix.hasPos = true;
            fix.speedKmh  = toD(f[7]) * 1.852;   /* knots -> km/h */
            fix.courseDeg = toD(f[8]);
            fix.lastFixUs = esp_timer_get_time();
        }
        if (f[9].size() >= 6) {              // ddmmyy
            fix.dy = toI(f[9].substr(0, 2)); fix.mo = toI(f[9].substr(2, 2));
            fix.yr = 2000 + toI(f[9].substr(4, 2)); fix.hasDate = true;
        }
        return true;
    }
    if (typeIs(t, "GGA") && f.size() >= 12) {
        fix.quality  = toI(f[6]);
        fix.satsUsed = toI(f[7]);
        fix.hdop     = toD(f[8]);
        if (!f[2].empty()) { fix.lat = toDegrees(f[2], f[3]);
                             fix.lon = toDegrees(f[4], f[5]); fix.hasPos = true; }
        fix.altMsl = toD(f[9]);
        fix.geoid  = toD(f[11]);
        return true;
    }
    if (typeIs(t, "GSA") && f.size() >= 18) {
        fix.fixType = toI(f[2]);             /* 1 none / 2 2D / 3 3D */
        fix.pdop    = toD(f[15]);
        fix.hdop    = toD(f[16]);
        fix.vdop    = toD(f[17]);
        return true;
    }
    if (typeIs(t, "GSV") && f.size() >= 4) {
        /* GSV is per-constellation (GP/GL/GA/GB...), each talker sending N
         * messages that all repeat its in-view count. Count it once — on this
         * talker's first message — and sum across talkers for a true total. The
         * accumulator is zeroed per epoch in drainUart(). */
        if (toI(f[2]) == 1) fix.satsInView += toI(f[3]);
        // per-sat groups of 4: prn, elev, azim, snr
        for (size_t i = 7; i < f.size(); i += 4) {
            int snr = toI(f[i]);
            if (snr > fix.snrMax) fix.snrMax = snr;
        }
        return true;
    }
    if (typeIs(t, "VTG") && f.size() >= 8) {
        if (!f[1].empty()) fix.courseDeg = toD(f[1]);
        if (!f[7].empty()) fix.speedKmh  = toD(f[7]);   /* already km/h */
        return true;
    }
    return false;
}

/* ─────────────── UART ─────────────── */

static bool gpsUartInstall(int baud) {
    uart_config_t cfg = {};
    cfg.baud_rate  = baud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(GPS_UART, kUartRxBuf, 0, 0, nullptr, 0) != ESP_OK)
        return false;
    uart_param_config(GPS_UART, &cfg);
    uart_set_pin(GPS_UART, BOARD_GPS_TX_PIN, BOARD_GPS_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    return true;
}

static void gpsUartUninstall(void) {
    if (uart_is_driver_installed(GPS_UART))
        uart_driver_delete(GPS_UART);
}

/* ─────────────── standby / wake ─────────────── */

/* UBX-RXM-PMREQ v0 payload: infinite duration, flags = backup|force, wakeup
 * source = uartrx — u-blox M10 software backup that revives on a UART RX edge. */
static const uint8_t kUbxPmreqBackup[16] = {
    0x00,                   /* version 0 */
    0x00, 0x00, 0x00,       /* reserved */
    0x00, 0x00, 0x00, 0x00, /* duration 0 = until wakeup */
    0x06, 0x00, 0x00, 0x00, /* flags: backup (0x02) | force (0x04) */
    0x08, 0x00, 0x00, 0x00, /* wakeupSources: uartrx (0x08) */
};

/* Frame + Fletcher-checksum a UBX message and write it. */
static void ubxSend(uint8_t cls, uint8_t id, const uint8_t* payload, size_t len) {
    uint8_t hdr[6] = { 0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < 6; i++) { a += hdr[i]; b += a; }
    for (size_t i = 0; i < len; i++) { a += payload[i]; b += a; }
    uint8_t ck[2] = { a, b };
    uart_write_bytes(GPS_UART, hdr, sizeof(hdr));
    if (len) uart_write_bytes(GPS_UART, payload, len);
    uart_write_bytes(GPS_UART, ck, sizeof(ck));
}

/* Frame "$<body>*<XOR-checksum>\r\n" (e.g. body "PMTK225,4") and write it. */
static void nmeaSend(const char* body) {
    uint8_t ck = 0;
    for (const char* p = body; *p; p++) ck ^= (uint8_t)*p;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "$%s*%02X\r\n", body, ck);
    if (n > 0) uart_write_bytes(GPS_UART, buf, n);
}

/* A UART RX edge wakes a u-blox out of software backup; harmless noise to a
 * fresh receiver. Sent at the top of each autobaud candidate. */
static void gpsWake(void) {
    const uint8_t wake[2] = { 0xFF, 0xFF };
    uart_write_bytes(GPS_UART, wake, sizeof(wake));
}

/* Put the detected receiver into its deepest reachable low-power state, then let
 * the caller drop the UART. */
static void gpsStandby(void) {
    if (s_baud == 38400) {            /* u-blox M10 — UART-wakeable software backup */
        ubxSend(0x02, 0x41, kUbxPmreqBackup, sizeof(kUbxPmreqBackup));
        info("u-blox software backup (wakes on UART)");
    } else if (s_baud == 9600) {      /* L76K — deep backup, FORCE-pin-only wake */
        nmeaSend("PMTK225,4");
        s_needsPowerCycle = true;     /* no FORCE pin here → power cycle to revive */
        info("L76K deep backup (power-cycle to wake)");
    }
    uart_wait_tx_done(GPS_UART, pdMS_TO_TICKS(100));   /* flush before drop */
}

/* Try each candidate baud; lock on the first that produces a checksum-valid,
 * recognized NMEA sentence. Returns the locked baud or 0. */
static int gpsAutobaud(void) {
    for (int baud : kBauds) {
        gpsUartUninstall();
        if (!gpsUartInstall(baud)) continue;
        gpsWake();   /* revive a u-blox in software backup; the listen window
                      * below covers its ~0.5-1 s restart */

        std::string line;
        uint8_t buf[256];
        int64_t deadline = esp_timer_get_time() + (int64_t)kAutobaudMs * 1000;
        GpsFix probe;
        while (esp_timer_get_time() < deadline) {
            int n = uart_read_bytes(GPS_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
            for (int i = 0; i < n; i++) {
                char c = (char)buf[i];
                if (c == '\n') {
                    while (!line.empty() && (line.back() == '\r')) line.pop_back();
                    if (nmeaChecksumOk(line) && nmeaApply(line, probe)) {
                        info("detected NMEA @ %d baud", baud);
                        return baud;
                    }
                    line.clear();
                } else if (c == '$') {
                    line.assign(1, c);     /* resync on every sentence start */
                } else if (!line.empty() && line.size() < 120) {
                    line.push_back(c);
                }
            }
        }
    }
    gpsUartUninstall();
    return 0;
}

/* ─────────────── publish ─────────────── */

static const char* modelForBaud(int baud) {
    if (baud == 38400) return "u-blox MIA-M10Q";
    if (baud == 9600)  return "Quectel L76K";
    return "unknown";
}

static void setF(const char* key, double v, int prec) {
    char b[32];
    snprintf(b, sizeof(b), "%.*f", prec, v);
    storageSet(key, b);
}

/* tm (UTC) -> Unix epoch. newlib ships no timegm, and mktime honours the
 * device's configured TZ — so convert directly (days-from-civil, proleptic
 * Gregorian; Howard Hinnant's algorithm). */
static time_t utcToEpoch(const struct tm& t) {
    int      y = t.tm_year + 1900;
    unsigned m = t.tm_mon + 1;
    unsigned d = t.tm_mday;
    y -= m <= 2;
    int64_t  era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t  days = era * 146097 + (int64_t)doe - 719468;
    return (time_t)(days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec);
}

/* Log a clock correction at a level scaled to its magnitude (the brief's tiers):
 *   < 100 ms verbose · 100 ms–5 s debug · 5–60 s info · > 60 s warn. */
static void logTimeUpdate(const char* src, int64_t deltaUs) {
    int64_t a  = deltaUs < 0 ? -deltaUs : deltaUs;
    long    ms = (long)(deltaUs / 1000);
    double  s  = deltaUs / 1e6;
    if      (a < 100000)   verb("%s time update %+ld ms",  src, ms);
    else if (a < 5000000)  dbg ("%s time update %+ld ms",  src, ms);
    else if (a < 60000000) info("%s time update %+.1f s",  src, s);
    else                   warn("%s time update %+.1f s",  src, s);
}

/* Claim the system clock, or hand it back. We own it only while it is valid
 * AND we have seen GPS time within the staleness window; otherwise — clock
 * invalid, gone stale, or the user pinned us off — we release it. The claim is
 * published on the storage bus as sys.time.ext (1 = a local authority owns the
 * clock); ntp subscribes and parks SNTP while it's set. Going through storage
 * keeps GPS free of any compile-time dependency on net — with no net staged
 * there's simply no subscriber, and GPS owns the clock outright. Mirrors the
 * last decision so we only write on a change. */
static void clockOwnershipReconcile(void) {
    bool desired;
    if (s_ignoreClock) {
        desired = false;
    } else {
        bool    valid = time(nullptr) >= kValidEpoch;
        int64_t age   = esp_timer_get_time() - s_lastGpsDisciplineUs;
        /* Treat unknown (-1, e.g. clock pinned off) as no-RTC: the cautious 1 h. */
        int64_t stale = (s_rtcPresent == 1) ? kNtpStaleRtcUs : kNtpStaleNoRtcUs;
        bool    fresh = s_lastGpsDisciplineUs != 0 && age <= stale;
        desired = valid && fresh;
    }
    if (desired != s_ownsClock) {
        storageSet("sys.time.ext", desired ? 1 : 0);
        s_ownsClock = desired;
        dbg("clock %s", desired ? "claimed (GPS time)" : "released");
    }
}

/* Discipline the system clock (and mirror to the RTC) from the current fix's
 * UTC date+time. Caller has already decided the fix qualifies. */
static void gpsDisciplineFromGps(void) {
    struct tm tmv = {};
    tmv.tm_year = s_fix.yr - 1900;
    tmv.tm_mon  = s_fix.mo - 1;
    tmv.tm_mday = s_fix.dy;
    tmv.tm_hour = s_fix.hh;
    tmv.tm_min  = s_fix.mi;
    tmv.tm_sec  = s_fix.se;
    time_t epoch = utcToEpoch(tmv);              /* fix time is UTC */
    if (epoch < kValidEpoch) return;             /* before 2025-01-01 = bogus, ignore */

    /* fix instant + receiver's own sub-second + static pipeline lag + however
     * long since we parsed the sentence = true UTC now. */
    int64_t gpsUs = (int64_t)epoch * 1000000 + (int64_t)s_fix.ms * 1000
                  + gpsPipelineLagUs(s_baud)
                  + (esp_timer_get_time() - s_fix.timeRxUs);

    struct timeval nowTv; gettimeofday(&nowTv, nullptr);
    int64_t nowUs = (int64_t)nowTv.tv_sec * 1000000 + nowTv.tv_usec;
    int64_t delta = gpsUs - nowUs;               /* +ve: system is behind GPS */
    bool    sysInvalid = nowUs < (int64_t)kValidEpoch * 1000000;

    logTimeUpdate("GPS", delta);

    /* Step only when worthwhile — settimeofday is a jump, and with no PPS the
     * sub-250 ms residual is serial jitter not worth chasing. Always step a
     * still-invalid clock. */
    if (sysInvalid || (delta < 0 ? -delta : delta) >= kStepThreshUs) {
        struct timeval tv = { .tv_sec  = (time_t)(gpsUs / 1000000),
                              .tv_usec = (suseconds_t)(gpsUs % 1000000) };
        settimeofday(&tv, nullptr);
        /* Mirror to the RTC (if one is actually present) so the clock can survive
         * a power cycle / GPS outage. Stock T-Deck has none — skip silently. */
        if (s_rtcPresent == 1) {
            struct tm utc; time_t s = tv.tv_sec; gmtime_r(&s, &utc);
            if (rtcWrite(&utc)) dbg("clock mirrored to RTC");
            else                warn("RTC write failed (no PCF8563 ACK at 0x51?)");
        }
    }
    storageSet("sys.time.valid", 1);

    s_lastGpsDisciplineUs = esp_timer_get_time();
    clockOwnershipReconcile();
}

/* Boot: adopt the RTC's time if it's trustworthy and the system clock is unset,
 * so the wall clock is live before the first fix. Whole-second only. */
static void rtcBootSync(void) {
    /* Probe once and remember. The stock T-Deck (incl. Plus) has NO RTC — its
     * I2C0 bus carries only the keyboard (0x55) and touch (0x5D). So absence is
     * the normal case: note it once at info and then never touch I2C for the RTC
     * again (no per-minute probes, no per-fix write attempts). The driver stays
     * wired up so an external PCF8563 on the Grove I2C header — or a board rev
     * that adds one — is picked up here and used automatically. */
    s_rtcPresent = rtcProbe() ? 1 : 0;
    if (!s_rtcPresent) {
        info("no PCF8563 at 0x51 — T-Deck has no RTC; GPS/NTP only");
        return;
    }
    info("PCF8563 present at 0x51");
    struct tm t; bool clockValid = false;
    if (!rtcRead(&t, &clockValid)) { warn("RTC present but read failed"); return; }
    time_t epoch = utcToEpoch(t);
    if (!clockValid) {
        warn("RTC present but VL set — lost power since last write (no backup cell?)");
        return;
    }
    if (epoch < kValidEpoch) {
        info("RTC time pre-2025 (%04d) — ignoring", t.tm_year + 1900);
        return;
    }
    if (time(nullptr) >= kValidEpoch) return;    /* something already set the clock */
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    storageSet("sys.time.valid", 1);
    info("clock set from RTC at boot: %04d-%02d-%02d %02d:%02d:%02d UTC",
         t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

/* Once a minute: with no recent GPS time, keep the system clock pinned to the
 * RTC (the brief's fallback keeper) and log the drift between them at debug.
 * Also reconciles ntp ownership as the GPS-staleness window expires. */
static void gpsHeartbeat(void) {
    if (s_ignoreClock) { clockOwnershipReconcile(); return; }

    int64_t now = esp_timer_get_time();
    bool gpsCoverage = s_lastGpsDisciplineUs != 0 &&
                       (now - s_lastGpsDisciplineUs) <= kCoverageUs;
    /* RTC keeper only makes sense when a chip is actually present (s_rtcPresent
     * is set by rtcBootSync before the first heartbeat). Stock T-Deck: no RTC,
     * so the fallback keeper is ntp via the staleness reconcile below. */
    if (!gpsCoverage && s_rtcPresent == 1) {
        struct tm t; bool clockValid = false;
        if (rtcRead(&t, &clockValid) && clockValid) {
            time_t epoch = utcToEpoch(t);
            if (epoch >= kValidEpoch) {
                struct timeval nowTv; gettimeofday(&nowTv, nullptr);
                int64_t nowUs = (int64_t)nowTv.tv_sec * 1000000 + nowTv.tv_usec;
                int64_t delta = (int64_t)epoch * 1000000 - nowUs;
                dbg("RTC sync: system %+ld ms vs RTC", (long)(delta / 1000));
                /* RTC is whole-second; only step for ≥1 s drift (or an unset
                 * clock) so sub-second quantization isn't mistaken for drift. */
                if (nowUs < (int64_t)kValidEpoch * 1000000 ||
                    (delta < 0 ? -delta : delta) >= 1000000) {
                    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
                    settimeofday(&tv, nullptr);
                    storageSet("sys.time.valid", 1);
                }
            }
        }
    }
    clockOwnershipReconcile();
}

static void publishFix(void) {
    const char* fixStr = (s_fix.fixType >= 3) ? "3D"
                       : (s_fix.fixType == 2) ? "2D" : "none";
    bool haveFix = s_fix.valid && s_fix.hasPos && s_fix.fixType >= 2;

    storageBegin();
    storageSet("gps.state", haveFix ? "fix" : "acquiring");
    storageSet("gps.fix",   fixStr);
    storageSet("gps.quality", s_fix.quality);

    if (s_fix.hasPos) {
        setF("gps.lat", s_fix.lat, 6);
        setF("gps.lon", s_fix.lon, 6);
    } else {
        storageSet("gps.lat", "");
        storageSet("gps.lon", "");
    }
    setF("gps.alt",    s_fix.altMsl,   1);
    setF("gps.geoid",  s_fix.geoid,    1);
    setF("gps.speed",  s_fix.speedKmh, 1);
    setF("gps.course", s_fix.courseDeg, 1);
    storageSet("gps.sats_used", s_fix.satsUsed);
    storageSet("gps.sats_view", s_fix.satsInView);
    setF("gps.hdop", s_fix.hdop, 2);
    setF("gps.vdop", s_fix.vdop, 2);
    setF("gps.pdop", s_fix.pdop, 2);
    storageSet("gps.snr", s_fix.snrMax);

    if (s_fix.hasDate && s_fix.hasTime) {
        char ts[24];
        snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                 s_fix.yr, s_fix.mo, s_fix.dy, s_fix.hh, s_fix.mi, s_fix.se);
        storageSet("gps.utc", ts);
    } else {
        storageSet("gps.utc", "");
    }

    if (s_fix.lastFixUs) {
        int ageS = (int)((esp_timer_get_time() - s_fix.lastFixUs) / 1000000);
        storageSet("gps.fix_age", ageS);
    } else {
        storageSet("gps.fix_age", -1);
    }
    storageEnd();

    /* Discipline the system clock from GPS, unless pinned off. While the clock
     * is invalid we take any valid date+time (no sats/position needed — the
     * receiver streams time before it locks); once valid we trust only a real
     * positioned fix. The step threshold in gpsDisciplineFromGps() keeps this
     * from churning when re-run every publish. */
    if (!s_ignoreClock && s_fix.hasDate && s_fix.hasTime) {
        bool sysInvalid = time(nullptr) < kValidEpoch;
        if (sysInvalid || haveFix)
            gpsDisciplineFromGps();
    }
}

static void publishModel(const char* state, const char* model, int baud) {
    storageBegin();
    storageSet("gps.state", state);
    storageSet("gps.model", model);
    storageSet("gps.baud",  baud);
    storageEnd();
}

/* ─────────────── config ─────────────── */

static void applyConfig(void) {
    s_enabled     = storageGetInt("s.gps.enable", 0) != 0;
    s_interval    = storageGetInt("s.gps.interval", 1);
    if (s_interval < 1) s_interval = 1;
    s_ignoreClock = storageGetInt("s.gps.ignore_clock", 0) != 0;

    if (!s_enabled) {
        if (s_running) {
            gpsStandby();              /* backup the chip before dropping the line */
            gpsUartUninstall();
            s_running = false;
            publishModel("standby", modelForBaud(s_baud), s_baud);
            info("disabled (standby)");
        } else {
            publishModel("off", s_baud ? modelForBaud(s_baud) : "-", s_baud);
        }
        return;
    }
    if (s_running) return;   /* interval change only — nothing to re-open */

    if (s_needsPowerCycle) {
        /* An L76K we put into FORCE-pin-only backup can't be revived over the
         * UART on this board — surface the message the user asked for. */
        warn("GPS in deep backup — power-cycle the device to use it again");
        publishModel("power-cycle to wake", modelForBaud(s_baud), s_baud);
        return;
    }

    publishModel("detecting", "detecting...", 0);
    s_baud = gpsAutobaud();
    if (s_baud == 0) {
        warn("no NMEA on UART (RX=%d) at any baud", BOARD_GPS_RX_PIN);
        publishModel("not detected", "not detected", 0);
        return;
    }
    s_running = true;
    s_fix = GpsFix{};
    s_line.clear();
    s_lastPublishUs = 0;
    publishModel("acquiring", modelForBaud(s_baud), s_baud);
    info("up: %s @ %d baud", modelForBaud(s_baud), s_baud);
}

static void onCfgChange(const char* /*key*/, const char* /*val*/) {
    s_cfgDirty = true;
    if (s_task) xTaskNotifyGive(s_task);
}

/* ─────────────── drain ─────────────── */

static void drainUart(void) {
    uint8_t buf[256];
    int n = uart_read_bytes(GPS_UART, buf, sizeof(buf), 0);
    if (n <= 0) return;
    /* Fresh batch (~one 1 Hz epoch): recompute the per-epoch aggregates that
     * span multiple sentences (in-view count, best C/N0) from scratch. */
    s_fix.satsInView = 0;
    s_fix.snrMax     = 0;
    do {
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n') {
                while (!s_line.empty() && s_line.back() == '\r') s_line.pop_back();
                if (nmeaChecksumOk(s_line)) nmeaApply(s_line, s_fix);
                s_line.clear();
            } else if (c == '$') {
                s_line.assign(1, c);                 /* resync on sentence start */
            } else if (s_line.size() >= 120) {
                s_line.clear();                       /* runaway line, drop it */
            } else if (!s_line.empty()) {
                s_line.push_back(c);
            }
        }
        if (n < (int)sizeof(buf)) break;             /* drained what was buffered */
        n = uart_read_bytes(GPS_UART, buf, sizeof(buf), 0);
    } while (n > 0);
}

/* ─────────────── CLI ─────────────── */

static void cliGps(const char* args) {
    if (args && strcmp(args, "help") == 0) {
        cliPrintf("  %-*s GNSS status\n",        CLI_HELP_COL, "gps");
        cliPrintf("  %-*s enable/disable GPS\n", CLI_HELP_COL, "gps on|off");
        return;
    }
    if (args && strcmp(args, "on")  == 0) { storageSet("s.gps.enable", 1); cliPrintf("enabled\n");  return; }
    if (args && strcmp(args, "off") == 0) { storageSet("s.gps.enable", 0); cliPrintf("disabled\n"); return; }

    cliPrintf("state:    %s\n", s_running ? (s_fix.valid ? "fix" : "acquiring")
                                          : (s_enabled ? "not detected" : "off"));
    cliPrintf("model:    %s\n", s_running ? modelForBaud(s_baud) : "-");
    cliPrintf("baud:     %d\n", s_baud);
    cliPrintf("interval: %d s\n", s_interval);
    if (s_fix.hasPos)
        cliPrintf("pos:      %.6f, %.6f  alt %.1f m\n",
                  s_fix.lat, s_fix.lon, s_fix.altMsl);
    cliPrintf("fix:      %s  q=%d\n",
              s_fix.fixType >= 3 ? "3D" : s_fix.fixType == 2 ? "2D" : "none", s_fix.quality);
    cliPrintf("sats:     %d used / %d in view  best snr %d dBHz\n",
              s_fix.satsUsed, s_fix.satsInView, s_fix.snrMax);
    cliPrintf("dop:      h=%.2f v=%.2f p=%.2f\n", s_fix.hdop, s_fix.vdop, s_fix.pdop);
    if (s_fix.hasDate && s_fix.hasTime)
        cliPrintf("utc:      %04d-%02d-%02d %02d:%02d:%02d\n",
                  s_fix.yr, s_fix.mo, s_fix.dy, s_fix.hh, s_fix.mi, s_fix.se);
}

/* ─────────────── task ─────────────── */

static void gpsTaskMain(void*) {
    info("task up (%s)", BOARD_NAME);
    itsClientInit(2);
    storageSubscribeChanges("s.gps", onCfgChange);

    /* Bring the wall clock up from the RTC before anything else. Read the gate
     * directly — applyConfig() hasn't run yet. */
    s_ignoreClock = storageGetInt("s.gps.ignore_clock", 0) != 0;
    if (!s_ignoreClock) rtcBootSync();

    for (;;) {
        if (s_cfgDirty) { s_cfgDirty = false; applyConfig(); }

        if (s_running) {
            drainUart();
            int64_t now = esp_timer_get_time();
            if (s_lastPublishUs == 0 ||
                now - s_lastPublishUs >= (int64_t)s_interval * 1000000) {
                publishFix();
                s_lastPublishUs = now;
            }
        }

        /* Heartbeat (RTC keeper + ntp-staleness reconcile) runs on its own ≤60 s
         * cadence regardless of whether the receiver is up — the clock must stay
         * alive even with GPS disabled or undetected. */
        int64_t now = esp_timer_get_time();
        if (s_lastHeartbeatUs == 0 || now - s_lastHeartbeatUs >= kHeartbeatUs) {
            gpsHeartbeat();
            s_lastHeartbeatUs = now;
        }

        /* Single wait point. Running: wake ≤1 s to drain the 1 Hz stream and to
         * hit the next publish deadline (no IRQ/PPS wired). Otherwise block until
         * a config change — but never longer than the next heartbeat is due. */
        int waitMs = (int)((kHeartbeatUs - (esp_timer_get_time() - s_lastHeartbeatUs)) / 1000);
        if (waitMs < 0) waitMs = 0;
        if (s_running) {
            int64_t toPub = (int64_t)s_interval * 1000000 -
                            (esp_timer_get_time() - s_lastPublishUs);
            int pubMs = (toPub <= 0) ? 0 : (int)(toPub / 1000);
            if (pubMs > 1000) pubMs = 1000;
            if (pubMs < waitMs) waitMs = pubMs;
        }
        itsPoll(pdMS_TO_TICKS(waitMs));
    }
}

/* ─────────────── settings (on-device) ───────────────
 * The GPS enable/interval and the detected-model readout live in the board's
 * own "T-Deck" Settings pane (tdeck.cpp), not a pane of our own — that's where
 * the user asked for them, alongside the touch-probe status. */

void gpsInit(void) {
    if (storageGetInt("s.gps.version", 0) < GPS_VERSION) {
        storageDefault("s.gps.enable", 0);
        storageDefault("s.gps.interval", 1);   /* seconds */
        storageDefault("s.gps.ignore_clock", 0);   /* 1 = don't set the system clock from GPS */
        storageSet("s.gps.version", GPS_VERSION);
    }
    cliRegisterCmd("gps", cliGps);
    s_task = spawnTask(gpsTaskMain, TAG, 6144, nullptr, 2, 0, STACK_PSRAM);
}
