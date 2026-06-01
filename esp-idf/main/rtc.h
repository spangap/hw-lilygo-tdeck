/**
 * rtc — PCF8563 real-time clock (T-Deck Plus, I2C0 @ 0x51).
 *
 * A thin BCD<->struct-tm shim over the on-board PCF8563, sharing the board's
 * I2C0 bus via tdeckI2cBus(). Time is always treated as UTC and always in the
 * 2000-2099 century: we write the century bit as 0 and mask it on read, which
 * sidesteps the PCF8563 century-bit polarity confusion (it only matters past
 * 2099, long after this hardware is relevant).
 *
 * The chip's voltage-low (VL) flag latches whenever the oscillator may have
 * stopped (cold boot with no/empty RTC backup cell). rtcRead() surfaces it as
 * `clockValid == false` so callers never trust a clock that may have lost time.
 * rtcWrite() clears VL, marking the time trustworthy again.
 *
 * gps.cpp owns all policy (when to read/write/sync); this module is pure HW.
 */
#pragma once
#include <time.h>

/** Read the RTC into *out (UTC). Returns true on a successful I2C read.
 *  *clockValid (if non-null) is set true only when the VL flag is clear, i.e.
 *  the oscillator has run continuously since the last rtcWrite(); when false
 *  the contents are stale/garbage and must not be trusted. */
bool rtcRead(struct tm* out, bool* clockValid);

/** Write *t (UTC) to the RTC and clear the VL flag. Returns true on success. */
bool rtcWrite(const struct tm* t);

/** True iff the PCF8563 ACKs its address on the shared bus (i2c probe). Lets
 *  callers distinguish "no chip / unreachable" from "present but time invalid". */
bool rtcProbe(void);
