/**
 * rtc — PCF8563 real-time clock driver. See rtc.h.
 */
#include "rtc.h"
#include "tdeck.h"
#include "log.h"

#include "driver/i2c_master.h"

#include <cstdint>

/* PCF8563: 7-bit I2C slave, 100 kHz, time registers auto-increment from 0x02. */
#define PCF8563_ADDR        0x51
#define PCF8563_REG_SECONDS 0x02   /* bit7 = VL (voltage low / integrity lost) */

/* The device handle is added to the shared bus once, lazily. */
static i2c_master_dev_handle_t s_dev = nullptr;

static i2c_master_dev_handle_t rtcDev(void) {
    if (s_dev) return s_dev;
    i2c_master_bus_handle_t bus = tdeckI2cBus();
    if (!bus) return nullptr;
    i2c_device_config_t dcfg = {};
    dcfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dcfg.device_address  = PCF8563_ADDR;
    dcfg.scl_speed_hz    = 100000;
    if (i2c_master_bus_add_device(bus, &dcfg, &s_dev) != ESP_OK) {
        warn("i2c add device failed (no PCF8563?)\n");
        s_dev = nullptr;
    }
    return s_dev;
}

bool rtcProbe(void) {
    i2c_master_bus_handle_t bus = tdeckI2cBus();
    if (!bus) return false;
    return i2c_master_probe(bus, PCF8563_ADDR, 50) == ESP_OK;
}

static inline uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + 10 * (v >> 4); }
static inline uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool rtcRead(struct tm* out, bool* clockValid) {
    i2c_master_dev_handle_t dev = rtcDev();
    if (!dev) return false;

    uint8_t reg = PCF8563_REG_SECONDS;
    uint8_t r[7] = {};
    /* Pointer write + 7-byte read in one transaction (repeated start). 50 ms
     * (xfer_timeout is in ms, per the i2c_master API) is generous; the bus is
     * shared and may be mid-keyboard-poll. */
    if (i2c_master_transmit_receive(dev, &reg, 1, r, sizeof(r), 50) != ESP_OK)
        return false;

    if (clockValid) *clockValid = !(r[0] & 0x80);   /* VL bit in seconds reg */

    struct tm t = {};
    t.tm_sec  = bcd2bin(r[0] & 0x7F);
    t.tm_min  = bcd2bin(r[1] & 0x7F);
    t.tm_hour = bcd2bin(r[2] & 0x3F);
    t.tm_mday = bcd2bin(r[3] & 0x3F);
    /* r[4] weekday (0-6) — unused; we recompute on write. */
    t.tm_mon  = bcd2bin(r[5] & 0x1F) - 1;            /* mask century bit */
    t.tm_year = bcd2bin(r[6]) + 100;                 /* always 20xx → years-since-1900 */
    *out = t;
    return true;
}

bool rtcWrite(const struct tm* t) {
    i2c_master_dev_handle_t dev = rtcDev();
    if (!dev) return false;

    /* Years outside 2000-2099 can't be represented in our century convention;
     * refuse rather than write a wrapped/garbage year. */
    int year = t->tm_year + 1900;
    if (year < 2000 || year > 2099) return false;

    uint8_t buf[8];
    buf[0] = PCF8563_REG_SECONDS;
    buf[1] = bin2bcd(t->tm_sec) & 0x7F;   /* VL = 0 → marks the clock trustworthy */
    buf[2] = bin2bcd(t->tm_min);
    buf[3] = bin2bcd(t->tm_hour);
    buf[4] = bin2bcd(t->tm_mday);
    buf[5] = 0;                            /* weekday — unused, written 0 */
    buf[6] = bin2bcd(t->tm_mon + 1);       /* century bit 0 = 20xx */
    buf[7] = bin2bcd((uint8_t)(year - 2000));
    return i2c_master_transmit(dev, buf, sizeof(buf), 50) == ESP_OK;
}
